// #600: the per-script hook table (runtime/hook/script_hooks.h) and the CScriptResource load/free wraps
// (runtime/hook/script_resource_hook.h), through MinHook and PatchDetour, against fake images.
//
// A fake script is one RWX allocation: at its function's RVA a prologue (`mov [rsp+8],rbx`) then `mov eax,1; ret`, so
// a call returns 1 stock and the detour's value once hooked. The fake game image carries the two CScriptResource
// functions at their real RVAs with their real first bytes and a short body, so the production wraps run around them.
//
// NOT shown by these tests: that the shipped echovr.exe loads and frees scripts through these two functions in a run
// (the PR's live run shows the load and free lines), and any real script's bytes.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"
#include "runtime/hook/script_resource_hook.h"

namespace {

namespace sh = nevr_script_hooks;
namespace srh = nevr_script_resource_hook;

std::vector<std::string> g_logged;
std::vector<EchoVR::LogLevel> g_levels;
void CaptureLog(EchoVR::LogLevel level, const CHAR* message) {
  g_levels.push_back(level);
  g_logged.push_back(message);
}
int Count(const char* needle, EchoVR::LogLevel level) {
  int n = 0;
  for (std::size_t i = 0; i < g_logged.size(); ++i) {
    if (g_levels[i] == level && g_logged[i].find(needle) != std::string::npos) ++n;
  }
  return n;
}

using Fn = int (*)();

constexpr std::uint8_t kPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08};  // mov [rsp+8], rbx
constexpr std::uint8_t kBody[] = {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};   // mov eax, 1; ret
constexpr std::uint8_t kOther[] = {0x48, 0x89, 0x6c, 0x24, 0x08};     // mov [rsp+8], rbp: not what a row expects
constexpr std::uint32_t kRvaA = 0x100;
constexpr std::uint32_t kRvaB = 0x200;
constexpr std::uint64_t kScript = 0x97d0806e6ea89130ULL;

Fn g_origA = nullptr;
Fn g_origB = nullptr;
int DetourA() { return 10 + g_origA(); }
int DetourB() { return 20 + g_origB(); }

// A fake script image: one committed page with two functions, and a reserved page after it that is not committed.
struct FakeScript {
  std::uint8_t* base = nullptr;
  FakeScript() {
    base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x2000, MEM_RESERVE, PAGE_NOACCESS));
    VirtualAlloc(base, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    for (std::uint32_t rva : {kRvaA, kRvaB}) {
      std::memcpy(base + rva, kPrologue, sizeof kPrologue);
      std::memcpy(base + rva + sizeof kPrologue, kBody, sizeof kBody);
    }
  }
  ~FakeScript() {
    MH_RemoveHook(base + kRvaA);
    MH_RemoveHook(base + kRvaB);
    VirtualFree(base, 0, MEM_RELEASE);
  }
  int Call(std::uint32_t rva) const { return reinterpret_cast<Fn>(base + rva)(); }
};

sh::Hook Row(std::uint32_t rva, const std::uint8_t* expected, void* detour, Fn* original, const char* name) {
  return sh::Hook{kScript, rva, expected, sizeof kPrologue, detour, reinterpret_cast<void**>(original), name};
}

class ScriptHooks : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    g_logged.clear();
    g_levels.clear();
    SetBootLogSink(&CaptureLog);
  }
  void TearDown() override {
    sh::ResetForTest();
    SetBootLogSink(nullptr);
  }
  FakeScript s_;
};

TEST_F(ScriptHooks, AScriptIsHookedWhenItLoadsAndUnhookedBeforeItIsFreed) {
  const sh::Hook rows[] = {Row(kRvaA, kPrologue, reinterpret_cast<void*>(&DetourA), &g_origA, "test A"),
                           Row(kRvaB, kPrologue, reinterpret_cast<void*>(&DetourB), &g_origB, "test B")};
  sh::Register(rows, 2);
  EXPECT_EQ(s_.Call(kRvaA), 1);
  sh::OnLoaded(kScript, s_.base);
  EXPECT_EQ(s_.Call(kRvaA), 11) << "the detour runs and reaches the original through the trampoline";
  EXPECT_EQ(s_.Call(kRvaB), 21);
  EXPECT_EQ(sh::HookedModules(), 1u);
  EXPECT_EQ(Count("script hooks installed script=97d0806e6ea89130", EchoVR::LogLevel::Info), 1);
  EXPECT_EQ(Count("hooks=2", EchoVR::LogLevel::Info), 1);

  sh::OnUnloading(kScript, s_.base);
  EXPECT_EQ(s_.Call(kRvaA), 1) << "the game's bytes are back before the free";
  EXPECT_EQ(s_.Call(kRvaB), 1);
  EXPECT_EQ(sh::LoadedModules(), 0u);
  EXPECT_EQ(Count("script hooks removed script=97d0806e6ea89130", EchoVR::LogLevel::Info), 1);

  // Loaded again (a level change): hooked again.
  sh::OnLoaded(kScript, s_.base);
  EXPECT_EQ(s_.Call(kRvaA), 11);
  sh::OnUnloading(kScript, s_.base);
  EXPECT_EQ(s_.Call(kRvaA), 1);
}

TEST_F(ScriptHooks, OneRowWhoseBytesDifferRefusesTheWholeScript) {
  const sh::Hook rows[] = {Row(kRvaA, kPrologue, reinterpret_cast<void*>(&DetourA), &g_origA, "test A"),
                           Row(kRvaB, kOther, reinterpret_cast<void*>(&DetourB), &g_origB, "test B")};
  sh::Register(rows, 2);
  sh::OnLoaded(kScript, s_.base);
  EXPECT_EQ(s_.Call(kRvaA), 1) << "the row whose bytes match is not installed either";
  EXPECT_EQ(s_.Call(kRvaB), 1);
  EXPECT_EQ(sh::HookedModules(), 0u);
  EXPECT_EQ(Count("hook not installed script=97d0806e6ea89130 name=\"test B\" rva=0x200", EchoVR::LogLevel::Warning), 1);
  EXPECT_EQ(Count("expected=48896c2408 actual=48895c2408", EchoVR::LogLevel::Warning), 1);
  EXPECT_EQ(Count("script hooks refused script=97d0806e6ea89130", EchoVR::LogLevel::Warning), 1);
  EXPECT_EQ(Count("mismatched=1", EchoVR::LogLevel::Warning), 1);
  sh::OnUnloading(kScript, s_.base);
  EXPECT_EQ(sh::LoadedModules(), 0u);
}

TEST_F(ScriptHooks, ARowPastTheImageIsRefusedNotRead) {
  const sh::Hook rows[] = {Row(0x1800, kPrologue, reinterpret_cast<void*>(&DetourA), &g_origA, "past the image")};
  sh::Register(rows, 1);
  sh::OnLoaded(kScript, s_.base);
  EXPECT_EQ(sh::HookedModules(), 0u);
  EXPECT_EQ(Count("actual=unreadable", EchoVR::LogLevel::Warning), 1);
}

TEST_F(ScriptHooks, AModuleLoadedTwiceIsUnhookedWhenTheLastReferenceGoes) {
  const sh::Hook rows[] = {Row(kRvaA, kPrologue, reinterpret_cast<void*>(&DetourA), &g_origA, "test A")};
  sh::Register(rows, 1);
  sh::OnLoaded(kScript, s_.base);
  sh::OnLoaded(kScript, s_.base);
  EXPECT_EQ(Count("script hooks installed", EchoVR::LogLevel::Info), 1) << "hooked once";
  sh::OnUnloading(kScript, s_.base);
  EXPECT_EQ(s_.Call(kRvaA), 11) << "one reference is still loaded";
  sh::OnUnloading(kScript, s_.base);
  EXPECT_EQ(s_.Call(kRvaA), 1);
}

TEST_F(ScriptHooks, AHookThatFailsToInstallTakesBackTheOnesBeforeIt) {
  // Another module already detours B (MinHook refuses a second hook at one address): A goes in, B fails, A comes out.
  static Fn s_taken = nullptr;
  ASSERT_EQ(MH_CreateHook(s_.base + kRvaB, reinterpret_cast<void*>(&DetourB), reinterpret_cast<void**>(&s_taken)), MH_OK);
  const sh::Hook rows[] = {Row(kRvaA, kPrologue, reinterpret_cast<void*>(&DetourA), &g_origA, "test A"),
                           Row(kRvaB, kPrologue, reinterpret_cast<void*>(&DetourB), &g_origB, "test B")};
  sh::Register(rows, 2);
  sh::OnLoaded(kScript, s_.base);
  EXPECT_EQ(s_.Call(kRvaA), 1) << "A was taken back";
  EXPECT_EQ(sh::HookedModules(), 0u);
  EXPECT_EQ(Count("script hooks refused script=97d0806e6ea89130", EchoVR::LogLevel::Warning), 1);
  EXPECT_EQ(Count("failed=\"test B\"", EchoVR::LogLevel::Warning), 1);
}

TEST_F(ScriptHooks, AnotherScriptIsNotHooked) {
  const sh::Hook rows[] = {Row(kRvaA, kPrologue, reinterpret_cast<void*>(&DetourA), &g_origA, "test A")};
  sh::Register(rows, 1);
  sh::OnLoaded(0x1234, s_.base);
  EXPECT_EQ(s_.Call(kRvaA), 1);
  EXPECT_EQ(sh::LoadedModules(), 1u);
  EXPECT_EQ(sh::HookedModules(), 0u);
  EXPECT_EQ(Count("script loaded script=0000000000001234", EchoVR::LogLevel::Debug), 1);
  sh::OnUnloading(0x1234, s_.base);
  EXPECT_EQ(Count("script freed script=0000000000001234", EchoVR::LogLevel::Debug), 1);
  EXPECT_EQ(sh::LoadedModules(), 0u);
}

// The CScriptResource wraps, around fake Load and free functions at their real RVAs in a fake game image.
class ScriptResourceHook : public ScriptHooks {
 protected:
  void SetUp() override {
    ScriptHooks::SetUp();
    srh::ResetForTest();
    image_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(image_, nullptr);
  }
  void TearDown() override {
    MH_RemoveHook(At(srh::kLoadVa));
    MH_RemoveHook(At(srh::kUnloadVa));
    srh::ResetForTest();
    VirtualFree(image_, 0, MEM_RELEASE);
    ScriptHooks::TearDown();
  }
  static constexpr std::size_t kImageBytes = 0x1300000;  // past the free function at 0x1412ece90
  std::uint8_t* At(std::uint64_t va) { return image_ + (va - 0x140000000ULL); }
  void PutStockFunctions() {
    // Load: its first 15 bytes (saves rbx, rsi, rdi), then `xor eax, eax; ret`: a load that succeeds.
    std::memcpy(At(srh::kLoadVa), srh::kLoadPrologue, sizeof srh::kLoadPrologue);
    const std::uint8_t load_tail[] = {0x31, 0xc0, 0xc3};
    std::memcpy(At(srh::kLoadVa) + sizeof srh::kLoadPrologue, load_tail, sizeof load_tail);
    // free: its first 13 bytes, then `mov qword [rbx+0x70], 0; add rsp, 0x20; pop rbx; ret`, as the game's clears it.
    std::memcpy(At(srh::kUnloadVa), srh::kUnloadPrologue, sizeof srh::kUnloadPrologue);
    const std::uint8_t free_tail[] = {0x48, 0xc7, 0x43, 0x70, 0, 0, 0, 0, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3};
    std::memcpy(At(srh::kUnloadVa) + sizeof srh::kUnloadPrologue, free_tail, sizeof free_tail);
  }
  std::uint8_t* image_ = nullptr;
};

struct FakeResource {
  std::uint8_t bytes[0x100] = {};
  FakeResource(std::uint64_t name, void* module) {
    std::memcpy(bytes + srh::kNameOffset, &name, sizeof name);
    std::memcpy(bytes + srh::kModuleOffset, &module, sizeof module);
  }
};

TEST_F(ScriptResourceHook, TheGamesLoadAndFreeHookAndUnhookTheScript) {
  PutStockFunctions();
  const sh::Hook rows[] = {Row(kRvaA, kPrologue, reinterpret_cast<void*>(&DetourA), &g_origA, "test A")};
  sh::Register(rows, 1);
  srh::Install(reinterpret_cast<std::uintptr_t>(image_));
  EXPECT_EQ(Count("script load hooks installed load_va=0x1412ebb50 free_va=0x1412ece90", EchoVR::LogLevel::Info), 1);

  FakeResource res(kScript, s_.base);
  using LoadFn = std::uint64_t (*)(void*);
  using FreeFn = void (*)(void*);
  EXPECT_EQ(reinterpret_cast<LoadFn>(At(srh::kLoadVa))(res.bytes), 0u) << "the game's Load runs and its result passes through";
  EXPECT_EQ(s_.Call(kRvaA), 11) << "hooked after the game's Load";
  reinterpret_cast<FreeFn>(At(srh::kUnloadVa))(res.bytes);
  EXPECT_EQ(s_.Call(kRvaA), 1) << "unhooked before the game's free";
  void* module = nullptr;
  std::memcpy(&module, res.bytes + srh::kModuleOffset, sizeof module);
  EXPECT_EQ(module, nullptr) << "the game's free ran after the wrap";
}

TEST_F(ScriptResourceHook, AGameWhoseBytesDifferGetsNeitherHook) {
  PutStockFunctions();
  At(srh::kUnloadVa)[0] = 0x90;
  srh::Install(reinterpret_cast<std::uintptr_t>(image_));
  EXPECT_EQ(Count("hook not installed name=\"CScriptResource free\" va=0x1412ece90 expected=4053", EchoVR::LogLevel::Warning), 1);
  EXPECT_EQ(Count("script load hooks not installed load_bytes=match free_bytes=differ", EchoVR::LogLevel::Warning), 1);
  EXPECT_EQ(std::memcmp(At(srh::kLoadVa), srh::kLoadPrologue, sizeof srh::kLoadPrologue), 0)
      << "the Load whose bytes match is not hooked either";
}

TEST_F(ScriptResourceHook, AFreeHookThatFailsTakesBackTheLoadHook) {
  PutStockFunctions();
  static void* s_taken = nullptr;
  ASSERT_EQ(MH_CreateHook(At(srh::kUnloadVa), reinterpret_cast<void*>(&DetourB), &s_taken), MH_OK);
  srh::Install(reinterpret_cast<std::uintptr_t>(image_));
  EXPECT_EQ(Count("script load hooks not installed load_bytes=match free_bytes=match", EchoVR::LogLevel::Warning), 1);
  EXPECT_EQ(std::memcmp(At(srh::kLoadVa), srh::kLoadPrologue, sizeof srh::kLoadPrologue), 0) << "the Load hook was removed";
}

TEST_F(ScriptResourceHook, AFailedLoadAndAnEmptyFreeTouchNoScript) {
  const sh::Hook rows[] = {Row(kRvaA, kPrologue, reinterpret_cast<void*>(&DetourA), &g_origA, "test A")};
  sh::Register(rows, 1);
  FakeResource res(kScript, s_.base);
  srh::AfterLoad(res.bytes, 1);
  EXPECT_EQ(s_.Call(kRvaA), 1);
  EXPECT_EQ(sh::LoadedModules(), 0u);
  FakeResource empty(kScript, nullptr);
  srh::AfterLoad(empty.bytes, 0);
  EXPECT_EQ(sh::LoadedModules(), 0u) << "a resource with no module holds nothing to hook";
  srh::BeforeUnload(empty.bytes);
  EXPECT_EQ(sh::LoadedModules(), 0u);
  EXPECT_EQ(Count("script", EchoVR::LogLevel::Debug), 0) << "an empty resource is not a script load or free";
  srh::AfterLoad(res.bytes, 0);
  EXPECT_EQ(s_.Call(kRvaA), 11);
  srh::BeforeUnload(res.bytes);
  EXPECT_EQ(s_.Call(kRvaA), 1);
}

}  // namespace

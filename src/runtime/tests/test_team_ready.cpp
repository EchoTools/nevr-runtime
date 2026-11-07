// #600: TEAM READY's hooks and slot on Combat's game-mode script (runtime/combat/team_ready.h), through the script hook
// table (runtime/hook/script_hooks.h, slot rows) under MinHook, on a fake script.
//
// The fake script is one RWX allocation with the real first bytes of the four functions at their real RVAs, each with a
// short body written here: the value expressions return false (the stock `!flag` of a set flag); the shouldexecutes and a
// third function at 0x30000 call the field accessor through the slot at 0x33BC0 (`call [rip+...]`, as the script does)
// for the field id stored at 0x33968, and return the byte it wrote. The slot holds a function of this test executable,
// standing in for the engine's (the table requires an engine function from the game executable, which this is here).
//
// NOT shown by these tests: the shipped script's bytes in a run (the PR's live run shows the install), and a private
// match with a demo account readying its team.

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
#include "runtime/combat/team_ready.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace tr = nevr_combat::team_ready;
namespace sh = nevr_script_hooks;

constexpr std::uint32_t kDemoField = 0x1234;
// The script binds the next dword (0x3396c) as another field: nonzero, so a read of 8 bytes at 0x33968 is not the id.
constexpr std::uint32_t kNextField = 0x5678;
constexpr std::uint32_t kOtherCallerRva = 0x30000;
constexpr std::size_t kImage = 0x34000;

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

// The engine's field accessor, stood in: every field reads as 1 ("a demo account"), and it returns a marker. It records
// what it was called with.
int g_engine_calls = 0;
void* g_engine_ctx = nullptr;
std::uint64_t g_engine_field = 0;
std::uint64_t EngineAccessor(void* ctx, std::uint64_t field, void* out) {
  ++g_engine_calls;
  g_engine_ctx = ctx;
  g_engine_field = field;
  *static_cast<std::uint8_t*>(out) = 1;
  return 0x55;
}

void Emit(std::vector<std::uint8_t>& v, std::initializer_list<std::uint8_t> b) { v.insert(v.end(), b); }
void Rel32(std::vector<std::uint8_t>& v, std::uint32_t at_rva, std::uint32_t target_rva) {
  const std::int32_t d = static_cast<std::int32_t>(target_rva) - static_cast<std::int32_t>(at_rva + v.size() + 4);
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(d) >> (8 * i)));
}

// rsp is 16-aligned at the call when `frame` is chosen for the function's entry (0x38 bare, 0x30 after push rbp).
std::vector<std::uint8_t> CallThroughSlot(std::uint32_t at_rva, std::uint8_t frame, bool pop_rbp) {
  std::vector<std::uint8_t> v;
  Emit(v, {0x48, 0x83, 0xec, frame});        // sub rsp, frame
  Emit(v, {0x4c, 0x8d, 0x44, 0x24, 0x28});   // lea r8, [rsp+0x28]
  Emit(v, {0xc6, 0x44, 0x24, 0x28, 0x00});   // mov byte [rsp+0x28], 0
  Emit(v, {0x8b, 0x15});                     // mov edx, dword [rip+field id], as the script loads it
  Rel32(v, at_rva, tr::kDemoFieldIdRva);
  Emit(v, {0x31, 0xc9});                     // xor ecx, ecx
  Emit(v, {0xff, 0x15});                     // call [rip+slot]
  Rel32(v, at_rva, tr::kAccessorSlotRva);
  Emit(v, {0x0f, 0xb6, 0x44, 0x24, 0x28});   // movzx eax, byte [rsp+0x28]
  Emit(v, {0x48, 0x83, 0xc4, frame});        // add rsp, frame
  if (pop_rbp) Emit(v, {0x5d});              // pop rbp
  Emit(v, {0xc3});                           // ret
  return v;
}

class TeamReady : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    tr::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    // Value expressions: the real start, an immediate, then `add rsp, 0x358; xor eax, eax; ret` (false).
    for (std::uint32_t rva : {tr::kBlueValueRva, tr::kRedValueRva}) {
      std::vector<std::uint8_t> v(tr::kValuePrologue, tr::kValuePrologue + sizeof tr::kValuePrologue);
      Emit(v, {1, 2, 3, 4, 5, 6, 7, 8, 0x48, 0x81, 0xc4, 0x58, 0x03, 0x00, 0x00, 0x31, 0xc0, 0xc3});
      std::memcpy(base_ + rva, v.data(), v.size());
    }
    // Shouldexecutes: the real start (which ends `push rbp`), then the demo read through the slot.
    for (std::uint32_t rva : {tr::kBlueShouldExecuteRva, tr::kRedShouldExecuteRva}) {
      std::memcpy(base_ + rva, tr::kShouldExecutePrologue, sizeof tr::kShouldExecutePrologue);
      const std::uint32_t body = rva + sizeof tr::kShouldExecutePrologue;
      const auto v = CallThroughSlot(body, 0x30, true);
      std::memcpy(base_ + body, v.data(), v.size());
    }
    const auto other = CallThroughSlot(kOtherCallerRva, 0x38, false);
    std::memcpy(base_ + kOtherCallerRva, other.data(), other.size());
    void* engine = reinterpret_cast<void*>(&EngineAccessor);
    std::memcpy(base_ + tr::kAccessorSlotRva, &engine, sizeof engine);
    std::memcpy(base_ + tr::kDemoFieldIdRva, &kDemoField, sizeof kDemoField);
    std::memcpy(base_ + tr::kDemoFieldIdRva + 4, &kNextField, sizeof kNextField);
  }
  void TearDown() override {
    for (std::uint32_t rva : {tr::kBlueValueRva, tr::kRedValueRva, tr::kBlueShouldExecuteRva, tr::kRedShouldExecuteRva}) {
      MH_RemoveHook(base_ + rva);
    }
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    tr::ResetForTest();
    SetBootLogSink(nullptr);
  }
  std::uint64_t Call(std::uint32_t rva) { return reinterpret_cast<std::uint64_t (*)(void*)>(base_ + rva)(nullptr) & 0xff; }
  void* Slot() {
    void* v = nullptr;
    std::memcpy(&v, base_ + tr::kAccessorSlotRva, sizeof v);
    return v;
  }
  std::uint8_t* base_ = nullptr;
};

TEST(TeamReadyField, TheEngineIsAskedWithTheCallersArguments) {
  g_engine_calls = 0;
  std::uint8_t out = 9;
  int ctx = 0;
  tr::ReadField(&EngineAccessor, kDemoField, false, &ctx, 0x77, &out);
  EXPECT_EQ(g_engine_calls, 1);
  EXPECT_EQ(g_engine_ctx, &ctx);
  EXPECT_EQ(g_engine_field, 0x77u);
}

TEST(TeamReadyField, TheFieldIdIsItsLowDword) {
  // A 32-bit parameter leaves the register's upper half unspecified: the id is compared on the low dword only.
  std::uint8_t out = 1;
  const std::uint64_t field = (std::uint64_t{0xdeadbeef} << 32) | kDemoField;
  tr::ReadField(&EngineAccessor, kDemoField, true, nullptr, field, &out);
  EXPECT_EQ(out, 0);
}

TEST(TeamReadyField, ADemoSenderReadsAsNotADemoOnlyInsideAShouldExecute) {
  std::uint8_t out = 0;
  EXPECT_EQ(tr::ReadField(&EngineAccessor, kDemoField, true, nullptr, kDemoField, &out), 0x55u) << "the engine's result";
  EXPECT_EQ(out, 0) << "inside a shouldexecute, the demo field reads as not a demo";
  EXPECT_EQ(tr::ReadField(&EngineAccessor, kDemoField, true, nullptr, kDemoField + 1, &out), 0x55u);
  EXPECT_EQ(out, 1) << "another field keeps the engine's value";
  EXPECT_EQ(tr::ReadField(&EngineAccessor, kDemoField, false, nullptr, kDemoField, &out), 0x55u);
  EXPECT_EQ(out, 1) << "outside a shouldexecute, the demo field keeps the engine's value";
}

TEST_F(TeamReady, StockTheValuesFlipAndADemoSenderCannotReady) {
  EXPECT_EQ(Call(tr::kBlueValueRva), 0u);
  EXPECT_EQ(Call(tr::kBlueShouldExecuteRva), 1u) << "the stock read: a demo account";
}

TEST_F(TeamReady, InstalledTheValuesAreTrueAndADemoSenderCanReady) {
  tr::Register();
  EXPECT_EQ(Count("team ready fix registered script=6fa8cc9cd721a563 hooks=4 slots=1"), 1);
  sh::OnLoaded(tr::kScript, base_);
  EXPECT_EQ(Count("script hooks installed script=6fa8cc9cd721a563"), 1);
  EXPECT_EQ(Count("hooks=4 slots=1"), 2);
  EXPECT_EQ(Call(tr::kBlueValueRva), 1u) << "blue: always true";
  EXPECT_EQ(Call(tr::kRedValueRva), 1u) << "red: always true";
  EXPECT_EQ(Call(tr::kBlueShouldExecuteRva), 0u) << "blue's demo test reads not a demo";
  EXPECT_EQ(Call(tr::kRedShouldExecuteRva), 0u) << "red's demo test reads not a demo";
  EXPECT_EQ(Call(kOtherCallerRva), 1u) << "the accessor's other callers read the engine's value";

  sh::OnUnloading(tr::kScript, base_);
  EXPECT_EQ(Slot(), reinterpret_cast<void*>(&EngineAccessor)) << "the engine's function is back in the slot";
  EXPECT_EQ(Call(tr::kBlueValueRva), 0u);
  EXPECT_EQ(Call(tr::kBlueShouldExecuteRva), 1u);
  EXPECT_EQ(Count("script hooks removed script=6fa8cc9cd721a563 base="), 1);
}

TEST_F(TeamReady, ASlotThatHoldsNoEngineFunctionRefusesTheScript) {
  void* not_engine = base_ + kOtherCallerRva;  // inside the script, not the game executable
  std::memcpy(base_ + tr::kAccessorSlotRva, &not_engine, sizeof not_engine);
  tr::Register();
  sh::OnLoaded(tr::kScript, base_);
  EXPECT_EQ(Count("slot not replaced script=6fa8cc9cd721a563"), 1);
  EXPECT_EQ(Count("script hooks refused script=6fa8cc9cd721a563"), 1);
  EXPECT_EQ(Call(tr::kBlueValueRva), 0u) << "the detours are not installed either";
  EXPECT_EQ(Slot(), not_engine);
}

TEST_F(TeamReady, AnEmptySlotRefusesTheScript) {
  void* empty = nullptr;
  std::memcpy(base_ + tr::kAccessorSlotRva, &empty, sizeof empty);
  tr::Register();
  sh::OnLoaded(tr::kScript, base_);
  EXPECT_EQ(Count("script hooks refused script=6fa8cc9cd721a563"), 1);
  EXPECT_EQ(Call(tr::kBlueValueRva), 0u);
}

TEST_F(TeamReady, ASlotRewrittenSinceTheInstallKeepsWhatItHolds) {
  tr::Register();
  sh::OnLoaded(tr::kScript, base_);
  void* other = reinterpret_cast<void*>(&CaptureLog);
  std::memcpy(base_ + tr::kAccessorSlotRva, &other, sizeof other);
  sh::OnUnloading(tr::kScript, base_);
  EXPECT_EQ(Slot(), other) << "only the replacement this table wrote is put back";
  EXPECT_EQ(Count("slot not put back name=\"team ready: field accessor"), 1) << "and it says so";
}

TEST_F(TeamReady, ADetourThatFailsRefusesTheScriptAndCountsItsSlotsToo) {
  static void* s_taken = nullptr;
  ASSERT_EQ(MH_CreateHook(base_ + tr::kRedValueRva, reinterpret_cast<void*>(&EngineAccessor), &s_taken), MH_OK);
  tr::Register();
  sh::OnLoaded(tr::kScript, base_);
  EXPECT_EQ(Count("script hooks refused script=6fa8cc9cd721a563"), 1);
  EXPECT_EQ(Count("rows=5 failed="), 1) << "four detours and one slot";
  EXPECT_EQ(Slot(), reinterpret_cast<void*>(&EngineAccessor)) << "no slot is replaced in a refused script";
}

TEST_F(TeamReady, AnotherScriptKeepsItsSlot) {
  tr::Register();
  sh::OnLoaded(0x1234, base_);
  EXPECT_EQ(Slot(), reinterpret_cast<void*>(&EngineAccessor)) << "the slot row is for 6fa8cc9cd721a563 only";
  EXPECT_EQ(Call(tr::kBlueValueRva), 0u);
  sh::OnUnloading(0x1234, base_);
}

TEST_F(TeamReady, AScriptWithOnlySlotRowsGetsThemReplaced) {
  static void* s_original = nullptr;
  static std::atomic<void*> s_module{nullptr};
  const sh::Slot slot = {0x4242, tr::kAccessorSlotRva, reinterpret_cast<void*>(&CaptureLog), &s_original, &s_module, "only a slot"};
  sh::Register(&slot, 1);
  sh::OnLoaded(0x4242, base_);
  EXPECT_EQ(Slot(), reinterpret_cast<void*>(&CaptureLog));
  EXPECT_EQ(s_original, reinterpret_cast<void*>(&EngineAccessor));
  EXPECT_EQ(s_module.load(), base_);
  EXPECT_EQ(sh::HookedModules(), 1u) << "a module with only a slot replaced is a hooked module";
  sh::OnUnloading(0x4242, base_);
  EXPECT_EQ(Slot(), reinterpret_cast<void*>(&EngineAccessor));
  EXPECT_EQ(s_module.load(), nullptr);
  EXPECT_EQ(Count("script hooks removed script=0000000000004242 base="), 1);
}

TEST_F(TeamReady, RegisteringTwiceAddsTheRowsOnce) {
  tr::Register();
  tr::Register();
  sh::OnLoaded(tr::kScript, base_);
  EXPECT_EQ(Count("team ready fix registered"), 1);
  EXPECT_EQ(Count("script hooks installed script=6fa8cc9cd721a563 base="), 1);
  EXPECT_EQ(Count("hooks=4 slots=1"), 2) << "the registration line and the install line, each with 4 and 1";
}

}  // namespace

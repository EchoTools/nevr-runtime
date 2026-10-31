// P46 (#1774): the SNSEarlyQuitFeatureFlags callback, replaced whole under MinHook against a fake game image.
//
// The fake image is one RWX allocation. The callback sits at its real RVA with its real 35 bytes, so before Install the
// stock body runs (it masks the byte with 0xDD and tail-calls DispatchEventToSession). DispatchEventToSession sits at its
// real RVA as a stub with the real 16 prologue bytes, which jumps to RecordDispatch. The event word sits at its real RVA.
// Install, PatchDetour, ValidatePrologue and the detour are the production code; only the game is stood in for.
//
// NOT shown by these tests: that echovr.exe has these bytes on a live process. That is the run on a game copy.

#include <gtest/gtest.h>

#include <windows.h>

#include <MinHook.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/patch/early_quit_lockout.h"

namespace {

constexpr std::uint64_t kImageBase = 0x140000000ULL;
constexpr std::uint64_t kCallbackVa = 0x1401618B0ULL;
constexpr std::uint64_t kDispatchVa = 0x1401A9FE0ULL;
constexpr std::uint64_t kEventVa = 0x1416DCD78ULL;
constexpr std::uint64_t kStockEvent = 0xC690A8FF1CF8AF99ULL;
constexpr std::size_t kImageBytes = 0x1800000;  // covers kEventVa + 8 (RVA 0x16DCD78) and every other VA Install reads
constexpr std::size_t kFlagsOffset = 0x64847;
constexpr std::size_t kNetGameBytes = 0x65000;
constexpr std::size_t kPageBytes = 0x1000;

// The real callback (echovr.exe 0x1401618b0, 35 bytes): mov rax,[rdx+8]; movzx edx,byte [rax]; and dl,0xdd;
// mov [rcx+0x64847],dl; lea rdx,[rip+0x157b4b8]; and rdx,-8; mov rdx,[rdx]; jmp 0x1401a9fe0.
constexpr std::array<std::uint8_t, 35> kRealCallback = {
    0x48, 0x8B, 0x42, 0x08,                    // mov rax,[rdx+8]
    0x0F, 0xB6, 0x10,                          // movzx edx,byte [rax]
    0x80, 0xE2, 0xDD,                          // and dl,0xdd
    0x88, 0x91, 0x47, 0x48, 0x06, 0x00,        // mov [rcx+0x64847],dl
    0x48, 0x8D, 0x15, 0xB8, 0xB4, 0x57, 0x01,  // lea rdx,[rip+0x157b4b8]
    0x48, 0x83, 0xE2, 0xF8,                    // and rdx,-8
    0x48, 0x8B, 0x12,                          // mov rdx,[rdx]
    0xE9, 0x0D, 0x87, 0x04, 0x00};             // jmp 0x1401a9fe0

// DispatchEventToSession's first 16 bytes (the runtime's own prologue pin). The last byte is a REX.W prefix, so the stub
// continues with movabs rax, imm64 and jmp rax.
constexpr std::array<std::uint8_t, 16> kDispatchPrologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                            0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48};

std::array<std::uint8_t, 27> MakeDispatchStub(std::uintptr_t target) {
  std::array<std::uint8_t, 27> stub{};
  std::memcpy(stub.data(), kDispatchPrologue.data(), kDispatchPrologue.size());
  stub[16] = 0xB8;  // movabs rax, imm64 (completes the REX.W prefix at stub[15])
  std::memcpy(stub.data() + 17, &target, sizeof(target));
  stub[25] = 0xFF;  // jmp rax
  stub[26] = 0xE0;
  return stub;
}

struct Dispatched {
  std::uintptr_t netGame;
  std::uint64_t event;
};
std::vector<Dispatched> g_dispatched;

void RecordDispatch(void* netGame, std::uint64_t event) {
  g_dispatched.push_back({reinterpret_cast<std::uintptr_t>(netGame), event});
}

std::vector<std::string> g_logged;

void CaptureLog(EchoVR::LogLevel /*level*/, const CHAR* message) { g_logged.emplace_back(message); }

int CountContaining(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) {
    if (line.find(needle) != std::string::npos) ++n;
  }
  return n;
}

using CallbackFn = void (*)(void* netGame, void* message);

// A separate call site, so the callback is entered the way the game's dispatcher enters it.
__attribute__((noinline)) void CallCallback(CallbackFn fn, void* netGame, void* message) { fn(netGame, message); }

class EarlyQuitFeatureFlagsHook : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    g_logged.clear();
    g_dispatched.clear();
    SetBootLogSink(&CaptureLog);

    image_ = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, kImageBytes, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(image_, nullptr) << "VirtualAlloc error=" << GetLastError();

    Put(kCallbackVa, kRealCallback.data(), kRealCallback.size());
    const std::array<std::uint8_t, 27> stub = MakeDispatchStub(reinterpret_cast<std::uintptr_t>(&RecordDispatch));
    Put(kDispatchVa, stub.data(), stub.size());
    PutValue<std::uint64_t>(kEventVa, kStockEvent);
    net_.assign(kNetGameBytes, 0);
  }

  void TearDown() override {
    if (image_ != nullptr) {
      // Hooking::Detach only disables; MinHook keeps the entry, and the next allocation can land on the same address.
      MH_RemoveHook(At(kCallbackVa));
      VirtualFree(image_, 0, MEM_RELEASE);
    }
    image_ = nullptr;
    SetBootLogSink(nullptr);
  }

  std::uintptr_t Base() const { return reinterpret_cast<std::uintptr_t>(image_); }
  std::uint8_t* At(std::uint64_t va) { return image_ + (va - kImageBase); }
  void Put(std::uint64_t va, const void* data, std::size_t length) { std::memcpy(At(va), data, length); }
  template <typename T>
  void PutValue(std::uint64_t va, T value) {
    Put(va, &value, sizeof(value));
  }

  // The game's dispatcher hands the callback a service message whose payload pointer is at +8. Returns the stored byte.
  std::uint8_t Send(std::uint8_t sent) {
    std::uint8_t payload = sent;
    const std::uint8_t* payloadPtr = &payload;
    std::array<std::uint8_t, 16> message{};
    std::memcpy(message.data() + 8, &payloadPtr, sizeof(payloadPtr));
    CallCallback(reinterpret_cast<CallbackFn>(At(kCallbackVa)), net_.data(), message.data());
    return net_[kFlagsOffset];
  }

  std::uint8_t* image_ = nullptr;
  std::vector<std::uint8_t> net_;
};

// Before Install the stock body runs: bits 1 and 5 are cleared and the event the game holds is dispatched. This is
// the baseline the replacement is measured against, and it proves the fixture is the game's bytes.
TEST_F(EarlyQuitFeatureFlagsHook, TheStockCallbackClearsBit1AndBit5AndDispatches) {
  EXPECT_EQ(Send(0xFF), 0xDD);
  ASSERT_EQ(g_dispatched.size(), 1U);
  EXPECT_EQ(g_dispatched[0].netGame, reinterpret_cast<std::uintptr_t>(net_.data()));
  EXPECT_EQ(g_dispatched[0].event, kStockEvent);
}

// The replacement stores the byte as sent: bit 1, which the stock mask clears, survives. Without the hook the first
// expectation reads 0xdd and fails.
TEST_F(EarlyQuitFeatureFlagsHook, TheReplacementStoresEveryBitTheServiceSends) {
  nevr_early_quit_lockout::Install(Base());
  EXPECT_EQ(Send(0xFF), 0xFF);
  EXPECT_EQ(Send(0x02), 0x02) << "bit 1, which the stock mask clears, is kept";
  EXPECT_EQ(Send(0x00), 0x00);
  ASSERT_EQ(g_dispatched.size(), 3U);
  for (const Dispatched& d : g_dispatched) {
    EXPECT_EQ(d.netGame, reinterpret_cast<std::uintptr_t>(net_.data()));
    EXPECT_EQ(d.event, kStockEvent);
  }
}

// Each message is dispatched once, after the byte is stored, and logged at Debug with the value and the byte it replaced.
TEST_F(EarlyQuitFeatureFlagsHook, EachMessageIsLoggedAtDebugWithTheValueAndThePreviousByte) {
  nevr_early_quit_lockout::Install(Base());
  EXPECT_EQ(Send(0x04), 0x04);
  EXPECT_EQ(Send(0x06), 0x06);
  EXPECT_EQ(CountContaining("[NEVR.EARLYQUIT] feature flags set value=0x04 was=0x00"), 1);
  EXPECT_EQ(CountContaining("[NEVR.EARLYQUIT] feature flags set value=0x06 was=0x04"), 1);
}

// Install says what it hooked, once, and does not say the hook was skipped.
TEST_F(EarlyQuitFeatureFlagsHook, InstallSaysWhatItHooked) {
  nevr_early_quit_lockout::Install(Base());
  EXPECT_EQ(CountContaining("hooked name=EarlyQuitFeatureFlagsCB va=0x1401618B0"), 1);
  EXPECT_EQ(CountContaining("name=EarlyQuitFeatureFlagsCB va=0x1401618B0 reason=prologue_mismatch"), 0);
}

// A prologue that does not match leaves the stock callback in place (its own mask applies), and the Warning carries the
// expected and the actual bytes (logging.md Rule 6).
TEST_F(EarlyQuitFeatureFlagsHook, APrologueMismatchLeavesTheStockCallbackAndSaysWhy) {
  PutValue<std::uint8_t>(kCallbackVa + 9, 0xEE);  // the mask byte: the stock callback now masks with 0xee
  nevr_early_quit_lockout::Install(Base());
  EXPECT_EQ(CountContaining("hooked name=EarlyQuitFeatureFlagsCB"), 0);
  EXPECT_EQ(CountContaining("reason=prologue_mismatch expected=488b42080fb61080e2dd889147480600 "
                            "actual=488b42080fb61080e2ee889147480600"),
            1);
  EXPECT_EQ(Send(0xFF), 0xEE) << "the stock body runs with its own mask";
}

// The event is the one the game's data holds, read on each call, not a constant.
TEST_F(EarlyQuitFeatureFlagsHook, TheReplacementDispatchesTheEventTheGameDataHolds) {
  PutValue<std::uint64_t>(kEventVa, 0x1234567890ABCDEFULL);
  nevr_early_quit_lockout::Install(Base());
  EXPECT_EQ(Send(0x04), 0x04);
  ASSERT_EQ(g_dispatched.size(), 1U);
  EXPECT_EQ(g_dispatched[0].event, 0x1234567890ABCDEFULL);
}

// An event word that cannot be read is said so, and nothing is dispatched; the byte is still stored as sent. The page is
// decommitted, which ResolveVA_Checked reports as unreadable.
TEST_F(EarlyQuitFeatureFlagsHook, AnUnreadableEventWordIsSaidAndNothingIsDispatched) {
  nevr_early_quit_lockout::Install(Base());
  const std::uint64_t page = kEventVa & ~static_cast<std::uint64_t>(kPageBytes - 1);
  ASSERT_TRUE(VirtualFree(At(page), kPageBytes, MEM_DECOMMIT)) << "VirtualFree error=" << GetLastError();
  EXPECT_EQ(Send(0x04), 0x04);
  EXPECT_TRUE(g_dispatched.empty());
  EXPECT_EQ(CountContaining("event word unreadable va=0x1416DCD78"), 1);
}

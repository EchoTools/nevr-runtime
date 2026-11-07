// #600: the capture point's unlock time and its countdown under PAUSE (runtime/combat/unlock_time.h): the decisions,
// and the five hooks and three pins installed through the script hook table under MinHook on a fake capture point
// script.
//
// The countdown node is its pin, the whole stock node, so it runs as shipped (it reads the frame's now at the context's
// +0x58 and divides by the 1e6 the fake lays out at 0x2A1D8). The others are their pins and tails this test writes in
// the stock code's shape:
//  - 0x1F130: its pin through the lock read, then `subss xmm0, xmm1` and its epilogue (lock - elapsed);
//  - the get-by-name: its pinned head, a jump to its pinned lock read at 0x179CF, then `mov eax, 1; mov [r9], ecx; ret`;
//    its two misses return 0;
//  - the state pack: its pinned head (the pushes), `push r15; sub rsp, 0x38; mov rsi, rcx`, a jump to its pinned read at
//    0x91D4, then `mov [rdx], ecx` and the epilogue;
//  - the typed getter: its pinned head, a jump from 0x4C9A to its pinned read at 0x4CCB, then `mov [r9+8], eax;
//    mov eax, 1; ret`; on a name miss it writes the object's +0xEC to [r9] and returns 0, so a swap made on that path
//    would show.
// The node, the context and the globals are test buffers with nonzero bytes around every field read or written.
//
// NOT shown by these tests: the shipped script's bytes in a run, a capture point unlocking in a Combat match, a PAUSE.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include <MinHook.h>

#include "core/hooking.h"
#include "core/logging.h"
#include "runtime/combat/rules_state.h"
#include "runtime/combat/unlock_time.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace ut = nevr_combat::unlock_time;
namespace rules = nevr_combat::rules;
namespace sh = nevr_script_hooks;

constexpr std::size_t kImage = 0x2B000;
constexpr std::uint32_t kNameRva = 0x2997F, kMillionRva = 0x2A1D8;
constexpr std::uint64_t kLockName = 0x3acad89454787129ULL;  // the get-by-name's name for the lock time

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

void Jmp(std::uint8_t* base, std::uint32_t at, std::uint32_t to) {
  base[at] = 0xe9;
  const std::int32_t d = static_cast<std::int32_t>(to) - static_cast<std::int32_t>(at + 5);
  std::memcpy(base + at + 1, &d, sizeof d);
}
void Put(std::uint8_t* base, std::uint32_t rva, const std::uint8_t* p, std::size_t n, std::initializer_list<std::uint8_t> tail) {
  std::vector<std::uint8_t> v(p, p + n);
  v.insert(v.end(), tail);
  std::memcpy(base + rva, v.data(), v.size());
}
float Float(const std::uint8_t* at) {
  float f = 0;
  std::memcpy(&f, at, sizeof f);
  return f;
}
void SetFloat(std::uint8_t* at, float f) { std::memcpy(at, &f, sizeof f); }

TEST(UnlockTimeDecision, TheLockIsTOverSixtyAndPauseFreezesTheCountdown) {
  std::uint8_t box[0x40];
  std::memset(box, 0xa5, sizeof box);
  const rules::RulesState r(box);
  SetFloat(box + rules::kRoundTotal, 0.0f);
  EXPECT_EQ(ut::EffectiveLock(r, 30.0f), 30.0f) << "unset: the map's own";
  SetFloat(box + rules::kRoundTotal, 2704.0f);
  EXPECT_EQ(ut::EffectiveLock(r, 30.0f), 45.0f) << "45:04 -> 45";
  SetFloat(box + rules::kRoundTotal, 125.9f);
  EXPECT_EQ(ut::EffectiveLock(r, 30.0f), 2.0f) << "cvttss2si, then idiv";
  SetFloat(box + rules::kRoundTotal, 0.5f);
  EXPECT_EQ(ut::EffectiveLock(r, 30.0f), 30.0f) << "set, but int(T) is 0: his `test eax, eax; jle keep`";
  SetFloat(box + rules::kRoundTotal, std::numeric_limits<float>::quiet_NaN());
  EXPECT_EQ(ut::EffectiveLock(r, 30.0f), 30.0f) << "NaN converts to INT_MIN";
  SetFloat(box + rules::kRoundTotal, 3e9f);
  EXPECT_EQ(ut::EffectiveLock(r, 30.0f), 30.0f) << "out of range converts to INT_MIN";
  SetFloat(box + rules::kRoundTotal, 59.0f);
  EXPECT_EQ(ut::EffectiveLock(r, 30.0f), 0.0f) << "under a minute: an unlock at 0";
  const ut::Frozen f = ut::Freeze(45.0f, 6.0f, 4.0f, 20.0f);
  EXPECT_EQ(f.countdown, 20.0f);
  EXPECT_EQ(f.start, -15.0f) << "(6 + 4) - (45 - 20)";
  // Values where another order of the same sums rounds differently (found by search over float32).
  volatile float l = 1486.81f, e = 224.746f, s = 1954.779f, c = 226.053f;
  const ut::Frozen g = ut::Freeze(l, e, s, c);
  volatile float x1 = l - c, now = e + s;
  EXPECT_EQ(g.start, now - x1) << "his order, bit for bit";
  EXPECT_EQ(g.countdown, l - x1);
  EXPECT_NE(g.countdown, c) << "lock - (lock - countdown) is not the countdown here: the write is his";
}

class UnlockTimeInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    ut::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    std::memset(box_, 0xa5, sizeof box_);
    SetFloat(box_ + rules::kRoundTotal, 0.0f);
    box_[rules::kPausedShown] = 0;
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    Put(base_, 0x10030, ut::kCountdownNode, sizeof ut::kCountdownNode, {});
    Put(base_, 0x1F130, ut::kLockLeftExpression, sizeof ut::kLockLeftExpression,
        {0xf3, 0x0f, 0x5c, 0xc1, 0x48, 0x83, 0xc4, 0x58, 0xc3});
    Put(base_, 0x178F0, ut::kGetterHead, sizeof ut::kGetterHead, {});
    Jmp(base_, 0x178F0 + sizeof ut::kGetterHead, 0x179CF);
    Put(base_, 0x179CF, ut::kGetterLockRead, sizeof ut::kGetterLockRead, {0xb8, 0x01, 0x00, 0x00, 0x00, 0x41, 0x89, 0x09, 0xc3});
    Put(base_, 0x179F1, nullptr, 0, {0x31, 0xc0, 0xc3});
    Put(base_, 0x17A7D, nullptr, 0, {0x31, 0xc0, 0xc3});
    Put(base_, 0x8AC0, ut::kStatePackHead, sizeof ut::kStatePackHead,
        {0x41, 0x57, 0x48, 0x83, 0xec, 0x38, 0x48, 0x89, 0xce});  // push r15; sub rsp, 0x38; mov rsi, rcx
    Jmp(base_, 0x8AC0 + sizeof ut::kStatePackHead + 9, 0x91D4);
    Put(base_, 0x91D4, ut::kStatePackLockRead, sizeof ut::kStatePackLockRead,
        {0x89, 0x0a, 0x48, 0x83, 0xc4, 0x38, 0x41, 0x5f, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f, 0x5e, 0x5d, 0x5b,
         0x31, 0xc0, 0xc3});
    Put(base_, 0x4BC0, ut::kTypedGetterHead, sizeof ut::kTypedGetterHead, {0x31, 0xc0, 0xc3});
    Jmp(base_, 0x4C9A, 0x4CCB);
    Put(base_, 0x4CCB, ut::kTypedGetterLockRead, sizeof ut::kTypedGetterLockRead,
        {0x41, 0x89, 0x41, 0x08, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3});
    Put(base_, 0x4D83, nullptr, 0, {0x8b, 0x81, 0xec, 0x00, 0x00, 0x00, 0x41, 0x89, 0x01, 0x31, 0xc0, 0xc3});
    Put(base_, 0x4D39, nullptr, 0, {0x8b, 0x81, 0xec, 0x00, 0x00, 0x00, 0x41, 0x89, 0x01, 0x31, 0xc0, 0xc3});
    std::memcpy(base_ + kNameRva, "point", 6);
    SetFloat(base_ + kMillionRva, 1e6f);

    std::memset(node_, 0xa5, sizeof node_);
    std::memset(context_, 0xa5, sizeof context_);
    std::memset(globals_, 0xa5, sizeof globals_);
    std::uint8_t* context = context_;
    std::memcpy(node_, &context, sizeof context);
    node_[ut::kNodeNoWrite] = 0;
    std::uint8_t* globals = globals_;
    std::memcpy(context_ + ut::kContextGlobals, &globals, sizeof globals);
    const std::int64_t now = 10000000;  // 10 s
    std::memcpy(context_ + ut::kContextNow, &now, sizeof now);
    SetFloat(globals_ + ut::kCountdownStart, 4.0f);  // elapsed 6 s
    SetFloat(globals_ + ut::kLockTime, 30.0f);       // the map's own
    SetFloat(globals_ + ut::kCountdown, 20.0f);
  }
  void TearDown() override {
    for (std::uint32_t rva : {0x10030u, 0x1F130u, 0x178F0u, 0x8AC0u, 0x4BC0u}) MH_RemoveHook(base_ + rva);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    ut::ResetForTest();
    SetBootLogSink(nullptr);
  }
  void Load() {
    ut::Register(box_);
    sh::OnLoaded(ut::kScript, base_);
  }
  void SetT(float t) { SetFloat(box_ + rules::kRoundTotal, t); }
  std::uint64_t Countdown() { return reinterpret_cast<std::uint64_t (*)(void*)>(base_ + 0x10030)(node_); }
  float LockLeft() { return reinterpret_cast<float (*)(void*)>(base_ + 0x1F130)(node_); }
  using Four = std::uint64_t (*)(void*, std::uint64_t, std::uint64_t, void*);
  std::uint64_t Four4(std::uint32_t rva, void* a, std::uint64_t b, std::uint64_t c, void* d) {
    return reinterpret_cast<Four>(base_ + rva)(a, b, c, d);
  }
  float Own() const { return Float(globals_ + ut::kLockTime); }
  std::uint8_t* base_ = nullptr;
  std::uint8_t box_[0x40];
  std::uint8_t node_[0x100];
  std::uint8_t context_[0x80];
  std::uint8_t globals_[0x120];
};

TEST_F(UnlockTimeInstalled, EveryReaderSeesTOverSixtyAndTheMapsOwnIsKept) {
  Load();
  EXPECT_EQ(Count("script hooks installed script=b7d10bcffcb2a780"), 1);
  EXPECT_EQ(Count("hooks=5 slots=0 pins=3"), 1);
  // Unset: the map's own 30.
  EXPECT_EQ(Countdown(), 0u);
  EXPECT_EQ(Float(globals_ + ut::kCountdown), 24.0f) << "30 - 6, the stock node";
  EXPECT_EQ(LockLeft(), 24.0f);
  SetT(2704.0f);  // 45:04 -> 45
  EXPECT_EQ(Countdown(), 0u);
  EXPECT_EQ(Float(globals_ + ut::kCountdown), 39.0f) << "45 - 6";
  EXPECT_EQ(Own(), 30.0f) << "the map's own is put back";
  EXPECT_EQ(LockLeft(), 39.0f);
  EXPECT_EQ(Own(), 30.0f);
  float out[4] = {};
  EXPECT_EQ(Four4(0x178F0, context_, kLockName, 0xc8c33e4828761fbcULL, out), 1u);
  EXPECT_EQ(out[0], 45.0f) << "get-by-name";
  EXPECT_EQ(Own(), 30.0f);
  std::uint32_t packed = 0;
  Four4(0x8AC0, context_, reinterpret_cast<std::uint64_t>(&packed), 0, nullptr);
  float packed_f = 0;
  std::memcpy(&packed_f, &packed, sizeof packed_f);
  EXPECT_EQ(packed_f, 45.0f) << "the state pack copies the float's bits";
  EXPECT_EQ(Own(), 30.0f);
  std::memset(out, 0, sizeof out);
  EXPECT_EQ(Four4(0x4BC0, globals_, ut::kTypedGetterLockName, 1, out), 1u);
  EXPECT_EQ(out[2], 45.0f) << "the typed getter's lock path";
  EXPECT_EQ(Own(), 30.0f);
  // Another name on the typed getter's type-1 path: no swap there (the fake shows the object's +0xEC it saw).
  std::memset(out, 0, sizeof out);
  EXPECT_EQ(Four4(0x4BC0, globals_, 0x1234, 1, out), 0u);
  EXPECT_EQ(out[0], 30.0f) << "not the lock path: the object is left as it is";
  std::memset(out, 0, sizeof out);
  EXPECT_EQ(Four4(0x4BC0, globals_, ut::kTypedGetterLockName, 0, out), 0u) << "another type";
  EXPECT_EQ(out[0], 30.0f) << "not the lock path: the object is left as it is";
  EXPECT_EQ(globals_[ut::kLockTime + 4], 0xa5);
  EXPECT_EQ(globals_[ut::kLockTime - 1], 0xa5);
}

TEST_F(UnlockTimeInstalled, ThePauseHoldsTheCountdown) {
  Load();
  SetT(2704.0f);
  box_[rules::kPausedShown] = static_cast<std::uint8_t>(rules::Pause::kOn);
  EXPECT_EQ(Countdown(), 0u);
  EXPECT_EQ(Float(globals_ + ut::kCountdown), 20.0f) << "the countdown stands still";
  EXPECT_EQ(Float(globals_ + ut::kCountdownStart), -15.0f) << "the start moved: (6 + 4) - (45 - 20)";
  EXPECT_EQ(Own(), 30.0f);
  box_[rules::kPausedShown] = static_cast<std::uint8_t>(rules::Pause::kResuming);
  SetFloat(globals_ + ut::kCountdownStart, 4.0f);
  EXPECT_EQ(Countdown(), 0u);
  EXPECT_EQ(Float(globals_ + ut::kCountdown), 20.0f) << "the resume countdown holds it too";
  // Running again: the stock node counts on from where it stood.
  box_[rules::kPausedShown] = 0;
  SetFloat(globals_ + ut::kCountdownStart, -15.0f);
  EXPECT_EQ(Countdown(), 0u);
  EXPECT_EQ(Float(globals_ + ut::kCountdown), 20.0f) << "45 - (10 - (-15))";
  // +0xDB set: nothing written, -1, paused or not.
  box_[rules::kPausedShown] = 1;
  node_[ut::kNodeNoWrite] = 1;
  SetFloat(globals_ + ut::kCountdown, 7.0f);
  EXPECT_EQ(Countdown(), ~std::uint64_t{0});
  EXPECT_EQ(Float(globals_ + ut::kCountdown), 7.0f);
  EXPECT_EQ(Float(globals_ + ut::kCountdownStart), -15.0f);
}

TEST_F(UnlockTimeInstalled, StockAfterTheFree) {
  Load();
  sh::OnUnloading(ut::kScript, base_);
  SetT(2704.0f);
  box_[rules::kPausedShown] = 1;
  EXPECT_EQ(Countdown(), 0u);
  EXPECT_EQ(Float(globals_ + ut::kCountdown), 24.0f) << "the stock node: the map's own, no freeze";
}

TEST_F(UnlockTimeInstalled, APinWhoseBytesDifferRefusesTheScript) {
  base_[0x91D8 + 2] = 0xF0;  // the state pack reads another field
  Load();
  EXPECT_EQ(Count("script hooks refused script=b7d10bcffcb2a780"), 1);
  SetT(2704.0f);
  Countdown();
  EXPECT_EQ(Float(globals_ + ut::kCountdown), 24.0f) << "the whole script runs stock";
}

TEST_F(UnlockTimeInstalled, RegisteringTwiceAddsTheRowsOnce) {
  ut::Register(box_);
  ut::Register(box_);
  sh::OnLoaded(ut::kScript, base_);
  EXPECT_EQ(Count("unlock time fix registered"), 1);
  EXPECT_EQ(Count("hooks=5 slots=0 pins=3"), 1);
}

}  // namespace

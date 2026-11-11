// #600: POINT RATE and PAUSE on the capture points (runtime/combat/point_rate.h): the decisions, and the two hooks and
// the slot installed through the script hook table under MinHook on a fake capture point script.
//
// Each fake expression is the row's pinned bytes (the stock expression from its start through its read of required, so
// the game's own reads, its engine calls through the slots at 0x2C960 and 0x2CB58, its frame-time load and divide and
// its multiply and add run as shipped), then the stock tail this test writes: the clamp and the epilogue. The fake lays
// out what those bytes reach at the stock RVAs: the name string at 0x2997F, 1.0 at 0x2A194, 1e6 at 0x2A1D8, the
// answer's field id at 0x2C6B4 (a dword with a nonzero dword after it). The slots hold functions of this test standing
// in for the engine's: a context builder that does nothing, and an accessor that writes a qword answer.
//
// The node, its context and the globals are test buffers with nonzero bytes around every field the hooks read.
//
// NOT shown by these tests: the shipped script's bytes in a run, and a held capture point in a Combat match.

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
#include "runtime/combat/point_rate.h"
#include "runtime/combat/rules_state.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace pr = nevr_combat::point_rate;
namespace rules = nevr_combat::rules;
namespace sh = nevr_script_hooks;

constexpr std::uint32_t kBlue = 0x230A0;
constexpr std::uint32_t kOrange = 0x223F0;
constexpr std::uint32_t kNameRva = 0x2997F;
constexpr std::uint32_t kOneRva = 0x2A194;
constexpr std::uint32_t kMillionRva = 0x2A1D8;
constexpr std::uint32_t kBuilderSlotRva = 0x2C960;
constexpr std::uint32_t kOtherCallerRva = 0x20000;
constexpr std::uint32_t kOtherFieldRva = 0x2C700;  // a field id the fake can be pointed at instead
constexpr std::uint32_t kAnswerField = 0x44;
constexpr std::size_t kImage = 0x2D000;
// Where each expression loads the answer's field id (`mov edx, dword [rip]`), from its start.
constexpr std::uint32_t kBlueIdLoad = 0x23174 - kBlue;
constexpr std::uint32_t kOrangeIdLoad = 0x2251E - kOrange;

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

std::uint64_t g_answer = 0;
int g_asked = 0;
void Builder(void*, void*, void*, void*) {}
std::uint64_t EngineAccessor(void*, std::uint64_t, void* out) {
  ++g_asked;
  std::memcpy(out, &g_answer, sizeof g_answer);
  return 0x55;
}

void Emit(std::vector<std::uint8_t>& v, std::initializer_list<std::uint8_t> b) { v.insert(v.end(), b); }
void Rel32(std::vector<std::uint8_t>& v, std::uint32_t at_rva, std::uint32_t target_rva) {
  const std::int32_t d = static_cast<std::int32_t>(target_rva) - static_cast<std::int32_t>(at_rva + v.size() + 4);
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(d) >> (8 * i)));
}

// The stock tails after the read of required: the clamp and the epilogue.
void BlueTail(std::vector<std::uint8_t>& v) {
  Emit(v, {0x8b, 0xc3, 0x0f, 0x2f, 0xc1, 0xc6, 0x44, 0x04, 0x7c, 0x00, 0x77, 0x08, 0x0f, 0x57, 0xc9, 0x0f, 0x2f, 0xc8,
           0x76, 0x03, 0x0f, 0x28, 0xc1});
  Emit(v, {0x4c, 0x8d, 0x9c, 0x24, 0xe0, 0x03, 0x00, 0x00, 0x49, 0x8b, 0x5b, 0x10, 0x49, 0x8b, 0x73, 0x18, 0x41, 0x0f,
           0x28, 0x73, 0xf0, 0x41, 0x0f, 0x28, 0x7b, 0xe0, 0x49, 0x8b, 0xe3, 0x5f, 0xc3});
}
void OrangeTail(std::vector<std::uint8_t>& v) {
  Emit(v, {0x8b, 0xc3, 0x0f, 0x2f, 0xf0, 0xc6, 0x44, 0x04, 0x7c, 0x00, 0x77, 0x0b, 0x0f, 0x57, 0xc0, 0x0f, 0x2f, 0xc6,
           0x77, 0x03, 0x0f, 0x28, 0xc6});
  Emit(v, {0x4c, 0x8d, 0x9c, 0x24, 0x00, 0x04, 0x00, 0x00, 0x49, 0x8b, 0x5b, 0x10, 0x49, 0x8b, 0x73, 0x18, 0x41, 0x0f,
           0x28, 0x73, 0xf0, 0x41, 0x0f, 0x28, 0x7b, 0xe0, 0x45, 0x0f, 0x28, 0x43, 0xd0, 0x45, 0x0f, 0x28, 0x4b, 0xc0,
           0x49, 0x8b, 0xe3, 0x5f, 0xc3});
}

TEST(PointRateDecision, TheRateScalesDtAndPauseMakesItZero) {
  EXPECT_FALSE(pr::Acts(rules::Pause::kOff, 0));
  EXPECT_TRUE(pr::Acts(rules::Pause::kOff, 4)) << "x1 is a set rate";
  EXPECT_TRUE(pr::Acts(rules::Pause::kOn, 0));
  EXPECT_TRUE(pr::Acts(rules::Pause::kResuming, 0)) << "his `cmp byte [paused], 0; jne`";
  EXPECT_EQ(pr::Dt(500000, rules::Pause::kOff, 0), 0.5f);
  EXPECT_EQ(pr::Dt(500000, rules::Pause::kOff, 4), 0.5f);
  EXPECT_EQ(pr::Dt(500000, rules::Pause::kOff, 8), 1.0f);
  EXPECT_EQ(pr::Dt(500000, rules::Pause::kOff, 1), 0.125f);
  EXPECT_EQ(pr::Dt(500000, rules::Pause::kOn, 8), 0.0f) << "paused wins over the rate";
  EXPECT_EQ(pr::Dt(500000, rules::Pause::kResuming, 0), 0.0f);
  // Bit for bit his order: (dt / 1e6) * (rate * 0.25), not dt * rate / 4e6.
  const std::int64_t us = 16667;
  volatile float d = static_cast<float>(us);
  d = d / 1e6f;
  volatile float s = static_cast<float>(3);
  s = s * 0.25f;
  d = d * s;
  EXPECT_EQ(pr::Dt(us, rules::Pause::kOff, 3), d);
}

TEST(PointRateDecision, RiseIsTheGamesExpressionAndClamp) {
  EXPECT_EQ(pr::Rise(10.0f, 0.5f, false, 2.0f, true, 3.0f, 100.0f), 13.0f);
  EXPECT_EQ(pr::Rise(10.0f, 0.5f, true, 2.0f, true, 3.0f, 100.0f), 11.5f) << "the team's answer: k1 is 1";
  EXPECT_EQ(pr::Rise(10.0f, 0.5f, false, 2.0f, false, 3.0f, 100.0f), 11.0f) << "+0x11B clear: k2 is 1";
  EXPECT_EQ(pr::Rise(99.0f, 0.5f, false, 2.0f, true, 3.0f, 100.0f), 100.0f) << "above required";
  EXPECT_EQ(pr::Rise(-5.0f, 0.5f, false, 2.0f, true, 1.0f, 100.0f), 0.0f) << "below 0";
  EXPECT_EQ(pr::Rise(10.0f, 0.0f, false, 2.0f, true, 3.0f, 100.0f), 10.0f) << "dt 0: the points stand still";
  EXPECT_TRUE(std::isnan(pr::Rise(std::numeric_limits<float>::quiet_NaN(), 0.5f, false, 2.0f, true, 3.0f, 100.0f)))
      << "comiss: NaN is neither above nor below";
  EXPECT_EQ(pr::Rise(100.0f, 0.0f, false, 2.0f, true, 3.0f, 100.0f), 100.0f) << "equal to required: kept";
}

TEST(PointRateTable, TwoRowsEachTheStockExpression) {
  ASSERT_EQ(sizeof pr::kTeams / sizeof pr::kTeams[0], 2u);
  EXPECT_EQ(pr::kTeams[0].rva, kBlue);
  EXPECT_EQ(pr::kTeams[0].charge, 0xF8u);
  EXPECT_EQ(pr::kTeams[0].answer, 0xc8c33e48387f0bb5ULL);
  EXPECT_EQ(pr::kTeams[1].rva, kOrange);
  EXPECT_EQ(pr::kTeams[1].charge, 0xF0u);
  EXPECT_EQ(pr::kTeams[1].answer, 0x41d2c82f1b2d001bULL);
  for (const pr::Team& t : pr::kTeams) {
    // The pin ends with the read of required: movss xmm, dword [reg+0xFC] (disp32).
    std::uint32_t disp = 0;
    std::memcpy(&disp, t.expected + t.expected_len - 4, sizeof disp);
    EXPECT_EQ(disp, 0xFCu) << t.name;
    EXPECT_EQ(t.expected[t.expected_len - 6], 0x10) << t.name;  // movss load
  }
}

class PointRateInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    pr::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    g_answer = 0x1234;  // not a team's symbol
    g_asked = 0;
    std::memset(box_, 0xa5, sizeof box_);
    box_[rules::kPausedShown] = 0;
    box_[rules::kPointRate] = 0;
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    std::vector<std::uint8_t> blue(pr::kTeams[0].expected, pr::kTeams[0].expected + pr::kTeams[0].expected_len);
    blue.reserve(blue.size() + 128);
    BlueTail(blue);
    std::memcpy(base_ + kBlue, blue.data(), blue.size());
    std::vector<std::uint8_t> orange(pr::kTeams[1].expected, pr::kTeams[1].expected + pr::kTeams[1].expected_len);
    orange.reserve(orange.size() + 128);
    OrangeTail(orange);
    std::memcpy(base_ + kOrange, orange.data(), orange.size());
    std::vector<std::uint8_t> o;
    Emit(o, {0x48, 0x83, 0xec, 0x38, 0x8b, 0x15});  // sub rsp, 0x38; mov edx, dword [rip+answer id]
    Rel32(o, kOtherCallerRva, pr::kAnswerFieldIdRva);
    Emit(o, {0x4c, 0x8d, 0x44, 0x24, 0x28, 0x31, 0xc9, 0xff, 0x15});  // lea r8, [rsp+0x28]; xor ecx, ecx; call [rip]
    Rel32(o, kOtherCallerRva, pr::kAccessorSlotRva);
    Emit(o, {0x48, 0x8b, 0x44, 0x24, 0x28, 0x48, 0x83, 0xc4, 0x38, 0xc3});
    std::memcpy(base_ + kOtherCallerRva, o.data(), o.size());

    std::memcpy(base_ + kNameRva, "point", 6);
    const float one = 1.0f, million = 1e6f;
    std::memcpy(base_ + kOneRva, &one, sizeof one);
    std::memcpy(base_ + kMillionRva, &million, sizeof million);
    const std::uint32_t field = kAnswerField, next = 0x5a5a5a5a, other = kAnswerField + 1;
    std::memcpy(base_ + pr::kAnswerFieldIdRva, &field, sizeof field);
    std::memcpy(base_ + pr::kAnswerFieldIdRva + 4, &next, sizeof next);
    std::memcpy(base_ + kOtherFieldRva, &other, sizeof other);
    void* builder = reinterpret_cast<void*>(&Builder);
    std::memcpy(base_ + kBuilderSlotRva, &builder, sizeof builder);
    void* engine = reinterpret_cast<void*>(&EngineAccessor);
    std::memcpy(base_ + pr::kAccessorSlotRva, &engine, sizeof engine);

    std::memset(node_, 0xa5, sizeof node_);
    std::memset(context_, 0xa5, sizeof context_);
    std::memset(globals_, 0xa5, sizeof globals_);
    std::uint8_t* context = context_;
    std::memcpy(node_, &context, sizeof context);
    std::uint8_t* globals = globals_;
    std::memcpy(context_ + pr::kContextGlobals, &globals, sizeof globals);
    SetDt(500000);  // 0.5 s
    SetFloat(pr::kBlueCharge, 10.0f);
    SetFloat(pr::kOrangeCharge, 20.0f);
    SetFloat(pr::kRateE4, 2.0f);
    SetFloat(pr::kRateE0, 3.0f);
    globals_[pr::kRateE0On] = 1;
    SetFloat(pr::kRequired, 100.0f);
  }
  void TearDown() override {
    MH_RemoveHook(base_ + kBlue);
    MH_RemoveHook(base_ + kOrange);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    pr::ResetForTest();
    SetBootLogSink(nullptr);
  }
  void SetDt(std::int64_t us) { std::memcpy(context_ + pr::kContextDt, &us, sizeof us); }
  void SetFloat(std::uint32_t off, float v) { std::memcpy(globals_ + off, &v, sizeof v); }
  float Call(std::uint32_t rva) { return reinterpret_cast<float (*)(void*)>(base_ + rva)(node_); }
  std::uint64_t Other() { return reinterpret_cast<std::uint64_t (*)()>(base_ + kOtherCallerRva)(); }
  void Load() {
    pr::Register(box_);
    sh::OnLoaded(pr::kScript, base_);
  }
  void Set(rules::Pause paused, std::uint8_t rate) {
    box_[rules::kPausedShown] = static_cast<std::uint8_t>(paused);
    box_[rules::kPointRate] = rate;
  }
  std::uint8_t* base_ = nullptr;
  std::uint8_t box_[0x40];
  std::uint8_t node_[0x40];
  std::uint8_t context_[0x80];
  std::uint8_t globals_[0x140];
};

TEST_F(PointRateInstalled, ThePointsRiseAtTheRateAndStandStillWhilePaused) {
  Load();
  EXPECT_EQ(Count("script hooks installed script=b7d10bcffcb2a780"), 1);
  EXPECT_EQ(Count("hooks=2 slots=1"), 2);
  // Stock: charge + (dt * k1) * k2, k1 = +0xE4 (2) for another answer, k2 = +0xE0 (3).
  EXPECT_EQ(Call(kBlue), 13.0f);
  EXPECT_EQ(Call(kOrange), 23.0f);
  Set(rules::Pause::kOff, 4);
  EXPECT_EQ(Call(kBlue), 13.0f) << "x1";
  EXPECT_EQ(Call(kOrange), 23.0f);
  Set(rules::Pause::kOff, 8);
  EXPECT_EQ(Call(kBlue), 16.0f) << "x2";
  EXPECT_EQ(Call(kOrange), 26.0f);
  Set(rules::Pause::kOff, 2);
  EXPECT_EQ(Call(kBlue), 11.5f) << "x0.5";
  g_answer = pr::kBlueAnswer;
  EXPECT_EQ(Call(kBlue), 10.75f) << "blue's answer: k1 is 1";
  EXPECT_EQ(Call(kOrange), 21.5f) << "not orange's answer";
  g_answer = pr::kOrangeAnswer;
  EXPECT_EQ(Call(kOrange), 20.75f) << "orange's answer: k1 is 1";
  g_answer = (std::uint64_t{0xdeadbeef} << 32) | (pr::kBlueAnswer & 0xffffffffULL);  // blue's low dword only
  EXPECT_EQ(Call(kBlue), 11.5f) << "the answer is compared in full";
  g_answer = (pr::kBlueAnswer & ~0xffffffffULL) | 0x1234;  // blue's high dword only
  EXPECT_EQ(Call(kBlue), 11.5f);
  globals_[pr::kRateE0On] = 0;
  EXPECT_EQ(Call(kBlue), 10.5f) << "+0x11B clear: k2 is 1";
  globals_[pr::kRateE0On] = 1;
  for (const rules::Pause p : {rules::Pause::kOn, rules::Pause::kResuming}) {
    Set(p, 8);
    g_asked = 0;
    EXPECT_EQ(Call(kBlue), 10.0f) << "paused: the points stand still";
    EXPECT_EQ(Call(kOrange), 20.0f);
    EXPECT_EQ(g_asked, 2) << "the engine is still asked, as the game asks it";
  }
  SetFloat(pr::kBlueCharge, 150.0f);
  EXPECT_EQ(Call(kBlue), 100.0f) << "paused, above required: clamped as the game clamps";
  // The accessor's other callers get the engine's answer and change nothing.
  g_answer = 0x77;
  EXPECT_EQ(Other(), 0x77u);
  sh::OnUnloading(pr::kScript, base_);
  SetFloat(pr::kBlueCharge, 10.0f);
  g_answer = 0x1234;
  EXPECT_EQ(Call(kBlue), 13.0f) << "stock after the free, paused or not";
}

TEST_F(PointRateInstalled, EachChangeOfHowThePointsRiseIsLoggedOnce) {
  Load();
  Call(kBlue);
  Call(kOrange);
  EXPECT_EQ(Count("point rate: points rise changed"), 0) << "the stock rate, not paused: nothing logged";
  Set(rules::Pause::kOff, 8);
  Call(kBlue);
  Call(kOrange);
  Call(kBlue);
  EXPECT_EQ(Count("point rate: points rise changed from rate=0 held=0 to rate=8 held=0 paused=0 script=b7d10bcffcb2a780"),
            1);
  Set(rules::Pause::kOn, 8);
  Call(kBlue);
  EXPECT_EQ(Count("point rate: points rise changed from rate=8 held=0 to rate=8 held=1 paused=1 script=b7d10bcffcb2a780"),
            1);
  Set(rules::Pause::kResuming, 8);
  Call(kBlue);
  EXPECT_EQ(Count("point rate: points rise changed from rate=8 held=1 to rate=8 held=1 paused=2"), 0)
      << "resuming is still held: no new line";
  Set(rules::Pause::kOff, 0);
  Call(kOrange);
  EXPECT_EQ(Count("point rate: points rise changed from rate=8 held=1 to rate=0 held=0 paused=0"), 1);
  EXPECT_EQ(Count("point rate: points rise changed"), 3) << "one line a change, none a frame";
}

TEST_F(PointRateInstalled, TheFrameTimeIsReadAsAQword) {
  Load();
  SetFloat(pr::kRequired, 1e9f);
  SetDt((std::int64_t{1} << 32) + 500000);  // a high dword: a dword read would see 0.5 s
  Set(rules::Pause::kOff, 0);
  const float stock = Call(kBlue);
  Set(rules::Pause::kOff, 4);
  EXPECT_EQ(Call(kBlue), stock) << "x1 computes what the game computes";
  EXPECT_GT(stock, 1000.0f);
}

TEST_F(PointRateInstalled, AnExpressionWhoseAnswerIsNotReadKeepsTheGamesValue) {
  Load();
  // After the install, point blue's id load at another field: the engine is asked, but not for the answer.
  std::uint8_t* load = base_ + kBlue + kBlueIdLoad;
  ASSERT_EQ(load[0], 0x8b);
  ASSERT_EQ(load[1], 0x15);
  const std::int32_t d = static_cast<std::int32_t>(kOtherFieldRva) - static_cast<std::int32_t>(kBlue + kBlueIdLoad + 6);
  std::memcpy(load + 2, &d, sizeof d);
  Set(rules::Pause::kOn, 0);
  ASSERT_EQ(base_[kOrange + kOrangeIdLoad], 0x8b);
  EXPECT_EQ(Call(kOrange), 20.0f) << "orange still read its answer";
  EXPECT_EQ(Call(kBlue), 13.0f) << "nothing recorded by this call (orange's record does not carry over): the game's value";
}

TEST_F(PointRateInstalled, AnExpressionWhoseReadOfRequiredDiffersRefusesTheScript) {
  base_[kOrange + pr::kTeams[1].expected_len - 4] = 0xF8;
  Load();
  EXPECT_EQ(Count("script hooks refused"), 1);
  EXPECT_EQ(Count("script hooks installed"), 0);
  Set(rules::Pause::kOn, 0);
  EXPECT_EQ(Call(kBlue), 13.0f) << "the whole script runs stock";
}

TEST_F(PointRateInstalled, RegisteringTwiceAddsTheRowsOnce) {
  pr::Register(box_);
  pr::Register(box_);
  sh::OnLoaded(pr::kScript, base_);
  EXPECT_EQ(Count("point rate fix registered"), 1);
  EXPECT_EQ(Count("script hooks installed script=b7d10bcffcb2a780 base="), 1);
}

TEST(RulesStateView, PointRateIsOneByte) {
  std::uint8_t box[0x40];
  std::memset(box, 0xa5, sizeof box);
  box[rules::kPointRate] = 6;
  EXPECT_EQ(rules::RulesState(box).PointRate(), 6);
  EXPECT_EQ(rules::kPointRate, 0x2du);
  EXPECT_EQ(rules::RulesState(nullptr).PointRate(), 0);
}

}  // namespace

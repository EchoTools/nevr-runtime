// #600: Combat's clocks and PAUSE (runtime/combat/clock_pause.h): the decisions, and the 15 hooks installed through the
// script hook table under MinHook on a fake game-mode script.
//
// Each fake function is the row's pinned bytes (the stock function from its start through its timer read, so the game's
// own frame-time load, its divide by 1e6 and its globals read run as shipped), then a tail this test writes for the
// function's shape, the stock instructions of that tail: subtract, max with 0, and for a node the +0xDB check and the
// write back. The fake lays out what those bytes reach at the stock RVAs: a name string at 0x2FA09 and 1e6 at 0x30344.
// The game_timer expression's tail asks a stand-in engine for its answer through a slot at 0x33CC0 (the stock function
// makes two engine calls first to build what it asks; the fake makes only the one that answers), with the field id at
// 0x33794, and returns the timer unchanged when the answer is set.
//
// The node, its context and the globals are test buffers whose bytes around every field the hooks read or write are
// nonzero, and the frame time is a qword with a nonzero high dword's worth of neighbours around it.
//
// NOT shown by these tests: the shipped script's bytes in a run, and a PAUSE in a Combat match.

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
#include "runtime/combat/clock_pause.h"
#include "runtime/combat/rules_state.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace cp = nevr_combat::clock_pause;
namespace rules = nevr_combat::rules;
namespace sh = nevr_script_hooks;

constexpr std::uint32_t kNameRva = 0x2FA09;
constexpr std::uint32_t kMillionRva = 0x30344;
constexpr std::uint32_t kAnswerFieldRva = 0x33794;
constexpr std::uint32_t kAnswerSlotRva = 0x33CC0;
constexpr std::size_t kImage = 0x34000;

// What each function is, read from the stock script, independent of the header's table.
enum class Shape { kNodeA, kNodeB, kExprA, kExprB, kGameTimer };
struct Expect {
  std::uint32_t rva;
  Shape shape;
  std::uint32_t timer;
};
constexpr Expect kExpect[] = {
    {0x28CA0, Shape::kGameTimer, 0xA8}, {0x0FAB0, Shape::kNodeA, 0xB0}, {0x23300, Shape::kExprA, 0xB0},
    {0x0E5C0, Shape::kNodeB, 0xB4},     {0x0F220, Shape::kNodeB, 0xB4}, {0x12B70, Shape::kNodeA, 0xB4},
    {0x134B0, Shape::kNodeA, 0xB4},     {0x137D0, Shape::kNodeA, 0xB4}, {0x145E0, Shape::kNodeA, 0xB4},
    {0x220A0, Shape::kExprB, 0xB4},     {0x22A40, Shape::kExprB, 0xB4}, {0x26500, Shape::kExprA, 0xB4},
    {0x26D70, Shape::kExprA, 0xB4},     {0x26F00, Shape::kExprA, 0xB4}, {0x28030, Shape::kExprA, 0xB4},
};
constexpr std::size_t kExpectCount = sizeof kExpect / sizeof kExpect[0];
static_assert(kExpectCount == cp::kClockCount);

bool IsNode(Shape s) { return s == Shape::kNodeA || s == Shape::kNodeB; }

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

std::uint8_t g_answer = 0;  // what the stand-in engine answers the game_timer expression
int g_answers = 0;
void Answer(void*, std::uint32_t, std::uint8_t* out) {
  ++g_answers;
  *out = g_answer;
}

void Emit(std::vector<std::uint8_t>& v, std::initializer_list<std::uint8_t> b) { v.insert(v.end(), b); }
void Rel32(std::vector<std::uint8_t>& v, std::uint32_t at_rva, std::uint32_t target_rva) {
  const std::int32_t d = static_cast<std::int32_t>(target_rva) - static_cast<std::int32_t>(at_rva + v.size() + 4);
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(d) >> (8 * i)));
}
void Imm32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

// The stock tail of each shape, after the pinned timer read.
void Tail(std::vector<std::uint8_t>& v, const Expect& e) {
  switch (e.shape) {
    case Shape::kNodeA:  // timer in xmm0, dt in xmm1, globals in rax
      Emit(v, {0xf3, 0x0f, 0x5c, 0xc1, 0x0f, 0x57, 0xc9, 0x0f, 0x2f, 0xc1, 0x73, 0x03, 0x0f, 0x28, 0xc1});
      Emit(v, {0x80, 0xb9, 0xdb, 0x00, 0x00, 0x00, 0x00, 0x74, 0x09, 0x48, 0x83, 0xc8, 0xff, 0x48, 0x83, 0xc4, 0x58, 0xc3});
      Emit(v, {0xf3, 0x0f, 0x11, 0x80});  // movss [rax+timer], xmm0
      Imm32(v, e.timer);
      Emit(v, {0x33, 0xc0, 0x48, 0x83, 0xc4, 0x58, 0xc3});
      break;
    case Shape::kNodeB:  // dt in xmm0 (not yet divided), timer in xmm1, globals in rdx
      Emit(v, {0xf3, 0x0f, 0x5e, 0x05});
      Rel32(v, e.rva, kMillionRva);
      Emit(v, {0xf3, 0x0f, 0x5c, 0xc8, 0x0f, 0x2f, 0xca, 0x73, 0x03, 0x0f, 0x28, 0xca});
      Emit(v, {0x80, 0xb9, 0xdb, 0x00, 0x00, 0x00, 0x00, 0x74, 0x09, 0x48, 0x83, 0xc8, 0xff, 0x48, 0x83, 0xc4, 0x58, 0xc3});
      Emit(v, {0xf3, 0x0f, 0x11, 0x8a});  // movss [rdx+timer], xmm1
      Imm32(v, e.timer);
      Emit(v, {0x33, 0xc0, 0x48, 0x83, 0xc4, 0x58, 0xc3});
      break;
    case Shape::kExprA:
      Emit(v, {0xf3, 0x0f, 0x5c, 0xc1, 0x0f, 0x57, 0xc9, 0x0f, 0x2f, 0xc1, 0x73, 0x03, 0x0f, 0x28, 0xc1});
      Emit(v, {0x48, 0x83, 0xc4, 0x58, 0xc3});
      break;
    case Shape::kExprB:
      Emit(v, {0xf3, 0x0f, 0x5e, 0x0d});
      Rel32(v, e.rva, kMillionRva);
      Emit(v, {0xf3, 0x0f, 0x5c, 0xc1, 0x0f, 0x2f, 0xc2, 0x73, 0x03, 0x0f, 0x28, 0xc2, 0x48, 0x83, 0xc4, 0x58, 0xc3});
      break;
    case Shape::kGameTimer:  // timer in xmm7, dt in xmm0
      Emit(v, {0x0f, 0x28, 0xf7, 0xf3, 0x0f, 0x5c, 0xf0, 0x0f, 0x57, 0xc0, 0x0f, 0x2f, 0xf0, 0x73, 0x03, 0x0f, 0x28, 0xf0});
      Emit(v, {0x8b, 0x15});  // mov edx, dword [rip+field id]
      Rel32(v, e.rva, kAnswerFieldRva);
      Emit(v, {0x4c, 0x8d, 0x44, 0x24, 0x28, 0x48, 0x8d, 0x4c, 0x24, 0x70, 0xff, 0x15});  // lea r8; lea rcx; call [rip]
      Rel32(v, e.rva, kAnswerSlotRva);
      Emit(v, {0x80, 0x7c, 0x24, 0x28, 0x00, 0x74, 0x05, 0x0f, 0x28, 0xc7, 0xeb, 0x03, 0x0f, 0x28, 0xc6});
      Emit(v, {0x0f, 0x28, 0xb4, 0x24, 0x80, 0x03, 0x00, 0x00, 0x0f, 0x28, 0xbc, 0x24, 0x70, 0x03, 0x00, 0x00});
      Emit(v, {0x48, 0x81, 0xc4, 0x98, 0x03, 0x00, 0x00, 0xc3});
      break;
  }
}

TEST(ClockPauseDecision, HeldForAnyPauseButOffWithTheGamesMaxWithZero) {
  EXPECT_FALSE(cp::Held(rules::Pause::kOff));
  EXPECT_TRUE(cp::Held(rules::Pause::kOn));
  EXPECT_TRUE(cp::Held(rules::Pause::kResuming)) << "the resume countdown holds the clocks too";
  EXPECT_TRUE(cp::Held(static_cast<rules::Pause>(7))) << "his `cmp byte, 0; jne`";
  EXPECT_EQ(cp::HeldValue(30.0f), 30.0f);
  EXPECT_EQ(cp::HeldValue(-2.0f), 0.0f);
  EXPECT_EQ(cp::HeldValue(std::numeric_limits<float>::quiet_NaN()), 0.0f) << "comiss: unordered is not above";
  EXPECT_TRUE(std::signbit(cp::HeldValue(-0.0f))) << "-0.0 >= 0: kept";
  EXPECT_EQ(cp::HeldGameTimer(30.0f, 30.0f), 30.0f);
  EXPECT_EQ(cp::HeldGameTimer(30.0f, 29.5f), 30.0f) << "counted down: held";
  EXPECT_EQ(cp::HeldGameTimer(-2.0f, -2.0f), -2.0f) << "the engine's answer kept the timer";
  EXPECT_EQ(cp::HeldGameTimer(-2.0f, 0.0f), 0.0f);
  EXPECT_EQ(cp::HeldGameTimer(-0.0f, 0.0f), -0.0f);
  EXPECT_TRUE(std::signbit(cp::HeldGameTimer(-0.0f, 0.0f))) << "compared bit for bit, not by value";
}

TEST(ClockPauseTable, FifteenRowsEachTheStockFunction) {
  for (std::size_t i = 0; i < cp::kClockCount; ++i) {
    SCOPED_TRACE(cp::kClocks[i].name);
    EXPECT_EQ(cp::kClocks[i].rva, kExpect[i].rva);
    EXPECT_EQ(cp::kClocks[i].timer, kExpect[i].timer);
    const cp::Kind kind = kExpect[i].shape == Shape::kGameTimer ? cp::Kind::kGameTimer
                          : IsNode(kExpect[i].shape)             ? cp::Kind::kNode
                                                                 : cp::Kind::kExpression;
    EXPECT_EQ(cp::kClocks[i].kind, kind);
    // The pin ends with the timer read: movss xmm, dword [reg+timer] (f3 0f 10 /r disp32).
    const std::uint8_t* e = cp::kClocks[i].expected;
    const std::size_t n = cp::kClocks[i].expected_len;
    ASSERT_GE(n, 8u);
    EXPECT_EQ(e[n - 8], 0xf3);
    EXPECT_EQ(e[n - 7], 0x0f);
    EXPECT_EQ(e[n - 6], 0x10);
    std::uint32_t disp = 0;
    std::memcpy(&disp, e + n - 4, sizeof disp);
    EXPECT_EQ(disp, kExpect[i].timer);
  }
}

class ClockPauseInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    cp::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    g_answer = 0;
    g_answers = 0;
    std::memset(box_, 0xa5, sizeof box_);
    box_[rules::kPausedShown] = 0;
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kImage, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    for (std::size_t i = 0; i < kExpectCount; ++i) {
      std::vector<std::uint8_t> v(cp::kClocks[i].expected, cp::kClocks[i].expected + cp::kClocks[i].expected_len);
      v.reserve(v.size() + 128);
      Tail(v, kExpect[i]);
      std::memcpy(base_ + kExpect[i].rva, v.data(), v.size());
    }
    std::memcpy(base_ + kNameRva, "clock", 6);
    const float million = 1e6f;
    std::memcpy(base_ + kMillionRva, &million, sizeof million);
    const std::uint32_t field = 0x44;
    std::memcpy(base_ + kAnswerFieldRva, &field, sizeof field);
    void* answer = reinterpret_cast<void*>(&Answer);
    std::memcpy(base_ + kAnswerSlotRva, &answer, sizeof answer);

    // node -> context -> globals, every byte around what is read or written nonzero.
    std::memset(node_, 0xa5, sizeof node_);
    std::memset(context_, 0xa5, sizeof context_);
    std::memset(globals_, 0xa5, sizeof globals_);
    std::uint8_t* context = context_;
    std::memcpy(node_, &context, sizeof context);
    node_[cp::kNodeNoWrite] = 0;
    std::uint8_t* globals = globals_;
    std::memcpy(context_ + cp::kContextGlobals, &globals, sizeof globals);
    const std::int64_t dt = 500000;  // 0.5 s
    std::memcpy(context_ + 0x50, &dt, sizeof dt);
  }
  void TearDown() override {
    for (const Expect& e : kExpect) MH_RemoveHook(base_ + e.rva);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    cp::ResetForTest();
    SetBootLogSink(nullptr);
  }
  // The clock `target` at `v`, the other two at kOther, so a function that reads the wrong clock is seen.
  static constexpr float kOther = 77.0f;
  void SetTimers(std::uint32_t target, float v) {
    for (std::uint32_t off : {cp::kGameTimer, cp::kLobbyLockTimer, cp::kCountdownTimer}) {
      const float x = off == target ? v : kOther;
      std::memcpy(globals_ + off, &x, sizeof x);
    }
  }
  float Timer(std::uint32_t off) const {
    float v = 0;
    std::memcpy(&v, globals_ + off, sizeof v);
    return v;
  }
  float Expression(std::uint32_t rva) { return reinterpret_cast<float (*)(void*)>(base_ + rva)(node_); }
  std::uint64_t Node(std::uint32_t rva) { return reinterpret_cast<std::uint64_t (*)(void*)>(base_ + rva)(node_); }
  // The other two timers, and the bytes around all three, are as SetTimers left them.
  void ExpectOthersUntouched(std::uint32_t target) {
    for (std::uint32_t off : {cp::kGameTimer, cp::kLobbyLockTimer, cp::kCountdownTimer}) {
      if (off != target) {
        EXPECT_EQ(Timer(off), kOther) << "timer +" << std::hex << off;
      }
    }
    EXPECT_EQ(globals_[cp::kGameTimer - 1], 0xa5);
    EXPECT_EQ(globals_[cp::kGameTimer + 4], 0xa5);
    EXPECT_EQ(globals_[cp::kCountdownTimer + 4], 0xa5);
  }
  void Load() {
    cp::Register(box_);
    sh::OnLoaded(cp::kScript, base_);
  }
  // Each function with the timers at `timer`: what it returns and leaves, for PAUSE `paused`.
  void ExpectEveryClock(float timer, float expected, rules::Pause paused) {
    box_[rules::kPausedShown] = static_cast<std::uint8_t>(paused);
    for (const Expect& e : kExpect) {
      SCOPED_TRACE(testing::Message() << "rva 0x" << std::hex << e.rva << " pause " << static_cast<int>(paused));
      SetTimers(e.timer, timer);
      if (IsNode(e.shape)) {
        node_[cp::kNodeNoWrite] = 0;
        EXPECT_EQ(Node(e.rva), 0u);
        EXPECT_EQ(Timer(e.timer), expected);
        ExpectOthersUntouched(e.timer);
        SetTimers(e.timer, timer);
        node_[cp::kNodeNoWrite] = 1;
        EXPECT_EQ(Node(e.rva), ~std::uint64_t{0}) << "+0xDB set: no write";
        EXPECT_EQ(Timer(e.timer), timer);
        node_[cp::kNodeNoWrite] = 0;
      } else if (e.shape == Shape::kGameTimer) {
        g_answer = 0;
        EXPECT_EQ(Expression(e.rva), expected);
        g_answer = 1;
        EXPECT_EQ(Expression(e.rva), timer) << "the engine's answer keeps the timer";
        g_answer = 0;
      } else {
        EXPECT_EQ(Expression(e.rva), expected);
      }
      EXPECT_EQ(Timer(e.timer), timer) << "the +0xDB call and the expressions write nothing";
      ExpectOthersUntouched(e.timer);
    }
  }
  std::uint8_t* base_ = nullptr;
  std::uint8_t box_[0x40];
  std::uint8_t node_[0x100];
  std::uint8_t context_[0x80];
  std::uint8_t globals_[0xC0];
};

TEST_F(ClockPauseInstalled, EveryClockCountsDownWhenNotPaused) {
  Load();
  EXPECT_EQ(Count("script hooks installed script=6fa8cc9cd721a563"), 1);
  EXPECT_EQ(Count("hooks=15 slots=0"), 1);
  ExpectEveryClock(30.0f, 29.5f, rules::Pause::kOff);
  ExpectEveryClock(0.25f, 0.0f, rules::Pause::kOff);
}

TEST_F(ClockPauseInstalled, EveryClockStandsStillWhilePausedOrResuming) {
  Load();
  for (const rules::Pause p : {rules::Pause::kOn, rules::Pause::kResuming}) {
    ExpectEveryClock(30.0f, 30.0f, p);
    ExpectEveryClock(-2.0f, 0.0f, p);
  }
}

TEST_F(ClockPauseInstalled, EachChangeOfTheHeldStateIsLoggedOnce) {
  Load();
  ExpectEveryClock(30.0f, 29.5f, rules::Pause::kOff);
  EXPECT_EQ(Count("clock pause: clocks held"), 0) << "a match that is not paused logs nothing";
  ExpectEveryClock(30.0f, 30.0f, rules::Pause::kOn);
  EXPECT_EQ(Count("clock pause: clocks held from=0 to=1 paused=1 script=6fa8cc9cd721a563"), 1);
  ExpectEveryClock(30.0f, 30.0f, rules::Pause::kResuming);
  EXPECT_EQ(Count("clock pause: clocks held"), 1) << "resuming is still held: no new line";
  ExpectEveryClock(30.0f, 29.5f, rules::Pause::kOff);
  EXPECT_EQ(Count("clock pause: clocks held from=1 to=0 paused=0 script=6fa8cc9cd721a563"), 1);
  ExpectEveryClock(30.0f, 30.0f, rules::Pause::kOn);
  EXPECT_EQ(Count("clock pause: clocks held from=0 to=1"), 2);
  EXPECT_EQ(Count("clock pause: clocks held"), 3) << "one line a change, none a frame";
}

TEST_F(ClockPauseInstalled, TheGameTimerAsksTheEngineWhilePaused) {
  Load();
  box_[rules::kPausedShown] = static_cast<std::uint8_t>(rules::Pause::kOn);
  SetTimers(cp::kGameTimer, -2.0f);
  g_answer = 1;
  g_answers = 0;
  EXPECT_EQ(Expression(0x28CA0), -2.0f) << "the answer kept it unchanged, below 0";
  EXPECT_EQ(g_answers, 1);
  g_answer = 0;
  EXPECT_EQ(Expression(0x28CA0), 0.0f);
  EXPECT_EQ(g_answers, 2);
}

TEST_F(ClockPauseInstalled, StockAfterTheFree) {
  Load();
  sh::OnUnloading(cp::kScript, base_);
  ExpectEveryClock(30.0f, 29.5f, rules::Pause::kOn);
}

TEST_F(ClockPauseInstalled, AFunctionWhoseTimerReadDiffersRefusesTheScript) {
  // The countdown expression at 0x26500 reads another offset: the pin covers the timer read.
  std::uint8_t* last = base_ + 0x26500 + cp::kClocks[11].expected_len - 4;
  ASSERT_EQ(cp::kClocks[11].rva, 0x26500u);
  *last = 0xB8;
  Load();
  EXPECT_EQ(Count("script hooks refused"), 1);
  EXPECT_EQ(Count("script hooks installed"), 0);
  box_[rules::kPausedShown] = static_cast<std::uint8_t>(rules::Pause::kOn);
  SetTimers(cp::kLobbyLockTimer, 30.0f);
  EXPECT_EQ(Expression(0x23300), 29.5f) << "the whole script runs stock";
}

TEST_F(ClockPauseInstalled, RegisteringTwiceAddsTheRowsOnce) {
  cp::Register(box_);
  cp::Register(box_);
  sh::OnLoaded(cp::kScript, base_);
  EXPECT_EQ(Count("clock pause fix registered"), 1);
  EXPECT_EQ(Count("hooks=15 slots=0"), 1);
}

}  // namespace

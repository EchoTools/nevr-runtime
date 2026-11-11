// #600: the spawn orbs' PAUSE hook and slot (runtime/combat/orb_pause.h) and the RULES mailbox view
// (runtime/combat/rules_state.h): the decisions, and the hook and the slot installed through the script hook table under
// MinHook on a fake spawn point script.
//
// The fake node at RVA 0x104C0 starts with the real first bytes, then does the script's read: loads the spawn_access
// field id as a dword (`mov edx, dword [rip+0x40B64]`, with a nonzero dword after it), calls the field accessor through
// the slot at 0x411A0 with out = [rsp+0x28], and returns that qword (the stock node returns the value for the four states
// it knows; the fake returns it whatever it is). The slot holds a function of this test executable standing in for the
// engine's. The mailbox is a test buffer with nonzero bytes on both sides of +0x2E and +0x2F.
//
// NOT shown by these tests: the shipped script's bytes in a run, and a PAUSE in a Combat match.

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
#include "runtime/combat/orb_pause.h"
#include "runtime/combat/rules_state.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/script_hooks.h"

namespace {

namespace op = nevr_combat::orb_pause;
namespace rules = nevr_combat::rules;
namespace sh = nevr_script_hooks;

constexpr std::uint32_t kSpawnField = 0x31;
constexpr std::uint32_t kNextField = 0x5a5a5a5a;
constexpr std::uint64_t kMainLoop = 0x1509ee2745041c7eULL;
// pre_match's low dword with other high bits: a set value that is not pre_match.
constexpr std::uint64_t kNotPreMatch = (std::uint64_t{0xdeadbeef} << 32) | (op::kPreMatch & 0xffffffffULL);
constexpr std::uint32_t kOtherCallerRva = 0x20000;
constexpr std::uint32_t kRaxCallerRva = 0x20100;

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.push_back(message); }
int Count(const char* needle) {
  int n = 0;
  for (const std::string& line : g_logged) n += line.find(needle) != std::string::npos ? 1 : 0;
  return n;
}

std::uint64_t g_value = 0;  // what the stand-in engine writes for any field
int g_engine_calls = 0;
std::uint64_t EngineAccessor(void*, std::uint64_t, void* out) {
  ++g_engine_calls;
  if (out != nullptr) std::memcpy(out, &g_value, sizeof g_value);
  return 0x55;
}

void Emit(std::vector<std::uint8_t>& v, std::initializer_list<std::uint8_t> b) { v.insert(v.end(), b); }
void Rel32(std::vector<std::uint8_t>& v, std::uint32_t at_rva, std::uint32_t target_rva) {
  const std::int32_t d = static_cast<std::int32_t>(target_rva) - static_cast<std::int32_t>(at_rva + v.size() + 4);
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(d) >> (8 * i)));
}

TEST(OrbPauseDecision, LiveIsASetValueOtherThanPreMatchAndPausedReadsPreMatch) {
  EXPECT_FALSE(op::Live(op::kUnset)) << "not set yet";
  EXPECT_FALSE(op::Live(op::kPreMatch));
  EXPECT_TRUE(op::Live(kMainLoop));
  EXPECT_TRUE(op::Live(0x1234)) << "a value the node does not know is live, as his patch has it";
  EXPECT_TRUE(op::Live(kNotPreMatch)) << "the whole symbol is compared";
  EXPECT_EQ(op::Acted(kMainLoop, rules::Pause::kOn), op::kPreMatch);
  EXPECT_EQ(op::Acted(kMainLoop, rules::Pause::kResuming), kMainLoop) << "the countdown leaves the round's orbs";
  EXPECT_EQ(op::Acted(kMainLoop, rules::Pause::kOff), kMainLoop);
}

TEST(RulesStateView, ReadsAndWritesOneByteEach) {
  std::uint8_t box[0x40];
  std::memset(box, 0xa5, sizeof box);
  box[rules::kPausedShown] = 1;
  const rules::RulesState r(box);
  EXPECT_EQ(r.Paused(), rules::Pause::kOn);
  r.SetMatchLive(true);
  EXPECT_EQ(box[rules::kMatchLive], 1);
  r.SetMatchLive(false);
  EXPECT_EQ(box[rules::kMatchLive], 0);
  EXPECT_EQ(box[rules::kMatchLive + 1], 0xa5) << "one byte written";
  EXPECT_EQ(box[rules::kPausedShown - 1], 0xa5);
  EXPECT_EQ(r.StationCells(), box + rules::kStationCells);
  const rules::RulesState none(nullptr);
  EXPECT_EQ(none.Paused(), rules::Pause::kOff);
  none.SetMatchLive(true);  // dropped, no fault
  EXPECT_EQ(none.StationCells(), nullptr);
}

TEST(OrbPauseField, OnlyTheSpawnAccessFieldInsideTheNodeIsActedOn) {
  std::uint8_t box[0x40] = {};
  box[rules::kPausedShown] = 1;
  const rules::RulesState r(box);
  g_value = kMainLoop;
  std::uint64_t out = 0;
  EXPECT_EQ(op::ReadField(&EngineAccessor, kSpawnField, false, r, nullptr, kSpawnField, &out), 0x55u)
      << "the engine's result, which the script's callers return";
  EXPECT_EQ(out, kMainLoop) << "outside the node";
  EXPECT_EQ(box[rules::kMatchLive], 0);
  op::ReadField(&EngineAccessor, kSpawnField, true, r, nullptr, kSpawnField + 1, &out);
  EXPECT_EQ(out, kMainLoop) << "another field";
  EXPECT_EQ(op::ReadField(&EngineAccessor, kSpawnField, true, r, nullptr, (std::uint64_t{0xffff} << 32) | kSpawnField, &out),
            0x55u) << "the engine's result, also when acted on";
  EXPECT_EQ(out, op::kPreMatch) << "the id is a dword: the register's upper half does not matter";
  EXPECT_EQ(box[rules::kMatchLive], 1);
  box[rules::kMatchLive] = 7;
  EXPECT_EQ(op::ReadField(&EngineAccessor, kSpawnField, true, r, nullptr, kSpawnField, nullptr), 0x55u)
      << "no out: nothing read or written";
  EXPECT_EQ(box[rules::kMatchLive], 7);
}

class OrbPauseInstalled : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(Hooking::Initialize());
    nevr_hook_guard::ResetForTest();
    sh::ResetForTest();
    op::ResetForTest();
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
    std::memset(box_, 0xa5, sizeof box_);
    box_[rules::kPausedShown] = 0;
    box_[rules::kMatchLive] = 0;
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 0x42000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ASSERT_NE(base_, nullptr);
    const std::uint32_t at = op::kNodeRva;
    std::vector<std::uint8_t> v;
    v.reserve(128);  // reserved first: GCC's -Warray-bounds misreads growing a vector built from a 17-byte array
    v.insert(v.end(), op::kNodePrologue, op::kNodePrologue + sizeof op::kNodePrologue);
    Emit(v, {0x8b, 0x15});                             // mov edx, dword [rip+spawn_access id]
    Rel32(v, at, op::kSpawnAccessFieldIdRva);
    Emit(v, {0x4c, 0x8d, 0x44, 0x24, 0x28});           // lea r8, [rsp+0x28]
    Emit(v, {0x48, 0x8d, 0x4c, 0x24, 0x70});           // lea rcx, [rsp+0x70]
    Emit(v, {0xff, 0x15});                             // call [rip+slot]
    Rel32(v, at, op::kAccessorSlotRva);
    Emit(v, {0x48, 0x8b, 0x44, 0x24, 0x28,             // mov rax, [rsp+0x28]: the value the node acts on
             0x48, 0x81, 0xc4, 0x78, 0x03, 0x00, 0x00, 0xc3});  // add rsp, 0x378; ret
    std::memcpy(base_ + at, v.data(), v.size());
    std::vector<std::uint8_t> o;
    Emit(o, {0x48, 0x83, 0xec, 0x38, 0x8b, 0x15});     // sub rsp, 0x38; mov edx, dword [rip+id]
    Rel32(o, kOtherCallerRva, op::kSpawnAccessFieldIdRva);
    Emit(o, {0x4c, 0x8d, 0x44, 0x24, 0x28, 0x31, 0xc9, 0xff, 0x15});  // lea r8, [rsp+0x28]; xor ecx, ecx; call [rip+slot]
    Rel32(o, kOtherCallerRva, op::kAccessorSlotRva);
    Emit(o, {0x48, 0x8b, 0x44, 0x24, 0x28, 0x48, 0x83, 0xc4, 0x38, 0xc3});  // mov rax, [rsp+0x28]; add rsp, 0x38; ret
    std::memcpy(base_ + kOtherCallerRva, o.data(), o.size());
    // A caller that returns the accessor's rax, as the script's callers at 0x2BB29 and 0x30CC9 do.
    std::vector<std::uint8_t> x;
    Emit(x, {0x48, 0x83, 0xec, 0x38, 0x8b, 0x15});
    Rel32(x, kRaxCallerRva, op::kSpawnAccessFieldIdRva);
    Emit(x, {0x4c, 0x8d, 0x44, 0x24, 0x28, 0x31, 0xc9, 0xff, 0x15});
    Rel32(x, kRaxCallerRva, op::kAccessorSlotRva);
    Emit(x, {0x48, 0x83, 0xc4, 0x38, 0xc3});  // add rsp, 0x38; ret (rax is the accessor's)
    std::memcpy(base_ + kRaxCallerRva, x.data(), x.size());
    void* engine = reinterpret_cast<void*>(&EngineAccessor);
    std::memcpy(base_ + op::kAccessorSlotRva, &engine, sizeof engine);
    std::memcpy(base_ + op::kSpawnAccessFieldIdRva, &kSpawnField, sizeof kSpawnField);
    std::memcpy(base_ + op::kSpawnAccessFieldIdRva + 4, &kNextField, sizeof kNextField);
  }
  void TearDown() override {
    MH_RemoveHook(base_ + op::kNodeRva);
    VirtualFree(base_, 0, MEM_RELEASE);
    sh::ResetForTest();
    op::ResetForTest();
    SetBootLogSink(nullptr);
  }
  std::uint64_t Node(std::uint64_t value) {
    g_value = value;
    return reinterpret_cast<std::uint64_t (*)(void*)>(base_ + op::kNodeRva)(nullptr);
  }
  std::uint64_t RaxCaller() { return reinterpret_cast<std::uint64_t (*)()>(base_ + kRaxCallerRva)(); }
  std::uint64_t Other(std::uint64_t value) {
    g_value = value;
    return reinterpret_cast<std::uint64_t (*)()>(base_ + kOtherCallerRva)();
  }
  std::uint8_t* base_ = nullptr;
  std::uint8_t box_[0x40];
};

TEST_F(OrbPauseInstalled, PauseGivesThePreMatchOrbsAndTheOrbsSayWhenTheMatchIsLive) {
  op::Register(box_);
  sh::OnLoaded(op::kScript, base_);
  EXPECT_EQ(Count("script hooks installed script=e57332952e1cd478"), 1);
  EXPECT_EQ(Count("hooks=1 slots=1"), 2);
  EXPECT_EQ(Node(op::kUnset), op::kUnset);
  EXPECT_EQ(box_[rules::kMatchLive], 0) << "not set: not live";
  EXPECT_EQ(Node(op::kPreMatch), op::kPreMatch);
  EXPECT_EQ(box_[rules::kMatchLive], 0) << "pre_match: not live";
  EXPECT_EQ(Node(kMainLoop), kMainLoop);
  EXPECT_EQ(box_[rules::kMatchLive], 1) << "main_loop: live";
  EXPECT_EQ(Node(kNotPreMatch), kNotPreMatch) << "the whole qword is read";
  EXPECT_EQ(box_[rules::kMatchLive], 1);
  box_[rules::kPausedShown] = static_cast<std::uint8_t>(rules::Pause::kOn);
  EXPECT_EQ(Node(kMainLoop), op::kPreMatch) << "paused: the pre-match orbs";
  EXPECT_EQ(box_[rules::kMatchLive], 1) << "live is the value read, before the pause";
  box_[rules::kPausedShown] = static_cast<std::uint8_t>(rules::Pause::kResuming);
  EXPECT_EQ(Node(kMainLoop), kMainLoop) << "the resume countdown: the round's orbs";
  EXPECT_EQ(box_[rules::kMatchLive + 1], 0xa5) << "the mailbox byte after match live is untouched";
  EXPECT_EQ(box_[rules::kPausedShown - 1], 0xa5);
  // The accessor's other callers: untouched, and no mailbox write.
  box_[rules::kPausedShown] = static_cast<std::uint8_t>(rules::Pause::kOn);
  box_[rules::kMatchLive] = 7;
  EXPECT_EQ(Other(kMainLoop), kMainLoop);
  EXPECT_EQ(box_[rules::kMatchLive], 7);
  sh::OnUnloading(op::kScript, base_);
  EXPECT_EQ(Node(kMainLoop), kMainLoop) << "stock after the free";
}

TEST_F(OrbPauseInstalled, TheAccessorsResultReachesTheScript) {
  op::Register(box_);
  sh::OnLoaded(op::kScript, base_);
  g_value = kMainLoop;
  EXPECT_EQ(RaxCaller(), 0x55u) << "the replaced slot returns the engine's rax";
}

TEST_F(OrbPauseInstalled, WithoutTheModuleTheSlotOnlyPassesThrough) {
  op::Register(box_);
  sh::OnLoaded(op::kScript, base_);
  op::ForgetModuleForTest();
  box_[rules::kPausedShown] = static_cast<std::uint8_t>(rules::Pause::kOn);
  EXPECT_EQ(Node(kMainLoop), kMainLoop) << "no field id to compare: the engine's value";
  EXPECT_EQ(box_[rules::kMatchLive], 0);
  const std::uint32_t zero = 0;
  std::memcpy(base_ + op::kSpawnAccessFieldIdRva, &zero, sizeof zero);
  EXPECT_EQ(Node(kMainLoop), kMainLoop) << "field 0 is not taken for spawn_access";
  EXPECT_EQ(box_[rules::kMatchLive], 0);
  g_value = kMainLoop;
  EXPECT_EQ(RaxCaller(), 0x55u) << "the engine's result";
}

TEST_F(OrbPauseInstalled, EachChangeIsLoggedOnce) {
  op::Register(box_);
  sh::OnLoaded(op::kScript, base_);
  Node(op::kPreMatch);
  Node(op::kPreMatch);
  EXPECT_EQ(Count("orb pause: match live from=unset to=0 script=e57332952e1cd478 paused=0"), 1);
  Node(kMainLoop);
  Node(kMainLoop);
  EXPECT_EQ(Count("orb pause: match live from=0 to=1 script=e57332952e1cd478 paused=0"), 1);
  EXPECT_EQ(Count("orb pause: pre-match orbs"), 0) << "no swap yet";
  box_[rules::kPausedShown] = static_cast<std::uint8_t>(rules::Pause::kOn);
  Node(kMainLoop);
  Node(kMainLoop);
  EXPECT_EQ(Count("orb pause: pre-match orbs started script=e57332952e1cd478 paused=1"), 1);
  box_[rules::kPausedShown] = static_cast<std::uint8_t>(rules::Pause::kResuming);
  Node(kMainLoop);
  Node(kMainLoop);
  EXPECT_EQ(Count("orb pause: pre-match orbs stopped script=e57332952e1cd478 paused=2"), 1);
  Node(op::kPreMatch);
  EXPECT_EQ(Count("orb pause: match live from=1 to=0 script=e57332952e1cd478 paused=2"), 1);
  EXPECT_EQ(Count("orb pause: match live"), 3) << "one line per change, none per frame";
}

TEST_F(OrbPauseInstalled, RegisteringTwiceAddsTheRowsOnce) {
  op::Register(box_);
  op::Register(box_);
  sh::OnLoaded(op::kScript, base_);
  EXPECT_EQ(Count("orb pause fix registered"), 1);
  EXPECT_EQ(Count("script hooks installed script=e57332952e1cd478 base="), 1);
}

}  // namespace

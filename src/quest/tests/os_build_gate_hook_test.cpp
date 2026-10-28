// Host test for "the game never refuses a headset for its OS build number" (#945): the real handler
// (src/quest/sentinel/os_build_gate_hook.cpp), built without exceptions as the sentinel builds it, driven through the
// thunk's entry with a stand-in for libr15's CSysInfo::OSBuildVersion. No GOT slot is touched: got_pinned_test.cpp
// resolves the pinned slot, the floor and the one call site in the real library.
//
// Built and run by `just test-quest-hooks`.

#include <cstdint>
#include <cstdio>

#include "os_build_gate_hook.h"
#include "quest/tests/test_check.h"

namespace {

namespace gate = nevr_quest::os_build_gate;

constexpr std::uint64_t kFloor = sentinel::pinned::kMinOSBuildVersionValue;

gate::Thunk::Fn Entry() { return reinterpret_cast<gate::Thunk::Fn>(gate::Thunk::EntryAddress()); }

// The stand-in for the game's function: what ro.build.version.incremental would make it answer.
int g_answer = 0;
std::uint64_t g_number = 0;
int g_originalCalls = 0;

int StandIn(std::uint64_t* version) {
  ++g_originalCalls;
  if (g_answer != 0) *version = g_number;
  return g_answer;
}

void Reset(int answer, std::uint64_t number) {
  g_answer = answer;
  g_number = number;
  g_originalCalls = 0;
  gate::ResetForTest();
  gate::Thunk::Reset();
  *gate::Thunk::OriginalOut() = reinterpret_cast<void*>(&StandIn);
}

// Without the hook armed the entry is the game's call: a number below the floor is returned as read, which is what
// sends Initialize to "os needs update".
void WithoutTheHookTheGameSeesItsNumber() {
  Reset(1, 1234);
  std::uint64_t version = 0;
  QCHECK(Entry()(&version) == 1);
  QCHECK(version == 1234);
  QCHECK(gate::CurrentSeen().calls == 0);
}

// The headset the floor refuses: the game is told "no number" and its variable is not written; the line says the game
// would have refused.
void ANumberBelowTheFloorIsAnsweredNoNumber() {
  Reset(1, kFloor - 1);
  gate::ArmForTest(kFloor);
  std::uint64_t version = 77;
  QCHECK(Entry()(&version) == 0);
  QCHECK(version == 77);
  QCHECK(g_originalCalls == 1);
  const gate::Seen seen = gate::CurrentSeen();
  QCHECK(seen.calls == 1);
  QCHECK(seen.read);
  QCHECK(seen.os_build == kFloor - 1);
  QCHECK(seen.floor == kFloor);
  QCHECK(seen.would_refuse);
}

// The Quest 3 and Quest 2 numbers on record (docs/adr/0021) are above the floor: the answer is the same, and the line
// says the game would not have refused.
void ANumberAboveTheFloorIsAnsweredNoNumberToo() {
  const std::uint64_t numbers[] = {52641960030600520ULL, 52242990035800150ULL, kFloor};
  for (const std::uint64_t number : numbers) {
    Reset(1, number);
    gate::ArmForTest(kFloor);
    std::uint64_t version = 0;
    QCHECK(Entry()(&version) == 0);
    QCHECK(version == 0);
    const gate::Seen seen = gate::CurrentSeen();
    QCHECK(seen.read && seen.os_build == number);
    QCHECK(!seen.would_refuse);
  }
}

// A property the game cannot read as a number: the original answers 0 and so does the hook.
void NoNumberStaysNoNumber() {
  Reset(0, 0);
  gate::ArmForTest(kFloor);
  std::uint64_t version = 5;
  QCHECK(Entry()(&version) == 0);
  QCHECK(version == 5);
  const gate::Seen seen = gate::CurrentSeen();
  QCHECK(seen.calls == 1 && !seen.read && seen.os_build == 0 && !seen.would_refuse);
}

// An unknown floor (the install could not read it) still answers "no number"; the line cannot say whether the game would
// have refused, so it says it would not.
void AnUnknownFloorStillAnswersNoNumber() {
  Reset(1, 1);
  gate::ArmForTest(0);
  std::uint64_t version = 0;
  QCHECK(Entry()(&version) == 0);
  QCHECK(!gate::CurrentSeen().would_refuse);
}

// Every call is answered, and only the first is measured.
void EveryCallIsAnsweredAndTheFirstIsMeasured() {
  Reset(1, 10);
  gate::ArmForTest(kFloor);
  std::uint64_t version = 0;
  QCHECK(Entry()(&version) == 0);
  g_number = 20;
  QCHECK(Entry()(&version) == 0);
  QCHECK(g_originalCalls == 2);
  const gate::Seen seen = gate::CurrentSeen();
  QCHECK(seen.calls == 2);
  QCHECK(seen.os_build == 10);
}

}  // namespace

int main() {
  WithoutTheHookTheGameSeesItsNumber();
  ANumberBelowTheFloorIsAnsweredNoNumber();
  ANumberAboveTheFloorIsAnsweredNoNumberToo();
  NoNumberStaysNoNumber();
  AnUnknownFloorStillAnswersNoNumber();
  EveryCallIsAnsweredAndTheFirstIsMeasured();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "os_build_gate_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("os_build_gate_hook_test: all checks pass\n");
  return 0;
}

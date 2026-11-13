// Host test of the frozen-frame exit (src/quest/combat/frozen_frame_exit.cpp, #2377 under #1737): the rule
// (frozen_frame_exit_rules.h), the observer the render-configuration hook calls on every submit, and the watchdog against a libr15 image built in
// memory that holds the pinned words and the in-VR word, with the combat tick and the level answer of combat_scope_world.h, a fake clock and fake
// finish, grace and end steps.
//
// What it pins: Expired's table (below and at the limit false, limit + 1 ms true, out of VR and not armed false); a submit outside a combat level
// reads no clock and writes nothing, one inside stamps and arms once; the watchdog writes its record, then finishes the activity, then waits the
// grace, then writes the end record and ends the process with exit code 1 and reason frozen_frame, once; with no activity it says so and does not
// finish; a null word or an arena level disarms; a leave from VR between its two reads is not a freeze; NoteEnd joins the thread and does not when
// the watchdog is the one ending the process; the install's refusals (off, scope_not_live, submit_slot_not_held, word_unreadable, pinned_word_mismatch).
//
// Built and run by `just test-quest-combat frozen_frame_exit`.
#include "quest/combat/frozen_frame_exit.h"

#include <time.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/frozen_frame_exit_rules.h"
#include "quest/combat/frozen_frame_exit_targets.h"
#include "quest/sentinel/android_glue_layout.h"
#include "quest/sentinel/process_end_hook.h"
#include "quest/sentinel/quit_hook.h"
#include "quest/sentinel/render_config.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_log_capture.h"
#include "quest/tests/combat_pins.h"
#include "quest/tests/combat_scope_world.h"
#include "quest/tests/test_check.h"

namespace {

namespace ffe = nevr_quest::combat::frozen_frame_exit;
namespace rules = nevr_quest::combat::frozen_frame_exit::rules;
namespace targets = nevr_quest::combat::frozen_frame_exit::targets;
namespace scope = nevr_quest::combat::scope;
namespace world = combat_test::scope_world;
namespace pinned = sentinel::pinned;
using combat_test::Has;
using combat_test::Image;
using combat_test::Line;
using combat_test::Lines;
using nevr_quest::combat::ModuleResult;


// ---- the clock and the steps that end the game -----------------------------------------------------------------------------------

std::atomic<std::int64_t> g_now{1000};
std::atomic<int> g_clockReads{0};
// Run once, on the next clock read: a change the game makes between the watchdog's two reads.
void (*g_onRead)() = nullptr;
std::int64_t Clock() noexcept {
  g_clockReads.fetch_add(1);
  if (g_onRead != nullptr) {
    void (*const f)() = g_onRead;
    g_onRead = nullptr;
    f();
  }
  return g_now.load();
}

std::vector<std::string> g_steps;  // what the watchdog did, in order
std::atomic<bool> g_canFinish{true};
std::size_t g_recordsAtFinish = 99;
unsigned g_graceMs = 0;
int g_exitCode = -1;
std::string g_reason;
bool CanFinish() noexcept { return g_canFinish.load(); }
bool Finish() noexcept {
  g_steps.push_back("finish");
  g_recordsAtFinish = Lines("combat_frozen_frame_exit").size();
  return true;
}
void Grace(unsigned ms) noexcept {
  g_steps.push_back("grace");
  g_graceMs = ms;
}
void End(int code, const char* reason) noexcept {
  g_steps.push_back(Lines("combat_frozen_frame_exit_end").size() == 1U ? "end_after_record" : "end_without_record");
  g_exitCode = code;
  g_reason = reason;
}
// The before-exit step, as process_end runs it inside EndWithExitCode: each combat module's end, here on the watchdog thread itself.
std::atomic<bool> g_endRan{false};
void EndRunningTheRowsEnd(int code, const char* reason) noexcept {
  End(code, reason);
  ffe::NoteEnd();
  g_endRan = true;
}

// ---- the libr15 image ---------------------------------------------------------------------------------------------------------------

Image g_image;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

void PutWord(std::uint64_t value) { g_image.Put<std::uint64_t>(targets::kInVrWordVaddr, value); }
constexpr std::uint64_t kSession = 0x7a11e55100ULL;  // a non-null ovrMobile*

// A fresh world: the module forgotten, a libr15 with the pinned words, the tick's slot and the in-VR word (in VR), the render-configuration hook
// holding the submit slot, the capture started. `scopeLive`: the tick installed and the scope row registered.
void Begin(bool scopeLive = true) {
  combat_test::StartCapture();
  ffe::ResetForTest();
  world::Reset();
  g_image.Build({world::TickRow()});
  world::PutTickSlot(g_image);
  combat_test::PutPins(g_image, &targets::kWordPins, 1);
  PutWord(kSession);
  nevr_quest::render_config::detail::g_submitSlotHeld.store(true);
  g_now = 1000;
  g_clockReads = 0;
  g_onRead = nullptr;
  g_steps.clear();
  g_canFinish = true;
  g_recordsAtFinish = 99;
  g_graceMs = 0;
  g_exitCode = -1;
  g_reason.clear();
  g_endRan = false;
  ffe::SetClockForTest(&Clock);
  ffe::SetPollMsForTest(3600000);  // the thread waits; the test calls Tick itself
  ffe::Seams seams;
  seams.canFinish = &CanFinish;
  seams.finish = &Finish;
  seams.grace = &Grace;
  seams.end = &End;
  ffe::SetSeamsForTest(seams);
  if (scopeLive) {
    QCHECK(world::StartScope(&Lookup));
    combat_test::Captured().clear();
  }
}
void Finish_() {
  ffe::ResetForTest();
  world::Reset();
  nevr_quest::render_config::detail::g_submitSlotHeld.store(false);
  g_image.Drop();
  combat_test::StopCapture();
}

ModuleResult Install(bool on = true, int limitS = 30, bool fromFile = false) { return ffe::InstallAt(on, limitS, fromFile, &Lookup); }

// Installed, in a combat map, one submit at t = 1000.
void Armed() {
  Begin();
  world::Levels({"mpl_combat_dyson"});
  QCHECK(Install().installed);
  ffe::NoteSubmit();
  QCHECK(ffe::LastSubmitMs() == 1000);
}

// ---- the rule -------------------------------------------------------------------------------------------------------------------------

void Expired() {
  QCHECK(!rules::Expired(5000, 1000, true, 30));    // a recent submit
  QCHECK(!rules::Expired(31000, 1000, true, 30));   // exactly the limit
  QCHECK(rules::Expired(31001, 1000, true, 30));    // the limit and 1 ms
  QCHECK(!rules::Expired(99999, 1000, false, 30));  // out of VR
  QCHECK(!rules::Expired(99999, 0, true, 30));      // not armed
  QCHECK(rules::kDefaultLimitS == 30 && rules::kMinLimitS == 10 && rules::kMaxLimitS == 900);  // upstream's bound and range
  QCHECK(rules::Expired(10002, 1, true, 10) && !rules::Expired(10001, 1, true, 10));  // the limit counts from the submit, in ms
}

// The second look: the same gap seen at an earlier tick, with no submit since, at least kConfirmMs before.
void Confirmed() {
  QCHECK(rules::kConfirmMs == 1000);
  QCHECK(rules::Confirmed(1000, 31001, 1000, 32001));    // exactly kConfirmMs later
  QCHECK(!rules::Confirmed(1000, 31001, 1000, 32000));   // 1 ms short
  QCHECK(!rules::Confirmed(1000, 0, 1000, 99999));       // no first look
  QCHECK(!rules::Confirmed(0, 31001, 0, 99999));         // nothing armed
  QCHECK(!rules::Confirmed(1000, 31001, 1500, 99999));   // a submit since: another gap
}

// ---- the observer ---------------------------------------------------------------------------------------------------------------------

// Outside a combat level a submit reads no clock and changes nothing: no Echo Arena level, an arena level loaded beside a combat map, no level at
// all. And in a combat level an unarmed module stamps nothing.
void NoteSubmitOutsideScopeTouchesNothing() {
  Begin();
  ffe::ArmForTest();
  for (const auto& levels : {std::initializer_list<const char*>{"mpl_arena_a"}, {"mpl_combat_gauss", "mpl_arena_b"}, {"mpl_lobby_b"}}) {
    world::Levels(levels);
    QCHECK(!ffe::Watching());
    ffe::NoteSubmit();
  }
  QCHECK(g_clockReads.load() == 0 && ffe::LastSubmitMs() == 0 && ffe::Submits() == 0 && ffe::Arms() == 0);
  Finish_();
  Begin();  // a combat map, not armed (not installed)
  world::Levels({"mpl_combat_dyson"});
  ffe::NoteSubmit();
  QCHECK(g_clockReads.load() == 0 && ffe::LastSubmitMs() == 0 && ffe::Submits() == 0);
  Finish_();
}

void NoteSubmitInScopeStamps() {
  Begin();
  ffe::ArmForTest();
  world::Levels({"mpl_lobby_b_combat"});  // the combat lobby is in scope
  QCHECK(ffe::Watching());
  g_now = 5000;
  ffe::NoteSubmit();
  QCHECK(ffe::LastSubmitMs() == 5000 && ffe::Submits() == 1 && ffe::Arms() == 1 && g_clockReads.load() == 1);
  g_now = 5011;
  ffe::NoteSubmit();
  QCHECK(ffe::LastSubmitMs() == 5011 && ffe::Submits() == 2 && ffe::Arms() == 1);  // armed once
  for (const Line& l : combat_test::Captured()) QCHECK(!Has(l.text, "frozen_frame"));  // a submit writes no line (the scope's own change lines aside)
  Finish_();
}

// ---- the watchdog ---------------------------------------------------------------------------------------------------------------------

void NotPastTheLimitDoesNothing() {
  Armed();
  g_now = 31000;  // exactly 30 s since the submit
  ffe::Tick();
  QCHECK(!ffe::Fired() && g_steps.empty() && Lines("combat_frozen_frame_exit").empty());
  ffe::NoteSubmit();  // a frame: the limit counts from here
  g_now = 31000 + 30000;
  ffe::Tick();
  QCHECK(!ffe::Fired());
  ffe::NoteEnd();
  Finish_();
}

// The tick that first sees the gap past the limit only notes it; a tick kConfirmMs later that sees the same gap fires.
void NeedsASecondTick() {
  Armed();
  g_now = 31001;
  ffe::Tick();
  QCHECK(!ffe::Fired() && g_steps.empty() && Lines("combat_frozen_frame_exit").empty());  // noted, not fired
  g_now = 32000;
  ffe::Tick();
  QCHECK(!ffe::Fired() && g_steps.empty());  // 999 ms after the first look
  g_now = 32001;
  ffe::Tick();
  QCHECK(ffe::Fired() && g_steps.size() == 3U);
  ffe::NoteEnd();
  Finish_();
}

// A submit between the two ticks (the pause ended) is not a freeze, and the gap that follows is counted from its own first look.
void ASubmitBetweenTheTicksIsNotAFreeze() {
  Armed();
  g_now = 31001;
  ffe::Tick();  // noted
  g_now = 31500;
  ffe::NoteSubmit();  // the game is drawing again
  g_now = 32001;
  ffe::Tick();
  QCHECK(!ffe::Fired() && g_steps.empty());
  g_now = 31500 + 30001;  // a new gap past the limit: noted again, not fired
  ffe::Tick();
  QCHECK(!ffe::Fired());
  g_now = 31500 + 31001;
  ffe::Tick();
  QCHECK(ffe::Fired());
  ffe::NoteEnd();
  Finish_();
}

// The first look is forgotten when the gap stops being past the limit or the game leaves VR: a later expiry starts the count again.
void ALeaveOrAnArenaLevelForgetsTheFirstLook() {
  Armed();
  g_now = 31001;
  ffe::Tick();  // noted
  PutWord(0);
  g_now = 31500;
  ffe::Tick();  // out of VR: disarmed, forgotten
  PutWord(kSession);
  ffe::NoteSubmit();  // arms again at 31500
  g_now = 31500 + 30001;
  ffe::Tick();
  QCHECK(!ffe::Fired());  // a first look again, not a second
  ffe::NoteEnd();
  Finish_();
}

void FiresRecordBeforeFinisher() {
  Armed();
  g_now = 31001;
  ffe::Tick();
  g_now = 32001;
  ffe::Tick();
  QCHECK(ffe::Fired());
  QCHECK(g_recordsAtFinish == 1U);  // the record was written before the activity was finished
  const std::vector<Line> fired = Lines("combat_frozen_frame_exit");
  QCHECK(fired.size() == 1U);
  if (fired.size() == 1U) {
    QCHECK(fired[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Has(fired[0].text, "\"since_submit_ms\":31001,\"limit_s\":30,\"scope_bits\":1,\"submits\":1,\"armed_ms\":31001,\"action\":\"finish_activity\""));
  }
  ffe::NoteEnd();
  Finish_();
}

void EndsProcessAfterGrace() {
  Armed();
  g_now = 40000;
  ffe::Tick();
  g_now = 41000;
  ffe::Tick();
  QCHECK(g_steps == (std::vector<std::string>{"finish", "grace", "end_after_record"}));
  QCHECK(g_graceMs == ffe::kGraceMs && g_graceMs == 5000U);
  QCHECK(g_exitCode == 1 && g_reason == "frozen_frame");
  const std::vector<Line> end = Lines("combat_frozen_frame_exit_end");
  QCHECK(end.size() == 1U && end[0].level == sentinel::LogLevel::kWarn && Has(end[0].text, "\"grace_ms\":5000,\"action\":\"end_process\""));
  g_now = 90000;
  ffe::Tick();  // once per process
  g_now = 91000;
  ffe::Tick();
  QCHECK(g_steps.size() == 3U && Lines("combat_frozen_frame_exit").size() == 1U);
  ffe::NoteEnd();
  Finish_();
}

void NoActivityIsSaidAndNotFinished() {
  Armed();
  g_canFinish = false;
  g_now = 40000;
  ffe::Tick();
  g_now = 41000;
  ffe::Tick();
  QCHECK(g_steps == (std::vector<std::string>{"grace", "end_after_record"}));
  const std::vector<Line> fired = Lines("combat_frozen_frame_exit");
  QCHECK(fired.size() == 1U && Has(fired[0].text, "\"action\":\"no_activity\""));
  ffe::NoteEnd();
  Finish_();
}

// The process is ending (a QUIT, the OS closing the game, the end step): a gap past the limit does not fire, the watch is off for good, and later
// submits do not arm it again.
const char* g_cause = nullptr;
const char* Cause() noexcept { return g_cause; }
int g_causeCalls = 0;
const char* CauseFromTheThirdLook() noexcept { return ++g_causeCalls >= 3 ? "quit" : nullptr; }

void AnEndingProcessIsNotAFreeze() {
  for (const char* cause : {"quit", "process_end", "destroy_requested"}) {
    Armed();
    g_cause = cause;
    ffe::SetEndingCauseForTest(&Cause);
    g_now = 90000;  // far past the limit, and the gap was seen before
    ffe::Tick();
    g_now = 91000;
    ffe::Tick();
    QCHECK(!ffe::Fired() && ffe::Halted() && g_steps.empty() && ffe::LastSubmitMs() == 0);
    const std::vector<Line> halted = Lines("combat_frozen_frame_exit_halted");
    QCHECK(halted.size() == 1U && halted[0].level == sentinel::LogLevel::kInfo &&
           Has(halted[0].text, std::string("\"cause\":\"") + cause + "\",\"was_armed\":1"));
    g_cause = nullptr;  // even if the cause goes away, the watch stays off
    ffe::NoteSubmit();
    QCHECK(ffe::LastSubmitMs() == 0 && ffe::Arms() == 1U);
    g_now = 200000;
    ffe::Tick();
    ffe::Tick();
    QCHECK(!ffe::Fired() && Lines("combat_frozen_frame_exit_halted").size() == 1U);
    ffe::NoteEnd();  // calls Halt again: the second call writes nothing
    QCHECK(Lines("combat_frozen_frame_exit_halted").size() == 1U);
    QCHECK(Has(Lines("combat_frozen_frame_exit_counts")[0].text, "\"fired\":0,\"halted\":1"));
    Finish_();
  }
}

// A submit that passed Stamp's halted test just before the halt arms the stamp again after it (the test and the exchange are two steps; the clock
// read between them is where the test makes the process begin to end). The halted watch must still not fire.
void CauseThenTick() {
  g_cause = "quit";
  ffe::SetEndingCauseForTest(&Cause);
  ffe::Tick();  // the watchdog halts the watch
  g_cause = nullptr;
}
void ATickAfterHaltNeverFiresEvenWithAStampThatRacedIt() {
  Armed();
  g_onRead = &CauseThenTick;
  ffe::NoteSubmit();  // Stamp: not halted -> clock read (the halt happens here) -> exchange
  QCHECK(ffe::Halted() && ffe::LastSubmitMs() != 0);  // the stamp armed after the halt
  g_now = 90000;
  ffe::Tick();
  g_now = 91001;
  ffe::Tick();
  QCHECK(!ffe::Fired() && g_steps.empty() && Lines("combat_frozen_frame_exit").empty());
  ffe::NoteSubmit();  // and a later submit does not count
  QCHECK(ffe::Submits() == 2U);
  ffe::NoteEnd();
  Finish_();
}

// The production check of the three signals, with no seam on the cause: a QUIT's (or FinishActivity's) finish, the end step, and the glue's
// destroyRequested in the android_app the game was started with; and none of them in an ordinary run.
ANativeActivity* g_someActivity = reinterpret_cast<ANativeActivity*>(0x1000);
alignas(8) unsigned char g_fakeApp[0x100];
const void* FakeApp() noexcept { return g_fakeApp; }
const void* NoApp() noexcept { return nullptr; }
void StubFinish(ANativeActivity*) {}
sentinel::pinned::AndroidAppOpaque* QuitApp() noexcept { return reinterpret_cast<sentinel::pinned::AndroidAppOpaque*>(g_fakeApp); }
void NoExit(int) noexcept {}
void NoEnder() noexcept {}

// Each production signal halts the watch with its own cause: Armed() installs with the production probe, a gap past the limit is noted by the first
// tick, and the second would fire.
void HaltsOn(const char* cause) {
  g_now = 31001;
  ffe::Tick();
  g_now = 32001;
  ffe::Tick();
  QCHECK(!ffe::Fired() && ffe::Halted() && g_steps.empty());
  const std::vector<Line> halted = Lines("combat_frozen_frame_exit_halted");
  QCHECK(halted.size() == 1U && Has(halted[0].text, std::string("\"cause\":\"") + cause + "\""));
}

void TheProductionCheckSeesQuitProcessEndAndDestroy() {
  // nothing ending: the watch runs and fires
  Armed();
  ffe::SetAppSourceForTest(&FakeApp);
  std::memset(g_fakeApp, 0, sizeof(g_fakeApp));
  g_now = 31001;
  ffe::Tick();
  g_now = 32001;
  ffe::Tick();
  QCHECK(ffe::Fired() && !ffe::Halted());
  ffe::NoteEnd();
  Finish_();
  // a QUIT's finish (FinishActivity is the same step)
  Armed();
  std::memset(g_fakeApp, 0, sizeof(g_fakeApp));
  std::memcpy(g_fakeApp + sentinel::glue::kAppActivity, &g_someActivity, sizeof(g_someActivity));
  nevr_quest::quit::ArmForTest(&StubFinish, &QuitApp);
  QCHECK(nevr_quest::quit::FinishActivity());
  HaltsOn("quit");
  ffe::NoteEnd();
  nevr_quest::quit::ResetForTest();
  Finish_();
  // android_main returned or the end step began
  Armed();
  nevr_quest::process_end::ArmForTest(&NoEnder, 10);
  nevr_quest::process_end::SetExitForTest(&NoExit);
  nevr_quest::process_end::EndWithExitCode(1, "test");
  HaltsOn("process_end");
  ffe::NoteEnd();
  nevr_quest::process_end::ResetForTest();
  Finish_();
  // the OS asked the game to close: destroyRequested set in the android_app
  Armed();
  ffe::SetAppSourceForTest(&FakeApp);
  std::memset(g_fakeApp, 0, sizeof(g_fakeApp));
  const int one = 1;
  std::memcpy(g_fakeApp + sentinel::glue::kAppDestroyRequested, &one, sizeof(one));
  HaltsOn("destroy_requested");
  ffe::NoteEnd();
  Finish_();
  // no app yet (android_main not entered): nothing to read, no halt
  Armed();
  ffe::SetAppSourceForTest(&NoApp);
  g_now = 31001;
  ffe::Tick();
  QCHECK(!ffe::Halted());
  ffe::NoteEnd();
  Finish_();
}

// The process begins to end between the second look and the fire: the last look before the record catches it.
void AnEndBegunBeforeTheFireIsNotAFreeze() {
  Armed();
  g_causeCalls = 0;
  ffe::SetEndingCauseForTest(&CauseFromTheThirdLook);
  g_now = 31001;
  ffe::Tick();  // look 1 at its start: not ending; the gap is noted
  g_now = 32001;
  ffe::Tick();  // look 2 at its start: not ending; confirmed; look 3 before the fire: ending
  QCHECK(!ffe::Fired() && ffe::Halted() && g_steps.empty());
  QCHECK(Lines("combat_frozen_frame_exit_halted").size() == 1U && Lines("combat_frozen_frame_exit").empty());
  ffe::NoteEnd();
  Finish_();
}

// The row's end is the end step: the halted record names it, and the thread is joined as before.
void NoteEndHaltsTheWatch() {
  Armed();
  ffe::NoteEnd();
  const std::vector<Line> halted = Lines("combat_frozen_frame_exit_halted");
  QCHECK(halted.size() == 1U && Has(halted[0].text, "\"cause\":\"note_end\""));
  QCHECK(ffe::Halted());
  Finish_();
}

void NullWordDisarms() {
  Armed();
  PutWord(0);  // the game left VR
  g_now = 90000;
  ffe::Tick();
  QCHECK(!ffe::Fired() && ffe::LastSubmitMs() == 0);
  PutWord(kSession);
  ffe::Tick();
  QCHECK(!ffe::Fired());  // nothing armed: no limit runs until the next submit
  ffe::NoteSubmit();
  QCHECK(ffe::LastSubmitMs() == 90000 && ffe::Arms() == 2);
  ffe::NoteEnd();
  Finish_();
}

void ArenaDisarms() {
  Armed();
  world::Levels({"mpl_arena_a"});
  g_now = 90000;
  ffe::Tick();
  QCHECK(!ffe::Fired() && ffe::LastSubmitMs() == 0);
  ffe::NoteEnd();
  Finish_();
}

// The game leaves VR between the watchdog's two reads: not a freeze.
void LeaveBetweenTheReadsIsNotAFreeze() {
  Armed();
  g_now = 90000;
  g_onRead = [] { PutWord(0); };
  ffe::Tick();
  QCHECK(!ffe::Fired() && g_steps.empty());
  PutWord(kSession);  // and when it is back, the gap still has to be seen twice
  g_now = 91000;
  ffe::Tick();
  QCHECK(!ffe::Fired() && g_steps.empty());
  ffe::NoteEnd();
  Finish_();
}

void WaitFor(const std::atomic<bool>& flag) {
  for (int i = 0; i < 2000 && !flag.load(); ++i) {
    const timespec ms{0, 1000000};
    nanosleep(&ms, nullptr);
  }
}

void NoteEndJoinsThread() {
  Begin();
  world::Levels({"mpl_combat_dyson"});
  ffe::SetPollMsForTest(5);
  QCHECK(Install().installed);
  QCHECK(ffe::ThreadRunningForTest());
  ffe::NoteSubmit();
  ffe::NoteEnd();
  QCHECK(!ffe::ThreadRunningForTest());
  const std::vector<Line> counts = Lines("combat_frozen_frame_exit_counts");
  QCHECK(counts.size() == 1U && counts[0].level == sentinel::LogLevel::kInfo &&
         Has(counts[0].text, "\"arms\":1,\"submits\":1,\"fired\":0,\"halted\":1,\"joined\":1"));
  Finish_();
  Begin();  // not installed: no line
  ffe::NoteEnd();
  QCHECK(Lines("combat_frozen_frame_exit_counts").empty());
  Finish_();
}

// The watchdog's own thread fires and, inside the end (as process_end's before-exit step does), the row's end runs: it does not join the thread
// that is ending the process.
void NoteEndWhileEndingDoesNotJoin() {
  Begin();
  world::Levels({"mpl_combat_dyson"});
  ffe::Seams seams;
  seams.canFinish = &CanFinish;
  seams.finish = &Finish;
  seams.grace = &Grace;
  seams.end = &EndRunningTheRowsEnd;
  ffe::SetSeamsForTest(seams);
  ffe::SetPollMsForTest(5);
  QCHECK(Install().installed);
  ffe::NoteSubmit();
  g_now = 1000 + 30001;
  const timespec some{0, 60000000};  // the thread ticks a dozen times and notes the gap
  nanosleep(&some, nullptr);
  g_now = 1000 + 31001;
  WaitFor(g_endRan);
  QCHECK(g_endRan.load() && ffe::Fired());
  const std::vector<Line> counts = Lines("combat_frozen_frame_exit_counts");
  QCHECK(counts.size() == 1U && Has(counts[0].text, "\"fired\":1,\"halted\":0,\"joined\":0"));
  Finish_();
  QCHECK(!ffe::ThreadRunningForTest());
}

// ---- the install ----------------------------------------------------------------------------------------------------------------------

void InstallLineSaysWhatItWatches() {
  Begin();
  QCHECK(Install(true, 45, true).installed);
  const std::vector<Line> lines = Lines("combat_frozen_frame_exit_install");
  QCHECK(lines.size() == 1U && lines[0].level == sentinel::LogLevel::kInfo &&
         Has(lines[0].text, "\"result\":\"installed\",\"status\":\"ok\",\"limit_s\":45,\"limit_source\":\"config\",\"scope\":\"combat_or_lobby\""));
  ffe::NoteEnd();
  Finish_();
  Begin();  // a limit outside the range: the default
  QCHECK(Install(true, 5, true).installed);
  QCHECK(Has(Lines("combat_frozen_frame_exit_install")[0].text, "\"limit_s\":30,\"limit_source\":\"default\""));
  ffe::NoteEnd();
  Finish_();
}

void SwitchOffInstallsNothing() {
  Begin();
  const ModuleResult r = Install(false);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off") == 0 && !ffe::ThreadRunningForTest());
  const std::vector<Line> lines = Lines("combat_frozen_frame_exit_install");
  QCHECK(lines.size() == 1U && lines[0].level == sentinel::LogLevel::kInfo && Has(lines[0].text, "\"result\":\"off\",\"status\":\"off_config\""));
  world::Levels({"mpl_combat_dyson"});
  ffe::NoteSubmit();
  QCHECK(ffe::LastSubmitMs() == 0);
  Finish_();
}

void Refuses(const char* status, const char* why = nullptr) {
  const ModuleResult r = Install();
  QCHECK(!r.installed && !r.off && std::strcmp(r.status, status) == 0);
  QCHECK(!ffe::ThreadRunningForTest());
  const std::vector<Line> lines = Lines("combat_frozen_frame_exit_install");
  QCHECK(!lines.empty() && lines.back().level == sentinel::LogLevel::kError && Has(lines.back().text, std::string("\"status\":\"") + status + "\""));
  if (why != nullptr) QCHECK(!lines.empty() && Has(lines.back().text, std::string("\"why\":\"") + why + "\""));
  world::Levels({"mpl_combat_dyson"});
  ffe::NoteSubmit();
  QCHECK(ffe::LastSubmitMs() == 0);  // not armed
}

void InstallRefusesScopeNotLive() {
  Begin(/*scopeLive=*/false);
  Refuses("scope_not_live");
  Finish_();
}

void InstallRefusesSubmitSlotNotHeld() {
  Begin();
  nevr_quest::render_config::detail::g_submitSlotHeld.store(false);
  Refuses("submit_slot_not_held");
  Finish_();
}

void InstallRefusesWordUnreadable() {
  Begin();
  g_image.Build({world::TickRow()}, pinned::kLibR15StockBuildId);  // the store file's ID, not the running patched one
  Refuses("word_unreadable", "build_id_mismatch");
  Finish_();
  Begin();
  g_image.Build({world::TickRow()}, nullptr, /*note=*/false);
  Refuses("word_unreadable", "no_build_id");
  Finish_();
  Begin();
  ffe::ResetForTest();
  const ModuleResult r = ffe::InstallAt(true, 30, false, nullptr);
  QCHECK(std::strcmp(r.status, "word_unreadable") == 0 && Has(Lines("combat_frozen_frame_exit_install").back().text, "\"why\":\"module_not_loaded\""));
  Finish_();
}

void InstallRefusesChangedWord() {
  for (const nevr_quest::combat::pinned::Word& w : targets::kWords) {
    Begin();
    g_image.Put<std::uint32_t>(w.vaddr, w.word ^ 1U);
    Refuses("pinned_word_mismatch");
    const std::vector<Line> lines = Lines("combat_frozen_frame_exit_install");
    QCHECK(!lines.empty() && Has(lines.back().text, std::string("\"pin\":\"") + w.what + "\""));
    Finish_();
  }
}

}  // namespace

int main() {
  Expired();
  Confirmed();
  NoteSubmitOutsideScopeTouchesNothing();
  NoteSubmitInScopeStamps();
  NotPastTheLimitDoesNothing();
  NeedsASecondTick();
  ASubmitBetweenTheTicksIsNotAFreeze();
  ALeaveOrAnArenaLevelForgetsTheFirstLook();
  AnEndingProcessIsNotAFreeze();
  AnEndBegunBeforeTheFireIsNotAFreeze();
  ATickAfterHaltNeverFiresEvenWithAStampThatRacedIt();
  NoteEndHaltsTheWatch();
  TheProductionCheckSeesQuitProcessEndAndDestroy();
  FiresRecordBeforeFinisher();
  EndsProcessAfterGrace();
  NoActivityIsSaidAndNotFinished();
  NullWordDisarms();
  ArenaDisarms();
  LeaveBetweenTheReadsIsNotAFreeze();
  NoteEndJoinsThread();
  NoteEndWhileEndingDoesNotJoin();
  InstallLineSaysWhatItWatches();
  SwitchOffInstallsNothing();
  InstallRefusesScopeNotLive();
  InstallRefusesSubmitSlotNotHeld();
  InstallRefusesWordUnreadable();
  InstallRefusesChangedWord();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "frozen_frame_exit_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("frozen_frame_exit_test: all checks passed");
  return 0;
}

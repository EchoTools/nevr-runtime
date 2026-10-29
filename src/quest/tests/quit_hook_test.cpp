// Host test for "the main-menu QUIT ends the game on every Horizon OS" (#1276): the real handler
// (src/quest/sentinel/quit_hook.cpp), built without exceptions as the sentinel builds it, driven through the thunk's entry
// with a stand-in for libr15's CVR::ShowSystemUI and for ANativeActivity_finish. No GOT slot is touched:
// got_pinned_test.cpp resolves the pinned slot, its PLT stub and the one call site in the real library.
//
// Built and run by `just test-quest-hooks`.

#include <cstdio>
#include <cstring>

#include "android_glue_layout.h"
#include "quest/tests/test_check.h"
#include "quit_hook.h"

struct ANativeActivity {
  int marker;
};

namespace {

namespace quit = nevr_quest::quit;

quit::Thunk::Fn Entry() { return reinterpret_cast<quit::Thunk::Fn>(quit::Thunk::EntryAddress()); }

// The game's CVR::ShowSystemUI: what the player got before the hook (a system UI chosen by number).
int g_gameUiCalls = 0;
void StandInShowSystemUI() { ++g_gameUiCalls; }

// ANativeActivity_finish: records the activity it was given.
int g_finishCalls = 0;
ANativeActivity* g_finished = nullptr;
void StandInFinish(ANativeActivity* activity) {
  ++g_finishCalls;
  g_finished = activity;
}

// An android_app with the activity at the glue's offset, as android_main receives it.
ANativeActivity g_activity{7};
alignas(8) unsigned char g_app[0x100];
void SetAppActivity(ANativeActivity* activity) {
  std::memset(g_app, 0, sizeof(g_app));
  std::memcpy(g_app + sentinel::glue::kAppActivity, &activity, sizeof(activity));
}
sentinel::pinned::AndroidAppOpaque* App() noexcept { return reinterpret_cast<sentinel::pinned::AndroidAppOpaque*>(g_app); }
sentinel::pinned::AndroidAppOpaque* NoApp() noexcept { return nullptr; }

void Reset() {
  g_gameUiCalls = 0;
  g_finishCalls = 0;
  g_finished = nullptr;
  SetAppActivity(&g_activity);
  quit::ResetForTest();
  quit::Thunk::Reset();
  *quit::Thunk::OriginalOut() = reinterpret_cast<void*>(&StandInShowSystemUI);
}

// Without the hook armed the entry is the game's call.
void WithoutTheHookQuitIsTheGamesSystemUi() {
  Reset();
  Entry()();
  QCHECK(g_gameUiCalls == 1);
  QCHECK(g_finishCalls == 0);
}

// The case the hook exists for: QUIT finishes the game's own activity, and no system UI is asked for by number.
void QuitFinishesTheGamesActivity() {
  Reset();
  quit::ArmForTest(&StandInFinish, &App);
  Entry()();
  QCHECK(g_finishCalls == 1);
  QCHECK(g_finished == &g_activity);
  QCHECK(g_gameUiCalls == 0);
  QCHECK(quit::CurrentCounts().finished == 1);
  QCHECK(quit::CurrentCounts().game_ui == 0);
}

// Before android_main there is no app: the QUIT goes to the game's own call, never dropped.
void WithNoAppTheGameKeepsItsQuit() {
  Reset();
  quit::ArmForTest(&StandInFinish, &NoApp);
  Entry()();
  QCHECK(g_finishCalls == 0);
  QCHECK(g_gameUiCalls == 1);
  QCHECK(quit::CurrentCounts().game_ui == 1);
  QCHECK(quit::CurrentCounts().finished == 0);  // the previous case's finish was cleared by ResetForTest
}

// An app whose activity is null is treated the same.
void WithNoActivityTheGameKeepsItsQuit() {
  Reset();
  SetAppActivity(nullptr);
  quit::ArmForTest(&StandInFinish, &App);
  Entry()();
  QCHECK(g_finishCalls == 0);
  QCHECK(g_gameUiCalls == 1);
}

// Without ANativeActivity_finish the install refuses (the game's call stays in the slot), and an armed handler with no
// finisher passes the QUIT to the game.
void WithNoFinisherTheGameKeepsItsQuit() {
  Reset();
  QCHECK(!quit::InstallAt(sentinel::pinned::LibR15ShowSystemUI(), nullptr, &App));
  Entry()();  // the refused install armed nothing: the entry is the game's call and the handler counted nothing
  QCHECK(g_gameUiCalls == 1);
  QCHECK(quit::CurrentCounts().game_ui == 0);
  quit::ArmForTest(nullptr, &App);
  Entry()();
  QCHECK(g_finishCalls == 0);
  QCHECK(g_gameUiCalls == 2);
  QCHECK(quit::CurrentCounts().game_ui == 1);
}

}  // namespace

int main() {
  WithoutTheHookQuitIsTheGamesSystemUi();
  QuitFinishesTheGamesActivity();
  WithNoAppTheGameKeepsItsQuit();
  WithNoActivityTheGameKeepsItsQuit();
  WithNoFinisherTheGameKeepsItsQuit();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "quit_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("quit_hook_test: all checks pass\n");
  return 0;
}

/* The main-menu QUIT on the headset ends the game, on every Horizon OS (#1276, #392).
 *
 * What the game does (pinned libr15; pinned_targets.h, LibR15ShowSystemUI): QUIT is state 0 of
 * CR15VRMenuWidgetCS::UpdateExclusive, which calls CVR::ShowSystemUI, which asks VrApi for system UI type 1. What a type
 * shows belongs to the system's VrApi, not to the game: on Horizon OS v2.7, type 1 is a quit confirmation and type 0 the
 * Settings panel (#1276); on a current OS, type 1 opens the Meta library (#392). Nothing here depends on that enum.
 *
 * One GOT hook, on libr15's own CVR::ShowSystemUI slot. The handler finishes the game's activity, the end the game
 * already handles when the OS closes it: ANativeActivity_finish (NDK android/native_activity.h: it "can be called from
 * *any* thread; it will send a message to the main thread of the process where the Java finish call will take place").
 * The activity is destroyed, the native-app glue sets destroyRequested on APP_CMD_DESTROY, CVR::ShouldQuit
 * (CSysJava::AppDestroyed) turns true, CR15Game::UpdateGame sets the engine's quit word, NRadEngine::Main returns and
 * android_main ends the process (process_end_hook.h).
 *
 * When there is no activity (android_main has not been called) or no ANativeActivity_finish to call, the handler calls
 * the game's own CVR::ShowSystemUI, so a QUIT is never dropped.
 *
 * Log lines:
 *   install {"event":"quit_install","result":"installed|refused","status":...,"slot":...,"finisher":"found|missing"}
 *   call    {"event":"quit","action":"finish_activity|game_system_ui","reason":"no_activity|no_finisher"}
 * The call line is written before the activity is finished or the game's function runs, never while game code is on
 * the stack above the handler (callback_thunk.h rule 1).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include "pinned_targets.h"

struct ANativeActivity;  // android/native_activity.h; only ever passed through

namespace nevr_quest::quit {

using Thunk = sentinel::pinned::LibR15ShowSystemUIThunk;
// ANativeActivity_finish's shape.
using FinishFn = void (*)(ANativeActivity* activity);
// The android_app the game's android_main runs with (null until then).
using AppFn = sentinel::pinned::AndroidAppOpaque* (*)() noexcept;

struct Counts {
  unsigned long long finished;  // QUITs that finished the activity
  unsigned long long game_ui;   // QUITs passed to the game's own CVR::ShowSystemUI
};
Counts CurrentCounts() noexcept;

// Installs the hook into libr15's CVR::ShowSystemUI slot, finishing with libandroid.so's ANativeActivity_finish and
// reading the app from process_end::CurrentApp. True when the slot holds the thunk. Without ANativeActivity_finish it
// installs nothing and the game's QUIT is untouched; either way it logs one quit_install line.
bool Install() noexcept;

// The install on any target, with `finish` and `app` (the host test installs it in a fixture library).
bool InstallAt(const sentinel::GotTarget& target, FinishFn finish, AppFn app) noexcept;

// Test support: arms the handler without touching a GOT slot. ResetForTest clears the counts.
void ArmForTest(FinishFn finish, AppFn app) noexcept;
void ResetForTest() noexcept;

}  // namespace nevr_quest::quit

/* A visible line on the headset, by an Android Toast on the game's activity (#807, docs/adr/0011-quest-game-data-first-run.md,
 * mechanism 2 of "What the player sees").
 *
 * Whether Horizon OS draws a Toast over an immersive native app, and whether one can be made from the android_main
 * thread, are not measured: the result token says how far the call got, and the data step logs it, so a headset run
 * reads the answer from the log (the test sheet in the issue lists the lines). The call never throws and clears every
 * Java exception it causes; it attaches the thread to the VM when it is not attached and detaches only then.
 *
 * Steps, each with its own result token on failure: the activity from the glue (android_glue_layout.h), the VM's
 * environment for this thread, `Looper.myLooper()` and, when the thread has none, `Looper.prepare()` (Toast.makeText
 * throws on a thread without a Looper), `Toast.makeText(activity, text, LENGTH_LONG)`, `show()`.
 */
#pragma once

namespace nevr_quest::toast {

// Shows `text` (ASCII; the VM takes modified UTF-8). `app` is the game's android_app (process_end::CurrentApp()).
// Returns a result token: "shown", "no_app", "no_activity", "no_vm", "attach_failed", "env_failed", "looper_failed",
// "class_failed", "toast_failed" or "show_failed"; `*looperPrepared` is set when Looper.prepare() was called.
const char* Show(void* app, const char* text, bool* looperPrepared) noexcept;

}  // namespace nevr_quest::toast

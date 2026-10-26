/* A visible line on the headset, by an Android Toast on the game's activity (#807, docs/adr/0011-quest-game-data-first-run.md,
 * mechanism 2 of "What the player sees").
 *
 * Whether Horizon OS draws a Toast over an immersive native app, and whether one can be made from the android_main
 * thread, are not measured: the result token says how far the call got ("requested" is as far as the call can know), and the data step logs it, so a headset run
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
// Returns a result token: "requested" (Toast.show() returned without an exception: the system was asked to draw it, which
// says nothing about whether it did), "no_app", "no_activity", "no_vm", "attach_failed", "env_failed",
// "exception_pending", "looper_failed", "class_failed", "toast_failed" or "show_failed"; `*looperPrepared` is set when
// Looper.prepare() was called. After every JNI call the call's exception, if any, is described and cleared, whether or
// not the call returned null.
const char* Show(void* app, const char* text, bool* looperPrepared) noexcept;

// The same Toast, made with the MAIN looper (candidate A of #829): `Toast.makeText(activity, Looper.getMainLooper(), text,
// LENGTH_LONG)`, so the toast's handler runs on the UI thread, which runs its looper, instead of on the android_main
// thread's looper that nobody runs. That overload is `@hide` (android-12.0.0_r1 Toast.java), so this is a hidden-API call
// from native code, not public API. No Looper.prepare() is made and `*looperPrepared` stays false. The result tokens are
// Show's, plus "looper_overload_unavailable" when the lookup of that overload failed (the device lacks it, or the
// hidden-API policy refused it; the token cannot tell which). Whether the headset draws it is not known; "requested" says
// only that show() returned.
const char* ShowOnMainLooper(void* app, const char* text, bool* looperPrepared) noexcept;

// A call on the UI thread with no Java class (candidate A of #829, the form that needs no Runnable and no dex).
// CaptureUiLooper() is called ON the UI thread (the ELF constructor, when the activity's own thread loads the library): it
// takes the thread's ALooper (ALooper_forThread, from libandroid.so by dlsym) and adds the read end of a pipe to it.
// ShowOnUiThread() then queues the text, from any thread, and the looper runs the same Toast as Show() on the UI thread, where
// Looper.myLooper() is the running main looper. Whether the constructor runs on the UI thread is not known; the capture result
// token says so ("no_looper_on_this_thread" when it does not). Returns: "captured", "already_captured", "no_libandroid",
// "no_looper_api", "no_looper_on_this_thread", "pipe_failed", "add_fd_failed" or "capture_in_progress_or_failed".
const char* CaptureUiLooper() noexcept;

// ShowOnUiThread's result is "posted" (the text is queued; nothing is known of the Toast yet) or "no_ui_looper",
// "text_too_long" (over 400 bytes), "alloc_failed", "queue_full" or "write_failed". `*looperPrepared` is false. The Toast's own
// result is logged from the UI thread as one `ui_post` line with state=ran, the result token of Show(), the thread id and
// whether that is the process's main thread.
const char* ShowOnUiThread(void* app, const char* text, bool* looperPrepared) noexcept;

// The NDK's looper functions the capture needs, so a host test can stand in for them.
struct LooperApi {
  using AddFd = int (*)(void* looper, int fd, int ident, int events, int (*callback)(int fd, int events, void* data),
                        void* data);
  void* (*forThread)() = nullptr;
  void (*acquire)(void* looper) = nullptr;
  AddFd addFd = nullptr;
};
const char* CaptureUiLooperWith(const LooperApi& api) noexcept;
// Texts queued and not yet shown or refused (a leak shows as a count that stays up). For tests.
int UiQueuedForTest() noexcept;
// Closes the pipe and allows a new capture. For tests.
void ResetUiLooperForTest() noexcept;

}  // namespace nevr_quest::toast

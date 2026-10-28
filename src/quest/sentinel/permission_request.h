/* The game's permission request, made with a defined request code (#947).
 *
 * libr15's CSysInfo::RequestPermissions (vaddr 0xf873ec) asks for its permissions through
 * CJavaObject::CallVoidMethod("requestPermissions", "([Ljava/lang/String;I)V", array) (vaddr 0xf7d6e4), and that
 * wrapper calls JNI's variadic CallVoidMethod with ONE argument after the method id (0xf7d84c-0xf7d858: x3 = the
 * String[], and nothing sets x4). The method takes a String[] and an int, so the int, the request code, is whatever x4
 * holds when the call is made. Activity.requestPermissions throws IllegalArgumentException for a negative request code, and the
 * uncaught exception aborts the process. Whether it is negative depends on what the runtime's own calls before it
 * left in that register.
 *
 * Request() makes the same Java call on the game's activity with the String[] of `names` and `requestCode`, so the
 * request code is the one the caller chose. The game never reads the code back (nothing in libr15 or the APK's
 * classes.dex has an onRequestPermissionsResult), so any non-negative value serves.
 *
 * Steps, each with its own result token on failure: the activity and the VM from the glue (android_glue_layout.h), the
 * VM's environment for this thread (attached here when it is not, and detached here only then), a local frame,
 * java.lang.String, the array and each name, the activity's class, the method, the call. After every JNI call a pending
 * Java exception is described (logcat) and cleared, so the game's next JNI call never runs with one pending. The call
 * never throws.
 */
#pragma once

#include <cstdint>

namespace nevr_quest::permission_request {

// Asks the activity of `app` (the game's android_app, process_end::CurrentApp()) for the `count` permissions in
// `names` (ASCII), with `requestCode`. Returns "requested" when requestPermissions returned without an exception (the
// system was asked; whether a dialog appears is the system's), else "no_names", "no_app", "no_activity", "no_vm",
// "attach_failed", "env_failed", "exception_pending" (the caller's own exception: no JNI call is made, and it is not
// ours to clear), "frame_failed", "class_failed", "array_failed", "string_failed", "method_failed" or "call_failed".
const char* Request(void* app, const char* const* names, std::uint64_t count, int requestCode) noexcept;

}  // namespace nevr_quest::permission_request

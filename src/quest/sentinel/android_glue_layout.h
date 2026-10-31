/* The fields of the NDK's native_app_glue `struct android_app` and `ANativeActivity` the first-run data step reads
 * (#807, docs/adr/0011-quest-game-data-first-run.md), as byte offsets: the glue headers are not on the include path of
 * the hook sources, and the game's `android_main` gets a pointer to a struct the hooks only know as opaque.
 *
 * Where the numbers come from. The glue headers (android_native_app_glue.h, android/native_activity.h in NDK r26d) give
 * the layout; android_glue_layout_check.cpp asserts every constant here against them with offsetof, so a header that
 * disagrees fails the Android build. The game's own android_main (libr15.so build b243509c..., disassembly VA = pinned
 * vaddr + 0x100000) reads the same offsets: 0x10784e0 keeps `app` in x20; 0x10787a4 `ldr w8,[x20,#0x64]` is
 * destroyRequested; 0x10787ac `ldr x0,[x20,#0x18]` and 0x1078820 `ldr x8,[x20,#0x18]` load `activity`; 0x1078824 `ldr x0,[x8,#0x8]` is
 * `activity->vm`, and 0x1078830 calls slot 0x28 of the JavaVM function table, which jni.h names DetachCurrentThread.
 */
#pragma once

#include <cstddef>
#include <cstring>

namespace sentinel::glue {

inline constexpr std::size_t kAppActivity = 0x18;           // android_app::activity (ANativeActivity*)
inline constexpr std::size_t kAppDestroyRequested = 0x64;   // android_app::destroyRequested (int)
inline constexpr std::size_t kActivityVm = 0x8;             // ANativeActivity::vm (JavaVM*)
inline constexpr std::size_t kActivityEnv = 0x10;           // ANativeActivity::env (JNIEnv*)
inline constexpr std::size_t kActivityClazz = 0x18;         // ANativeActivity::clazz (jobject)

// The pointer stored `offset` bytes into `base`; null when `base` is null.
inline void* ReadPointer(const void* base, std::size_t offset) noexcept {
  if (base == nullptr) return nullptr;
  void* value = nullptr;
  std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(value));
  return value;
}

}  // namespace sentinel::glue

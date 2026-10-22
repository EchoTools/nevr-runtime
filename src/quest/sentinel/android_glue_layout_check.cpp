// Proves android_glue_layout.h against the NDK's own headers. Nothing here runs: every check is a static_assert.

#include <android/native_activity.h>
#include <jni.h>
#include <android_native_app_glue.h>

#include <cstddef>

#include "android_glue_layout.h"

static_assert(offsetof(android_app, activity) == sentinel::glue::kAppActivity, "android_app::activity");
static_assert(offsetof(android_app, destroyRequested) == sentinel::glue::kAppDestroyRequested,
              "android_app::destroyRequested");
static_assert(offsetof(ANativeActivity, vm) == sentinel::glue::kActivityVm, "ANativeActivity::vm");
static_assert(offsetof(ANativeActivity, env) == sentinel::glue::kActivityEnv, "ANativeActivity::env");
static_assert(offsetof(ANativeActivity, clazz) == sentinel::glue::kActivityClazz, "ANativeActivity::clazz");
// The slot the game's android_main calls on the VM at 0x1078830 (offset 0x28) is DetachCurrentThread by name.
static_assert(offsetof(JNIInvokeInterface, DetachCurrentThread) == 0x28, "JNIInvokeInterface::DetachCurrentThread");

#include "permission_request.h"

#include <jni.h>

#include "android_glue_layout.h"

namespace nevr_quest::permission_request {

namespace {

// More names than any caller passes (the boot request has three); a request with more is refused, not truncated.
constexpr std::uint64_t kMaxNames = 16;

// True when the last JNI call left a Java exception pending: it is described (logcat) and cleared. Called after EVERY
// JNI call, before the call's result is looked at.
bool Pending(JNIEnv* env) noexcept {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionDescribe();
  env->ExceptionClear();
  return true;
}

const char* RequestOnEnv(JNIEnv* env, jobject activity, const char* const* names, std::uint64_t count,
                         int requestCode) noexcept {
  jclass stringClass = env->FindClass("java/lang/String");
  if (Pending(env) || stringClass == nullptr) return "class_failed";
  jobjectArray array = env->NewObjectArray(static_cast<jsize>(count), stringClass, nullptr);
  if (Pending(env) || array == nullptr) return "array_failed";
  for (std::uint64_t i = 0; i < count; ++i) {
    jstring name = env->NewStringUTF(names[i]);
    if (Pending(env) || name == nullptr) return "string_failed";
    env->SetObjectArrayElement(array, static_cast<jsize>(i), name);
    if (Pending(env)) return "string_failed";
    env->DeleteLocalRef(name);
  }
  jclass activityClass = env->GetObjectClass(activity);
  if (Pending(env) || activityClass == nullptr) return "class_failed";
  jmethodID method = env->GetMethodID(activityClass, "requestPermissions", "([Ljava/lang/String;I)V");
  if (Pending(env) || method == nullptr) return "method_failed";
  env->CallVoidMethod(activity, method, array, static_cast<jint>(requestCode));
  if (Pending(env)) return "call_failed";
  return "requested";
}

}  // namespace

const char* Request(void* app, const char* const* names, std::uint64_t count, int requestCode) noexcept {
  if (names == nullptr || count == 0 || count > kMaxNames) return "no_names";
  for (std::uint64_t i = 0; i < count; ++i) {
    if (names[i] == nullptr) return "no_names";
  }
  if (app == nullptr) return "no_app";
  void* const activity = sentinel::glue::ReadPointer(app, sentinel::glue::kAppActivity);
  if (activity == nullptr) return "no_activity";
  JavaVM* const vm = static_cast<JavaVM*>(sentinel::glue::ReadPointer(activity, sentinel::glue::kActivityVm));
  if (vm == nullptr) return "no_vm";
  jobject const clazz = static_cast<jobject>(sentinel::glue::ReadPointer(activity, sentinel::glue::kActivityClazz));
  if (clazz == nullptr) return "no_activity";

  JNIEnv* env = nullptr;
  bool attachedHere = false;
  const jint got = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (got == JNI_EDETACHED) {
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) return "attach_failed";
    attachedHere = true;
  } else if (got != JNI_OK || env == nullptr) {
    return "env_failed";
  }
  // The array, the names, the String class and the activity's class; each name is deleted once it is in the array.
  constexpr jint kLocalRefs = 8;
  const char* result = "frame_failed";
  if (env->ExceptionCheck()) {
    result = "exception_pending";
  } else {
    const jint pushed = env->PushLocalFrame(kLocalRefs);
    if (!Pending(env) && pushed == 0) {
      result = RequestOnEnv(env, clazz, names, count, requestCode);
      env->PopLocalFrame(nullptr);
    }
  }
  if (attachedHere) vm->DetachCurrentThread();
  return result;
}

}  // namespace nevr_quest::permission_request

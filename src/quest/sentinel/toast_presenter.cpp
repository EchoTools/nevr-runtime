#include "toast_presenter.h"

#include <jni.h>

#include "android_glue_layout.h"

namespace nevr_quest::toast {

namespace {

// True when the last call left a Java exception pending; it is cleared.
bool Failed(JNIEnv* env) noexcept {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionClear();
  return true;
}

const char* ShowOnEnv(JNIEnv* env, jobject activity, const char* text, bool* looperPrepared) noexcept {
  jclass looper = env->FindClass("android/os/Looper");
  if (looper == nullptr || Failed(env)) return "class_failed";
  jmethodID myLooper = env->GetStaticMethodID(looper, "myLooper", "()Landroid/os/Looper;");
  if (myLooper == nullptr || Failed(env)) return "looper_failed";
  jobject current = env->CallStaticObjectMethod(looper, myLooper);
  if (Failed(env)) return "looper_failed";
  if (current == nullptr) {
    jmethodID prepare = env->GetStaticMethodID(looper, "prepare", "()V");
    if (prepare == nullptr || Failed(env)) return "looper_failed";
    env->CallStaticVoidMethod(looper, prepare);
    if (Failed(env)) return "looper_failed";
    *looperPrepared = true;
  }

  jclass toastClass = env->FindClass("android/widget/Toast");
  if (toastClass == nullptr || Failed(env)) return "class_failed";
  jmethodID makeText = env->GetStaticMethodID(
      toastClass, "makeText", "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");
  if (makeText == nullptr || Failed(env)) return "toast_failed";
  jstring message = env->NewStringUTF(text);
  if (message == nullptr || Failed(env)) return "toast_failed";
  constexpr jint kLengthLong = 1;
  jobject toast = env->CallStaticObjectMethod(toastClass, makeText, activity, message, kLengthLong);
  if (toast == nullptr || Failed(env)) return "toast_failed";
  jmethodID show = env->GetMethodID(toastClass, "show", "()V");
  if (show == nullptr || Failed(env)) return "show_failed";
  env->CallVoidMethod(toast, show);
  if (Failed(env)) return "show_failed";
  return "shown";
}

}  // namespace

const char* Show(void* app, const char* text, bool* looperPrepared) noexcept {
  *looperPrepared = false;
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
  constexpr jint kLocalRefs = 16;
  const char* result = "toast_failed";
  if (env->PushLocalFrame(kLocalRefs) == 0) {
    result = ShowOnEnv(env, clazz, text, looperPrepared);
    env->PopLocalFrame(nullptr);
  }
  if (attachedHere) vm->DetachCurrentThread();
  return result;
}

}  // namespace nevr_quest::toast

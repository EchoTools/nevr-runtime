#include "toast_presenter.h"

#include <jni.h>

#include "android_glue_layout.h"

namespace nevr_quest::toast {

namespace {

// True when the last JNI call left a Java exception pending: it is described (logcat) and cleared. Called after EVERY
// JNI call, before the call's result is looked at: a call that throws may return null or not, and an exception that is
// left pending makes the game's own next JNI call undefined (an abort under CheckJNI).
bool Pending(JNIEnv* env) noexcept {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionDescribe();
  env->ExceptionClear();
  return true;
}

// Where the Toast's handler runs. kThisThread: Toast.makeText(context, text, duration) takes the calling thread's looper
// (made with Looper.prepare() when it has none). kMainLooper: Toast.makeText(context, Looper.getMainLooper(), text,
// duration), the API 30 overload, so the handler runs on the UI thread's running looper.
enum class Where { kThisThread, kMainLooper };

const char* ShowOnEnv(JNIEnv* env, jobject activity, const char* text, bool* looperPrepared, Where where) noexcept {
  jclass looper = env->FindClass("android/os/Looper");
  if (Pending(env) || looper == nullptr) return "class_failed";
  jobject mainLooper = nullptr;
  if (where == Where::kMainLooper) {
    jmethodID getMain = env->GetStaticMethodID(looper, "getMainLooper", "()Landroid/os/Looper;");
    if (Pending(env) || getMain == nullptr) return "looper_failed";
    mainLooper = env->CallStaticObjectMethod(looper, getMain);
    if (Pending(env) || mainLooper == nullptr) return "looper_failed";
  } else {
    jmethodID myLooper = env->GetStaticMethodID(looper, "myLooper", "()Landroid/os/Looper;");
    if (Pending(env) || myLooper == nullptr) return "looper_failed";
    jobject current = env->CallStaticObjectMethod(looper, myLooper);
    if (Pending(env)) return "looper_failed";
    if (current == nullptr) {
      jmethodID prepare = env->GetStaticMethodID(looper, "prepare", "()V");
      if (Pending(env) || prepare == nullptr) return "looper_failed";
      env->CallStaticVoidMethod(looper, prepare);
      if (Pending(env)) return "looper_failed";
      *looperPrepared = true;
    }
  }

  jclass toastClass = env->FindClass("android/widget/Toast");
  if (Pending(env) || toastClass == nullptr) return "class_failed";
  jstring message = nullptr;
  jobject toast = nullptr;
  constexpr jint kLengthLong = 1;
  if (where == Where::kMainLooper) {
    jmethodID makeText = env->GetStaticMethodID(
        toastClass, "makeText",
        "(Landroid/content/Context;Landroid/os/Looper;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");
    if (Pending(env) || makeText == nullptr) return "looper_api_missing";  // before API 30: NoSuchMethodError, cleared
    message = env->NewStringUTF(text);
    if (Pending(env) || message == nullptr) return "toast_failed";
    toast = env->CallStaticObjectMethod(toastClass, makeText, activity, mainLooper, message, kLengthLong);
  } else {
    jmethodID makeText = env->GetStaticMethodID(
        toastClass, "makeText", "(Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;");
    if (Pending(env) || makeText == nullptr) return "toast_failed";
    message = env->NewStringUTF(text);
    if (Pending(env) || message == nullptr) return "toast_failed";
    toast = env->CallStaticObjectMethod(toastClass, makeText, activity, message, kLengthLong);
  }
  if (Pending(env) || toast == nullptr) return "toast_failed";
  jmethodID show = env->GetMethodID(toastClass, "show", "()V");
  if (Pending(env) || show == nullptr) return "show_failed";
  env->CallVoidMethod(toast, show);
  if (Pending(env)) return "show_failed";
  return "requested";
}

const char* ShowWhere(void* app, const char* text, bool* looperPrepared, Where where) noexcept {
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
  if (env->ExceptionCheck()) {
    result = "exception_pending";  // the caller's own exception: no JNI call is safe, and it is not ours to clear
  } else {
    const jint pushed = env->PushLocalFrame(kLocalRefs);
    if (Pending(env) || pushed != 0) {
      result = "toast_failed";
    } else {
      result = ShowOnEnv(env, clazz, text, looperPrepared, where);
      env->PopLocalFrame(nullptr);
    }
  }
  if (attachedHere) vm->DetachCurrentThread();
  return result;
}

}  // namespace

const char* Show(void* app, const char* text, bool* looperPrepared) noexcept {
  return ShowWhere(app, text, looperPrepared, Where::kThisThread);
}

const char* ShowOnMainLooper(void* app, const char* text, bool* looperPrepared) noexcept {
  return ShowWhere(app, text, looperPrepared, Where::kMainLooper);
}

}  // namespace nevr_quest::toast

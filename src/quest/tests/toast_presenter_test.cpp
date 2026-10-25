// Host test for the toast presenter (src/quest/sentinel/toast_presenter.cpp) against a fake JNI. The fake counts every
// JNI call and, at one chosen call, makes it throw (a Java exception pending) while returning null or a non-null value.
// For every call of a successful run, in both modes, the test requires: no exception is left pending when Show returns
// (the game's own next JNI call would be undefined, and abort under CheckJNI), no JNI call is made while one is
// pending, the result is not "requested", and an attach is matched by a detach.
//
// Built and run by `just test-quest-data-net` (the NDK's jni.h is copied into the build tree for it).

#include <jni.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "android_glue_layout.h"
#include "toast_presenter.h"

namespace {

int g_failures = 0;
#define TCHECK(cond)                                                                    \
  do {                                                                                  \
    if (!(cond)) {                                                                      \
      std::fprintf(stderr, "toast_presenter_test: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                     \
    }                                                                                   \
  } while (false)

enum class Mode { kNone, kNullPending, kNonNullPending };

struct Fake {
  int calls = 0;           // JNI calls that can fail (everything but the Exception* and frame calls)
  int failAt = 0;          // the call that throws (1-based); 0: none
  Mode mode = Mode::kNone;
  bool pending = false;
  int calledWhilePending = 0;
  int describes = 0;
  int attaches = 0;
  int detaches = 0;
  bool startDetached = false;
  bool attachedNow = false;
};
Fake g;

// A distinct non-null token per call kind.
jobject Tok() { return reinterpret_cast<jobject>(0x1000); }

// The core of every failable call: count, maybe throw, and say whether to return null.
template <typename T>
T Failable(T nonNull) {
  if (g.pending) ++g.calledWhilePending;
  ++g.calls;
  if (g.calls == g.failAt) {
    g.pending = true;
    return g.mode == Mode::kNullPending ? T{} : nonNull;
  }
  return nonNull;
}

jclass FindClass(JNIEnv*, const char*) { return static_cast<jclass>(Failable(Tok())); }
jmethodID GetMethodID(JNIEnv*, jclass, const char*, const char*) {
  return Failable(reinterpret_cast<jmethodID>(0x2000));
}
jobject CallStaticObjectMethodV(JNIEnv*, jclass, jmethodID, va_list) { return Failable(Tok()); }
void CallStaticVoidMethodV(JNIEnv*, jclass, jmethodID, va_list) { Failable(Tok()); }
void CallVoidMethodV(JNIEnv*, jobject, jmethodID, va_list) { Failable(Tok()); }
jstring NewStringUTF(JNIEnv*, const char*) { return static_cast<jstring>(Failable(Tok())); }
jboolean ExceptionCheck(JNIEnv*) { return g.pending ? JNI_TRUE : JNI_FALSE; }
void ExceptionDescribe(JNIEnv*) {
  ++g.describes;
  g.pending = false;  // as the real one: it prints and clears
}
void ExceptionClear(JNIEnv*) { g.pending = false; }
jint PushLocalFrame(JNIEnv*, jint) { return 0; }
jobject PopLocalFrame(JNIEnv*, jobject) { return nullptr; }

JNINativeInterface g_envTable;
JNIEnv g_env;
JNIInvokeInterface g_vmTable;
JavaVM g_vm;

jint GetEnv(JavaVM*, void** env, jint) {
  if (g.startDetached && !g.attachedNow) return JNI_EDETACHED;
  *env = &g_env;
  return JNI_OK;
}
jint AttachCurrentThread(JavaVM*, JNIEnv** env, void*) {
  ++g.attaches;
  g.attachedNow = true;
  *env = &g_env;
  return JNI_OK;
}
jint DetachCurrentThread(JavaVM*) {
  ++g.detaches;
  g.attachedNow = false;
  return JNI_OK;
}

unsigned char g_app[0x100];
unsigned char g_activity[0x40];

void SetUpTables() {
  std::memset(&g_envTable, 0, sizeof(g_envTable));
  g_envTable.FindClass = FindClass;
  g_envTable.GetStaticMethodID = GetMethodID;
  g_envTable.GetMethodID = GetMethodID;
  g_envTable.CallStaticObjectMethodV = CallStaticObjectMethodV;
  g_envTable.CallStaticVoidMethodV = CallStaticVoidMethodV;
  g_envTable.CallVoidMethodV = CallVoidMethodV;
  g_envTable.NewStringUTF = NewStringUTF;
  g_envTable.ExceptionCheck = ExceptionCheck;
  g_envTable.ExceptionDescribe = ExceptionDescribe;
  g_envTable.ExceptionClear = ExceptionClear;
  g_envTable.PushLocalFrame = PushLocalFrame;
  g_envTable.PopLocalFrame = PopLocalFrame;
  g_env.functions = &g_envTable;
  std::memset(&g_vmTable, 0, sizeof(g_vmTable));
  g_vmTable.GetEnv = GetEnv;
  g_vmTable.AttachCurrentThread = AttachCurrentThread;
  g_vmTable.DetachCurrentThread = DetachCurrentThread;
  g_vm.functions = &g_vmTable;
  JavaVM* vm = &g_vm;
  jobject clazz = reinterpret_cast<jobject>(0x3000);
  void* activity = g_activity;
  std::memcpy(g_activity + sentinel::glue::kActivityVm, &vm, sizeof(vm));
  std::memcpy(g_activity + sentinel::glue::kActivityClazz, &clazz, sizeof(clazz));
  std::memcpy(g_app + sentinel::glue::kAppActivity, &activity, sizeof(activity));
}

const char* Run(int failAt, Mode mode, bool startDetached) {
  g = Fake{};
  g.failAt = failAt;
  g.mode = mode;
  g.startDetached = startDetached;
  bool prepared = false;
  return nevr_quest::toast::Show(g_app, "hello", &prepared);
}

}  // namespace

int main() {
  SetUpTables();

  // A clean run: every call succeeds. `myLooper` returns a token, so no Looper.prepare is made.
  const char* ok = Run(0, Mode::kNone, false);
  TCHECK(std::strcmp(ok, "requested") == 0);
  TCHECK(!g.pending && g.calledWhilePending == 0);
  const int total = g.calls;
  TCHECK(total >= 8);

  for (const bool detached : {false, true}) {
    for (const Mode mode : {Mode::kNullPending, Mode::kNonNullPending}) {
      for (int at = 1; at <= total; ++at) {
        const char* result = Run(at, mode, detached);
        if (std::strcmp(result, "requested") == 0) {
          std::fprintf(stderr, "toast_presenter_test: call %d (mode %d) was reported as requested\n", at,
                       static_cast<int>(mode));
          ++g_failures;
        }
        if (g.pending) {
          std::fprintf(stderr, "toast_presenter_test: exception left pending after call %d (mode %d, detached %d)\n", at,
                       static_cast<int>(mode), detached ? 1 : 0);
          ++g_failures;
        }
        if (g.calledWhilePending != 0) {
          std::fprintf(stderr, "toast_presenter_test: a JNI call ran with an exception pending after call %d\n", at);
          ++g_failures;
        }
        if (g.attaches != g.detaches) {
          std::fprintf(stderr, "toast_presenter_test: attach %d detach %d at call %d\n", g.attaches, g.detaches, at);
          ++g_failures;
        }
      }
    }
  }

  // A detached thread is attached for the call and detached after it, on success too.
  Run(0, Mode::kNone, true);
  TCHECK(g.attaches == 1 && g.detaches == 1);
  // An attached thread is never detached.
  Run(0, Mode::kNone, false);
  TCHECK(g.attaches == 0 && g.detaches == 0);

  // The caller's own pending exception: no call is made and it is not cleared.
  g = Fake{};
  g.pending = true;
  bool prepared = false;
  const char* held = nevr_quest::toast::Show(g_app, "hello", &prepared);
  TCHECK(std::strcmp(held, "exception_pending") == 0);
  TCHECK(g.pending && g.calls == 0);

  // The glue reads that fail leave nothing to clean up.
  TCHECK(std::strcmp(nevr_quest::toast::Show(nullptr, "x", &prepared), "no_app") == 0);

  if (g_failures != 0) {
    std::fprintf(stderr, "toast_presenter_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("toast_presenter_test: all checks pass (%d failable JNI calls, each thrown at in 4 variants)\n", total);
  return 0;
}

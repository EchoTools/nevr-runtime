// Host test for the permission request (src/quest/sentinel/permission_request.cpp, #947) against a fake JNI. The fake
// records the Java call the request makes: the method's name and signature, the object it is called on, the String[]
// it builds (each name, in order) and the int the variadic call carries, read with va_arg as the runtime reads it. It
// also, at one chosen JNI call, makes that call throw (a Java exception pending) while returning null or a non-null
// value: for every call, in both modes, the request must not report "requested", must leave no exception pending, must
// make no JNI call while one is pending, and must match an attach with a detach.
//
// Built and run by `just test-quest-hooks` (the NDK's jni.h is copied into the build tree for it).

#include <jni.h>

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "android_glue_layout.h"
#include "permission_request.h"

namespace {

int g_failures = 0;
#define PCHECK(cond)                                                                           \
  do {                                                                                         \
    if (!(cond)) {                                                                             \
      std::fprintf(stderr, "permission_request_test: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                                            \
    }                                                                                          \
  } while (false)

enum class Mode { kNone, kNullPending, kNonNullPending };

constexpr jint kNoCode = -12345;  // what `code` holds until the call is made

struct Fake {
  int calls = 0;  // JNI calls that can fail (everything but the Exception* and frame calls)
  int failAt = 0;
  Mode mode = Mode::kNone;
  bool pending = false;
  int calledWhilePending = 0;
  int attaches = 0;
  int detaches = 0;
  bool startDetached = false;
  bool attachedNow = false;
  int frames = 0;  // PushLocalFrame minus PopLocalFrame
  std::vector<std::string> classes;  // FindClass names
  std::string method;                // "name sig" of GetMethodID
  jobject calledOn = nullptr;
  jobject arrayPassed = nullptr;
  jint code = kNoCode;
  int voidCalls = 0;
  jsize arrayLength = -1;
  std::vector<std::string> strings;  // NewStringUTF texts, in order
  std::vector<std::string> elements; // what SetObjectArrayElement put at each index
  int liveStrings = 0;               // name strings made and not yet deleted
  int maxLiveStrings = 0;
};
Fake g;

jobject Tok(std::uintptr_t v) { return reinterpret_cast<jobject>(v); }
const jobject kActivity = Tok(0x3000);
const jobject kArray = Tok(0x5000);
const jobject kActivityClass = Tok(0x6000);

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

jclass FindClass(JNIEnv*, const char* name) {
  g.classes.push_back(name);
  return static_cast<jclass>(Failable(Tok(0x1000)));
}
jobjectArray NewObjectArray(JNIEnv*, jsize length, jclass, jobject) {
  g.arrayLength = length;
  g.elements.assign(length > 0 ? static_cast<std::size_t>(length) : 0, std::string());
  return static_cast<jobjectArray>(Failable(kArray));
}
jstring NewStringUTF(JNIEnv*, const char* text) {
  g.strings.push_back(text);
  if (++g.liveStrings > g.maxLiveStrings) g.maxLiveStrings = g.liveStrings;
  // Each string's token is its index + 0x7000, so SetObjectArrayElement can say which name went where.
  return static_cast<jstring>(Failable(Tok(0x7000 + g.strings.size() - 1)));
}
void SetObjectArrayElement(JNIEnv*, jobjectArray array, jsize index, jobject value) {
  Failable(Tok(1));
  if (array != kArray || index < 0 || index >= g.arrayLength) {
    ++g_failures;
    std::fprintf(stderr, "permission_request_test: SetObjectArrayElement out of the array: index %d\n", index);
    return;
  }
  const std::uintptr_t token = reinterpret_cast<std::uintptr_t>(value);
  g.elements[static_cast<std::size_t>(index)] =
      token >= 0x7000 && token - 0x7000 < g.strings.size() ? g.strings[token - 0x7000] : std::string("?");
}
void DeleteLocalRef(JNIEnv*, jobject ref) {
  const std::uintptr_t token = reinterpret_cast<std::uintptr_t>(ref);
  if (token >= 0x7000 && token - 0x7000 < g.strings.size()) --g.liveStrings;
}
jclass GetObjectClass(JNIEnv*, jobject object) {
  PCHECK(object == kActivity);
  return static_cast<jclass>(Failable(kActivityClass));
}
jmethodID GetMethodID(JNIEnv*, jclass clazz, const char* name, const char* sig) {
  PCHECK(clazz == kActivityClass);
  g.method = std::string(name) + " " + sig;
  return Failable(reinterpret_cast<jmethodID>(0x2000));
}
// The runtime's CallVoidMethod reads its variadic arguments by the method's signature, ([Ljava/lang/String;I)V: an
// object, then an int. Read them the same way.
void CallVoidMethodV(JNIEnv*, jobject object, jmethodID, va_list args) {
  ++g.voidCalls;
  g.calledOn = object;
  g.arrayPassed = va_arg(args, jobject);
  g.code = va_arg(args, jint);
  Failable(Tok(1));
}
jboolean ExceptionCheck(JNIEnv*) { return g.pending ? JNI_TRUE : JNI_FALSE; }
void ExceptionDescribe(JNIEnv*) { g.pending = false; }
void ExceptionClear(JNIEnv*) { g.pending = false; }
jint PushLocalFrame(JNIEnv*, jint capacity) {
  PCHECK(capacity >= 4);
  ++g.frames;
  return 0;
}
jobject PopLocalFrame(JNIEnv*, jobject) {
  --g.frames;
  return nullptr;
}

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
  g_envTable.NewObjectArray = NewObjectArray;
  g_envTable.NewStringUTF = NewStringUTF;
  g_envTable.SetObjectArrayElement = SetObjectArrayElement;
  g_envTable.DeleteLocalRef = DeleteLocalRef;
  g_envTable.GetObjectClass = GetObjectClass;
  g_envTable.GetMethodID = GetMethodID;
  g_envTable.CallVoidMethodV = CallVoidMethodV;
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
  jobject clazz = kActivity;
  void* activity = g_activity;
  std::memcpy(g_activity + sentinel::glue::kActivityVm, &vm, sizeof(vm));
  std::memcpy(g_activity + sentinel::glue::kActivityClazz, &clazz, sizeof(clazz));
  std::memcpy(g_app + sentinel::glue::kAppActivity, &activity, sizeof(activity));
}

const char* const kBoot[3] = {"android.permission.RECORD_AUDIO", "android.permission.READ_EXTERNAL_STORAGE",
                              "android.permission.WRITE_EXTERNAL_STORAGE"};

const char* Run(int failAt, Mode mode, bool startDetached, int code = 1) {
  g = Fake{};
  g.failAt = failAt;
  g.mode = mode;
  g.startDetached = startDetached;
  return nevr_quest::permission_request::Request(g_app, kBoot, 3, code);
}

bool Is(const char* got, const char* want) { return std::strcmp(got, want) == 0; }

// The call the game means to make, with the code the caller chose.
void TestTheCallCarriesTheArrayAndTheCode() {
  PCHECK(Is(Run(0, Mode::kNone, false), "requested"));
  PCHECK(g.method == "requestPermissions ([Ljava/lang/String;I)V");
  PCHECK(g.calledOn == kActivity);
  PCHECK(g.voidCalls == 1);
  PCHECK(g.arrayPassed == kArray);
  PCHECK(g.code == 1);
  PCHECK(g.arrayLength == 3);
  PCHECK(g.elements.size() == 3 && g.elements[0] == kBoot[0] && g.elements[1] == kBoot[1] &&
         g.elements[2] == kBoot[2]);
  PCHECK(g.classes.size() == 1 && g.classes[0] == "java/lang/String");
  PCHECK(g.maxLiveStrings == 1);  // each name is let go once it is in the array: the frame never fills
  PCHECK(g.frames == 0 && !g.pending && g.calledWhilePending == 0);
  // Another code is passed as given.
  PCHECK(Is(Run(0, Mode::kNone, false, 7), "requested"));
  PCHECK(g.code == 7);
}

// A thrown exception at every failable call, null or not, on an attached and on a detached thread.
void TestEveryFailureIsClearedAndReported() {
  Run(0, Mode::kNone, false);
  const int total = g.calls;
  PCHECK(total >= 9);  // FindClass, NewObjectArray, 3 x (NewStringUTF, SetObjectArrayElement), GetObjectClass,
                       // GetMethodID, CallVoidMethod
  for (const bool detached : {false, true}) {
    for (const Mode mode : {Mode::kNullPending, Mode::kNonNullPending}) {
      for (int at = 1; at <= total; ++at) {
        const char* result = Run(at, mode, detached);
        if (Is(result, "requested") || g.pending || g.calledWhilePending != 0 || g.attaches != g.detaches ||
            g.frames != 0) {
          std::fprintf(stderr,
                       "permission_request_test: call %d mode %d detached %d: result %s pending %d while_pending %d "
                       "attach %d detach %d frames %d\n",
                       at, static_cast<int>(mode), detached ? 1 : 0, result, g.pending ? 1 : 0, g.calledWhilePending,
                       g.attaches, g.detaches, g.frames);
          ++g_failures;
        }
        // Once a call failed, the request itself is never made.
        if (at < total && g.voidCalls != 0) {
          std::fprintf(stderr, "permission_request_test: call %d failed and requestPermissions still ran\n", at);
          ++g_failures;
        }
      }
    }
  }
  PCHECK(Is(Run(total, Mode::kNonNullPending, false), "call_failed"));
}

void TestAttachAndTheCallersException() {
  Run(0, Mode::kNone, true);
  PCHECK(g.attaches == 1 && g.detaches == 1);
  Run(0, Mode::kNone, false);
  PCHECK(g.attaches == 0 && g.detaches == 0);
  g = Fake{};
  g.pending = true;
  PCHECK(Is(nevr_quest::permission_request::Request(g_app, kBoot, 3, 1), "exception_pending"));
  PCHECK(g.pending && g.calls == 0 && g.voidCalls == 0);
}

void TestNothingToAskOrNoActivity() {
  g = Fake{};
  using nevr_quest::permission_request::Request;
  PCHECK(Is(Request(g_app, nullptr, 3, 1), "no_names"));
  PCHECK(Is(Request(g_app, kBoot, 0, 1), "no_names"));
  PCHECK(Is(Request(g_app, kBoot, 17, 1), "no_names"));
  const char* const withNull[2] = {kBoot[0], nullptr};
  PCHECK(Is(Request(g_app, withNull, 2, 1), "no_names"));
  PCHECK(Is(Request(nullptr, kBoot, 3, 1), "no_app"));
  unsigned char emptyApp[0x100] = {};
  PCHECK(Is(Request(emptyApp, kBoot, 3, 1), "no_activity"));
  // An activity with no VM, and one with no Java object.
  unsigned char activity[0x40] = {};
  unsigned char app[0x100] = {};
  void* activityPtr = activity;
  std::memcpy(app + sentinel::glue::kAppActivity, &activityPtr, sizeof(activityPtr));
  PCHECK(Is(Request(app, kBoot, 3, 1), "no_vm"));
  JavaVM* vm = &g_vm;
  std::memcpy(activity + sentinel::glue::kActivityVm, &vm, sizeof(vm));
  PCHECK(Is(Request(app, kBoot, 3, 1), "no_activity"));
  PCHECK(g.calls == 0 && g.voidCalls == 0);
}

}  // namespace

int main() {
  SetUpTables();
  TestTheCallCarriesTheArrayAndTheCode();
  TestEveryFailureIsClearedAndReported();
  TestAttachAndTheCallersException();
  TestNothingToAskOrNoActivity();
  if (g_failures != 0) {
    std::fprintf(stderr, "permission_request_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("permission_request_test: OK\n");
  return 0;
}

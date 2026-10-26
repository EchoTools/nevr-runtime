// Host test for the toast presenter (src/quest/sentinel/toast_presenter.cpp) against a fake JNI. The fake counts every
// JNI call and, at one chosen call, makes it throw (a Java exception pending) while returning null or a non-null value.
// For every call of a successful run, in both modes, the test requires: no exception is left pending when Show returns
// (the game's own next JNI call would be undefined, and abort under CheckJNI), no JNI call is made while one is
// pending, the result is not "requested", and an attach is matched by a detach.
//
// Built and run by `just test-quest-data-net` (the NDK's jni.h is copied into the build tree for it).

#include <jni.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "android_glue_layout.h"
#include "hook_log.h"
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
  std::vector<std::string> methods;  // "name sig" of every GetStaticMethodID / GetMethodID, in order
  const char* absentMethod = nullptr;  // a method name the fake VM does not have: NoSuchMethodError, null
  bool mainLooperNull = false;         // Looper.getMainLooper() returns null
  std::thread::id jniThread;           // the thread of the last FindClass
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

jclass FindClass(JNIEnv*, const char*) {
  g.jniThread = std::this_thread::get_id();
  return static_cast<jclass>(Failable(Tok()));
}
jmethodID GetMethodID(JNIEnv*, jclass, const char* name, const char* sig) {
  g.methods.push_back(std::string(name) + " " + sig);
  if (g.absentMethod != nullptr && std::strcmp(name, g.absentMethod) == 0 && g.failAt == 0) {
    if (g.pending) ++g.calledWhilePending;
    g.pending = true;  // NoSuchMethodError
    return nullptr;
  }
  return Failable(reinterpret_cast<jmethodID>(0x2000));
}
jobject CallStaticObjectMethodV(JNIEnv*, jclass, jmethodID, va_list) {
  if (g.mainLooperNull && !g.methods.empty() && g.methods.back().rfind("getMainLooper", 0) == 0) {
    ++g.calls;
    return nullptr;
  }
  return Failable(Tok());
}
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

using ShowFn = const char* (*)(void*, const char*, bool*) noexcept;
ShowFn g_fn = &nevr_quest::toast::Show;

const char* Run(int failAt, Mode mode, bool startDetached) {
  g = Fake{};
  g.failAt = failAt;
  g.mode = mode;
  g.startDetached = startDetached;
  bool prepared = false;
  return g_fn(g_app, "hello", &prepared);
}

// Everything the first test run checks, for one presenter: a clean run, then a thrown exception at every failable call.
void CheckPresenter(const char* name, ShowFn fn, int minCalls) {
  g_fn = fn;
  const char* ok = Run(0, Mode::kNone, false);
  if (std::strcmp(ok, "requested") != 0) {
    std::fprintf(stderr, "toast_presenter_test: %s: a clean run returned %s\n", name, ok);
    ++g_failures;
  }
  TCHECK(!g.pending && g.calledWhilePending == 0);
  const int total = g.calls;
  if (total < minCalls) {
    std::fprintf(stderr, "toast_presenter_test: %s: only %d failable calls\n", name, total);
    ++g_failures;
  }
  for (const bool detached : {false, true}) {
    for (const Mode mode : {Mode::kNullPending, Mode::kNonNullPending}) {
      for (int at = 1; at <= total; ++at) {
        const char* result = Run(at, mode, detached);
        if (std::strcmp(result, "requested") == 0) {
          std::fprintf(stderr, "toast_presenter_test: %s: call %d (mode %d) was reported as requested\n", name, at,
                       static_cast<int>(mode));
          ++g_failures;
        }
        if (g.pending) {
          std::fprintf(stderr, "toast_presenter_test: %s: exception left pending after call %d (mode %d, detached %d)\n",
                       name, at, static_cast<int>(mode), detached ? 1 : 0);
          ++g_failures;
        }
        if (g.calledWhilePending != 0) {
          std::fprintf(stderr, "toast_presenter_test: %s: a JNI call ran with an exception pending after call %d\n", name,
                       at);
          ++g_failures;
        }
        if (g.attaches != g.detaches) {
          std::fprintf(stderr, "toast_presenter_test: %s: attach %d detach %d at call %d\n", name, g.attaches, g.detaches,
                       at);
          ++g_failures;
        }
      }
    }
  }
  // A detached thread is attached for the call and detached after it, on success too; an attached one is never detached.
  Run(0, Mode::kNone, true);
  TCHECK(g.attaches == 1 && g.detaches == 1);
  Run(0, Mode::kNone, false);
  TCHECK(g.attaches == 0 && g.detaches == 0);
  // The caller's own pending exception: no call is made and it is not cleared.
  g = Fake{};
  g.pending = true;
  bool prepared = false;
  TCHECK(std::strcmp(fn(g_app, "hello", &prepared), "exception_pending") == 0);
  TCHECK(g.pending && g.calls == 0);
  TCHECK(std::strcmp(fn(nullptr, "x", &prepared), "no_app") == 0);
}

// ---- The UI-thread post (ShowOnUiThread) against a fake looper --------------------------------------------------------

struct FakeLooper {
  int forThreadCalls = 0;
  int acquires = 0;
  int addFds = 0;
  bool hasLooper = true;
  int addFdResult = 1;
  int fd = -1;
  int (*callback)(int, int, void*) = nullptr;
} g_looper;

void* FakeForThread() {
  ++g_looper.forThreadCalls;
  return g_looper.hasLooper ? reinterpret_cast<void*>(0x4000) : nullptr;
}
void FakeAcquire(void*) { ++g_looper.acquires; }
int FakeAddFd(void*, int fd, int, int events, int (*callback)(int, int, void*), void*) {
  ++g_looper.addFds;
  TCHECK(events == 1);  // ALOOPER_EVENT_INPUT
  g_looper.fd = fd;
  g_looper.callback = callback;
  return g_looper.addFdResult;
}

nevr_quest::toast::LooperApi FakeApi() {
  nevr_quest::toast::LooperApi api;
  api.forThread = &FakeForThread;
  api.acquire = &FakeAcquire;
  api.addFd = &FakeAddFd;
  return api;
}

std::vector<std::string> g_lines;
void Sink(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

// The fake UI thread: waits for the pipe to be readable, then runs the looper's callback, as the real looper does.
void RunLooperOnce() {
  pollfd p{g_looper.fd, POLLIN, 0};
  TCHECK(::poll(&p, 1, 2000) == 1);
  TCHECK(g_looper.callback(g_looper.fd, 1, nullptr) == 1);
}

void CheckUiPost() {
  namespace toast = nevr_quest::toast;
  bool prepared = true;
  sentinel::LogSink previous = sentinel::SetLogSink(&Sink);

  // Before any capture nothing is queued.
  toast::ResetUiLooperForTest();
  TCHECK(std::strcmp(toast::ShowOnUiThread(g_app, "hello", &prepared), "no_ui_looper") == 0 && !prepared);
  // A host has no libandroid.so: the real capture says so and leaves the state untried.
  TCHECK(std::strcmp(toast::CaptureUiLooper(), "no_libandroid") == 0);
  // A looper API with a missing function.
  toast::LooperApi missing = FakeApi();
  missing.addFd = nullptr;
  TCHECK(std::strcmp(toast::CaptureUiLooperWith(missing), "no_looper_api") == 0);

  // The thread has no looper (the constructor did not run on the UI thread): the token says so, and nothing can be posted.
  g_looper = FakeLooper{};
  g_looper.hasLooper = false;
  TCHECK(std::strcmp(toast::CaptureUiLooperWith(FakeApi()), "no_looper_on_this_thread") == 0);
  TCHECK(g_looper.acquires == 0 && g_looper.addFds == 0);
  TCHECK(std::strcmp(toast::ShowOnUiThread(g_app, "hello", &prepared), "no_ui_looper") == 0);

  // The looper refuses the fd: no capture, the pipe is closed (a post finds no looper).
  toast::ResetUiLooperForTest();
  g_looper = FakeLooper{};
  g_looper.addFdResult = -1;
  TCHECK(std::strcmp(toast::CaptureUiLooperWith(FakeApi()), "add_fd_failed") == 0);
  TCHECK(std::strcmp(toast::ShowOnUiThread(g_app, "hello", &prepared), "no_ui_looper") == 0);
  TCHECK(::fcntl(g_looper.fd, F_GETFD) == -1);  // closed

  // A good capture: the looper is acquired and polled for input once; a second capture does nothing. It logs one line.
  toast::ResetUiLooperForTest();
  g_looper = FakeLooper{};
  g_lines.clear();
  TCHECK(std::strcmp(toast::CaptureUiLooperWith(FakeApi()), "captured") == 0);
  TCHECK(g_lines.size() == 1 && g_lines[0].find("\"event\":\"ui_post\"") != std::string::npos &&
         g_lines[0].find("\"state\":\"capture\"") != std::string::npos &&
         g_lines[0].find("\"result\":\"captured\"") != std::string::npos &&
         g_lines[0].find("\"level\":\"info\"") != std::string::npos);
  TCHECK(g_looper.forThreadCalls == 1 && g_looper.acquires == 1 && g_looper.addFds == 1 && g_looper.fd >= 0);
  TCHECK(std::strcmp(toast::CaptureUiLooperWith(FakeApi()), "already_captured") == 0);
  TCHECK(g_looper.addFds == 1);

  // The text is checked before it is queued.
  TCHECK(std::strcmp(toast::ShowOnUiThread(g_app, std::string(401, 'x').c_str(), &prepared), "text_too_long") == 0);
  TCHECK(std::strcmp(toast::ShowOnUiThread(g_app, nullptr, &prepared), "no_text") == 0);
  TCHECK(toast::UiQueuedForTest() == 0);
  // 400 bytes is the longest text; it is queued whole and shown.
  g = Fake{};
  g_lines.clear();
  TCHECK(std::strcmp(toast::ShowOnUiThread(g_app, std::string(400, 'x').c_str(), &prepared), "posted") == 0);
  TCHECK(toast::UiQueuedForTest() == 1);
  std::thread longest(&RunLooperOnce);
  longest.join();
  TCHECK(toast::UiQueuedForTest() == 0 && g_lines.size() == 1);

  // A post from this thread runs the Toast on the looper's thread, never here, and logs what Show answered.
  g = Fake{};
  g_lines.clear();
  prepared = true;
  const std::thread::id poster = std::this_thread::get_id();
  TCHECK(std::strcmp(toast::ShowOnUiThread(g_app, "hello", &prepared), "posted") == 0);
  TCHECK(!prepared);
  TCHECK(g.calls == 0);  // nothing is made on the posting thread
  std::thread looperThread(&RunLooperOnce);
  looperThread.join();
  TCHECK(g.calls > 0 && !g.pending && g.calledWhilePending == 0);
  TCHECK(g.jniThread != poster && g.jniThread != std::thread::id{});
  bool sawMyLooper = false, sawPrepare = false, sawLooperOverload = false;
  for (const std::string& m : g.methods) {
    if (m.rfind("myLooper", 0) == 0) sawMyLooper = true;
    if (m.rfind("prepare", 0) == 0) sawPrepare = true;
    if (m.find("Landroid/os/Looper;Ljava/lang/CharSequence;") != std::string::npos) sawLooperOverload = true;
  }
  TCHECK(sawMyLooper && !sawPrepare && !sawLooperOverload);  // the plain overload on the looper's own (running) thread
  TCHECK(g_lines.size() == 1);
  if (g_lines.size() == 1) {
    TCHECK(g_lines[0].find("\"event\":\"ui_post\"") != std::string::npos);
    TCHECK(g_lines[0].find("\"result\":\"requested\"") != std::string::npos);
    TCHECK(g_lines[0].find("\"state\":\"ran\"") != std::string::npos);
  }

  // A failure on the UI thread is a warning with the token, and the exception is not left pending.
  g = Fake{};
  g.absentMethod = "show";
  g_lines.clear();
  TCHECK(std::strcmp(toast::ShowOnUiThread(g_app, "again", &prepared), "posted") == 0);
  std::thread second(&RunLooperOnce);
  second.join();
  TCHECK(!g.pending);
  TCHECK(g_lines.size() == 1 && g_lines[0].find("\"level\":\"warn\"") != std::string::npos &&
         g_lines[0].find("\"result\":\"show_failed\"") != std::string::npos);

  // Several posts before the looper runs are all shown, once each.
  g = Fake{};
  g_lines.clear();
  for (int i = 0; i < 3; ++i) TCHECK(std::strcmp(toast::ShowOnUiThread(g_app, "many", &prepared), "posted") == 0);
  std::thread third(&RunLooperOnce);
  third.join();
  TCHECK(g_lines.size() == 3);

  // A full pipe is a token, not a block or a leak: fill it, see queue_full, then drain it (every queued text is freed).
  g = Fake{};
  g_lines.clear();
  int posted = 0;
  const char* last = "posted";
  while (posted < 100000 && std::strcmp(last, "posted") == 0) {
    last = toast::ShowOnUiThread(g_app, "fill", &prepared);
    if (std::strcmp(last, "posted") == 0) ++posted;
  }
  TCHECK(std::strcmp(last, "queue_full") == 0 && posted > 100);
  TCHECK(toast::UiQueuedForTest() == posted);  // the refused one was freed at once
  std::thread drain(&RunLooperOnce);
  drain.join();
  TCHECK(static_cast<int>(g_lines.size()) == posted);
  TCHECK(toast::UiQueuedForTest() == 0);  // every queued text was shown or refused, and freed

  // The hangup of the write end unregisters the fd (the callback answers 0).
  toast::ResetUiLooperForTest();
  TCHECK(g_looper.callback(-1, 8, nullptr) == 0);  // ALOOPER_EVENT_HANGUP: returns before it reads
  sentinel::SetLogSink(previous);
}

}  // namespace

int main() {
  SetUpTables();

  // The thread's own looper (Show): a clean run, then an exception at every failable call. `myLooper` returns a token,
  // so no Looper.prepare is made.
  CheckPresenter("Show", &nevr_quest::toast::Show, 8);

  // The main looper (ShowOnMainLooper, #829).
  CheckPresenter("ShowOnMainLooper", &nevr_quest::toast::ShowOnMainLooper, 7);
  {
    // Which methods it asks for: the main looper and the makeText overload that takes a looper; never myLooper or prepare
    // (a looper made on android_main would be one nobody runs).
    g = Fake{};
    bool prepared = true;
    TCHECK(std::strcmp(nevr_quest::toast::ShowOnMainLooper(g_app, "hello", &prepared), "requested") == 0);
    TCHECK(!prepared);  // reset to false, and never set
    bool sawMain = false, sawLooperOverload = false, sawPlain = false, sawMyLooper = false, sawPrepare = false;
    for (const std::string& m : g.methods) {
      if (m == "getMainLooper ()Landroid/os/Looper;") sawMain = true;
      if (m == "makeText (Landroid/content/Context;Landroid/os/Looper;Ljava/lang/CharSequence;I)Landroid/widget/Toast;") {
        sawLooperOverload = true;
      }
      if (m == "makeText (Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;") sawPlain = true;
      if (m.rfind("myLooper", 0) == 0) sawMyLooper = true;
      if (m.rfind("prepare", 0) == 0) sawPrepare = true;
    }
    TCHECK(sawMain && sawLooperOverload && !sawPlain && !sawMyLooper && !sawPrepare);
    // A device without the (hidden) overload, or one whose hidden-API policy refuses it: a token that says so, nothing pending,
    // nothing shown.
    g = Fake{};
    g.absentMethod = "makeText";
    prepared = false;
    TCHECK(std::strcmp(nevr_quest::toast::ShowOnMainLooper(g_app, "hello", &prepared), "looper_overload_unavailable") == 0);
    TCHECK(!g.pending && g.calledWhilePending == 0);
    // No main looper: not a toast on some other thread.
    g = Fake{};
    g.mainLooperNull = true;
    TCHECK(std::strcmp(nevr_quest::toast::ShowOnMainLooper(g_app, "hello", &prepared), "looper_failed") == 0);
    TCHECK(!g.pending);
  }
  // Show's old path is unchanged: myLooper, and the plain overload.
  {
    g = Fake{};
    bool prepared = false;
    TCHECK(std::strcmp(nevr_quest::toast::Show(g_app, "hello", &prepared), "requested") == 0);
    bool sawPlain = false, sawLooperOverload = false;
    for (const std::string& m : g.methods) {
      if (m == "makeText (Landroid/content/Context;Ljava/lang/CharSequence;I)Landroid/widget/Toast;") sawPlain = true;
      if (m.find("Landroid/os/Looper;Ljava/lang/CharSequence;") != std::string::npos) sawLooperOverload = true;
    }
    TCHECK(sawPlain && !sawLooperOverload);
  }

  CheckUiPost();

  if (g_failures != 0) {
    std::fprintf(stderr, "toast_presenter_test: %d check(s) failed\n", g_failures);
    return 1;
  }
  std::printf("toast_presenter_test: all checks pass (the presenters, an exception thrown at each failable JNI call in 4 variants, the UI-thread post)\n");
  return 0;
}

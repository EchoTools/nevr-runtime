#include "toast_presenter.h"

#include <jni.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <new>

#include "android_glue_layout.h"
#include "hook_log.h"

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
// duration), the overload that takes the looper, so the handler runs on the UI thread's running looper. That overload is
// `@hide` (android-12.0.0_r1 Toast.java: "Make a standard toast to display using the specified looper ... @hide"), not API
// 30 public API: the runtime's hidden-API policy may refuse the lookup (NoSuchMethodError from GetStaticMethodID), and
// that is the "looper_overload_unavailable" token below. Whether it does on the headset is not known.
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
    if (Pending(env) || makeText == nullptr) return "looper_overload_unavailable";  // absent or refused by the hidden-API policy: NoSuchMethodError, cleared
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

// ---- A call on the UI thread without a Java class (candidate A of #829) --------------------------------------------------
//
// A Runnable is a Java object and the sentinel ships no dex, so nothing can be posted to the UI thread through JNI. The UI
// thread's native Looper can: ALooper_forThread() taken ON the UI thread (the ELF constructor runs there when the activity's
// own thread loads the library; the capture says whether it did), and a pipe whose read end that looper polls. A pointer
// written to the pipe is read by the looper's callback on the UI thread, which makes the Toast there, where Looper.myLooper()
// is the running main looper. The NDK's ALooper functions are reached by dlsym from libandroid.so (the sentinel's DT_NEEDED list
// is pinned).

constexpr int kLooperEventInput = 1;    // ALOOPER_EVENT_INPUT
constexpr int kLooperEventError = 4;    // ALOOPER_EVENT_ERROR
constexpr int kLooperEventHangup = 8;   // ALOOPER_EVENT_HANGUP
constexpr std::size_t kMaxPostedText = 400;

struct Posted {
  void* app;
  char text[kMaxPostedText + 1];
};

enum UiState : int { kUiUntried = 0, kUiCapturing = 1, kUiReady = 2, kUiFailed = 3 };
std::atomic<int> g_uiState{kUiUntried};
std::atomic<int> g_uiReadFd{-1};
std::atomic<int> g_uiWriteFd{-1};
std::atomic<int> g_uiQueued{0};  // texts allocated and not yet shown or refused

long long ThreadId() noexcept { return static_cast<long long>(::syscall(SYS_gettid)); }

// Runs on the UI thread, from the looper's poll. Reads every pointer the pipe holds and shows each.
int UiCallback(int fd, int events, void* /*data*/) noexcept {
  if ((events & (kLooperEventError | kLooperEventHangup)) != 0) return 0;  // unregister
  for (;;) {
    Posted* posted = nullptr;
    const ssize_t got = ::read(fd, &posted, sizeof(posted));
    if (got != static_cast<ssize_t>(sizeof(posted)) || posted == nullptr) break;
    bool prepared = false;
    const char* result = ShowWhere(posted->app, posted->text, &prepared, Where::kThisThread);
    const long long tid = ThreadId();
    sentinel::LogFields(std::strcmp(result, "requested") == 0 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn,
                        "ui_post",
                        {{"state", "ran"},
                         {"result", result},
                         {"tid", tid},
                         {"main_thread", tid == static_cast<long long>(::getpid()) ? "true" : "false"},
                         {"looper_prepared", prepared ? "true" : "false"}});
    g_uiQueued.fetch_sub(1);
    delete posted;
  }
  return 1;  // keep polling
}

}  // namespace

namespace {
const char* CaptureUiLooperQuietly(const LooperApi& api) noexcept;
}  // namespace

const char* CaptureUiLooperWith(const LooperApi& api) noexcept {
  const char* const result = CaptureUiLooperQuietly(api);
  const long long tid = ThreadId();
  sentinel::LogFields(std::strcmp(result, "captured") == 0 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn,
                      "ui_post",
                      {{"state", "capture"},
                       {"result", result},
                       {"tid", tid},
                       {"main_thread", tid == static_cast<long long>(::getpid()) ? "true" : "false"}});
  return result;
}

namespace {
const char* CaptureUiLooperQuietly(const LooperApi& api) noexcept {
  if (api.forThread == nullptr || api.acquire == nullptr || api.addFd == nullptr) return "no_looper_api";
  int expected = kUiUntried;
  if (!g_uiState.compare_exchange_strong(expected, kUiCapturing)) {
    return expected == kUiReady ? "already_captured" : "capture_in_progress_or_failed";
  }
  void* const looper = api.forThread();
  if (looper == nullptr) {
    g_uiState.store(kUiFailed);
    return "no_looper_on_this_thread";
  }
  int fds[2] = {-1, -1};
  if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) {
    g_uiState.store(kUiFailed);
    return "pipe_failed";
  }
  api.acquire(looper);
  if (api.addFd(looper, fds[0], 0, kLooperEventInput, &UiCallback, nullptr) != 1) {
    ::close(fds[0]);
    ::close(fds[1]);
    g_uiState.store(kUiFailed);
    return "add_fd_failed";
  }
  g_uiReadFd.store(fds[0]);
  g_uiWriteFd.store(fds[1]);
  g_uiState.store(kUiReady);
  return "captured";
}
}  // namespace

const char* CaptureUiLooper() noexcept {
  void* const lib = ::dlopen("libandroid.so", RTLD_NOW);
  if (lib == nullptr) return "no_libandroid";
  LooperApi api;
  api.forThread = reinterpret_cast<void* (*)()>(::dlsym(lib, "ALooper_forThread"));
  api.acquire = reinterpret_cast<void (*)(void*)>(::dlsym(lib, "ALooper_acquire"));
  api.addFd = reinterpret_cast<LooperApi::AddFd>(::dlsym(lib, "ALooper_addFd"));
  return CaptureUiLooperWith(api);
}

int UiQueuedForTest() noexcept { return g_uiQueued.load(); }

void ResetUiLooperForTest() noexcept {
  const int r = g_uiReadFd.exchange(-1);
  const int w = g_uiWriteFd.exchange(-1);
  if (r >= 0) ::close(r);
  if (w >= 0) ::close(w);
  g_uiState.store(kUiUntried);
}

const char* ShowOnUiThread(void* app, const char* text, bool* looperPrepared) noexcept {
  *looperPrepared = false;
  if (g_uiState.load() != kUiReady) return "no_ui_looper";
  if (text == nullptr || std::strlen(text) > kMaxPostedText) return "text_too_long";
  Posted* posted = new (std::nothrow) Posted;
  if (posted == nullptr) return "alloc_failed";
  posted->app = app;
  std::memcpy(posted->text, text, std::strlen(text) + 1);
  g_uiQueued.fetch_add(1);
  const ssize_t put = ::write(g_uiWriteFd.load(), &posted, sizeof(posted));
  if (put != static_cast<ssize_t>(sizeof(posted))) {
    g_uiQueued.fetch_sub(1);
    delete posted;
    return errno == EAGAIN ? "queue_full" : "write_failed";
  }
  return "posted";
}

const char* Show(void* app, const char* text, bool* looperPrepared) noexcept {
  return ShowWhere(app, text, looperPrepared, Where::kThisThread);
}

const char* ShowOnMainLooper(void* app, const char* text, bool* looperPrepared) noexcept {
  return ShowWhere(app, text, looperPrepared, Where::kMainLooper);
}

}  // namespace nevr_quest::toast

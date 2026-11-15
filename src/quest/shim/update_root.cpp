// The Android side of the Quest update root and the shim's version record (update_root.h). Compiled into the sentinel
// (libovrplatformloader.so), the shim, which is the host on Quest.

#include "quest/shim/update_root.h"

#include <jni.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "generated/nevr_build_info.h"
#include "quest/auth/app_files_rules.h"
#include "quest/sentinel/android_glue_layout.h"
#include "quest/sentinel/hook_log.h"
#include "quest/integration/entry_hooks.h"
#include "quest/shim/runtime_load.h"

namespace nevr_quest::update_root {
namespace {

// The shim is the Quest host, so its records carry the host's tag (docs/standards/logging.md, NEVR.HOST).
constexpr char kHostTag[] = "NEVR.HOST";
constexpr char kVersionsEvent[] = "versions";
constexpr char kOwnCmdlinePath[] = "/proc/self/cmdline";
constexpr std::size_t kCmdlineChunkBytes = 4096;
constexpr std::size_t kCmdlineMaxBytes = 64 * 1024;
// The fields a versions record carries beyond the facts: the tag, and on a failed read the reason and its consequence.
constexpr std::size_t kVersionsExtraFields = 3;
constexpr char kApkUnreadConsequence[] = "apk_version_code stays unknown in this record";
// The local references one read of the versionCode holds at once: the package name, the package manager, the package info and
// the three classes. The frame releases them all when it pops.
constexpr jint kLocalFrameRefs = 16;
// getPackageInfo's flags: none, because the versionCode is in the base package info.
constexpr jint kNoPackageFlags = 0;

// This process's command line, NUL-separated. Empty when it cannot be read or is over the cap: the root is then empty.
// The same read as quest/auth/file_store.cpp's, which the shim does not link.
std::string OwnCmdline() {
  const int fd = ::open(kOwnCmdlinePath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::string();
  std::string out;
  char buf[kCmdlineChunkBytes];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      out.clear();
      break;
    }
    if (n == 0) break;
    out.append(buf, static_cast<std::size_t>(n));
    if (out.size() > kCmdlineMaxBytes) {
      out.clear();
      break;
    }
  }
  ::close(fd);
  return out;
}

// True when the last JNI call left a Java exception pending: it is cleared (a pending exception makes the game's own next JNI
// call undefined). Called after every JNI call, before the call's result is looked at.
bool Pending(JNIEnv* env) noexcept {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionClear();
  return true;
}

// The activity's versionCode through `env`, one step at a time (the way quest/diag/hwdump_android.cpp reads PackageInfo).
// `context` is the activity object, a Context. Returns null with `code` set, or a token naming the step that failed. Called
// inside a local frame, so none of the references made here outlives the call.
const char* ReadVersionCode(JNIEnv* env, jobject context, std::int32_t* code) noexcept {
  const jclass context_class = env->GetObjectClass(context);
  if (Pending(env) || context_class == nullptr) return "context_class_failed";
  const jmethodID get_name = env->GetMethodID(context_class, "getPackageName", "()Ljava/lang/String;");
  if (Pending(env) || get_name == nullptr) return "package_name_method_failed";
  const jobject name = env->CallObjectMethod(context, get_name);
  if (Pending(env) || name == nullptr) return "package_name_failed";
  const jmethodID get_manager =
      env->GetMethodID(context_class, "getPackageManager", "()Landroid/content/pm/PackageManager;");
  if (Pending(env) || get_manager == nullptr) return "package_manager_method_failed";
  const jobject manager = env->CallObjectMethod(context, get_manager);
  if (Pending(env) || manager == nullptr) return "package_manager_failed";
  const jclass manager_class = env->GetObjectClass(manager);
  if (Pending(env) || manager_class == nullptr) return "package_manager_class_failed";
  const jmethodID get_info = env->GetMethodID(manager_class, "getPackageInfo",
                                              "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
  if (Pending(env) || get_info == nullptr) return "package_info_method_failed";
  const jobject info = env->CallObjectMethod(manager, get_info, name, kNoPackageFlags);
  if (Pending(env) || info == nullptr) return "package_info_failed";
  const jclass info_class = env->GetObjectClass(info);
  if (Pending(env) || info_class == nullptr) return "package_info_class_failed";
  const jfieldID field = env->GetFieldID(info_class, "versionCode", "I");
  if (Pending(env) || field == nullptr) return "version_code_field_failed";
  const jint value = env->GetIntField(info, field);
  if (Pending(env)) return "version_code_read_failed";
  *code = static_cast<std::int32_t>(value);
  return nullptr;
}

// The APK's versionCode from the activity the native glue passes. The activity's own `env` belongs to the thread that calls the
// activity's callbacks and android_main runs on another, so the env comes from the activity's VM for this thread, attached for
// the read when it is not already (the pattern of sentinel/toast_presenter.cpp). Returns null with `code` set, or a token.
const char* ReadApkVersionCode(const void* activity, std::int32_t* code) noexcept {
  if (activity == nullptr) return "no_activity";
  JavaVM* const vm = static_cast<JavaVM*>(sentinel::glue::ReadPointer(activity, sentinel::glue::kActivityVm));
  if (vm == nullptr) return "no_vm";
  const jobject context = static_cast<jobject>(sentinel::glue::ReadPointer(activity, sentinel::glue::kActivityClazz));
  if (context == nullptr) return "no_activity_object";
  JNIEnv* env = nullptr;
  bool attached_here = false;
  const jint got = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (got == JNI_EDETACHED) {
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || env == nullptr) return "attach_failed";
    attached_here = true;
  } else if (got != JNI_OK || env == nullptr) {
    return "env_failed";
  }
  const char* reason = nullptr;
  if (env->ExceptionCheck()) {
    reason = "exception_pending";  // the caller's own exception: no JNI call is safe, and it is not ours to clear
  } else {
    const jint pushed = env->PushLocalFrame(kLocalFrameRefs);
    if (Pending(env) || pushed != 0) {
      reason = "local_frame_failed";
    } else {
      reason = ReadVersionCode(env, context, code);
      env->PopLocalFrame(nullptr);
    }
  }
  if (attached_here) vm->DetachCurrentThread();
  return reason;
}

// The shim's versions as the load-time record has them (entry.cpp StartRuntime): the shim's own version, and the runtime's
// version and path when a runtime loaded. Nothing loaded leaves the runtime "not loaded".
VersionsFacts FactsAtAndroidMain() {
  // This record is written from the runtime's android_main callback (entry.cpp InstallProcessEndHook), so this copy is the one
  // running: its own build is the runtime's version, and the shim's version is the one the runtime was initialised with. The
  // loaded-runtime storage lives in the shim's copy, so it is not read here.
  VersionsFacts facts;
  facts.host_version = nevr_quest::integration::ShimVersion();
  facts.runtime_version = nevr_build::kVersion;
  facts.runtime_path = runtime_load::kBaselineFileName;
  return facts;
}

// One versions record: Info with the facts as given, or Warning with the reason the APK's versionCode was not read.
void WriteVersions(sentinel::LogLevel level, const VersionsFacts& facts, const char* unread_reason) {
  const std::vector<std::pair<std::string, std::string>> fields = VersionsFields(facts);
  std::vector<sentinel::LogField> list;
  list.reserve(fields.size() + kVersionsExtraFields);
  list.emplace_back("tag", kHostTag);
  for (const auto& field : fields) list.emplace_back(field.first.c_str(), field.second.c_str());
  if (unread_reason != nullptr) {
    list.emplace_back("reason", unread_reason);
    list.emplace_back("consequence", kApkUnreadConsequence);
  }
  sentinel::LogFieldList(level, kVersionsEvent, list.data(), list.size());
}

// The update_root record (H5): the computed root against the folder the activity names.
void LogRoot(const void* activity) noexcept {
  const char* internal_data_path =
      static_cast<const char*>(sentinel::glue::ReadPointer(activity, sentinel::glue::kActivityInternalDataPath));
  const std::string root = Resolve();
  if (internal_data_path == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "update_root",
                        {{"tag", kHostTag},
                         {"status", "no_internal_data_path"},
                         {"root", root.c_str()},
                         {"consequence", "the update root is not checked against the folder the activity names"}});
    return;
  }
  const bool match = RootMatchesInternalDataPath(root, internal_data_path);
  sentinel::LogFields(match ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "update_root",
                      {{"tag", kHostTag},
                       {"match", match ? "true" : "false"},
                       {"root", root.c_str()},
                       {"internal_data_path", internal_data_path},
                       {"consequence", match ? "the update reads and writes the folder the activity names"
                                             : "the update would read and write a folder the activity does not name"}});
}

// The versions record with the APK's versionCode, written a second time at android_main: the load-time record could not carry
// it, because the platform gives it only through the activity. Every other field is as the load-time record has it. A read that
// fails, or gives a code below 1, writes a Warning record that says why, and the value stays "unknown".
void LogVersionsWithApkCode(const void* activity) noexcept {
  VersionsFacts facts = FactsAtAndroidMain();
  std::int32_t code = 0;
  const char* reason = ReadApkVersionCode(activity, &code);
  if (reason != nullptr) {
    LogVersionsApkUnread(facts, reason);
    return;
  }
  facts.apk_version_code = ApkVersionCodeText(static_cast<long long>(code));
  if (facts.apk_version_code == kUnknownValue) {
    LogVersionsApkUnread(facts, "version_code_not_positive");
    return;
  }
  LogVersions(facts);
}

}  // namespace

std::string Resolve() {
  const unsigned uid = static_cast<unsigned>(::getuid());
  return RootFromFilesDir(::nevr_quest::app_files::FilesDirFromCmdline(OwnCmdline(), uid));
}

void LogRootAtAndroidMain(const void* app) noexcept {
  const void* activity = sentinel::glue::ReadPointer(app, sentinel::glue::kAppActivity);
  LogRoot(activity);
  LogVersionsWithApkCode(activity);
}

void LogVersions(const VersionsFacts& facts) { WriteVersions(sentinel::LogLevel::kInfo, facts, nullptr); }

void LogVersionsApkUnread(const VersionsFacts& facts, const char* reason) {
  WriteVersions(sentinel::LogLevel::kWarn, facts, reason);
}

}  // namespace nevr_quest::update_root

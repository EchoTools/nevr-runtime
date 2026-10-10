#include "quest/diag/hwdump_run.h"

#include "quest/auth/atomic_write.h"
#include "quest/diag/hwdump_android.h"
#include "quest/diag/hwdump_field.h"
#include "quest/diag/hwdump_handlers.h"
#include "quest/diag/hwdump_install.h"
#include "quest/diag/hwdump_os.h"
#include "quest/diag/hwdump_report.h"
#include "quest/sentinel/sentinel_log.h"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

namespace nevr_quest::hwdump {
namespace {

using nlohmann::json;
using nevr_quest::LogLevel;

constexpr const char* kFileName = "nevr-hwdump.json";
constexpr const char* kLibR15BuildId = "b243509c08ce677aeb95fa348016949b3fc45230";

std::uint64_t MonotonicMs() {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000U + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000U;
}

std::uint64_t UnixMs() {
  timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000U + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000U;
}

std::uint64_t g_start_ms = 0;

json Queries(std::size_t* records_out, std::uint64_t* faults_out, std::size_t* installed_out) {
  std::vector<RecordSnapshot> snaps(kMaxRecords);
  const std::size_t n = Records().Snapshot(snaps.data(), snaps.size());
  HookState hooks[kHookCount];
  HookStates(hooks);
  *records_out = n;
  *faults_out = 0;
  *installed_out = 0;
  for (const HookState& h : hooks) {
    *faults_out += h.faults;
    *installed_out += h.installed ? 1U : 0U;
  }
  return ComposeQueries(snaps.data(), n, hooks, kHookCount, Records().overflow(), Records().contended());
}

// Writes `doc` (with the current libr15_queries) as stage `stage` and logs the one line.
void Write(json& doc, const std::string& stage) {
  WriteSummary s;
  s.stage = stage;
  s.path = std::string(sentinel::FilesDir()) + "/" + kFileName;
  s.hooks_total = kHookCount;
  doc["stage"] = stage;
  doc["written_unix_ms"] = UnixMs();
  doc["libr15_queries"] = Queries(&s.records, &s.hook_faults, &s.hooks_installed);
  s.overflow = Records().overflow();
  CountFields(doc, &s.fields, &s.failed);
  std::string error, warning;
  s.written = nevr::quest_auth::AtomicWrite(s.path, doc.dump(2) + "\n", error, warning);
  s.write_error = error;
  sentinel::Emit(s.written ? LogLevel::kInfo : LogLevel::kError, LogLine(s));
  if (!warning.empty()) sentinel::Emit(LogLevel::kWarn, "hwdump write warning stage=" + stage + " " + warning);
}

void Run() {
  // Wait for the game (hwdump_report.h): polled once a second, nothing else on this thread meanwhile.
  DumpDecision d;
  GameCapture cap;
  for (;;) {
    cap = Captured();
    d = Decide(MonotonicMs(), g_start_ms, cap.initialize_seen, cap.initialize_seen_monotonic_ms);
    if (d.due) break;
    ::sleep(1);
  }
  sentinel::Emit(d.reason == std::string("after_vrapi_initialize") ? LogLevel::kInfo : LogLevel::kWarn,
                 std::string("hwdump trigger reason=") + d.reason + " vrapi_initialize_seen=" +
                     (cap.initialize_seen ? "1" : "0") + " vm_captured=" + (cap.vm != nullptr ? "1" : "0") +
                     " activity_captured=" + (cap.activity != nullptr ? "1" : "0"));

  json doc = json::object();
  doc["schema"] = "nevr-hwdump/1";
  doc["trigger"] = d.reason;
  doc["libr15_build_id"] = kLibR15BuildId;
  doc["notice"] = "Contains device identifiers (serials, MAC and IP addresses, android_id). Do not commit or attach publicly.";

  JniSession jni(cap);  // detached on every path out of Run, by its destructor
  if (!jni.attached()) sentinel::Emit(LogLevel::kError, "hwdump jni unavailable: the jni.* and vrapi.* fields carry the reason");
  std::vector<std::string> storage_paths;
  std::string package;
  doc["jni"] = ComposeJni(jni, &storage_paths, &package);
  OsInputs os;
  os.statvfs_paths = {"/", "/data", "/sdcard", std::string(sentinel::FilesDir())};
  os.statvfs_paths.insert(os.statvfs_paths.end(), storage_paths.begin(), storage_paths.end());
  doc["os"] = ComposeOs(os);
  doc["ndk"] = ComposeNdk(package);
  doc["gpu"] = json::object();
  doc["gpu"]["vulkan"] = ComposeVulkan();
  doc["vrapi"] = ComposeVrapi(jni, cap);
  doc["gpu"]["gl"] = {{"stage", Fail("stage gl", "not run yet: the file is rewritten when it completes")}};
  doc["openxr"] = {{"stage", Fail("stage openxr", "not run yet: the file is rewritten when it completes")}};
  Write(doc, "os");

  // The two stages that touch other graphics/XR runtimes run last, each after a complete file is on disk.
  sentinel::Emit(LogLevel::kInfo, "hwdump stage=gl begin");
  doc["gpu"]["gl"] = ComposeGl();
  Write(doc, "gl");
  sentinel::Emit(LogLevel::kInfo, "hwdump stage=openxr begin");
  doc["openxr"] = ComposeOpenXr();
  Write(doc, "openxr");
}

void* ThreadMain(void*) {
  try {
    Run();
  } catch (const std::exception& e) {
    sentinel::Emit(LogLevel::kError, std::string("hwdump failed: c++ exception ") + e.what());
  }
  return nullptr;
}

}  // namespace

bool StartHwDump() noexcept {
  try {
    g_start_ms = MonotonicMs();
    const std::size_t installed = InstallHooks();
    sentinel::Emit(installed == kHookCount ? LogLevel::kInfo : LogLevel::kWarn,
                   "hwdump hooks installed=" + std::to_string(installed) + "/" + std::to_string(kHookCount));
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int rc = pthread_create(&thread, &attr, &ThreadMain, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
      sentinel::Emit(LogLevel::kError, std::string("hwdump thread not started: ") + std::strerror(rc));
      return false;
    }
    pthread_setname_np(thread, "nevr-hwdump");
    sentinel::Emit(LogLevel::kInfo, "hwdump thread started settle_ms=" + std::to_string(kSettleAfterInitializeMs) +
                                        " fallback_ms=" + std::to_string(kFallbackAfterStartMs));
    return true;
  } catch (const std::exception&) {
    sentinel::EmitFixed(LogLevel::kError, "hwdump not started: c++ exception");
    return false;
  }
}

}  // namespace nevr_quest::hwdump

#include "data_wiring.h"

#include <time.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "hook_log.h"
#include "obb_skip_hook.h"
#include "permission_wait_hook.h"
#include "pinned_targets.h"
#include "process_end_hook.h"
#include "quest/data/curl_transport.h"
#include "quest/data/data_step.h"

namespace nevr_quest::data_wiring {

namespace {

constexpr unsigned kMaxProbeBlockSeconds = 900;
constexpr std::size_t kMaxProbeBytes = 512;
constexpr std::size_t kMaxFields = 16;
constexpr const char kProbeName[] = "nevr-data-probe";

// A function-local static: the sentinel carries no load-time initializer beyond its one ELF constructor
// (tools/check_quest_static_init.sh), and Options holds strings.
Options& Opts() noexcept {
  static Options options;
  return options;
}

void DefaultSleep(unsigned ms) noexcept {
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(ms / 1000);
  ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

void Sleep(unsigned ms) noexcept {
  if (Opts().sleep != nullptr) {
    Opts().sleep(ms);
  } else {
    DefaultSleep(ms);
  }
}

void* CurrentApp() noexcept {
  if (Opts().app != nullptr) return Opts().app();
  return process_end::CurrentApp();
}

// One log line from a step event: the strings stay alive in `event` for the call.
void LogEventFields(sentinel::LogLevel level, const data::Event& event) noexcept {
  sentinel::LogField fields[kMaxFields] = {{"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""},
                                           {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}, {"", ""}};
  std::size_t n = 0;
  for (const auto& kv : event.fields) {
    if (n == kMaxFields) break;
    fields[n++] = sentinel::LogField(kv.first.c_str(), kv.second.c_str());
  }
  sentinel::LogFieldList(level, event.name.c_str(), fields, n);
}

std::string UtcStamp() {
  time_t now = time(nullptr);
  tm parts{};
  gmtime_r(&now, &parts);
  char buf[32];
  if (strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &parts) == 0) return "unknown";
  return buf;
}

// Shows `text` and logs how far it got.
void Present(const std::string& text, const char* what) noexcept {
  if (Opts().show == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "data_present", {{"what", what}, {"result", "no_presenter"}});
    return;
  }
  bool prepared = false;
  const char* result = Opts().show(CurrentApp(), text.c_str(), &prepared);
  // "requested": Toast.show() returned. Whether the headset drew it is ADR 0011 measurement 1, so no line says "shown".
  sentinel::LogFields(std::strcmp(result, "requested") == 0 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn,
                      "data_present",
                      {{"what", what}, {"result", result}, {"looper_prepared", prepared ? "true" : "false"}});
}

std::unique_ptr<data::CurlTransport> MakeProductionTransport() {
  return std::make_unique<data::CurlTransport>(data::DataHostRootsPem(), /*allow_plain_http=*/false,
                                               kConnectTimeoutSeconds, kStallSeconds, data::CheckDataHostRoots,
                                               kMinBytesPerSecond);
}

// The measurement probe: `toast <text>` or `block <seconds>`, once.
void RunProbe() noexcept {
  const std::string path = !Opts().probe_path.empty()
                               ? Opts().probe_path
                               : std::string(data_root::kMediaRoot) + "/" + kProbeName;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return;
  char buf[kMaxProbeBytes + 1] = {};
  const std::size_t n = std::fread(buf, 1, kMaxProbeBytes, f);
  std::fclose(f);
  buf[n] = '\0';
  char* nl = std::strpbrk(buf, "\r\n");
  if (nl != nullptr) *nl = '\0';
  const std::string done = path + ".done";
  if (std::rename(path.c_str(), done.c_str()) != 0) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "data_probe", {{"result", "rename_failed"}});
    return;  // a probe that cannot be consumed would act on every launch
  }
  if (std::strncmp(buf, "toast ", 6) == 0) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "data_probe", {{"kind", "toast"}, {"state", "start"}});
    Present(buf + 6, "probe");
    sentinel::LogFields(sentinel::LogLevel::kInfo, "data_probe", {{"kind", "toast"}, {"state", "end"}});
  } else if (std::strncmp(buf, "block ", 6) == 0) {
    unsigned long seconds = std::strtoul(buf + 6, nullptr, 10);
    if (seconds > kMaxProbeBlockSeconds) seconds = kMaxProbeBlockSeconds;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "data_probe",
                        {{"kind", "block"}, {"state", "start"}, {"seconds", static_cast<long long>(seconds)}});
    for (unsigned long s = 0; s < seconds; ++s) Sleep(1000);
    sentinel::LogFields(sentinel::LogLevel::kInfo, "data_probe", {{"kind", "block"}, {"state", "end"}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "data_probe", {{"result", "unknown_kind"}});
  }
}

// The end of a failed step: nonzero, after the `android_main_returned` line and the before-exit report.
void EndFailed(const char* reason) noexcept {
  if (Opts().ender != nullptr) {
    Opts().ender(process_end::kFailureExitCode, reason);
  } else {
    process_end::EndWithExitCode(process_end::kFailureExitCode, reason);
  }
}

// The message stays up long enough to read: shown, held, shown again, held (two holds of hold_ms, ten seconds).
void HoldMessage(const std::string& message) noexcept {
  Sleep(Opts().hold_ms);
  Present(message, "floor");
  Sleep(Opts().hold_ms);
}

}  // namespace

void AfterStorage(permission_wait::StorageOutcome outcome) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "data_step_start",
                      {{"storage", permission_wait::StorageOutcomeName(outcome)}});
  // The media root (Android/media/<package>) is the app's own directory and needs no permission; the legacy root
  // does. Without the grant the step still runs, on the media root alone, and says it skipped the legacy root.
  const bool granted = permission_wait::StorageGranted(outcome);
  if (granted && Opts().probe_enabled) RunProbe();

  data::StepConfig config;
  config.exists = Opts().exists != nullptr ? Opts().exists : &obb_skip::DirectoryExists;
  config.legacy_root_readable = granted;
  config.build_id = sentinel::pinned::kLibR15BuildId;
  config.fetch_root_override = Opts().fetch_root_override;
  config.recovery_roots = Opts().recovery_roots;
  config.fetch.utc_stamp = UtcStamp();
  if (!Opts().zip_sha256.empty()) {
    config.fetch.sha256 = Opts().zip_sha256;
    config.fetch.bytes = Opts().zip_bytes;
    config.fetch.extracted_bytes = Opts().extracted_bytes;
    config.fetch.max_attempts = 2;
  }
  // No caller backoff: the fetch's own default applies (2, 4, 8, 16, then 30 s), slept through Sleep so a test can see
  // it. There is no cancel: this thread is android_main's and nothing reads the glue's destroy command while it
  // blocks here, so a player who quits ends the process (by the system) and the marked part stays for the next launch.
  config.fetch.sleep_ms = [](unsigned ms) { Sleep(ms); };
  config.fetch.on_event = [](const data::Event& e) { LogEventFields(sentinel::LogLevel::kInfo, e); };
  config.on_step = [](const data::Event& e) { LogEventFields(sentinel::LogLevel::kInfo, e); };
  std::string lastMessage;
  config.present = [&lastMessage](const std::string& message) {
    lastMessage = message;
    Present(message, "floor");
  };
  config.announce = [] { Present(data::kDownloadNotice, "download_notice"); };
  config.hold = [&lastMessage] {
    // The toast lasts a few seconds: show it again once so a player who looked away still reads it.
    HoldMessage(lastMessage);
  };

  std::unique_ptr<data::CurlTransport> ownedTransport;
  data::Transport* transport = Opts().transport;
  if (transport == nullptr) {
    ownedTransport = MakeProductionTransport();
    transport = ownedTransport.get();
  }
  const data::StepResult result = data::RunDataStep(config, *transport);

  const bool failed = result.outcome == data::StepOutcome::kFailed;
  sentinel::LogFields(failed ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo, "data_step",
                      {{"outcome", data::StepOutcomeName(result.outcome)},
                       {"root", result.root.c_str()},
                       {"status", data::FetchStatusName(result.status)},
                       {"message", result.message.c_str()}});
  if (result.outcome == data::StepOutcome::kFetched) obb_skip::ResolveRoot(config.exists);
  if (failed && !granted && result.status == data::FetchStatus::kBadRoot) {
    // The media root cannot be written and there is no permission: the legacy root is the only place the data could
    // be, so the message says how to allow Storage.
    sentinel::LogFields(sentinel::LogLevel::kError, "data_step",
                        {{"outcome", "no_storage_grant"}, {"storage", permission_wait::StorageOutcomeName(outcome)},
                         {"message", data::kNeedsStoragePermission}});
    Present(data::kNeedsStoragePermission, "floor");
    HoldMessage(data::kNeedsStoragePermission);
    EndFailed("no_storage_grant");
    return;
  }
  if (failed) EndFailed("data_step_failed");
}

bool Install(const Options& options) noexcept {
  Opts() = options;
  permission_wait::SetAfterStorage(&AfterStorage);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "data_step_install",
                      {{"result", "armed"}, {"presenter", options.show != nullptr ? "toast" : "none"}});
  return true;
}

}  // namespace nevr_quest::data_wiring

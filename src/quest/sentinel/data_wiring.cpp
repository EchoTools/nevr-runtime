#include "data_wiring.h"

#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <dlfcn.h>

#include "hook_log.h"
#include "obb_skip_hook.h"
#include "permission_wait_hook.h"
#include "pinned_targets.h"
#include "probe_command.h"
#include "process_end_hook.h"
#include "quest/data/curl_transport.h"
#include "quest/data/data_step.h"
#include "quest/data/pack.h"

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
  // "requested": Toast.show() returned on this thread. "posted": queued to the UI thread (ShowPreferUiThread, #829), whose
  // Toast result follows as a ui_post line. Whether the headset drew it is ADR 0011 measurement 1, so no line says "shown".
  const bool accepted = std::strcmp(result, "requested") == 0 || std::strcmp(result, "posted") == 0;
  sentinel::LogFields(accepted ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn,
                      "data_present",
                      {{"what", what}, {"result", result}, {"looper_prepared", prepared ? "true" : "false"}});
}

std::unique_ptr<data::CurlTransport> MakeProductionTransport() {
  return std::make_unique<data::CurlTransport>(data::DataHostRootsPem(), /*allow_plain_http=*/false,
                                               kConnectTimeoutSeconds, kStallSeconds, data::CheckDataHostRoots,
                                               kMinBytesPerSecond);
}

// One probe line for a presenter that is not the floor's (`ui-toast`, `ui-post`, `dialog`, #829): what the presenter answered, on which
// thread it was asked, and whether a looper was made for it. Only the tester can say whether anything was drawn.
void ProbeShow(const char* kind, ShowFn presenter, const char* text) noexcept {
  const long long tid = static_cast<long long>(::syscall(SYS_gettid));
  const bool mainThread = tid == static_cast<long long>(::getpid());  // the UI thread's id is the process id
  bool prepared = false;
  const char* result = "empty_text";
  if (*text == '\0') {
    // nothing to show: the presenter is not asked
  } else if (presenter == nullptr) {
    result = "no_presenter";
  } else {
    result = presenter(CurrentApp(), text, &prepared);
  }
  const bool asked = std::strcmp(result, "requested") == 0 || std::strcmp(result, "posted") == 0;
  sentinel::LogFields(asked ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn,
                      "data_probe",
                      {{"kind", kind},
                       {"state", "end"},
                       {"result", result},
                       {"tid", tid},
                       {"main_thread", mainThread ? "true" : "false"},
                       {"looper_prepared", prepared ? "true" : "false"}});
}

// The measurement probe: `toast <text>`, `ui-toast <text>`, `ui-post <text>`, `dialog <text>` or `block <seconds>` (probe_command.h), once.
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
  const ProbeCommand command = ParseProbeCommand(buf);
  if (command.kind == ProbeKind::kToast) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "data_probe", {{"kind", "toast"}, {"state", "start"}});
    Present(command.arg, "probe");
    sentinel::LogFields(sentinel::LogLevel::kInfo, "data_probe", {{"kind", "toast"}, {"state", "end"}});
  } else if (command.kind == ProbeKind::kUiToast) {
    ProbeShow("ui-toast", Opts().show_ui, command.arg);
  } else if (command.kind == ProbeKind::kUiPost) {
    ProbeShow("ui-post", Opts().show_post, command.arg);
  } else if (command.kind == ProbeKind::kDialog) {
    ProbeShow("dialog", Opts().show_dialog, command.arg);
  } else if (command.kind == ProbeKind::kBlock) {
    unsigned long seconds = std::strtoul(command.arg, nullptr, 10);
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

// The message stays up long enough to read: shown, held, shown again, held (two holds of hold_ms, ten seconds). With
// hold_ms 0 (a release build: no presenter is known to draw, docs/adr/0013) nothing is held and the message is not
// shown a second time.
void HoldMessage(const std::string& message) noexcept {
  if (Opts().hold_ms == 0) return;
  Sleep(Opts().hold_ms);
  Present(message, "floor");
  Sleep(Opts().hold_ms);
}

// The APK this process runs from, taken from where the linker mapped the sentinel (`<apk>!/lib/...`, or a library extracted
// next to `base.apk`); empty when neither says.
std::string OwnApkPath() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&AfterStorage), &info) == 0 || info.dli_fname == nullptr) return "";
  return data::ApkPathForLibrary(info.dli_fname, Opts().exists != nullptr ? Opts().exists : &obb_skip::PathExists);
}

sentinel::LogLevel LogLevelOf(data::EventLevel level) noexcept {
  switch (level) {
    case data::EventLevel::kWarn: return sentinel::LogLevel::kWarn;
    case data::EventLevel::kError: return sentinel::LogLevel::kError;
    case data::EventLevel::kInfo: break;
  }
  return sentinel::LogLevel::kInfo;
}

// The bundled content pack (docs/adr/0029-quest-bundled-content-pack.md): after the data step found or fetched the data. An APK
// with no pack is one `pack_check` line and nothing else. A failed placement ends the process like a failed data step.
void RunPack(const std::string& engineRoot) noexcept {
  data::PackConfig pack;
  pack.apk_path = !Opts().apk_path.empty() ? Opts().apk_path : OwnApkPath();
  // The root the data step used (the test seam moves it, as it moves the fetch).
  pack.root = Opts().fetch_root_override.empty() ? engineRoot : Opts().fetch_root_override;
  pack.available_bytes = Opts().pack_available_bytes;
  pack.probe = Opts().pack_probe;
  pack.on_event = [](data::EventLevel level, const data::Event& e) { LogEventFields(LogLevelOf(level), e); };
  std::string lastMessage;
  pack.present = [&lastMessage](const std::string& message) {
    lastMessage = message;
    Present(message, "floor");
  };
  pack.hold = [&lastMessage] { HoldMessage(lastMessage); };
  const data::PackResult result = data::RunPackStep(pack);
  if (result.outcome == data::PackOutcome::kFailed) EndFailed("pack_failed");
}

}  // namespace

// The one root the engine reads this launch, and why. With an OBB file the game's OBB callback records what the OBB skip
// served (obb_skip::CurrentRoot, after a skipped mount); without one CSysFile::Init returns before the mount and the
// engine asks GetDataRootDir, which our hook answers with the root ResolveRoot chose (obb_skip::DataRootRedirect, #866): the
// media root, unless the legacy root alone holds the data (docs/adr/0007, 0013).
struct EngineRootChoice {
  std::string root;
  const char* source;
};
EngineRootChoice EngineRoot() {
  if (obb_skip::CurrentCounts().mountSkipped > 0) return {obb_skip::CurrentRoot(), "obb_skip"};
  if (const char* redirect = obb_skip::DataRootRedirect()) return {redirect, "data_root_hook"};
  return {data_root::kLegacyRoot, "engine_default"};
}

bool StorageNeeded() noexcept { return EngineRoot().root == data_root::kLegacyRoot; }

void AfterStorage(permission_wait::StorageOutcome outcome) noexcept {
  const EngineRootChoice choice = EngineRoot();
  const std::string& engineRoot = choice.root;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "data_step_start",
                      {{"storage", permission_wait::StorageOutcomeName(outcome)},
                       {"engine_root", engineRoot.c_str()},
                       {"engine_root_source", choice.source}});
  // The legacy root is shared storage: reading it needs the grant. Without it nothing is looked at, fetched or
  // written, the player is told how to allow Storage, and the process ends.
  const bool granted = permission_wait::StorageGranted(outcome);
  if (!granted && engineRoot == data_root::kLegacyRoot) {
    sentinel::LogFields(sentinel::LogLevel::kError, "data_step",
                        {{"outcome", "no_storage_grant"}, {"storage", permission_wait::StorageOutcomeName(outcome)},
                         {"root", engineRoot.c_str()}, {"message", data::kNeedsStoragePermission}});
    Present(data::kNeedsStoragePermission, "floor");
    HoldMessage(data::kNeedsStoragePermission);
    EndFailed("no_storage_grant");
    return;
  }
  if (granted && Opts().probe_enabled) RunProbe();

  data::StepConfig config;
  config.exists = Opts().exists != nullptr ? Opts().exists : &obb_skip::PathExists;
  config.legacy_root_readable = granted;
  config.allow_fetch = Opts().allow_fetch;
  config.engine_root = engineRoot;
  // Without the grant an unwritable media root is reported once, with the Storage advice, not twice.
  if (!granted) config.bad_root_message = data::kNeedsStoragePermission;
  config.build_id = sentinel::pinned::kLibR15StockBuildId;
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
  const bool missing = result.outcome == data::StepOutcome::kMissing;
  sentinel::LogFields(failed || missing ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo, "data_step",
                      {{"outcome", data::StepOutcomeName(result.outcome)},
                       {"root", result.root.c_str()},
                       {"status", data::FetchStatusName(result.status)},
                       {"message", result.message.c_str()}});
  if (failed && !granted && result.status == data::FetchStatus::kBadRoot) {
    // The media root cannot be written and there is no permission: the step already showed and held the Storage
    // message (bad_root_message), so this only records why the process ends.
    sentinel::LogFields(sentinel::LogLevel::kError, "data_step",
                        {{"outcome", "no_storage_grant"}, {"storage", permission_wait::StorageOutcomeName(outcome)},
                         {"message", data::kNeedsStoragePermission}});
    EndFailed("no_storage_grant");
    return;
  }
  if (missing) {
    EndFailed("game_data_missing");
    return;
  }
  // The data is in place (found or fetched): the bundled pack, when this APK carries one, goes over it. A release build
  // takes this step too: the variant APK that carries a pack is a release build, and an APK without one only logs.
  if (failed) EndFailed("data_step_failed");
  else RunPack(engineRoot);
}

bool Install(const Options& options) noexcept {
  Opts() = options;
  permission_wait::SetAfterStorage(&AfterStorage);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "data_step_install",
                      {{"result", "armed"}, {"presenter", options.show != nullptr ? "toast" : "none"}});
  return true;
}

}  // namespace nevr_quest::data_wiring

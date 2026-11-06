// The process's update worker: the real core, the real verifier, the real log, the real clock (docs/adr/0024-pc-self-update.md,
// sections 3 and 6). The decisions and the two jobs are update_policy.cpp and update_run.cpp, which a test drives against a fake core;
// this file only binds them to the process.

#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

#include "core/logging.h"
#include "core/nevr_root.h"
#ifdef NEVR_UPDATE_CORE_LINKED
#include "installer/include/nevr_install.h"
#endif
#include "runtime/ext/plugin_signature.h"
#include "runtime/ext/plugin_trust.h"
#include "runtime/host/file_digest.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/lifecycle/update_worker.h"

namespace nevr_update {
namespace {

// The core's C ABI (installer/include/nevr_install.h), statically linked into this DLL (ADR 0024, section 3: one signed file, no
// extra DLL for the plugin gate to verify). One session per check. The session is used from one thread at a time: the check thread.
#ifdef NEVR_UPDATE_CORE_LINKED
Core MakeFfiCore() {
  auto session = std::make_shared<NevrSession*>(nullptr);
  Core core;
  core.open = [session](const std::string& config_json, std::string* error) {
    NevrSession* s = nullptr;
    if (NevrInstallOpen(config_json.c_str(), &s) != NEVR_OK || s == nullptr) {
      if (error != nullptr) *error = "NevrInstallOpen failed";
      return false;
    }
    *session = s;
    return true;
  };
  core.manifest_fetch = [session](std::string* error) {
    if (NevrManifestFetch(*session) == NEVR_OK) return true;
    if (error != nullptr) *error = NevrLastError(*session);
    return false;
  };
  core.stage = [session](const std::string& root_dir, const std::string& current_json, std::string* result, std::string* error) {
    char* out = nullptr;
    if (NevrPcStage(*session, root_dir.c_str(), current_json.c_str(), &out) != NEVR_OK) {
      if (error != nullptr) *error = NevrLastError(*session);
      if (out != nullptr) NevrStringFree(out);
      return false;
    }
    if (out != nullptr) {
      *result = out;
      NevrStringFree(out);
    }
    return true;
  };
  core.run_end = [session](const std::string& state, const std::string& reason) {
    if (*session != nullptr) NevrRunEnd(*session, state.c_str(), reason.c_str());
  };
  core.close = [session] {
    if (*session != nullptr) NevrClose(*session);
    *session = nullptr;
  };
  return core;
}
#else
// A build configured with -DNEVR_UPDATE_CORE=OFF has no core to call: the check ends in `update check action=failed reason=core_open_failed`.
Core MakeFfiCore() {
  Core core;
  core.open = [](const std::string&, std::string* error) {
    if (error != nullptr) *error = "this build does not link the core install method";
    return false;
  };
  return core;
}
#endif

// The version and the sha256 of the file this process loaded: the runtime's own module file, read once on the worker's thread. The
// version is the one in its NEVR-BUILD literal, the same source the host reads for `from` in its swap.
bool ReadRunningRuntime(std::string* version, std::string* sha256, std::string* error) {
  HMODULE self = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&ReadRunningRuntime), &self) == FALSE) {
    if (error != nullptr) *error = "GetModuleHandleExW failed";
    return false;
  }
  wchar_t path[MAX_PATH * 2] = {};
  const DWORD n = GetModuleFileNameW(self, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
  if (n == 0 || n >= sizeof(path) / sizeof(path[0])) {
    if (error != nullptr) *error = "GetModuleFileNameW failed";
    return false;
  }
  const HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (error != nullptr) *error = "the runtime file could not be opened";
    return false;
  }
  std::vector<unsigned char> bytes;
  const bool read = nevr_host::ReadWholeFile(file, &bytes);
  CloseHandle(file);
  if (!read) {
    if (error != nullptr) *error = "the runtime file could not be read";
    return false;
  }
  *sha256 = nevr_host::Sha256Hex(bytes);
  *version = nevr_host::BuildVersion(bytes);
  if (sha256->empty()) {
    if (error != nullptr) *error = "the runtime file could not be hashed";
    return false;
  }
  return true;
}

// Authenticode on one staged file with the production policy, the denylist included (ADR 0024, section 3).
VerifyOutcome VerifyStaged(const std::wstring& path) {
  VerifyOutcome out;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    out.reason = "the staged file could not be opened (Win32 error " + std::to_string(GetLastError()) + ")";
    return out;
  }
  FILETIME ft{};
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER t{};
  t.LowPart = ft.dwLowDateTime;
  t.HighPart = ft.dwHighDateTime;
  const std::int64_t now_unix = static_cast<std::int64_t>(t.QuadPart / 10000000ULL) - 11644473600LL;
  const nevr_plugin_signature::Verdict verdict = nevr_plugin_signature::VerifyPluginFile(file, nevr_plugin_trust::ProductionPolicy(), now_unix);
  CloseHandle(file);
  out.ok = verdict.accepted();
  out.reason = verdict.Describe();
  return out;
}

// The process's worker is never destroyed. Its threads end with the process; a destructor that ran at DLL_PROCESS_DETACH would join
// them under the loader lock (and the check may be inside the core, which cannot be cancelled).
std::atomic<Worker*> g_worker_for_frames{nullptr};

}  // namespace

void StartProcessWorker(bool host_can_swap, bool flag_no_update, bool server) {
  static std::atomic_bool s_once{false};
  if (s_once.exchange(true)) return;
  Deps deps;
  deps.switches.dev_build = nevr_plugin_trust::IsDevBuild();
  deps.switches.flag_no_update = flag_no_update;
  const int enabled = NevrCfgUpdateEnabled();
  if (enabled >= 0) deps.switches.config_enabled = enabled != 0;
  deps.switches.host_can_swap = host_can_swap;
  if (const char* channel = NevrCfgUpdateChannel()) deps.config_channel = channel;
  deps.server = server;
  deps.root = nevr_root::RootW();
  deps.read_running = &ReadRunningRuntime;
  deps.core = MakeFfiCore();
  deps.verify = &VerifyStaged;
  deps.log = [](int level, const std::string& line) {
    const EchoVR::LogLevel l = level >= kError ? EchoVR::LogLevel::Error
                               : level == kWarning ? EchoVR::LogLevel::Warning
                               : level == kDebug ? EchoVR::LogLevel::Debug
                                                 : EchoVR::LogLevel::Info;
    Log(l, "%s", line.c_str());
  };
  deps.now_ms = [] { return static_cast<std::uint64_t>(GetTickCount64()); };
  if (deps.root.empty()) {
    // No %LOCALAPPDATA%: nothing can be staged or confirmed, and nothing is written anywhere else.
    Log(EchoVR::LogLevel::Warning, "[NEVR.UPDATE] update disabled reason=no_root (LOCALAPPDATA is not set or does not fit)");
    return;
  }
  Worker* worker = new Worker(std::move(deps));  // NOLINT: never destroyed, see g_worker_for_frames
  worker->Start();
  g_worker_for_frames.store(worker, std::memory_order_release);
}

void OnFrame(std::uint64_t now_us) {
  Worker* w = g_worker_for_frames.load(std::memory_order_acquire);
  if (w != nullptr) w->OnFrame(now_us / 1000);
}

}  // namespace nevr_update

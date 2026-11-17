// The process's update worker: the real core, the real verifier, the real log, the real clock (docs/adr/0024-pc-self-update.md,
// sections 3 and 6). The decisions, the two jobs and the worker are core/update (policy.cpp, engine_check.cpp, worker.cpp), which a test
// drives against a fake core; this file only binds them to the process.

#include <windows.h>

#include <atomic>
#include <new>
#include <string>
#include <vector>

#include "core/logging.h"
#include "core/nevr_root.h"
#include "core/update/core_ffi.h"
#include "runtime/ext/plugin_signature.h"
#include "runtime/ext/plugin_trust.h"
#include "runtime/host/file_digest.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/lifecycle/update_fs_win32.h"
#include "runtime/lifecycle/update_run.h"
#include "runtime/lifecycle/update_worker.h"

namespace nevr_update {
namespace {

static_assert(EchoLevelBits(kDebug) == static_cast<int>(EchoVR::LogLevel::Debug));
static_assert(EchoLevelBits(kInfo) == static_cast<int>(EchoVR::LogLevel::Info));
static_assert(EchoLevelBits(kWarning) == static_cast<int>(EchoVR::LogLevel::Warning));
static_assert(EchoLevelBits(kError) == static_cast<int>(EchoVR::LogLevel::Error));

}  // namespace

// The version and the sha256 of the file this process loaded (update_worker.h): the runtime's own module file. The version is the one in
// its NEVR-BUILD literal, the same source the host reads for `from` in its swap.
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

namespace {

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
  out.refused = !out.ok;
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
  deps.platform = "pc";  // the core's platform word (ADR 0024, section 3)
  // The root's text as UTF-8, which the engine takes. A root that does not convert is not "no root": the check and the confirmation
  // run and fail closed, reading nothing. Out of memory while the root is read or converted is taken the same way: nothing is read
  // from the root, staged or confirmed. The check then ends with the reason root_unconvertible, and the confirmation with
  // pending_unreadable.
  try {
    deps.root_unconvertible = !RootToUtf8(nevr_root::RootW(), &deps.root);
  } catch (const std::bad_alloc&) {
    deps.root.clear();
    deps.root_unconvertible = true;
  }
  deps.fs = &Win32UpdateFs();
  deps.read_running = &ReadRunningRuntime;
  deps.core = nevr_update::MakeFfiCore(nullptr);  // the PC stage: NevrPcStage, the windows-zip profile
  deps.verify = WidenVerifier(&VerifyStaged);
  deps.log = [](int level, const std::string& line) {
    Log(static_cast<EchoVR::LogLevel>(EchoLevelBits(level)), "%s", line.c_str());
  };
  deps.now_ms = [] { return static_cast<std::uint64_t>(GetTickCount64()); };
  deps.switches.no_root = deps.root.empty() && !deps.root_unconvertible;  // the worker says so and starts no thread: nothing is staged or confirmed without a root
  Worker* worker = new Worker(std::move(deps));  // NOLINT: never destroyed, see g_worker_for_frames
  worker->Start();
  g_worker_for_frames.store(worker, std::memory_order_release);
}

void OnFrame(std::uint64_t now_us) {
  Worker* w = g_worker_for_frames.load(std::memory_order_acquire);
  if (w != nullptr) w->OnFrame(now_us / 1000);
}

}  // namespace nevr_update

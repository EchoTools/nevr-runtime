#include "runtime/host/crash_send_host.h"

#include "core/crash_send.h"
#include "runtime/host/crash_send_winhttp.h"
#include "runtime/host/host_identity.h"
#include "runtime/host/host_paths.h"
#include "runtime/host/path_buffer.h"

#include <windows.h>

#include <atomic>
#include <exception>
#include <nlohmann/json.hpp>
#include <string>

// RtlGenRandom: advapi32 exports it under this name, and advapi32 is an allowed import of the host.
extern "C" BOOLEAN WINAPI SystemFunction036(PVOID buffer, ULONG length);

namespace nevr_host {
namespace {

std::atomic<bool> g_started{false};
std::atomic<BootstrapDiagnostics*> g_diagnostics{nullptr};

struct Job {
  nevr_crash_send::Config config;
  WinHttpPostContext post;
};

// Used only outside NEVR_HOST_TESTING (the #else branch below); nevr_host_core defines it.
[[maybe_unused]] bool RandomBytes(std::uint8_t* bytes, std::size_t count) {
  return SystemFunction036(bytes, static_cast<ULONG>(count)) != FALSE;
}

void LogEvent(const nevr_crash_send::LogEvent& event) {
  BootstrapDiagnostics* diagnostics = g_diagnostics.load(std::memory_order_acquire);
  if (diagnostics == nullptr) return;
  try {
    nlohmann::json record;
    record["event"] = "crash_send";
    record["state"] = event.event;
    if (!event.report.empty()) record["report"] = event.report;
    record["status"] = event.status;
    record["error"] = event.errorCode;
    record["bytes"] = event.bytes;
    if (!event.detail.empty()) record["detail"] = event.detail;
    diagnostics->Emit(record);
  } catch (const std::exception&) {
    // The record could not be built: Emit reports its own build failures on stderr; there is nothing more to say here.
  }
}

[[maybe_unused]] DWORD WINAPI SendThread(LPVOID parameter) {
  // Never freed: the process ends with the thread.
  Job* job = static_cast<Job*>(parameter);
  try {
    nevr_crash_send::SendPending(job->config);
  } catch (const std::exception& error) {
    nevr_crash_send::LogEvent event;
    event.event = "failed";
    event.detail = error.what();
    LogEvent(event);
  }
  return 0;
}

}  // namespace

void StartCrashSend(BootstrapDiagnostics& diagnostics) noexcept {
#if defined(NEVR_HOST_TESTING)
  // The harnesses that drive the detour share a Wine prefix with the crash harness, whose crashes leave reports: a test run
  // never sends one to the service. The send's own tests call SendPending and WinHttpPost directly.
  static_cast<void>(diagnostics);
  return;
#else
  if (*nevr_crash_send::ConfiguredUrl() == '\0') return;  // switched off in this build: nothing runs
  if (g_started.exchange(true)) return;
  try {
    wchar_t local_app_data[MAX_PATH] = {};
    wchar_t crashes[MAX_PATH] = {};
    Job* job = new Job();
    g_diagnostics.store(&diagnostics, std::memory_order_release);
    if (CopyEnv(local_app_data, MAX_PATH, L"LOCALAPPDATA") &&
        AppendPath(crashes, MAX_PATH, local_app_data, kCrashesSubdirectory)) {
      job->config.dir = std::filesystem::path(crashes);
    }
    job->config.url = nevr_crash_send::ConfiguredUrl();
    job->config.build = Version();
    job->config.platform = "pc";
    job->config.post = &WinHttpPost;
    job->config.postContext = &job->post;
    job->config.random = &RandomBytes;
    job->config.log = &LogEvent;
    const HANDLE thread = CreateThread(nullptr, 0, &SendThread, job, 0, nullptr);
    if (thread == nullptr) {
      nevr_crash_send::LogEvent event;
      event.event = "failed";
      event.errorCode = static_cast<long>(GetLastError());
      event.detail = "the send thread could not be started";
      LogEvent(event);
      return;
    }
    CloseHandle(thread);
  } catch (const std::exception&) {
    // Out of memory or a path that cannot be built: the game starts without the send; nothing else depends on it.
  }
#endif
}

}  // namespace nevr_host

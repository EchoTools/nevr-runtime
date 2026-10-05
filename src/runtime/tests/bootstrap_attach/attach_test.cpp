#include "abi.h"

#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kEventCapacity = 4096;
LONG g_events[kEventCapacity] = {};
volatile LONG g_event_count = 0;
volatile LONG g_bad_results = 0;
volatile LONG g_heartbeat_count = 0;
volatile LONG g_stop_heartbeat = 0;

void Fail(const char* message) {
  std::fprintf(stderr, "bootstrap attach test: FAIL: %s\n", message);
  ExitProcess(1);
}

std::wstring ModulePath() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    Fail("could not resolve test executable path");
  }
  return std::wstring(path, length);
}

std::wstring DirectoryOf(const std::wstring& path) {
  const std::size_t separator = path.find_last_of(L"\\/");
  if (separator == std::wstring::npos) {
    Fail("test executable path has no directory");
  }
  return path.substr(0, separator + 1);
}

std::wstring ScenarioArgument(const wchar_t* mode) {
  return L"\"" + ModulePath() + L"\" --child " + mode;
}

void CopyRuntimeFixture(const std::wstring& root, const wchar_t* fixture) {
  const std::wstring source = root + L"fixtures\\" + fixture + L"\\nevr.dll";
  const std::wstring destination = root + L"nevr.dll";
  if (!CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
    std::fprintf(stderr, "bootstrap attach test: CopyFileW failed, Win32 error=%lu\n",
                 static_cast<unsigned long>(GetLastError()));
    Fail("could not stage runtime fixture");
  }
}

void RunChild(const wchar_t* mode, const char* label) {
  std::wstring command = ScenarioArgument(mode);
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                      nullptr, &startup, &process)) {
    Fail("could not launch scenario child");
  }
  CloseHandle(process.hThread);
  const DWORD wait_result = WaitForSingleObject(process.hProcess, 30000);
  if (wait_result != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 2);
    CloseHandle(process.hProcess);
    Fail("scenario child timed out");
  }
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
    CloseHandle(process.hProcess);
    Fail("could not read scenario child exit code");
  }
  CloseHandle(process.hProcess);
  if (exit_code != 0) {
    Fail("scenario child returned failure");
  }
  std::printf("bootstrap attach test: PASS: child scenario %s\n", label);
}

DWORD WINAPI HeartbeatThread(void*) {
  while (InterlockedCompareExchange(&g_stop_heartbeat, 0, 0) == 0) {
    InterlockedIncrement(&g_heartbeat_count);
    Sleep(1);
  }
  return 0;
}

DWORD WINAPI TargetWorker(void* raw_iterations) {
  const LONG iterations = static_cast<LONG>(reinterpret_cast<ULONG_PTR>(raw_iterations));
  for (LONG index = 0; index < iterations; ++index) {
    const std::uintptr_t first = static_cast<std::uintptr_t>(index + 1);
    const std::uintptr_t second = static_cast<std::uintptr_t>(index + 17);
    const auto target = reinterpret_cast<ProbeTargetFunction>(
        GetProcAddress(GetModuleHandleW(nullptr), "BootstrapProbeTarget"));
    if (target == nullptr || target(first, second) != first * 1000 + second) {
      InterlockedIncrement(&g_bad_results);
    }
  }
  return 0;
}

std::vector<LONG> Events() {
  const LONG count = InterlockedCompareExchange(&g_event_count, 0, 0);
  if (count < 0 || static_cast<std::size_t>(count) > kEventCapacity) {
    Fail("event buffer overflowed");
  }
  return std::vector<LONG>(g_events, g_events + count);
}

LONG CountEvent(const std::vector<LONG>& events, LONG event) {
  LONG count = 0;
  for (LONG current : events) {
    if (current == event) {
      ++count;
    }
  }
  return count;
}

std::size_t FirstEvent(const std::vector<LONG>& events, LONG event) {
  for (std::size_t index = 0; index < events.size(); ++index) {
    if (events[index] == event) {
      return index;
    }
  }
  return events.size();
}

int RunChildScenario(const wchar_t* mode) {
  const bool valid = wcscmp(mode, L"valid") == 0;
  const bool init_failure = wcscmp(mode, L"init-failure") == 0;
  if (init_failure) {
    SetEnvironmentVariableW(L"NEVR_PROBE_FAIL_INIT", L"1");
  }

  const std::wstring root = DirectoryOf(ModulePath());
  const std::wstring host_path = root + L"BugSplat64.dll";
  HANDLE heartbeat = CreateThread(nullptr, 0, HeartbeatThread, nullptr, 0, nullptr);
  if (heartbeat == nullptr) {
    Fail("could not create MinHook stress thread");
  }
  Sleep(30);
  HMODULE host = LoadLibraryW(host_path.c_str());
  if (host == nullptr) {
    Fail("host probe DLL failed to load");
  }
  BootstrapProbeRecordEvent(kHostLoadReturned);
  InterlockedExchange(&g_stop_heartbeat, 1);
  const DWORD heartbeat_done = WaitForSingleObject(heartbeat, 5000);
  if (heartbeat_done != WAIT_OBJECT_0) {
    Fail("MinHook stress thread did not stop");
  }
  CloseHandle(heartbeat);
  if (InterlockedCompareExchange(&g_heartbeat_count, 0, 0) < 2) {
    Fail("MinHook attach did not allow the concurrent thread to make progress");
  }

  const auto target = reinterpret_cast<ProbeTargetFunction>(
      GetProcAddress(GetModuleHandleW(nullptr), "BootstrapProbeTarget"));
  if (target == nullptr) {
    Fail("probe target export missing");
  }
  const std::uintptr_t first_result = target(11, 29);
  if (first_result != 11029) {
    Fail("detour changed the original result");
  }

  if (valid) {
    constexpr LONG worker_count = 4;
    constexpr LONG calls_per_worker = 100;
    HANDLE workers[worker_count] = {};
    for (LONG index = 0; index < worker_count; ++index) {
      workers[index] = CreateThread(
          nullptr, 0, TargetWorker,
          reinterpret_cast<void*>(static_cast<ULONG_PTR>(calls_per_worker)), 0, nullptr);
      if (workers[index] == nullptr) {
        Fail("could not create concurrent hook worker");
      }
    }
    const DWORD workers_done = WaitForMultipleObjects(worker_count, workers, TRUE, 10000);
    if (workers_done != WAIT_OBJECT_0) {
      Fail("concurrent hook workers timed out");
    }
    for (HANDLE worker : workers) {
      CloseHandle(worker);
    }
    if (InterlockedCompareExchange(&g_bad_results, 0, 0) != 0) {
      Fail("concurrent detour calls changed an original result");
    }
  }

  const std::vector<LONG> events = Events();
  const std::size_t attach_enter = FirstEvent(events, kHostAttachEnter);
  const std::size_t hook_enabled = FirstEvent(events, kHostHookEnabled);
  const std::size_t attach_exit = FirstEvent(events, kHostAttachExit);
  const std::size_t load_returned = FirstEvent(events, kHostLoadReturned);
  if (!(attach_enter < hook_enabled && hook_enabled < attach_exit &&
        attach_exit < load_returned)) {
    Fail("host attach/hook/load-return event order is invalid");
  }

  if (valid) {
    const std::size_t runtime_initialized = FirstEvent(events, kRuntimeInitialized);
    const std::size_t pre = FirstEvent(events, kRuntimePreprocessBefore);
    const std::size_t original = FirstEvent(events, kTargetOriginal);
    const std::size_t post = FirstEvent(events, kRuntimePreprocessAfter);
    if (!(load_returned < runtime_initialized && runtime_initialized < pre &&
          pre < original && original < post)) {
      Fail("runtime initialization or before/original/after order is invalid");
    }
    if (CountEvent(events, kRuntimePreprocessBefore) !=
            CountEvent(events, kRuntimePreprocessAfter) ||
        CountEvent(events, kTargetOriginal) !=
            CountEvent(events, kRuntimePreprocessBefore)) {
      Fail("runtime/original call counts do not match");
    }
    std::printf("bootstrap attach test: PASS: attach stress, runtime load order, "
                "reentrant and concurrent hook dispatch\n");
  } else {
    if (CountEvent(events, kTargetOriginal) != 1 ||
        CountEvent(events, kRuntimeFailure) != 1 ||
        CountEvent(events, kRuntimePreprocessBefore) != 0 ||
        CountEvent(events, kRuntimePreprocessAfter) != 0) {
      Fail("runtime failure did not degrade to one original call without dispatch");
    }
    std::printf("bootstrap attach test: PASS: %ls runtime failure degraded safely\n", mode);
  }
  // The installed detour points into host; keep it loaded until this child exits.
  return 0;
}

int RunSuite() {
  const std::wstring root = DirectoryOf(ModulePath());
  const std::wstring runtime = root + L"nevr.dll";
  const wchar_t* scenarios[] = {L"valid", L"missing", L"bad-version",
                                L"missing-entry", L"init-failure", L"bad-image"};
  const char* labels[] = {"valid", "missing runtime", "ABI mismatch",
                          "missing API entry", "init failure", "invalid PE image"};
  const wchar_t* fixtures[] = {L"good", nullptr, L"bad-version",
                               L"missing-entry", L"good", L"bad-image"};
  for (std::size_t index = 0; index < sizeof(scenarios) / sizeof(scenarios[0]); ++index) {
    if (fixtures[index] == nullptr) {
      DeleteFileW(runtime.c_str());
    } else {
      CopyRuntimeFixture(root, fixtures[index]);
    }
    RunChild(scenarios[index], labels[index]);
  }
  CopyRuntimeFixture(root, L"good");
  std::printf("bootstrap attach test: PASS: all loader/runtime failure scenarios\n");
  return 0;
}

}  // namespace

extern "C" __declspec(dllexport) void WINAPI BootstrapProbeRecordEvent(LONG event) {
  const LONG index = InterlockedIncrement(&g_event_count) - 1;
  if (index >= 0 && static_cast<std::size_t>(index) < kEventCapacity) {
    g_events[index] = event;
  }
}

extern "C" __declspec(dllexport) std::uintptr_t WINAPI BootstrapProbeTarget(
    std::uintptr_t first, std::uintptr_t second) {
  BootstrapProbeRecordEvent(kTargetOriginal);
  return first * 1000 + second;
}

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--child") == 0) {
    if (std::strcmp(argv[2], "valid") == 0) {
      return RunChildScenario(L"valid");
    }
    if (std::strcmp(argv[2], "missing") == 0) {
      return RunChildScenario(L"missing");
    }
    if (std::strcmp(argv[2], "bad-version") == 0) {
      return RunChildScenario(L"bad-version");
    }
    if (std::strcmp(argv[2], "missing-entry") == 0) {
      return RunChildScenario(L"missing-entry");
    }
    if (std::strcmp(argv[2], "init-failure") == 0) {
      return RunChildScenario(L"init-failure");
    }
    if (std::strcmp(argv[2], "bad-image") == 0) {
      return RunChildScenario(L"bad-image");
    }
    Fail("unknown child scenario");
  }
  return RunSuite();
}

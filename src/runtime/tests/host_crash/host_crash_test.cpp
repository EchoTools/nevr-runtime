// Child-process crash harness for the host-owned BugSplat_createReport minidump
// capture (docs/design/2026-10-05-bugsplat-nevr-split.md step 3).
//
// Each scenario runs a child process that: loads dbghelp.dll (as the game's
// HandleCrashDump already has), loads the BugSplat64.dll shim from a
// per-scenario directory, runs the host runtime-load state machine against the
// sibling nevr.dll fixture, installs the unhandled-exception filter that calls
// MiniDmpSender::createReport, and then deliberately faults with an access
// violation. The parent validates that a structurally valid minidump was written
// to the expected destination, names the faulting thread, and carries the
// deliberate access violation. It also proves the fallback chain
// (%LOCALAPPDATA% -> %TEMP% -> shim dir) and the total-failure path (no dump, no
// hang).

#include "runtime/tests/host_crash/minidump.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using NevrHostTest::MinidumpView;

constexpr const char* kMangledCreateReport =
    "?createReport@MiniDmpSender@@QEAAXPEAU_EXCEPTION_POINTERS@@@Z";
constexpr DWORD kAccessViolation = 0xC0000005;

using CreateReportFn = void (*)(void*, PEXCEPTION_POINTERS);
using PrepareFn = void (*)(HMODULE);
using LoadRuntimeFn = std::uint32_t (*)();
using SetCaptureProbeFn = void (*)(const wchar_t*);
using SetFailWritesFn = void (*)();
using RuntimeFaultFn = void (*)();

[[noreturn]] void Fail(const char* message) {
  std::fprintf(stderr, "host crash test: FAIL: %s\n", message);
  std::fflush(stderr);
  ExitProcess(1);
  __builtin_unreachable();
}

void Require(bool condition, const char* message) {
  if (!condition) Fail(message);
}

std::wstring ExecutableDirectory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  Require(length != 0 && length < MAX_PATH, "could not resolve the executable path");
  std::wstring full(path, length);
  const std::size_t separator = full.find_last_of(L"\\/");
  Require(separator != std::wstring::npos, "executable path has no directory");
  return full.substr(0, separator + 1);
}

void MakeDir(const std::wstring& path) {
  Require(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "could not create a scenario directory");
}

void CopyFileEx(const std::wstring& source, const std::wstring& destination) {
  Require(CopyFileW(source.c_str(), destination.c_str(), FALSE) != FALSE,
          "could not stage a file for the child");
}

void WriteMarker(const std::wstring& path, const char* text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(file != INVALID_HANDLE_VALUE, "could not create a marker file");
  DWORD written = 0;
  WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
  FlushFileBuffers(file);
  CloseHandle(file);
}

std::vector<std::uint8_t> ReadAllBytes(const wchar_t* path) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::vector<std::uint8_t> out;
  std::uint8_t buffer[4096] = {};
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE && read != 0) {
    out.insert(out.end(), buffer, buffer + read);
  }
  CloseHandle(file);
  return out;
}

std::wstring FindNewestDump(const wchar_t* dir, DWORD pid) {
  wchar_t pattern[MAX_PATH] = {};
  swprintf(pattern, MAX_PATH, L"%ls\\nevr-%lu-*.dmp", dir,
           static_cast<unsigned long>(pid));
  WIN32_FIND_DATAW data{};
  HANDLE find = FindFirstFileW(pattern, &data);
  std::wstring newest;
  FILETIME newest_time{};
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
      if (newest.empty() || CompareFileTime(&data.ftLastWriteTime, &newest_time) > 0) {
        newest = data.cFileName;
        newest_time = data.ftLastWriteTime;
      }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
  }
  if (newest.empty()) return {};
  wchar_t full[MAX_PATH] = {};
  swprintf(full, MAX_PATH, L"%ls\\%ls", dir, newest.c_str());
  return std::wstring(full);
}

bool HasDump(const wchar_t* dir, DWORD pid) {
  return !FindNewestDump(dir, pid).empty();
}

std::string ReadText(const wchar_t* path) {
  const std::vector<std::uint8_t> bytes = ReadAllBytes(path);
  return std::string(bytes.begin(), bytes.end());
}

std::wstring FindNewestText(const wchar_t* dir, DWORD pid) {
  wchar_t pattern[MAX_PATH] = {};
  swprintf(pattern, MAX_PATH, L"%ls\\nevr-%lu-*.txt", dir,
           static_cast<unsigned long>(pid));
  WIN32_FIND_DATAW data{};
  HANDLE find = FindFirstFileW(pattern, &data);
  std::wstring newest;
  FILETIME newest_time{};
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
      if (newest.empty() || CompareFileTime(&data.ftLastWriteTime, &newest_time) > 0) {
        newest = data.cFileName;
        newest_time = data.ftLastWriteTime;
      }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
  }
  if (newest.empty()) return {};
  wchar_t full[MAX_PATH] = {};
  swprintf(full, MAX_PATH, L"%ls\\%ls", dir, newest.c_str());
  return std::wstring(full);
}

bool HasText(const wchar_t* dir, DWORD pid) { return !FindNewestText(dir, pid).empty(); }

// The unhandled-exception filter for the child: write the minidump through the
// real MiniDmpSender::createReport, then terminate with the exception.
CreateReportFn g_create_report = nullptr;
LONG WINAPI ChildUnhandledFilter(PEXCEPTION_POINTERS pointers) {
  if (g_create_report != nullptr) g_create_report(nullptr, pointers);
  return EXCEPTION_EXECUTE_HANDLER;
}

__declspec(noinline) void WriteTo(std::uintptr_t address) {
  *reinterpret_cast<volatile int*>(address) = 0xDEAD;
}

void DeliberateFault() { WriteTo(0); }

int RunChild(int argc, wchar_t** argv) {
  std::wstring shim_path;
  std::wstring ready_path;
  std::wstring probe_path;
  bool vanish_shim = false;
  bool fault_in_runtime = false;
  bool reenter = false;
  bool fail_writes = false;
  for (int index = 2; index < argc; ++index) {
    if (std::wcscmp(argv[index], L"--shim") == 0) shim_path = argv[++index];
    else if (std::wcscmp(argv[index], L"--ready") == 0) ready_path = argv[++index];
    else if (std::wcscmp(argv[index], L"--vanish-shim") == 0) vanish_shim = true;
    else if (std::wcscmp(argv[index], L"--fault-in-runtime") == 0) fault_in_runtime = true;
    else if (std::wcscmp(argv[index], L"--reenter") == 0) {
      reenter = true;
      probe_path = argv[++index];
    }
    else if (std::wcscmp(argv[index], L"--fail-writes") == 0) fail_writes = true;
  }
  Require(!shim_path.empty(), "child missing --shim");
  Require(!ready_path.empty(), "child missing --ready");
  if (reenter) Require(!probe_path.empty(), "child missing --reenter probe path");

  // The game's HandleCrashDump loads dbghelp before reaching createReport.
  Require(LoadLibraryW(L"dbghelp.dll") != nullptr, "child could not load dbghelp.dll");

  HMODULE shim = LoadLibraryW(shim_path.c_str());
  Require(shim != nullptr, "child could not load the BugSplat64.dll shim");

  const auto prepare = reinterpret_cast<PrepareFn>(GetProcAddress(shim, "NevrHostTest_Prepare"));
  const auto load_runtime =
      reinterpret_cast<LoadRuntimeFn>(GetProcAddress(shim, "NevrHostTest_LoadRuntime"));
  g_create_report = reinterpret_cast<CreateReportFn>(GetProcAddress(shim, kMangledCreateReport));
  Require(prepare != nullptr && load_runtime != nullptr && g_create_report != nullptr,
          "child could not resolve the shim exports (mangled createReport or test seams)");

  prepare(shim);
  load_runtime();  // exercises the host state machine; the crash capture is independent

  // A compatible runtime (the crash fixture) installs its VEH during initialize;
  // the deliberate fault must then land INSIDE nevr.dll so the runtime's
  // BreakpointVEH attributes it (inNevrModule) and emits [NEVR.CRASH] evidence.
  RuntimeFaultFn runtime_fault = nullptr;
  if (fault_in_runtime) {
    const HMODULE runtime_module = GetModuleHandleW(L"nevr.dll");
    Require(runtime_module != nullptr, "nevr.dll was not loaded for the compatible-runtime scenario");
    runtime_fault = reinterpret_cast<RuntimeFaultFn>(
        GetProcAddress(runtime_module, "NevrHostTest_CrashRuntimeFault"));
    Require(runtime_fault != nullptr, "nevr.dll is missing NevrHostTest_CrashRuntimeFault");
  }

  if (reenter) {
    const auto set_probe =
        reinterpret_cast<SetCaptureProbeFn>(GetProcAddress(shim, "NevrHostTest_SetCaptureProbe"));
    Require(set_probe != nullptr, "shim is missing NevrHostTest_SetCaptureProbe");
    set_probe(probe_path.c_str());
  }
  if (fail_writes) {
    const auto set_fail_writes =
        reinterpret_cast<SetFailWritesFn>(GetProcAddress(shim, "NevrHostTest_SetFailWrites"));
    Require(set_fail_writes != nullptr, "shim is missing NevrHostTest_SetFailWrites");
    set_fail_writes();
  }

  // Make the shim directory unusable by renaming it away (the capture already
  // precomputed the old path in its fixed buffer, so the dump attempt there will
  // fail with PATH_NOT_FOUND). Renaming a directory containing a mapped DLL is
  // safe: the mapping pins the inode, not the name.
  if (vanish_shim) {
    std::wstring shim_dir = shim_path;
    const std::size_t separator = shim_dir.find_last_of(L"\\/");
    Require(separator != std::wstring::npos, "shim path has no directory");
    shim_dir.resize(separator);
    Require(MoveFileExW(shim_dir.c_str(), (shim_dir + L"-gone").c_str(),
                        MOVEFILE_REPLACE_EXISTING) != FALSE,
            "could not vanish the shim directory");
  }

  Require(SetUnhandledExceptionFilter(ChildUnhandledFilter) != nullptr,
          "child could not install the unhandled exception filter");

  WriteMarker(ready_path, "ready");

  // Deliberate fault on the child's main thread (or inside nevr.dll for the
  // compatible-runtime scenario). The filter writes the host dump; the runtime's
  // VEH (when loaded) writes [NEVR.CRASH] first, then the process terminates.
  if (runtime_fault != nullptr) runtime_fault();
  DeliberateFault();
  Fail("the deliberate fault did not terminate the child");
}

// A scenario: which nevr.dll fixture to stage, which destinations to block, and
// where the dump must (or must not) land.
struct Scenario {
  const wchar_t* name;
  const wchar_t* fixture;  // nullptr = no nevr.dll
  bool block_local;
  bool block_temp;
  bool vanish_shim;
  bool fault_in_runtime;    // fault inside nevr.dll (proves runtime VEH evidence)
  bool reenter;             // nested fault while reporting (guard must suppress)
  bool fail_writes;         // simulated full-disk/write failure -> text diagnostic
  enum class Destination { kLocal, kTemp, kShim, kNone } destination;
};

// Block a destination by creating "EchoVR" as a FILE under it, so the crashes
// subdirectory beneath it cannot be created or written. Deterministic under Wine
// (no reliance on POSIX-mode -> Windows-ACL mapping).
void BlockEnvDir(const std::wstring& env_dir) {
  const std::wstring blocker = env_dir + L"\\EchoVR";
  HANDLE file = CreateFileW(blocker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(file != INVALID_HANDLE_VALUE, "could not block a destination directory");
  CloseHandle(file);
}

void ValidateDump(const std::wstring& dump_path, DWORD expected_process, DWORD expected_thread) {
  const std::vector<std::uint8_t> bytes = ReadAllBytes(dump_path.c_str());
  Require(!bytes.empty(), "dump file is empty");
  MinidumpView view(bytes.data(), bytes.size());
  Require(view.Ok(), "dump is shorter than a MINIDUMP_HEADER");
  Require(view.HasSignature(), "dump does not carry the MDMP signature");
  Require(view.ValidVersion(), "dump version is not MINIDUMP_VERSION");
  std::uint32_t thread_id = 0;
  std::uint32_t code = 0;
  std::uint64_t address = 0;
  Require(view.ExceptionInfo(&thread_id, &code, &address), "dump has no exception stream");
  Require(thread_id == expected_thread, "dump names a different faulting thread");
  Require(code == kAccessViolation, "dump does not record the deliberate access violation");
  Require(view.ContainsThread(expected_thread), "dump thread list omits the faulting thread");
  const std::optional<std::uint32_t> process_id = view.ProcessId();
  Require(process_id.has_value(), "dump has no misc-info process id");
  Require(*process_id == expected_process, "dump names a different process");
  std::printf("  %-22s dump=%ls pid=%lu thread=%lu code=0x%08lX addr=0x%llX\n", "validated",
              dump_path.c_str(), static_cast<unsigned long>(*process_id),
              static_cast<unsigned long>(thread_id), static_cast<unsigned long>(code),
              static_cast<unsigned long long>(address));
}

void RunScenario(const Scenario& scenario, const std::wstring& base,
                 const std::wstring& exe_dir, const std::wstring& shim_source) {
  const std::wstring scenario_dir = base + L"\\" + scenario.name;
  const std::wstring local = scenario_dir + L"\\local";
  const std::wstring temp = scenario_dir + L"\\temp";
  const std::wstring shim_dir = scenario_dir + L"\\shim";
  MakeDir(scenario_dir);
  MakeDir(local);
  MakeDir(temp);
  MakeDir(shim_dir);

  // Stage the shim (and, for runtime scenarios, the sibling nevr.dll fixture).
  const std::wstring shim_path = shim_dir + L"\\BugSplat64.dll";
  CopyFileEx(shim_source, shim_path);
  if (scenario.fixture != nullptr) {
    const std::wstring fixture = exe_dir + L"fixtures\\" + scenario.fixture + L"\\nevr.dll";
    CopyFileEx(fixture, shim_dir + L"\\nevr.dll");
  }

  if (scenario.block_local) BlockEnvDir(local);
  if (scenario.block_temp) BlockEnvDir(temp);

  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"TEMP", temp.c_str());

  const std::wstring ready_path = scenario_dir + L"\\ready.txt";
  const std::wstring probe_path = scenario_dir + L"\\probe-count.txt";
  const std::wstring stderr_path = scenario_dir + L"\\stderr.txt";

  // Build the child command line with the scenario's fault knobs.
  std::wstring extra;
  if (scenario.vanish_shim) extra += L" --vanish-shim";
  if (scenario.fault_in_runtime) extra += L" --fault-in-runtime";
  if (scenario.reenter) extra += L" --reenter \"" + probe_path + L"\"";
  if (scenario.fail_writes) extra += L" --fail-writes";

  wchar_t command_line[2048] = {};
  swprintf(command_line, 2048, L"\"%ls\\test_host_crash.exe\" --child %ls --shim \"%ls\" --ready \"%ls\"%ls",
           exe_dir.c_str(), scenario.name, shim_path.c_str(), ready_path.c_str(), extra.c_str());

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);

  // The compatible-runtime scenario captures the child's stderr so the parent can
  // assert the runtime's own [NEVR.CRASH] evidence (VehPrintf writes to
  // STD_ERROR_HANDLE) — not just the host's minidump.
  SECURITY_ATTRIBUTES inherit{};
  inherit.nLength = sizeof(inherit);
  inherit.bInheritHandle = TRUE;
  HANDLE stderr_file = INVALID_HANDLE_VALUE;
  if (scenario.fault_in_runtime) {
    stderr_file = CreateFileW(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(stderr_file != INVALID_HANDLE_VALUE, "could not create the stderr capture file");
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nullptr;
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = stderr_file;
  }

  PROCESS_INFORMATION process = {};
  Require(CreateProcessW(nullptr, command_line, nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                         &startup, &process) != FALSE,
          "could not launch the crash child");
  const DWORD pid = process.dwProcessId;
  const DWORD main_thread = process.dwThreadId;
  CloseHandle(process.hThread);
  if (stderr_file != INVALID_HANDLE_VALUE) CloseHandle(stderr_file);

  if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 2);
    CloseHandle(process.hProcess);
    Fail("crash child timed out (capture hung or deadlocked)");
  }
  DWORD exit_code = 0;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);

  // The child reached the fault before dying: otherwise a failure here means it
  // crashed early, not that the capture failed.
  Require(ReadAllBytes(ready_path.c_str()).size() >= 5, "child never reached the fault");

  const std::wstring local_crashes = local + L"\\EchoVR\\crashes";
  const std::wstring temp_crashes = temp + L"\\EchoVR\\crashes";
  const std::wstring shim_dest = shim_dir;

  // A compatible runtime must emit its own crash evidence (the VEH's [NEVR.CRASH]
  // dump), independent of the host's BugSplat_createReport minidump.
  if (scenario.fault_in_runtime) {
    const std::string stderr_text = ReadText(stderr_path.c_str());
    Require(stderr_text.find("[NEVR.CRASH] exception name=ACCESS_VIOLATION") != std::string::npos,
            "the compatible runtime's VEH did not emit [NEVR.CRASH] evidence on stderr");
    std::printf("  %-22s runtime [NEVR.CRASH] evidence present (VEH active inside nevr.dll)\n",
                "validated");
  }

  // A second crash while the first report is being written must not recurse or
  // deadlock: the probe runs exactly once (the nested Capture is suppressed by the
  // atomic guard) and the process terminates inside the timeout.
  if (scenario.reenter) {
    Require(ReadText(probe_path.c_str()) == "1",
            "the reentrancy probe fired other than once (guard failed to suppress recursion)");
    std::printf("  %-22s reentrancy guard held (probe fired once, no recursion/deadlock)\n",
                "validated");
    return;
  }

  // Simulated full-disk / write failure: every destination's dump write fails after
  // the file opens, so Capture falls through to the fixed-size text diagnostic.
  if (scenario.fail_writes) {
    Require(HasText(local_crashes.c_str(), pid),
            "no text diagnostic was written after a simulated write failure");
    Require(!HasDump(local_crashes.c_str(), pid) && !HasDump(temp_crashes.c_str(), pid) &&
                !HasDump(shim_dest.c_str(), pid),
            "a dump was written despite the simulated write failure");
    std::printf("  %-22s write failure fell through to the text diagnostic (no dump)\n",
                "validated");
    return;
  }

  if (scenario.destination == Scenario::Destination::kNone) {
    Require(!HasDump(local_crashes.c_str(), pid) && !HasDump(temp_crashes.c_str(), pid) &&
                !HasDump(shim_dest.c_str(), pid),
            "a dump was written although every destination was unwritable");
    std::printf("  %-22s no dump (all destinations unwritable) exit=0x%lx\n",
                "validated", static_cast<unsigned long>(exit_code));
    return;
  }

  const std::wstring expected = scenario.destination == Scenario::Destination::kLocal
                                    ? local_crashes
                                    : (scenario.destination == Scenario::Destination::kTemp
                                           ? temp_crashes
                                           : shim_dest);
  const std::wstring dump = FindNewestDump(expected.c_str(), pid);
  Require(!dump.empty(), "no minidump found in the expected destination");
  ValidateDump(dump, pid, main_thread);
}

int RunSuite() {
  const std::wstring exe = ExecutableDirectory();
  const std::wstring shim_source = exe + L"BugSplat64.dll";
  Require(GetFileAttributesW(shim_source.c_str()) != INVALID_FILE_ATTRIBUTES,
          "BugSplat64.dll shim is missing from the harness directory");

  // Compute the base temp dir before any TEMP override below.
  wchar_t temp_base[MAX_PATH] = {};
  const DWORD temp_len = GetTempPathW(MAX_PATH, temp_base);
  Require(temp_len != 0 && temp_len < MAX_PATH, "no temp directory available");
  const std::wstring base = std::wstring(temp_base, temp_len) + L"nevr-host-crash-" +
                            std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(base.c_str(), nullptr);

  const Scenario scenarios[] = {
      {L"runtime-absent", nullptr, false, false, false, false, false, false,
       Scenario::Destination::kLocal},
      {L"runtime-abi-failed", L"bad-version", false, false, false, false, false, false,
       Scenario::Destination::kLocal},
      {L"runtime-init-failed", L"init-failure", false, false, false, false, false, false,
       Scenario::Destination::kLocal},
      {L"runtime-missing-entry", L"missing-entry", false, false, false, false, false, false,
       Scenario::Destination::kLocal},
      // A compatible runtime (the crash fixture) loads through the rendezvous and
      // installs its VEH; a fault inside nevr.dll must emit [NEVR.CRASH] evidence,
      // not just the host's BugSplat_createReport minidump.
      {L"runtime-good-crash", L"good", false, false, false, true, false, false,
       Scenario::Destination::kLocal},
      {L"dest-temp", nullptr, true, false, false, false, false, false,
       Scenario::Destination::kTemp},
      {L"dest-shim", nullptr, true, true, false, false, false, false,
       Scenario::Destination::kShim},
      {L"dest-none", nullptr, true, true, true, false, false, false,
       Scenario::Destination::kNone},
      // A second crash while the first report is being written: the atomic
      // reentrancy guard must suppress the nested capture (no recursion/deadlock).
      {L"reentrant-crash", nullptr, false, false, false, false, true, false,
       Scenario::Destination::kNone},
      // Simulated full-disk / write failure: the dump write fails after the file
      // opens, so Capture falls through to the text diagnostic (no dump).
      {L"write-failure", nullptr, false, false, false, false, false, true,
       Scenario::Destination::kNone},
  };

  for (const Scenario& scenario : scenarios) {
    std::printf("host crash test: [%ls]\n", scenario.name);
    RunScenario(scenario, base, exe, shim_source);
  }
  std::printf("host crash test: PASS: all %llu scenarios\n",
              static_cast<unsigned long long>(sizeof(scenarios) / sizeof(scenarios[0])));
  return 0;
}

}  // namespace

int main() {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return 2;
  const bool child = argc >= 2 && std::wcscmp(argv[1], L"--child") == 0;
  const int result = child ? RunChild(argc, argv) : RunSuite();
  LocalFree(argv);
  return result;
}

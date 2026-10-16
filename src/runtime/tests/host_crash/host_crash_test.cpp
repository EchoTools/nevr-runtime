// Child-process crash harness for the host-owned BugSplat_createReport minidump
// capture (docs/adr/0008-bugsplat-host-and-nevr-runtime.md).
//
// Each scenario runs a child process that: loads dbghelp.dll (as the game's
// HandleCrashDump already has), loads the BugSplat64.dll shim (the production host
// objects) from a per-scenario directory, runs the host runtime-load state machine
// against the sibling nevr.dll fixture, installs the unhandled-exception filter that
// calls MiniDmpSender::createReport, and then deliberately faults with an access
// violation. The parent validates that a structurally valid minidump was written to the
// expected destination, names the faulting thread and process, and carries the
// deliberate access violation. It proves the fallback chain (%LOCALAPPDATA% -> %TEMP%
// -> host directory), the total-failure path (no dump, no hang), a second crash while
// reporting, and a failed write.
//
// It also parses every JSON record the host wrote and asserts the decisions in them:
// the runtime-load outcome, each destination that failed and why, the destination that
// held the dump. When no directory is writable the records must reach the child's
// stderr, never vanish.

#include "runtime/tests/host_crash/minidump.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <dbghelp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using nevr_host_test::MinidumpView;

constexpr const char* kMangledCreateReport =
    "?createReport@MiniDmpSender@@QEAAXPEAU_EXCEPTION_POINTERS@@@Z";
constexpr DWORD kAccessViolation = 0xC0000005;

using CreateReportFn = void (*)(void*, PEXCEPTION_POINTERS);
using PrepareFn = void (*)(HMODULE);
using LoadRuntimeFn = std::uint32_t (*)();
using SetCaptureProbeFn = void (*)(const wchar_t*);
using SetFailWritesFn = void (*)();

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

void CopyFileTo(const std::wstring& source, const std::wstring& destination) {
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
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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

std::string ReadText(const std::wstring& path) {
  const std::vector<std::uint8_t> bytes = ReadAllBytes(path.c_str());
  return std::string(bytes.begin(), bytes.end());
}

std::wstring FindNewest(const wchar_t* dir, DWORD pid, const wchar_t* extension) {
  wchar_t pattern[MAX_PATH] = {};
  swprintf(pattern, MAX_PATH, L"%ls\\nevr-%lu-*.%ls", dir, static_cast<unsigned long>(pid), extension);
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

bool HasDump(const std::wstring& dir, DWORD pid) { return !FindNewest(dir.c_str(), pid, L"dmp").empty(); }
bool HasText(const std::wstring& dir, DWORD pid) { return !FindNewest(dir.c_str(), pid, L"txt").empty(); }

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
  bool reenter = false;
  bool fail_writes = false;
  for (int index = 2; index < argc; ++index) {
    if (std::wcscmp(argv[index], L"--shim") == 0) shim_path = argv[++index];
    else if (std::wcscmp(argv[index], L"--ready") == 0) ready_path = argv[++index];
    else if (std::wcscmp(argv[index], L"--vanish-shim") == 0) vanish_shim = true;
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

  const auto prepare = reinterpret_cast<PrepareFn>(GetProcAddress(shim, "nevr_host_test_Prepare"));
  const auto load_runtime =
      reinterpret_cast<LoadRuntimeFn>(GetProcAddress(shim, "nevr_host_test_LoadRuntime"));
  g_create_report = reinterpret_cast<CreateReportFn>(GetProcAddress(shim, kMangledCreateReport));
  Require(prepare != nullptr && load_runtime != nullptr && g_create_report != nullptr,
          "child could not resolve the shim exports (mangled createReport or test seams)");

  prepare(shim);
  load_runtime();  // exercises the host state machine; the crash capture is independent

  if (reenter) {
    const auto set_probe =
        reinterpret_cast<SetCaptureProbeFn>(GetProcAddress(shim, "nevr_host_test_SetCaptureProbe"));
    Require(set_probe != nullptr, "shim is missing nevr_host_test_SetCaptureProbe");
    set_probe(probe_path.c_str());
  }
  if (fail_writes) {
    const auto set_fail_writes =
        reinterpret_cast<SetFailWritesFn>(GetProcAddress(shim, "nevr_host_test_SetFailWrites"));
    Require(set_fail_writes != nullptr, "shim is missing nevr_host_test_SetFailWrites");
    set_fail_writes();
  }

  // Make the host directory unusable by renaming it away (the capture already
  // precomputed the old path in its fixed buffer, so the dump attempt there will
  // fail with PATH_NOT_FOUND). Renaming a directory containing a mapped DLL is
  // safe: the mapping pins the file, not the name.
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

  // Deliberate fault on the child's main thread. The filter writes the host dump,
  // then the process terminates.
  DeliberateFault();
  Fail("the deliberate fault did not terminate the child");
}

// A scenario: which nevr.dll fixture to stage, which destinations to block, and
// where the dump must (or must not) land.
struct Scenario {
  const wchar_t* name;
  const wchar_t* fixture;  // nullptr = no nevr.dll
  const char* load_outcome;  // the runtime_load record's outcome
  bool block_local;
  bool block_temp;
  bool vanish_shim;
  bool reenter;             // nested fault while reporting (guard must suppress)
  bool fail_writes;         // simulated full-disk/write failure -> text diagnostic
  enum class Destination { kLocal, kTemp, kHost, kNone } destination;
};

// Block a destination by creating "nevr" as a FILE under it, so the crashes
// subdirectory beneath it cannot be created or written. Deterministic under Wine
// (no reliance on POSIX-mode -> Windows-ACL mapping).
void BlockEnvDir(const std::wstring& env_dir) {
  const std::wstring blocker = env_dir + L"\\nevr";
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

// Every record of one JSONL file (or of the child's captured stderr): each line must
// parse, and each must carry the host header for this child.
void AppendRecords(const std::wstring& path, DWORD pid, std::vector<nlohmann::json>* out) {
  const std::string text = ReadText(path);
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    Require(end != std::string::npos, "a record is missing its terminating newline");
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    if (line.empty() || line[0] != '{') continue;  // the child's own printf output
    nlohmann::json record = nlohmann::json::parse(line, nullptr, false);
    Require(!record.is_discarded(), "a host record does not parse as JSON");
    if (!record.contains("tag")) continue;
    Require(record.at("tag") == "NEVR.HOST", "a host record has the wrong tag");
    Require(record.at("pid") == pid, "a host record names a different process");
    Require(record.contains("ts") && record.contains("event"), "a host record lacks ts or event");
    out->push_back(std::move(record));
  }
}

std::vector<nlohmann::json> Select(const std::vector<nlohmann::json>& records, const char* event) {
  std::vector<nlohmann::json> selected;
  for (const nlohmann::json& record : records) {
    if (record.at("event") == event) selected.push_back(record);
  }
  return selected;
}

void ValidateRecords(const Scenario& scenario, const std::vector<nlohmann::json>& records) {
  // The runtime load is recorded whatever its outcome.
  const std::vector<nlohmann::json> loads = Select(records, "runtime_load");
  Require(loads.size() == 1, "expected exactly one runtime_load record");
  Require(loads[0].at("outcome") == scenario.load_outcome, "runtime_load record has the wrong outcome");
  Require(loads[0].at("degraded_action") ==
              (std::strcmp(scenario.load_outcome, "loaded") == 0 ? "dispatch" : "run_original_only"),
          "runtime_load record names the wrong degraded action");

  // A fault raised inside the unhandled-exception filter is not dispatched to the filter
  // again: the OS ends the process. The nested capture the guard exists for therefore
  // cannot be observed end to end here (test_host_crash_capture records and asserts the
  // suppression), and this scenario's evidence is the probe count and the termination.
  if (scenario.reenter) return;

  const std::vector<nlohmann::json> crashes = Select(records, "crash_dump");
  Require(!crashes.empty(), "no crash_dump record was written");
  for (const nlohmann::json& crash : crashes) {
    Require(crash.contains("outcome") && crash.contains("thread"),
            "a crash_dump record lacks outcome or thread");
  }
  const auto count = [&crashes](const char* outcome) {
    std::size_t total = 0;
    for (const nlohmann::json& crash : crashes) total += crash.at("outcome") == outcome ? 1 : 0;
    return total;
  };

  if (scenario.fail_writes) {
    Require(count("destination_failed") == 3 && count("failed") == 1 && count("written") == 0,
            "a failed write was not recorded as three failed destinations and a failure");
    for (const nlohmann::json& crash : crashes) {
      if (crash.at("outcome") != "destination_failed") continue;
      Require(crash.at("stage") == "simulated_write_failure" && crash.at("win32_error") == ERROR_DISK_FULL,
              "a failed destination does not say why");
    }
    return;
  }
  if (scenario.destination == Scenario::Destination::kNone) {
    Require(count("written") == 0 && count("failed") == 1,
            "an unwritable run was not recorded as a failure");
    Require(count("destination_failed") == 3, "each of the three destinations did not record its failure");
    return;
  }

  Require(count("written") == 1 && count("failed") == 0, "a captured crash was not recorded as written once");
  const char* written_to = scenario.destination == Scenario::Destination::kLocal
                               ? "localappdata"
                               : (scenario.destination == Scenario::Destination::kTemp ? "temp"
                                                                                       : "host_directory");
  const std::size_t expected_failures = scenario.destination == Scenario::Destination::kLocal
                                            ? 0
                                            : (scenario.destination == Scenario::Destination::kTemp ? 1 : 2);
  Require(count("destination_failed") == expected_failures,
          "the destinations tried before the one that held the dump were not each recorded as failed");
  for (const nlohmann::json& crash : crashes) {
    if (crash.at("outcome") != "written") continue;
    Require(crash.at("destination") == written_to, "the dump was recorded at the wrong destination");
    Require(crash.at("bytes").get<std::uint64_t>() > 0 && crash.contains("path") &&
                crash.at("code") == "0xc0000005",
            "the written record lacks bytes, path or exception code");
  }
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
  CopyFileTo(shim_source, shim_path);
  if (scenario.fixture != nullptr) {
    const std::wstring fixture = exe_dir + L"fixtures\\" + scenario.fixture + L"\\nevr.dll";
    CopyFileTo(fixture, shim_dir + L"\\nevr.dll");
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
  if (scenario.reenter) extra += L" --reenter \"" + probe_path + L"\"";
  if (scenario.fail_writes) extra += L" --fail-writes";

  wchar_t command_line[2048] = {};
  swprintf(command_line, 2048, L"\"%ls\\test_host_crash.exe\" --child %ls --shim \"%ls\" --ready \"%ls\"%ls",
           exe_dir.c_str(), scenario.name, shim_path.c_str(), ready_path.c_str(), extra.c_str());

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);

  // The child's stderr is captured: when no directory is writable the host's records
  // must still reach it.
  SECURITY_ATTRIBUTES inherit{};
  inherit.nLength = sizeof(inherit);
  inherit.bInheritHandle = TRUE;
  HANDLE stderr_file = CreateFileW(stderr_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(stderr_file != INVALID_HANDLE_VALUE, "could not create the stderr capture file");
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = nullptr;
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = stderr_file;

  PROCESS_INFORMATION process = {};
  Require(CreateProcessW(nullptr, command_line, nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                         &startup, &process) != FALSE,
          "could not launch the crash child");
  const DWORD pid = process.dwProcessId;
  const DWORD main_thread = process.dwThreadId;
  CloseHandle(process.hThread);
  CloseHandle(stderr_file);

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

  const std::wstring local_crashes = local + L"\\nevr\\echovr\\crashes";
  const std::wstring temp_crashes = temp + L"\\nevr\\echovr\\crashes";
  const std::wstring host_dest = shim_dir;

  // Every record the child wrote, from wherever it landed.
  std::vector<nlohmann::json> records;
  AppendRecords(local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl", pid, &records);
  AppendRecords(temp + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl", pid, &records);
  AppendRecords(shim_dir + L"\\nevr-bootstrap.jsonl", pid, &records);
  AppendRecords(shim_dir + L"-gone\\nevr-bootstrap.jsonl", pid, &records);
  AppendRecords(stderr_path, pid, &records);
  ValidateRecords(scenario, records);
  std::printf("  %-22s %zu host records parsed (runtime_load=%s)\n", "validated", records.size(),
              scenario.load_outcome);

  // A second crash while the first report is being written must not recurse or
  // deadlock: the probe runs exactly once (the nested Capture is suppressed by the
  // atomic guard) and the process terminates inside the timeout.
  if (scenario.reenter) {
    Require(ReadText(probe_path) == "1",
            "the reentrancy probe fired other than once (guard failed to suppress recursion)");
    std::printf("  %-22s reentrancy guard held (probe fired once, no recursion/deadlock)\n",
                "validated");
    return;
  }

  // Simulated full-disk / write failure: every destination's dump write fails after
  // the file opens, so Capture falls through to the fixed-size text diagnostic.
  if (scenario.fail_writes) {
    Require(HasText(local_crashes, pid),
            "no text diagnostic was written after a simulated write failure");
    Require(!HasDump(local_crashes, pid) && !HasDump(temp_crashes, pid) && !HasDump(host_dest, pid),
            "a dump was written despite the simulated write failure");
    std::printf("  %-22s write failure fell through to the text diagnostic (no dump)\n",
                "validated");
    return;
  }

  if (scenario.destination == Scenario::Destination::kNone) {
    Require(!HasDump(local_crashes, pid) && !HasDump(temp_crashes, pid) && !HasDump(host_dest, pid),
            "a dump was written although every destination was unwritable");
    // Nothing was writable, so the crash records could only have reached stderr.
    std::vector<nlohmann::json> on_stderr;
    AppendRecords(stderr_path, pid, &on_stderr);
    Require(!Select(on_stderr, "crash_dump").empty(),
            "the crash records were lost although no directory was writable");
    std::printf("  %-22s no dump (all destinations unwritable), records reached stderr, exit=0x%lx\n",
                "validated", static_cast<unsigned long>(exit_code));
    return;
  }

  const std::wstring expected = scenario.destination == Scenario::Destination::kLocal
                                    ? local_crashes
                                    : (scenario.destination == Scenario::Destination::kTemp
                                           ? temp_crashes
                                           : host_dest);
  const std::wstring dump = FindNewest(expected.c_str(), pid, L"dmp");
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
                            std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GetTickCount64());
  CreateDirectoryW(base.c_str(), nullptr);

  using Destination = Scenario::Destination;
  const Scenario scenarios[] = {
      {L"runtime-absent", nullptr, "runtime_absent", false, false, false, false, false,
       Destination::kLocal},
      {L"runtime-abi-failed", L"bad-version", "abi_rejected", false, false, false, false, false,
       Destination::kLocal},
      {L"runtime-init-failed", L"init-failure", "init_failed", false, false, false, false, false,
       Destination::kLocal},
      {L"runtime-missing-entry", L"missing-entry", "missing_entry", false, false, false, false, false,
       Destination::kLocal},
      // A compatible runtime: the host dump is written all the same.
      {L"runtime-good", L"good", "loaded", false, false, false, false, false, Destination::kLocal},
      {L"dest-temp", nullptr, "runtime_absent", true, false, false, false, false, Destination::kTemp},
      {L"dest-host", nullptr, "runtime_absent", true, true, false, false, false, Destination::kHost},
      {L"dest-none", nullptr, "runtime_absent", true, true, true, false, false, Destination::kNone},
      // A second crash while the first report is being written: the atomic
      // reentrancy guard must suppress the nested capture (no recursion/deadlock).
      {L"reentrant-crash", nullptr, "runtime_absent", false, false, false, true, false,
       Destination::kNone},
      // Simulated full-disk / write failure: the dump write fails after the file
      // opens, so Capture falls through to the text diagnostic (no dump).
      {L"write-failure", nullptr, "runtime_absent", false, false, false, false, true,
       Destination::kNone},
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

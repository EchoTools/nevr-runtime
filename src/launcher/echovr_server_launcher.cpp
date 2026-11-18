// echovr_server.exe — thin Windows launcher for Echo VR dedicated server.
// Locates echovr.exe relative to itself and spawns it with -server -headless -no-console.
// For Windows users: drop next to echovr.exe and double-click to start a server.
// While the server runs, the launcher's window shows the server's own log (#1246): the run log under the nevr root.
//
// Hard-Stops (CPP-MINGW-ADDENDUM-GENERIC.md): no C-style casts, no catch(...),
// no throw across DLL boundary (N/A — standalone EXE), no I/O from DllMain
// (N/A — main(), not a DLL).

#include <cstdio>
#include <cstdlib>

#include <map>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX  // nlohmann-json (boot_replay.h) pulls <limits>; windows.h's min/max macros would break it
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core/nevr_root.h"
#include "core/run_id.h"
#include "launcher/launcher_core.h"
#include "launcher/server_log.h"
#include "runtime/log/boot_replay.h"

namespace {

// How often the launcher looks at the run log while the server runs.
constexpr DWORD kLogPollMs = 500;

// The file system's last-write time is coarser than the process's creation time, so a run log whose last write is up to
// this much earlier than the start still counts as this run's (2 s, in FILETIME ticks).
constexpr unsigned long long kStartSlackTicks = 20000000ULL;

// Returns the directory containing this executable (no trailing backslash).
// On failure, returns an empty string.
std::string GetExeDirectory() {
  char buffer[MAX_PATH];
  DWORD len = GetModuleFileNameA(nullptr, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) {
    return {};
  }
  return nevr_launcher::DirectoryOfPath(std::string(buffer, len));
}

void PrintError(const char* msg, DWORD err) {
  std::fprintf(stderr, "[echovr_server] ERROR: %s (code: %lu)\n", msg,
               static_cast<unsigned long>(err));
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::string exe_dir = GetExeDirectory();
  if (exe_dir.empty()) {
    std::fprintf(stderr,
                 "[echovr_server] ERROR: cannot determine launcher directory\n");
    return 1;
  }

  const std::string game_path = exe_dir + "\\echovr.exe";

  if (GetFileAttributesA(game_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    std::fprintf(stderr,
                 "[echovr_server] ERROR: echovr.exe not found at: %s\n",
                 game_path.c_str());
    std::fprintf(stderr,
                 "[echovr_server] Place this launcher next to echovr.exe.\n");
    return 1;
  }

  // -headless is required when -no-console is passed on native Windows.
  // Build command line: echovr.exe -server -headless -no-console [user args...]
  // Each argument is Windows-quoted so paths with spaces (e.g. -config-file
  // "C:\Program Files\Echo VR\config.yaml") are not split by CommandLineToArgvW.
  std::string cmd_line = "echovr.exe -server -headless -no-console";
  for (int i = 1; i < argc; ++i) {
    cmd_line += " ";
    cmd_line += nevr_launcher::QuoteArgForWindows(argv[i]);
  }

  std::fprintf(stderr, "[echovr_server] Launching: %s\n", cmd_line.c_str());
  std::fprintf(stderr, "[echovr_server] Game path: %s\n", game_path.c_str());

  // CreateProcess requires a mutable command-line buffer.
  std::vector<char> cmd_buf(cmd_line.begin(), cmd_line.end());
  cmd_buf.push_back('\0');

  STARTUPINFOA si = {};
  si.cb = sizeof(si);

  PROCESS_INFORMATION pi = {};

  // One window shows the server's output: this launcher's own console, which the child inherits (no
  // CREATE_NEW_CONSOLE). The server is started with -no-console, so it opens no console of its own, and the
  // run log (below) carries its records to this window (#1246).
  BOOL ok = CreateProcessA(
      game_path.c_str(),    // lpApplicationName
      cmd_buf.data(),       // lpCommandLine
      nullptr,              // lpProcessAttributes
      nullptr,              // lpThreadAttributes
      FALSE,                // bInheritHandles
      0,                    // dwCreationFlags
      nullptr,              // lpEnvironment
      exe_dir.c_str(),      // lpCurrentDirectory
      &si,                  // lpStartupInfo
      &pi                   // lpProcessInformation
  );

  if (!ok) {
    PrintError("CreateProcess failed", GetLastError());
    return 1;
  }

  CloseHandle(pi.hThread);

  // The run ID (N80) of the child names its records; the run log is the newest file of this run in the nevr logs folder.
  const unsigned long long created = nevr_run_id::CreationTime(pi.hProcess);
  char runId[40] = {};
  nevr_run_id::Format(pi.dwProcessId, created, runId, sizeof(runId));
  const std::string logsDir = nevr_root::LogsDir(nevr_root::RootA());

  std::fprintf(stderr, "[echovr_server] Server started (PID: %lu, run: %s). Logs: %s\n",
               static_cast<unsigned long>(pi.dwProcessId), runId, logsDir.c_str());

  const bool follow = !logsDir.empty() && created != 0;
  if (logsDir.empty()) {
    std::fprintf(stderr, "[echovr_server] ERROR: no %%LOCALAPPDATA%%, so the server's log cannot be found; its lines are not shown\n");
  }
  if (created == 0) {
    PrintError("GetProcessTimes failed for the server", GetLastError());
  }
  nevr_launcher::RunLogFollower follower(logsDir, runId, created > kStartSlackTicks ? created - kStartSlackTicks : 0);

  // Wait for the server, showing its run log as it is written until it exits.
  for (;;) {
    const DWORD wait = WaitForSingleObject(pi.hProcess, kLogPollMs);
    if (follow) follower.Poll();
    if (wait == WAIT_OBJECT_0) break;
    if (wait != WAIT_TIMEOUT) {
      PrintError("WaitForSingleObject failed", GetLastError());
      CloseHandle(pi.hProcess);
      return 1;
    }
  }
  if (follow) follower.Poll();  // the records written before the exit

  DWORD exit_code = 0;
  if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
    PrintError("GetExitCodeProcess failed", GetLastError());
    CloseHandle(pi.hProcess);
    return 1;
  }
  CloseHandle(pi.hProcess);

  std::fprintf(stderr, "[echovr_server] Server exited with code: %lu\n",
               static_cast<unsigned long>(exit_code));

  return static_cast<int>(exit_code);
}

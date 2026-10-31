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
  std::string full(buffer, len);
  auto pos = full.rfind('\\');
  if (pos == std::string::npos) {
    return {};
  }
  return full.substr(0, pos);
}

void PrintError(const char* msg, DWORD err) {
  std::fprintf(stderr, "[echovr_server] ERROR: %s (code: %lu)\n", msg,
               static_cast<unsigned long>(err));
}

// Quote an argument for Windows CommandLineToArgvW rules.
// Returns the argument quoted-and-escaped if it contains characters that would
// be misinterpreted (whitespace, double-quotes, or empty), or verbatim if not.
// Rules (from CommandLineToArgvW documentation):
//   1. Whitespace (space, tab) or empty -> wrap in double quotes.
//   2. Backslashes before a double-quote are doubled to prevent escape.
//   3. A double-quote is escaped as \" inside a quoted argument.
//   4. Trailing backslashes before the closing quote are doubled.
std::string QuoteArgForWindows(const std::string& arg) {
  if (!arg.empty() &&
      arg.find_first_of(" \t\"") == std::string::npos) {
    return arg;
  }

  std::string result = "\"";
  size_t i = 0;
  while (i < arg.size()) {
    size_t num_backslashes = 0;
    while (i < arg.size() && arg[i] == '\\') {
      ++i;
      ++num_backslashes;
    }

    if (i >= arg.size()) {
      // End of string: double trailing backslashes so the closing double-quote
      // is not consumed as part of an escape sequence.
      result.append(num_backslashes * 2, '\\');
    } else if (arg[i] == '"') {
      // Before a literal double-quote: double the backslashes and escape the
      // quote so CommandLineToArgvW treats it as a literal character.
      result.append(num_backslashes * 2, '\\');
      result.append("\\\"");
      ++i;
    } else {
      result.append(num_backslashes, '\\');
      result += arg[i];
      ++i;
    }
  }
  result += '"';
  return result;
}

// The files of the logs folder, with their last-write times. An absent folder lists nothing: the server creates it.
std::vector<nevr_server_log::LogFileInfo> ListLogFiles(const std::string& dir) {
  std::vector<nevr_server_log::LogFileInfo> files;
  WIN32_FIND_DATAA data{};
  HANDLE find = FindFirstFileA((dir + "\\*.jsonl").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return files;
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    ULARGE_INTEGER written{};
    written.LowPart = data.ftLastWriteTime.dwLowDateTime;
    written.HighPart = data.ftLastWriteTime.dwHighDateTime;
    files.push_back({data.cFileName, written.QuadPart});
  } while (FindNextFileA(find, &data));
  FindClose(find);
  return files;
}

// Shows this run's records as the server writes them. Each poll reads every run log of the folder that was written since
// the start, from where the previous poll stopped (nevr_boot_replay::ReadNew keeps the records of the one run); a file of
// another run yields no records. Rotation starts a new file for the same run, and it is followed the same way.
class RunLogFollower {
 public:
  RunLogFollower(std::string dir, std::string runId, unsigned long long since)
      : dir_(std::move(dir)), runId_(std::move(runId)), since_(since) {}

  void Poll() {
    for (const std::string& name : nevr_server_log::RunLogCandidates(ListLogFiles(dir_), since_)) {
      Followed& followed = followed_[name];
      const std::string path = dir_ + "\\" + name;
      std::vector<nevr_boot_replay::Line> lines;
      int err = 0;
      if (!nevr_boot_replay::ReadNew(path.c_str(), runId_, followed.cursor, lines, &err)) {
        if (!followed.unreadableReported) {
          std::fprintf(stderr, "[echovr_server] ERROR: log file %s is not readable (errno: %d); its lines are not shown\n",
                       path.c_str(), err);
          followed.unreadableReported = true;
        }
        continue;
      }
      if (!lines.empty() && !followed.announced) {
        std::fprintf(stderr, "[echovr_server] Log file: %s\n", path.c_str());
        followed.announced = true;
      }
      for (const nevr_boot_replay::Line& line : lines) {
        const std::string text = nevr_server_log::RenderLine(line);
        if (!text.empty()) std::printf("%s\n", text.c_str());
      }
      std::fflush(stdout);
    }
  }

 private:
  struct Followed {
    nevr_boot_replay::Cursor cursor;
    bool announced = false;
    bool unreadableReported = false;
  };

  std::string dir_;
  std::string runId_;
  unsigned long long since_;
  std::map<std::string, Followed> followed_;
};

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
    cmd_line += QuoteArgForWindows(argv[i]);
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
  RunLogFollower follower(logsDir, runId, created > kStartSlackTicks ? created - kStartSlackTicks : 0);

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

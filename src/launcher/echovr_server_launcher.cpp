// echovr_server.exe — thin Windows launcher for Echo VR dedicated server.
// Locates echovr.exe relative to itself and spawns it with -server -noconsole.
// For Windows users: drop next to echovr.exe and double-click to start a server.
//
// Hard-Stops (CPP-MINGW-ADDENDUM-GENERIC.md): no C-style casts, no catch(...),
// no throw across DLL boundary (N/A — standalone EXE), no I/O from DllMain
// (N/A — main(), not a DLL).

#include <cstdio>
#include <cstdlib>

#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

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

  // Build command line: echovr.exe -server -noconsole [user args...]
  std::string cmd_line = "echovr.exe -server -noconsole";
  for (int i = 1; i < argc; ++i) {
    cmd_line += " ";
    cmd_line += argv[i];
  }

  std::fprintf(stderr, "[echovr_server] Launching: %s\n", cmd_line.c_str());
  std::fprintf(stderr, "[echovr_server] Game path: %s\n", game_path.c_str());

  // CreateProcess requires a mutable command-line buffer.
  std::vector<char> cmd_buf(cmd_line.begin(), cmd_line.end());
  cmd_buf.push_back('\0');

  STARTUPINFOA si = {};
  si.cb = sizeof(si);

  PROCESS_INFORMATION pi = {};

  // CREATE_NEW_CONSOLE: echovr.exe is linked with /SUBSYSTEM:WINDOWS, so
  // without this flag its stdout/stderr would be invisible. The new console
  // gives the user a window where all [NEVR] log output appears.
  BOOL ok = CreateProcessA(
      game_path.c_str(),    // lpApplicationName
      cmd_buf.data(),       // lpCommandLine
      nullptr,              // lpProcessAttributes
      nullptr,              // lpThreadAttributes
      FALSE,                // bInheritHandles
      CREATE_NEW_CONSOLE,   // dwCreationFlags
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

  std::fprintf(stderr, "[echovr_server] Server started (PID: %lu). "
                       "Waiting for shutdown...\n",
               static_cast<unsigned long>(pi.dwProcessId));

  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exit_code = 0;
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hProcess);

  std::fprintf(stderr, "[echovr_server] Server exited with code: %lu\n",
               static_cast<unsigned long>(exit_code));

  return static_cast<int>(exit_code);
}

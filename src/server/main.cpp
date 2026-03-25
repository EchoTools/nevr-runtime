/// echovr_server.exe — thin wrapper that launches echovr.exe in dedicated server mode.
///
/// Launches echovr.exe with CREATE_NO_WINDOW to prevent the console window that
/// would otherwise block the server loop when clicked (Windows Quick Edit Mode).
///
/// Implicitly passes: -server -noconsole
/// (-server implies -headless and -noovr via gamepatches)
/// Any additional arguments are forwarded to echovr.exe.
///
/// Usage:
///   echovr_server.exe                          # defaults
///   echovr_server.exe -timestep 120 -region us # extra flags forwarded

#include <windows.h>

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

static const wchar_t* kDefaultFlags[] = {
    L"-server",
    L"-noconsole",
};

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  // Parse our own command line to forward extra args
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  // Locate echovr.exe next to this executable
  wchar_t exePath[MAX_PATH];
  GetModuleFileNameW(NULL, exePath, MAX_PATH);
  fs::path exeDir = fs::path(exePath).parent_path();
  fs::path gamePath = exeDir / "echovr.exe";

  if (!fs::exists(gamePath)) {
    gamePath = exeDir / "bin" / "win10" / "echovr.exe";
  }

  if (!fs::exists(gamePath)) {
    MessageBoxA(NULL, "echovr.exe not found.\n\nPlace echovr_server.exe next to echovr.exe.",
                "echovr_server", MB_ICONERROR);
    LocalFree(argv);
    return 1;
  }

  // Build command line: "path\to\echovr.exe" -server -headless -noovr -noconsole [user args...]
  std::wstring cmdLine = L"\"" + gamePath.wstring() + L"\"";

  for (const auto* flag : kDefaultFlags) {
    cmdLine += L" ";
    cmdLine += flag;
  }

  // Forward any extra arguments (skip argv[0])
  for (int i = 1; i < argc; ++i) {
    cmdLine += L" ";
    cmdLine += argv[i];
  }

  LocalFree(argv);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};

  // CREATE_NO_WINDOW prevents echovr.exe from getting a console window.
  // Without this, the console's Quick Edit Mode freezes the entire server
  // loop when a user clicks in the window.
  if (!CreateProcessW(
          gamePath.c_str(),
          cmdLine.data(),
          NULL, NULL, FALSE,
          CREATE_NO_WINDOW,
          NULL,
          exeDir.c_str(),
          &si, &pi)) {
    MessageBoxA(NULL, "Failed to launch echovr.exe.", "echovr_server", MB_ICONERROR);
    return 1;
  }

  // Wait for the game process to exit and propagate its exit code
  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);

  return static_cast<int>(exitCode);
}

#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Find the launcher (echovr.exe) next to this binary
    char exeDir[MAX_PATH];
    GetModuleFileNameA(NULL, exeDir, MAX_PATH);
    char* lastSlash = strrchr(exeDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';

    std::string launcherPath = std::string(exeDir) + "echovr.exe";
    if (!fs::exists(launcherPath)) {
        MessageBoxA(NULL, "echovr.exe not found.", "echovr_server", MB_ICONERROR);
        return 1;
    }

    // Build command line: echovr.exe -server [forwarded args]
    std::wstring cmdLine = L"\"" + fs::path(launcherPath).wstring() + L"\" -server";

    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; i++) {
        cmdLine += L" \"";
        cmdLine += argv[i];
        cmdLine += L"\"";
    }
    LocalFree(argv);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(NULL, cmdLine.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        MessageBoxA(NULL, "Failed to launch echovr.exe.", "echovr_server", MB_ICONERROR);
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
}

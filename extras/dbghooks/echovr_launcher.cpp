#include <stdio.h>
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

#include "static_entry.h"

namespace fs = std::filesystem;

typedef void (*InitializeFunc)();

void PrintError(const char* msg) {
  DWORD err = GetLastError();
  fprintf(stderr, "[ERROR] %s (code: %lu)\n", msg, err);
}

void Log(const char* format, ...) {
  va_list args;
  va_start(args, format);
  printf("[Launcher] ");
  vprintf(format, args);
  printf("\n");
  va_end(args);
}

int main(int argc, char* argv[]) {
  printf("=============================================================================\n");
  printf("Echo VR DLL Launcher\n");
  printf("Loads echovr.dll and injects patches in-process\n");
  printf("=============================================================================\n\n");

  char buffer[MAX_PATH];
  GetModuleFileNameA(NULL, buffer, MAX_PATH);
  fs::path exeDir = fs::path(buffer).parent_path();

  std::string echovrDllPath = (exeDir / "echovr.dll").string();
  std::string gamepatchesPath = (exeDir / "GamePatches.dll").string();

  if (!fs::exists(fs::path(echovrDllPath)) && fs::exists(exeDir / "bin" / "echovr.dll")) {
    echovrDllPath = (exeDir / "bin" / "echovr.dll").string();
  }
  if (!fs::exists(fs::path(gamepatchesPath))) {
    if (fs::exists(exeDir / "bin" / "GamePatches.dll")) {
      gamepatchesPath = (exeDir / "bin" / "GamePatches.dll").string();
    } else if (fs::exists(exeDir / "dbgcore.dll")) {
      gamepatchesPath = (exeDir / "dbgcore.dll").string();
    } else if (fs::exists(exeDir / "bin" / "dbgcore.dll")) {
      gamepatchesPath = (exeDir / "bin" / "dbgcore.dll").string();
    }
  }

  try {
    if (fs::exists(fs::path(echovrDllPath))) echovrDllPath = fs::absolute(fs::path(echovrDllPath)).string();
    if (fs::exists(fs::path(gamepatchesPath))) gamepatchesPath = fs::absolute(fs::path(gamepatchesPath)).string();
  } catch (...) {
  }

  Log("Target: %s", echovrDllPath.c_str());

  if (fs::exists(fs::path(gamepatchesPath))) {
    Log("Loading GamePatches: %s", gamepatchesPath.c_str());
    HMODULE hPatches = LoadLibraryA(gamepatchesPath.c_str());
    if (!hPatches) {
      PrintError("Failed to load GamePatches.dll");
    }
  } else {
    Log("Warning: GamePatches.dll not found");
  }

  // DbgHooks is now statically linked and initialized after EchoVR loads

  if (!fs::exists(fs::path(echovrDllPath))) {
    PrintError("echovr.dll not found! Please convert echovr.exe to echovr.dll first.");
    return 1;
  }

  Log("Loading EchoVR DLL...");
  HMODULE hGame = LoadLibraryA(echovrDllPath.c_str());
  if (!hGame) {
    PrintError("Failed to load echovr.dll");
    return 1;
  }

  Log("Echo VR loaded. Installing static hooks...");
  dbghooks::InitializeStatic(hGame);

  typedef void (*StartFunc)();
  StartFunc Start = (StartFunc)GetProcAddress(hGame, "Start");
  if (Start) {
    Log("Calling Start...");
    Start();
  } else {
    PrintError("Failed to find Start function in echovr.dll");
  }

  Log("Keeping process alive...");

  Sleep(INFINITE);

  dbghooks::ShutdownStatic();

  return 0;
}

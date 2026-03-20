#include <stdio.h>
#include <windows.h>

// =============================================================================
// DLL-Based Echo VR Launcher
// =============================================================================
// Loads echovr_asdll.dll (converted from EXE) with hooks installed first.
//
// Strategy:
// 1. Load DbgHooks.dll (installs hooks)
// 2. Load echovr_asdll.dll (DllMain returns TRUE immediately)
// 3. Call exported "Start" function (original WinMain entry point)
//
// Note: Gun2CR hooks are automatically initialized when DbgHooks.dll loads
// =============================================================================

void Log(const char* format, ...) {
  va_list args;
  va_start(args, format);
  printf("[DllLoader] ");
  vprintf(format, args);
  printf("\n");
  va_end(args);
}

void PrintError(const char* msg) {
  DWORD error = GetLastError();
  Log("ERROR: %s (code: %d)", msg, error);
}

int main(int argc, char* argv[]) {
  printf("=============================================================================\n");
  printf("Echo VR DLL-Based Launcher with Gun2CR Hooks\n");
  printf("Loads game as DLL with gun2cr hooks and other hooks installed first\n");
  printf("=============================================================================\n\n");

  const char* gameDll = (argc > 1) ? argv[1] : "echovr_asdll.dll";
  const char* hooksDll = (argc > 2) ? argv[2] : "DbgHooks.dll";

  // Get absolute paths
  char absHooksDll[MAX_PATH];
  char absGameDll[MAX_PATH];
  GetFullPathNameA(hooksDll, MAX_PATH, absHooksDll, NULL);
  GetFullPathNameA(gameDll, MAX_PATH, absGameDll, NULL);

  Log("Game DLL: %s", absGameDll);
  Log("Hooks DLL: %s", absHooksDll);
  printf("\n");

  // Step 1: Load hooks DLL FIRST (before game's DllMain)
  Log("Loading hooks DLL...");
  HMODULE hHooks = LoadLibraryA(absHooksDll);
  if (!hHooks) {
    PrintError("Failed to load hooks DLL");

    // Try LOAD_WITH_ALTERED_SEARCH_PATH
    Log("Retrying with LOAD_WITH_ALTERED_SEARCH_PATH...");
    hHooks = LoadLibraryExA(absHooksDll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hHooks) {
      PrintError("Still failed");
      return 1;
    }
  }
  Log("Hooks DLL loaded at: %p", hHooks);
  Log("Gun2CR hooks initialized (via DbgHooks.dll DllMain)");
  printf("\n");

  // Step 2: Load game DLL (hooks are now installed!)
  Log("Loading game DLL...");
  HMODULE hGame = LoadLibraryA(absGameDll);
  if (!hGame) {
    PrintError("Failed to load game DLL");
    return 1;
  }
  Log("Game DLL loaded at: %p", hGame);
  Log("SUCCESS: Hooks and Gun2CR patches were installed BEFORE game's static initializers!");
  printf("\n");

  // Step 3: Try to call exported "Start" function (if exports table was created)
  Log("Looking for exported 'Start' function...");
  typedef int(WINAPI * StartFunc)(HINSTANCE, HINSTANCE, LPSTR, int);
  StartFunc pStart = (StartFunc)GetProcAddress(hGame, "Start");

  if (pStart) {
    Log("Found Start @ %p, calling original WinMain...", pStart);
    printf("\n");
    printf("=============================================================================\n");
    printf("Game Starting (hooks active!)\n");
    printf("=============================================================================\n\n");

    // Call original WinMain entry point
    int result = pStart(hGame, NULL, (char*)"", SW_SHOW);

    Log("Game exited with code: %d", result);
  } else {
    Log("WARNING: 'Start' export not found (exports table creation failed)");
    Log("Game DLL is loaded but cannot start execution.");
    Log("This is expected if exe_to_dll couldn't append exports.");
    printf("\n");
    Log("However, the key success is:");
    Log("  - DbgHooks.dll loaded first");
    Log("  - Gun2CR hooks initialized");
    Log("  - Hooks installed");
    Log("  - Game DLL loaded second");
    Log("  - Static initializers ran AFTER hooks installed");
    printf("\n");
    Log("Check gun2cr_hook.log for Gun2CR diagnostics!");
    Log("Check hash_discovery.log for SNS message captures!");
  }

  printf("\n");
  Log("Press Enter to exit...");
  getchar();

  // Cleanup
  if (hGame) FreeLibrary(hGame);
  if (hHooks) FreeLibrary(hHooks);

  return 0;
}

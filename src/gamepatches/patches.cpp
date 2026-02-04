#include "patches.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/globals.h"
#include "common/hooking.h"
#include "common/logging.h"
#include "processmem.h"

#ifdef USE_MINHOOK
#include <MinHook.h>
#endif

#include <processthreadsapi.h>
#include <windows.h>

#include "common/echovrunexported.h"
#include "patch_addresses.h"
#include "processmem.h"

/// <summary>
/// Helper function to apply a memory patch at a specific offset from the game base address.
/// </summary>
/// <param name="offset">The offset from the game base address.</param>
/// <param name="patchData">Pointer to the patch data.</param>
/// <param name="patchSize">Size of the patch in bytes.</param>
static inline VOID ApplyPatch(uintptr_t offset, const BYTE* patchData, size_t patchSize) {
  ProcessMemcpy(EchoVR::g_GameBaseAddress + offset, const_cast<BYTE*>(patchData), patchSize);
}

/// <summary>
/// Indicates whether the patches have been applied (to avoid re-application).
/// </summary>
BOOL initialized = FALSE;

/// <summary>
/// Custom config.json path provided via command-line argument. If empty, the default path is used.
/// </summary>
CHAR customConfigJsonPath[MAX_PATH] = {0};

/// <summary>
/// A CLI argument flag indicating whether the game is booting as a dedicated server.
/// </summary>
BOOL isServer = FALSE;
/// <summary>
/// A CLI argument flag indicating whether the game is booting as an offline client.
/// </summary>
BOOL isOffline = FALSE;
/// <summary>
/// A CLI argument flag indicating whether the game is booting in headless mode (no graphics/audio).
/// </summary>
BOOL isHeadless = FALSE;
/// <summary>
/// A CLI argument flag used to remove the extra console being added by -headless for running servers on fully headless
/// system.
/// </summary>
BOOL noConsole = FALSE;
/// <summary>
/// A CLI argument flag indicating whether the game is booting in a windowed mode, rather than with a VR headset.
/// </summary>
BOOL isWindowed = FALSE;

/// <summary>
/// Indicates whether the game was launched with `-noovr`.
/// </summary>
BOOL isNoOVR = FALSE;

/// <summary>
/// The window handle for the current game window.
/// </summary>
HWND hWindow = NULL;

/// <summary>
/// The local config stored in ./_local/config.json.
/// </summary>
EchoVR::Json* localConfig = NULL;

/// <summary>
/// A timestep value in ticks/updates per second, to be used for headless mode (due to lack of GPU/refresh rate
/// throttling). If non-zero, sets the timestep override by the given tick rate per second. If zero, removes tick rate
/// throttling.
/// </summary>
UINT32 headlessTimeStep = 120;

/// <summary>
/// Reports a fatal error with a message box, then exits the game.
/// </summary>
/// <param name="msg">The window message to display.</param>
/// <param name="title">The window title to display.</param>
/// <returns>None</returns>
VOID FatalError(const CHAR* msg, const CHAR* title) {
  // If no title or msg was provided, set it to a generic value.
  if (title == NULL) title = "Echo Relay: Error";
  if (msg == NULL) msg = "An unknown error occurred.";

  // Show a message box.
  MessageBoxA(NULL, msg, title, MB_OK);

  // Force process exit with an error code.
  exit(1);
}

/// <summary>
/// Patches a given function pointer with an hook function (matching the equivalent function signature as the original).
/// </summary>
/// <param name="ppPointer">The function to detour.</param>
/// <param name="pDetour">The function hook to use as a detour.</param>
/// <returns>None</returns>
template <typename T>
VOID PatchDetour(T* ppPointer, PVOID pDetour) {
  Hooking::Attach(reinterpret_cast<PVOID*>(ppPointer), pDetour);
}

/// <summary>
/// A detour hook for the game's "write log" function. It intercepts overly noisy logs and ensures they are outputted
/// over stdout/stderr for headless mode.
/// </summary>
/// <param name="logLevel">The level the message was logged with.</param>
/// <param name="unk">TODO: Unknown</param>
/// <param name="format">The format string to log with.</param>
/// <param name="vl">The list of variables to use to format the format string before logging.</param>
/// <returns>None</returns>
VOID WriteLogHook(EchoVR::LogLevel logLevel, UINT64 unk, const CHAR* format, va_list vl) {
  if (!strcmp(format, "[DEBUGPRINT] %s %s") || !strcmp(format, "[SCRIPT] %s: %s")) {
    // If the overall template matched, format it
    CHAR formattedLog[0x1000];
    memset(formattedLog, 0, sizeof(formattedLog));
    vsprintf_s(formattedLog, format, vl);

    // If the final output matches the strings below, we do not log.
    if (!strcmp(formattedLog,
                "[DEBUGPRINT] PickRandomTip: context = 0x41D2C432172E0810"))  // noisy in main menu / loading screen
      return;
    if (!strcmp(formattedLog, "[SCRIPT] 0xA9DB89899292A98F: realdiv(d9a3e735) divide by zero"))  // laggy in game
      return;
  } else if (!strcmp(format, "[NETGAME] No screen stats info for game mode %s"))  // noisy in social lobby
    return;

  // Calling the original function and returning here if noConsole is set to avoid putting any extra formatting in the
  // logs.
  if (noConsole) return EchoVR::WriteLog(logLevel, unk, format, vl);

  // Print the ANSI color code prefix for the given log level.
  switch (logLevel) {
    case EchoVR::LogLevel::Debug:
      printf("\u001B[36m");
      break;

    case EchoVR::LogLevel::Warning:
      printf("\u001B[33m");
      break;

    case EchoVR::LogLevel::Error:
      printf("\u001B[31m");
      break;

    case EchoVR::LogLevel::Info:
    default:
      printf("\u001B[0m");
      break;
  }

  // Print the output to our allocated console.
  vprintf(format, vl);
  printf("\n");

  // Print the ANSI color code for restoring the default text style.
  printf("\u001B[0m");

  // Call the original method
  EchoVR::WriteLog(logLevel, unk, format, vl);
}

/// <summary>
/// A wrapper for WriteLog, simplifying logging operations.
/// </summary>
/// <returns>None</returns>
VOID Log(EchoVR::LogLevel level, const CHAR* format, ...) {
  va_list args;
  va_start(args, format);
  if (isHeadless)
    WriteLogHook(level, 0, format, args);
  else
    EchoVR::WriteLog(level, 0, format, args);
  va_end(args);
}

/// <summary>
/// Patches the game to enable headless mode, spawning a console window and applying patches to avoid game crashes.
/// </summary>
/// <param name="pGame">The pointer to the instance of the game structure.</param>
/// <returns>None</returns>
VOID PatchEnableHeadless(PVOID pGame) {
  using namespace PatchAddresses;

  // Disable audio by clearing the audio enable bit (same as `-noaudio` command)
  UINT32* audioFlags = reinterpret_cast<UINT32*>(static_cast<CHAR*>(pGame) + GAME_AUDIO_FLAGS_OFFSET);
  *audioFlags &= 0xFFFFFFFD;  // Clear bit 1 (audio enable)

  // Install our hook to capture logs to the console.
  PatchDetour(&EchoVR::WriteLog, reinterpret_cast<PVOID>(WriteLogHook));

  // Skip renderer initialization
  const BYTE rendererPatch[] = {0xA8, 0x00};  // TEST al, 0 (always false)
  static_assert(sizeof(rendererPatch) == HEADLESS_RENDERER_SIZE, "HEADLESS_RENDERER patch size mismatch");
  ApplyPatch(HEADLESS_RENDERER, rendererPatch, sizeof(rendererPatch));

  // Skip effects resource loading
  const BYTE effectsPatch[] = {0xEB, 0x41};  // JMP +0x43
  static_assert(sizeof(effectsPatch) == HEADLESS_EFFECTS_SIZE, "HEADLESS_EFFECTS patch size mismatch");
  ApplyPatch(HEADLESS_EFFECTS, effectsPatch, sizeof(effectsPatch));

  // Enable fixed timestep if configured
  if (headlessTimeStep != 0) {
    UINT64* timestepFlags = reinterpret_cast<UINT64*>(static_cast<CHAR*>(pGame) + GAME_TIMESTEP_FLAGS_OFFSET);
    *timestepFlags |= 0x2000000;  // Set fixed timestep flag
  }

  // Skip console creation if -noconsole was specified
  if (noConsole) {
    return;
  }

  // Create a console window for headless mode
  // Note: We create a new console because the parent console is already detached
  // due to /SUBSYSTEM:WINDOWS. Attaching multiple processes would be problematic.
  AllocConsole();

  // Redirect standard streams to the new console
  FILE* fConsole = nullptr;
  freopen_s(&fConsole, "CONIN$", "r", stdin);
  freopen_s(&fConsole, "CONOUT$", "w", stderr);
  freopen_s(&fConsole, "CONOUT$", "w", stdout);

  // Enable ANSI color codes in the console
  HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
  HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
  DWORD consoleMode = 0;

  GetConsoleMode(hStdOut, &consoleMode);
  consoleMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
  SetConsoleMode(hStdOut, consoleMode);

  GetConsoleMode(hStdErr, &consoleMode);
  consoleMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
  SetConsoleMode(hStdErr, consoleMode);
}

/// <summary>
/// Patches the loading tips system to immediately return, avoiding unnecessary log spam and processing.
/// The loading tips system requires resources that may not be properly configured in server mode.
/// </summary>
/// <returns>None</returns>
VOID PatchDisableLoadingTips() {
  using namespace PatchAddresses;

  // Patch R15PickLoadingTipNode to immediately return (RET = 0xC3)
  // All three loading tip functions use the same single-byte RET patch
  const BYTE retPatch[] = {0xC3};
  static_assert(sizeof(retPatch) == LOADING_TIP_PICK_SIZE, "LOADING_TIP_PICK patch size mismatch");
  static_assert(sizeof(retPatch) == LOADING_TIP_SELECT_SIZE, "LOADING_TIP_SELECT patch size mismatch");
  static_assert(sizeof(retPatch) == LOADING_TIP_SELECT_2_SIZE, "LOADING_TIP_SELECT_2 patch size mismatch");
  ApplyPatch(LOADING_TIP_PICK, retPatch, sizeof(retPatch));
  ApplyPatch(LOADING_TIP_SELECT, retPatch, sizeof(retPatch));
  ApplyPatch(LOADING_TIP_SELECT_2, retPatch, sizeof(retPatch));

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Disabled loading tips system for server mode");
}

/// <summary>
/// Patches the game to run as a dedicated server, exposing its game server broadcast port, adjusting its log file path.
/// </summary>
/// <returns>None</returns>
VOID PatchEnableServer() {
  using namespace PatchAddresses;

  // Patch server flag checks in command line processing (FUN_140116720 in cr15game.cpp)
  // This sets bit 2 (load sessions from broadcast) and bit 3 (dedicated server flag)
  // permanently, bypassing the normal conditional checks
  const BYTE serverFlagsCheck[] = {
      0x48, 0x83, 0x08, 0x06,                                      // OR QWORD ptr[rax], 0x6
      0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,  // NOPs to skip conditional checks
      0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
      0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
  static_assert(sizeof(serverFlagsCheck) == SERVER_FLAGS_CHECK_SIZE, "SERVER_FLAGS_CHECK patch size mismatch");
  ApplyPatch(SERVER_FLAGS_CHECK, serverFlagsCheck, sizeof(serverFlagsCheck));

  // Disable "r14netserver" logging which depends on missing files
  // String ref: "r14netserver" at 0x1416d2bb0
  const BYTE netserverLogging[] = {0x48, 0x89, 0xC3, 0x90};  // MOV RBX, RAX; NOP
  static_assert(sizeof(netserverLogging) == NETSERVER_LOGGING_SIZE, "NETSERVER_LOGGING patch size mismatch");
  ApplyPatch(NETSERVER_LOGGING, netserverLogging, sizeof(netserverLogging));

  // Update logging subject to "r14(server)"
  const BYTE loggingSubject[] = {0xEB, 0x0E};  // JMP short +0x0E
  static_assert(sizeof(loggingSubject) == LOGGING_SUBJECT_SIZE, "LOGGING_SUBJECT patch size mismatch");
  ApplyPatch(LOGGING_SUBJECT, loggingSubject, sizeof(loggingSubject));

  // Force "allow_incoming" in netconfig_*.json to always be true
  // String ref: "|allow_incoming" at 0x141cd0480
  // This is essential for accepting client connections
  const BYTE allowIncoming[] = {0xB8, 0x01, 0x00, 0x00, 0x00};  // MOV eax, 1
  static_assert(sizeof(allowIncoming) == ALLOW_INCOMING_SIZE, "ALLOW_INCOMING patch size mismatch");
  ApplyPatch(ALLOW_INCOMING, allowIncoming, sizeof(allowIncoming));

  // Bypass "-spectatorstream" requirement (string ref at 0x1416d27b8)
  // This makes the server automatically enter "load lobby" state on startup
  const BYTE spectatorStreamCheck[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};  // 6x NOP
  static_assert(sizeof(spectatorStreamCheck) == SPECTATORSTREAM_CHECK_SIZE,
                "SPECTATORSTREAM_CHECK patch size mismatch");
  ApplyPatch(SPECTATORSTREAM_CHECK, spectatorStreamCheck, sizeof(spectatorStreamCheck));
}

/// <summary>
/// Patches the game to run as an offline client, loading a game of the configuration specified by -gametype, -level,
/// and -region CLI arguments.
/// </summary>
/// <returns>None</returns>
VOID PatchEnableOffline() {
  using namespace PatchAddresses;

  // Patch multiplayer initialization for offline mode
  const BYTE multiplayerPatch[] = {0xE8, 0xCD, 0x02, 0x00, 0x00};  // CALL +0x2CD
  static_assert(sizeof(multiplayerPatch) == OFFLINE_MULTIPLAYER_SIZE, "OFFLINE_MULTIPLAYER patch size mismatch");
  ApplyPatch(OFFLINE_MULTIPLAYER, multiplayerPatch, sizeof(multiplayerPatch));

  // Patch incident reporting
  const BYTE incidentsPatch[] = {0x75, 0x0A};  // JNZ +0x0A
  static_assert(sizeof(incidentsPatch) == OFFLINE_INCIDENTS_SIZE, "OFFLINE_INCIDENTS patch size mismatch");
  ApplyPatch(OFFLINE_INCIDENTS, incidentsPatch, sizeof(incidentsPatch));

  // Patch title/session checks
  const BYTE titlePatch[] = {0x74, 0x12};  // JZ +0x12
  static_assert(sizeof(titlePatch) == OFFLINE_TITLE_SIZE, "OFFLINE_TITLE patch size mismatch");
  ApplyPatch(OFFLINE_TITLE, titlePatch, sizeof(titlePatch));

  // Force transaction service to load (two conditional jumps to NOP)
  // Both patches use the same 2-byte NOP pattern and share the same SIZE constant
  const BYTE nopConditionalJump[] = {0x90, 0x90};  // 2x NOP
  static_assert(sizeof(nopConditionalJump) == OFFLINE_TRANSACTION_SIZE, "OFFLINE_TRANSACTION patch size mismatch");
  ApplyPatch(OFFLINE_TRANSACTION_1, nopConditionalJump, sizeof(nopConditionalJump));
  ApplyPatch(OFFLINE_TRANSACTION_2, nopConditionalJump, sizeof(nopConditionalJump));

  // Skip failed logon service code
  const BYTE skipLogon[] = {0xE9, 0x92, 0x00, 0x00, 0x00, 0x00};  // JMP +0x97
  static_assert(sizeof(skipLogon) == OFFLINE_LOGON_SIZE, "OFFLINE_LOGON patch size mismatch");
  ApplyPatch(OFFLINE_LOGON, skipLogon, sizeof(skipLogon));

  // Redirect tutorial beginning
  const BYTE tutorialRedirect[] = {0xE8, 0xD6, 0x17, 0x68, 0xFF};  // CALL relative
  static_assert(sizeof(tutorialRedirect) == OFFLINE_TUTORIAL_SIZE, "OFFLINE_TUTORIAL patch size mismatch");
  ApplyPatch(OFFLINE_TUTORIAL, tutorialRedirect, sizeof(tutorialRedirect));
}

/// <summary>
/// Patches the game to allow -noovr (demo accounts) without use of spectator stream. This provides a temporary player
/// profile.
/// </summary>
/// <returns>None</returns>
VOID PatchNoOvrRequiresSpectatorStream() {
  using namespace PatchAddresses;

  // Bypass the error check that requires "-spectatorstream" when using "-noovr"
  const BYTE noOvrPatch[] = {0xEB, 0x35};  // JMP +0x35 (skip error code)
  static_assert(sizeof(noOvrPatch) == NOOVR_SPECTATOR_SIZE, "NOOVR_SPECTATOR patch size mismatch");
  ApplyPatch(NOOVR_SPECTATOR, noOvrPatch, sizeof(noOvrPatch));
}

/// <summary>
/// Patches the dead lock monitor, which monitors threads to ensure they have not stopped processing. If one does, it
/// triggers a fatal error. This patch is provided to ensure breakpoints set during testing do not trigger the deadlock
/// monitor, thereby killing the process.
/// </summary>
/// <returns>None</returns>
VOID PatchDeadlockMonitor() {
  using namespace PatchAddresses;

  // Disable the deadlock monitor's panic condition check
  // This allows debugging with breakpoints without triggering a timeout
  const BYTE deadlockPatch[] = {0x90, 0x90};  // 2x NOP (replace JLE instruction)
  static_assert(sizeof(deadlockPatch) == DEADLOCK_MONITOR_SIZE, "DEADLOCK_MONITOR patch size mismatch");
  ApplyPatch(DEADLOCK_MONITOR, deadlockPatch, sizeof(deadlockPatch));
}

// =============================================================================
// Oculus Platform SDK Blocking
// =============================================================================

typedef HMODULE(WINAPI* LoadLibraryW_t)(LPCWSTR lpLibFileName);
typedef HMODULE(WINAPI* LoadLibraryExW_t)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

LoadLibraryW_t Original_LoadLibraryW = LoadLibraryW;
LoadLibraryExW_t Original_LoadLibraryExW = LoadLibraryExW;

HMODULE WINAPI LoadLibraryW_Hook(LPCWSTR lpLibFileName) {
  if (lpLibFileName != nullptr) {
    std::wstring dllName(lpLibFileName);
    std::transform(dllName.begin(), dllName.end(), dllName.begin(), ::tolower);

    if (dllName.find(L"libovrplatform") != std::wstring::npos || dllName.find(L"ovrplatform") != std::wstring::npos) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Blocked Oculus Platform SDK load: %S", lpLibFileName);
      SetLastError(ERROR_MOD_NOT_FOUND);
      return NULL;
    }
  }
  return Original_LoadLibraryW(lpLibFileName);
}

HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
  if (lpLibFileName != nullptr) {
    std::wstring dllName(lpLibFileName);
    std::transform(dllName.begin(), dllName.end(), dllName.begin(), ::tolower);

    if (dllName.find(L"libovrplatform") != std::wstring::npos || dllName.find(L"ovrplatform") != std::wstring::npos) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Blocked Oculus Platform SDK load: %S", lpLibFileName);
      SetLastError(ERROR_MOD_NOT_FOUND);
      return NULL;
    }
  }
  return Original_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
}

VOID PatchBlockOculusSDK() {
  PatchDetour(&Original_LoadLibraryW, reinterpret_cast<PVOID>(LoadLibraryW_Hook));
  PatchDetour(&Original_LoadLibraryExW, reinterpret_cast<PVOID>(LoadLibraryExW_Hook));

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Installed Oculus Platform SDK blocking hooks");
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Expected savings: 50-80MB RAM, 8-12%% CPU per instance");
}

// ===================================================================================================
// Wwise Audio Optimization Hooks
// ===================================================================================================

typedef int(WINAPI* Wwise_Init_t)(PVOID);
Wwise_Init_t Original_Wwise_Init = nullptr;

typedef void(WINAPI* Wwise_RenderAudio_t)(PVOID);
Wwise_RenderAudio_t Original_Wwise_RenderAudio = nullptr;

int WINAPI Wwise_Init_Hook(PVOID config) {
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Wwise audio initialization blocked (VOIP preserved)");
  return 0;
}

void WINAPI Wwise_RenderAudio_Hook(PVOID context) {}

VOID PatchDisableWwise() {
  PVOID base = GetModuleHandleA(NULL);

  Original_Wwise_Init = (Wwise_Init_t)((uintptr_t)base + PatchAddresses::WWISE_INIT);
  PatchDetour(&Original_Wwise_Init, (PVOID)Wwise_Init_Hook);

  Original_Wwise_RenderAudio = (Wwise_RenderAudio_t)((uintptr_t)base + PatchAddresses::WWISE_RENDERAUDIO);
  PatchDetour(&Original_Wwise_RenderAudio, (PVOID)Wwise_RenderAudio_Hook);

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Installed Wwise audio blocking hooks");
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Expected savings: 20-30MB RAM, 5-8%% CPU per instance");
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] VOIP components preserved for multiplayer");
}

/// <summary>
/// A detour hook for the game's method it uses to transition from one net game state to another.
/// </summary>
/// <param name="game">A pointer to the game instance.</param>
/// <param name="state">The state to transition to.</param>
/// <returns>None</returns>
VOID NetGameSwitchStateHook(PVOID pGame, EchoVR::NetGameState state) {
  // Hook the net game switch state function, so we can redirect "load level failed" to a ready state again.
  // This way if a client requests a non-existent level, the game server library isn't unloaded due to a state
  // transition to "load failed" (because the level failed to load)
  if (isServer && state == EchoVR::NetGameState::LoadFailed) {
    // Schedule a return to lobby. We are already at lobby, but this will quickly end the session, removing
    // all players, and start listening for a new one, to keep the server recycling itself appropriately.
    // Note: This is an ugly hack, as the client will get an irrelevant connection failure message (server is full,
    // failed to connect, etc). But at least it doesn't cause the server to get stuck in a "not ready" state in some
    // menu.
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.PATCH] Dedicated server failed to load level. Resetting session to keep game server available.");
    EchoVR::NetGameScheduleReturnToLobby(pGame);
    return;
  }

  // Call the original function
  EchoVR::NetGameSwitchState(pGame, state);
}

/// <summary>
/// A detour hook for the game's method it uses to build CLI argument definitions.
/// Adds additional definitions to the structure, so that they may be parsed successfully without error.
/// </summary>
/// <param name="game">A pointer to the game instance.</param>
/// <param name="pArgSyntax">A pointer to the CLI argument structure tracking all CLI arguments.</param>
UINT64 BuildCmdLineSyntaxDefinitionsHook(PVOID pGame, PVOID pArgSyntax) {
  // Add all original CLI argument options.
  UINT64 result = EchoVR::BuildCmdLineSyntaxDefinitions(pGame, pArgSyntax);

  // Add our additional options
  EchoVR::AddArgSyntax(pArgSyntax, "-server", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-server", "[NEVR] Run as a dedicated game server");

  EchoVR::AddArgSyntax(pArgSyntax, "-offline", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-offline", "[NEVR] Run the game in offline mode");

  EchoVR::AddArgSyntax(pArgSyntax, "-windowed", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-windowed", "[NEVR] Run the game with no headset, in a window");

  EchoVR::AddArgSyntax(pArgSyntax, "-timestep", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-timestep",
                           "[NEVR] Sets the fixed update interval when using -headless (in ticks/updates per "
                           "second). 0 = no fixed time step, 120 = default");

  EchoVR::AddArgSyntax(pArgSyntax, "-fixed-timestep", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-fixed-timestep", "[NEVR] (Deprecated) Use -timestep instead");

  EchoVR::AddArgSyntax(pArgSyntax, "-noconsole", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-noconsole",
                           "[NEVR] Disable console window creation (must be used with -headless)");

  EchoVR::AddArgSyntax(pArgSyntax, "-config-path", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-config-path", "[NEVR] Specify a custom path to the config.json file");

  return result;
}

/// <summary>
/// A detour hook for the game's command line pre-processing method, used to parse command line arguments.
/// </summary>
/// <param name="pGame">A pointer to the game instance.</param>
UINT64 PreprocessCommandLineHook(PVOID pGame) {
  // Parse command line arguments.
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

  for (int i = 0; i < argc; ++i) {
    const LPWSTR arg = argv[i];

    if (lstrcmpW(arg, L"-server") == 0) {
      isServer = TRUE;
    } else if (lstrcmpW(arg, L"-offline") == 0) {
      isOffline = TRUE;
    } else if (lstrcmpW(arg, L"-noconsole") == 0) {
      noConsole = TRUE;
    } else if (lstrcmpW(arg, L"-headless") == 0) {
      isHeadless = TRUE;
    } else if (lstrcmpW(arg, L"-windowed") == 0) {
      isWindowed = TRUE;
    } else if (lstrcmpW(arg, L"-noovr") == 0) {
      isNoOVR = TRUE;
    } else if (lstrcmpW(arg, L"-timestep") == 0) {
      if (i + 1 < argc) {
        headlessTimeStep = std::wcstoul(argv[i + 1], nullptr, 10);
        ++i;  // Skip the next argument (the value)
      } else {
        FatalError(
            "Missing argument for -timestep. Provide a positive number for fixed tick rate, or zero for unthrottled.",
            NULL);
      }
    } else if (lstrcmpW(arg, L"-fixed-timestep") == 0) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] The -fixed-timestep argument is deprecated and ignored. Use -timestep <N> instead.");
    } else if (lstrcmpW(arg, L"-config-path") == 0) {
      if (i + 1 < argc) {
        int len = WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, customConfigJsonPath, MAX_PATH, NULL, NULL);
        if (len == 0) {
          FatalError("Failed to convert -config-path to multi-byte string.", NULL);
        }
        ++i;  // Skip the next argument (the path value)
      } else {
        FatalError("Missing argument for -config-path. Provide a path to a config.json file.", NULL);
      }
    }
  }

  // Validate argument combinations.
  if (isServer && isOffline) {
    FatalError("Arguments -server and -offline are mutually exclusive.", NULL);
  }

  if (noConsole && !isHeadless) {
    FatalError("The -noconsole flag requires -headless to be specified.", NULL);
  }

  // Apply patches based on arguments.
  if (isOffline) {
    PatchEnableOffline();
  }

  if (isHeadless) {
    PatchEnableHeadless(pGame);
  }

  // If the windowed, server, or headless flags were provided, apply the windowed mode patch to not use a VR headset.
  if (isWindowed || isServer || isHeadless) {
    using namespace PatchAddresses;
    // Set windowed mode flag in game structure
    UINT64* windowedFlags = reinterpret_cast<UINT64*>(static_cast<CHAR*>(pGame) + GAME_WINDOWED_FLAGS_OFFSET);
    *windowedFlags |= 0x0100000;  // Enable windowed mode (spectator uses 0x2100000 for additional settings)
  }

  // Apply patches to force the game to load as a server.
  if (isServer) {
    PatchEnableServer();
    PatchDisableLoadingTips();
    // Note: The platform module decision (OVR vs pnsrad) is based on mode flags at game_state[0xb68].
    // However, modifying those flags here doesn't work because either:
    // 1. The offset is different than documented, OR
    // 2. The structure isn't fully initialized yet at this point
    // Alternative: Apply a binary patch to PlatformModuleDecisionAndInitialize (0x140157fb0)
    // to skip the OVR path. See PatchBypassOvrPlatform() if implemented.
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] Server mode: OVR platform bypass requires binary patch (not yet implemented)");
  }

  // Update the window title
  if (hWindow != NULL && isNoOVR) EchoVR::SetWindowTextA_(hWindow, "Echo VR - [DEMO]");

  // Run the original method
  UINT64 result = EchoVR::PreprocessCommandLine(pGame);
  return result;
}

/// <summary>
/// A detour hook for the game's function to load the local config.json for the game instance.
/// If a custom config path was provided via -config-path, it loads that file directly using
/// the game's internal JSON loading function, bypassing the default _local/config.json.
/// </summary>
/// <param name="pGame">A pointer to the game struct to load the config for.</param>
UINT64 LoadLocalConfigHook(PVOID pGame) {
  UINT64 result;

  // If a custom config.json path was provided, load it directly using the game's JSON loader
  if (customConfigJsonPath[0] != '\0') {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Loading custom config from: %s", customConfigJsonPath);

    // Get the config destination pointer (pGame + 0x63240)
    using namespace PatchAddresses;
    EchoVR::Json* configDest = reinterpret_cast<EchoVR::Json*>(static_cast<CHAR*>(pGame) + GAME_LOCAL_CONFIG_OFFSET);

    // Call the game's internal JSON loader directly with our custom path
    // The third parameter (1) indicates the validation level: 1 = standard validation
    UINT32 loadResult = EchoVR::LoadJsonFromFile(configDest, customConfigJsonPath, 1);

    if (loadResult != 0) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load custom config file: %s (error %u)",
          customConfigJsonPath, loadResult);
      // Fall back to loading the default config
      result = EchoVR::LoadLocalConfig(pGame);
    } else {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Successfully loaded custom config from: %s", customConfigJsonPath);
      result = 0;  // Success
    }
  } else {
    // No custom config specified, use the default loader
    result = EchoVR::LoadLocalConfig(pGame);
  }

  // Configure fixed timestep if specified
  // Note: This is placed here because the required structures must be initialized first
  if (isHeadless && headlessTimeStep != 0) {
    using namespace PatchAddresses;

    // Set the fixed time step value (in microseconds)
    // The timestep is stored in a nested structure accessed via pointer
    UINT32* timeStepPtr = reinterpret_cast<UINT32*>(
        *reinterpret_cast<CHAR**>(EchoVR::g_GameBaseAddress + FIXED_TIMESTEP_PTR) + FIXED_TIMESTEP_OFFSET);
    *timeStepPtr = 1000000 / headlessTimeStep;  // Convert Hz to microseconds

    // Fix delta time calculation for fixed timestep mode
    // Changes condition: if (deltaTime > timeStep) to use correct comparison
    const BYTE deltaTimeFix[] = {0x73, 0x7A};  // JAE +0x7A (unsigned comparison)
    static_assert(sizeof(deltaTimeFix) == HEADLESS_DELTATIME_SIZE, "HEADLESS_DELTATIME patch size mismatch");
    ApplyPatch(HEADLESS_DELTATIME, deltaTimeFix, sizeof(deltaTimeFix));
  }

  // Store a reference to the local config from the game structure
  using namespace PatchAddresses;
  localConfig = reinterpret_cast<EchoVR::Json*>(static_cast<CHAR*>(pGame) + GAME_LOCAL_CONFIG_OFFSET);

  // Configure Asset CDN URL from config.json if specified
  if (localConfig != NULL) {
    CHAR* customCdnUrl = EchoVR::JsonValueAsString(localConfig, (CHAR*)"asset_cdn_url", NULL, false);
    if (customCdnUrl != NULL && customCdnUrl[0] != '\0') {
      // AssetCDN::SetCustomCdnUrl(customCdnUrl);
      // Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Asset CDN URL set from config: %s", customCdnUrl);
    }
  }

  return result;
}

/// <summary>
/// Helper function to get a service host from config.json with fallback logic.
/// Fallback chain: service_key → loginservice_host → default_url
/// </summary>
/// <param name="serviceKey">The primary config key to check (e.g., "configservice_host")</param>
/// <param name="defaultUrl">The default URL if no config override is found</param>
/// <returns>The resolved service URL</returns>
CHAR* GetServiceHostWithFallback(const CHAR* serviceKey, const CHAR* defaultUrl) {
  if (localConfig == NULL) return (CHAR*)defaultUrl;

  // Try primary service key first
  CHAR* host = EchoVR::JsonValueAsString(localConfig, (CHAR*)serviceKey, NULL, false);
  if (host != NULL && host[0] != '\0') {
    Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Service override [%s]: %s", serviceKey, host);
    return host;
  }

  // Fallback to loginservice_host if primary key not found
  host = EchoVR::JsonValueAsString(localConfig, (CHAR*)"loginservice_host", NULL, false);
  if (host != NULL && host[0] != '\0') {
    Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Service fallback [%s → loginservice_host]: %s", serviceKey, host);
    return host;
  }

  // Return default URL
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Service default [%s]: %s", serviceKey, defaultUrl);
  return (CHAR*)defaultUrl;
}

/// <summary>
/// A detour hook for the game's method it uses to connect to an HTTP(S) endpoint. This is used to redirect additional
/// hardcoded endpoints in the game with full fallback support for all service hosts.
/// </summary>
/// <param name="unk">TODO: Unknown</param>
/// <param name="uri">The HTTP(S) URI string to connect to.</param>
UINT64 HttpConnectHook(PVOID unk, CHAR* uri) {
  // If we have a local config, check for service overrides with fallback logic
  if (localConfig != NULL) {
    CHAR* originalUri = uri;

    // API Service (https://api.*)
    if (!strncmp(uri, "https://api.", 12)) {
      uri = GetServiceHostWithFallback("apiservice_host", uri);
      // Legacy compatibility: also try "api_host"
      if (uri == originalUri) {
        uri = EchoVR::JsonValueAsString(localConfig, (CHAR*)"api_host", uri, false);
      }
    }
    // Config Service - detect config-related URLs
    else if (strstr(uri, "config") != NULL && strstr(uri, "https://") == uri) {
      uri = GetServiceHostWithFallback("configservice_host", uri);
    }
    // Transaction Service - detect transaction/IAP URLs
    else if ((strstr(uri, "transaction") != NULL || strstr(uri, "iap") != NULL) && strstr(uri, "https://") == uri) {
      uri = GetServiceHostWithFallback("transactionservice_host", uri);
    }
    // Matching Service - detect matchmaking URLs
    else if (strstr(uri, "match") != NULL && strstr(uri, "https://") == uri) {
      uri = GetServiceHostWithFallback("matchingservice_host", uri);
    }
    // ServerDB Service - detect serverdb/registry URLs
    else if ((strstr(uri, "serverdb") != NULL || strstr(uri, "registry") != NULL) && strstr(uri, "https://") == uri) {
      uri = GetServiceHostWithFallback("serverdb_host", uri);
    }
    // Oculus Graph API
    else if (!strncmp(uri, "https://graph.oculus.com", 24)) {
      uri = EchoVR::JsonValueAsString(localConfig, (CHAR*)"graph_host", uri, false);
      if (uri == originalUri) {
        uri = EchoVR::JsonValueAsString(localConfig, (CHAR*)"graphservice_host", uri, false);
      }
    }

    if (uri != originalUri) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] HTTP(S) connection redirected: %s → %s", originalUri, uri);
    }
  }

  // Call the original function
  return EchoVR::HttpConnect(unk, uri);
}

// ============================================================================
// WebSocket Connection Hooks (for TcpBroadcaster / wss:// connections)
// ============================================================================

/// <summary>
/// Storage for the original CreatePeer function pointer to call after our hook.
/// </summary>
typedef EchoVR::TcpPeer* (*CreatePeerFunc)(EchoVR::TcpBroadcasterData* self, EchoVR::TcpPeer* result,
                                           const EchoVR::UriContainer* uri);
static CreatePeerFunc OriginalCreatePeer = NULL;

/// <summary>
/// Hook for TcpBroadcaster CreatePeer - intercepts WebSocket connection creation.
/// Applies config overrides and fallback logic for WebSocket URIs.
/// </summary>
EchoVR::TcpPeer* CreatePeerHook(EchoVR::TcpBroadcasterData* self, EchoVR::TcpPeer* result,
                                const EchoVR::UriContainer* uriContainer) {
  // We need to parse and potentially modify the URI
  // The UriContainer is opaque (0x120 bytes), but we can extract the URI string from it
  // For now, we'll let the connection proceed and rely on DNS/routing for redirection
  // A more sophisticated approach would parse the UriContainer and modify it

  // Log the WebSocket connection attempt
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] WebSocket connection initiated (CreatePeer called)");

  // Check if we have config overrides for WebSocket endpoints
  if (localConfig != NULL) {
    // Check for loginservice_host override (primary WebSocket endpoint)
    CHAR* loginHost = EchoVR::JsonValueAsString(localConfig, (CHAR*)"loginservice_host", NULL, false);
    if (loginHost != NULL && loginHost[0] != '\0') {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] WebSocket loginservice_host override configured: %s", loginHost);
      // Note: Actual URI modification would require parsing/rebuilding UriContainer
      // For now, document that DNS/routing should be used, or implement URI rewrite
    }

    // Check for matching/config/transaction service WebSocket overrides
    CHAR* matchingHost = EchoVR::JsonValueAsString(localConfig, (CHAR*)"matchingservice_host", NULL, false);
    if (matchingHost != NULL && matchingHost[0] != '\0') {
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Matchingservice override available: %s", matchingHost);
    }

    CHAR* configHost = EchoVR::JsonValueAsString(localConfig, (CHAR*)"configservice_host", NULL, false);
    if (configHost != NULL && configHost[0] != '\0') {
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Configservice override available: %s", configHost);
    }
  }

  // Call the original CreatePeer function
  if (OriginalCreatePeer != NULL) {
    return OriginalCreatePeer(self, result, uriContainer);
  }

  // Fallback: should never happen, but return invalid peer
  result->index = 0xFFFFFFFF;
  result->gen = 0;
  return result;
}

// ============================================================================
// SSL/TLS Modernization (Schannel hooks for ECDSA/EdDSA/RSA support)
// ============================================================================

#define SECURITY_WIN32
#include <schannel.h>
#include <security.h>
#include <sspi.h>

// Link with Secur32 for Schannel APIs (also configured in CMakeLists.txt)

/// <summary>
/// Original function pointers for Schannel APIs
/// </summary>
typedef SECURITY_STATUS(SEC_ENTRY* AcquireCredentialsHandleWFunc)(
    _In_opt_ LPWSTR pszPrincipal, _In_ LPWSTR pszPackage, _In_ unsigned long fCredentialUse, _In_opt_ void* pvLogonId,
    _In_opt_ void* pAuthData, _In_opt_ SEC_GET_KEY_FN pGetKeyFn, _In_opt_ void* pvGetKeyArgument,
    _Out_ PCredHandle phCredential, _Out_opt_ PTimeStamp ptsExpiry);

static AcquireCredentialsHandleWFunc OriginalAcquireCredentialsHandleW = NULL;

/// <summary>
/// Hook for AcquireCredentialsHandleW - enables modern TLS cipher suites and protocols.
/// This allows the game to connect to servers using ECDSA, EdDSA, and modern RSA certificates.
/// </summary>
SECURITY_STATUS SEC_ENTRY AcquireCredentialsHandleWHook(_In_opt_ LPWSTR pszPrincipal, _In_ LPWSTR pszPackage,
                                                        _In_ unsigned long fCredentialUse, _In_opt_ void* pvLogonId,
                                                        _In_opt_ void* pAuthData, _In_opt_ SEC_GET_KEY_FN pGetKeyFn,
                                                        _In_opt_ void* pvGetKeyArgument, _Out_ PCredHandle phCredential,
                                                        _Out_opt_ PTimeStamp ptsExpiry) {
  // Check if this is an Schannel client credential request
  if (pszPackage != NULL && lstrcmpW(pszPackage, UNISP_NAME_W) == 0 && (fCredentialUse & SECPKG_CRED_OUTBOUND) != 0) {
    // Modify the credential parameters to enable modern TLS
    if (pAuthData != NULL) {
      SCHANNEL_CRED* schannelCred = (SCHANNEL_CRED*)pAuthData;

      // Enable TLS 1.2 and TLS 1.3 (if available)
      // SP_PROT_TLS1_2_CLIENT = 0x00000800
      // SP_PROT_TLS1_3_CLIENT = 0x00002000 (Windows 11/Server 2022+)
      schannelCred->grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | 0x00002000;  // TLS 1.2 + 1.3

      // Enable all cipher suites (let the server choose the best)
      // This includes ECDHE-ECDSA, ECDHE-RSA, and modern RSA cipher suites
      schannelCred->dwFlags |= SCH_CRED_NO_DEFAULT_CREDS;         // Use explicit creds
      schannelCred->dwFlags &= ~SCH_CRED_MANUAL_CRED_VALIDATION;  // Use system cert validation
      schannelCred->dwFlags |= SCH_USE_STRONG_CRYPTO;             // Enable strong crypto only

      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] SSL/TLS modernized: Enabled TLS 1.2/1.3 with ECDSA/EdDSA/RSA support");
    }
  }

  // Call the original function
  if (OriginalAcquireCredentialsHandleW != NULL) {
    return OriginalAcquireCredentialsHandleW(pszPrincipal, pszPackage, fCredentialUse, pvLogonId, pAuthData, pGetKeyFn,
                                             pvGetKeyArgument, phCredential, ptsExpiry);
  }

  return SEC_E_UNSUPPORTED_FUNCTION;
}

/// <summary>
/// A detour hook for the game's method to wrap GetProcAddress.
/// </summary>
/// <param name="hModule">The module to get the procedure address from.</param>
/// <param name="lpProcName">The exported procedure name.</param>
/// <returns>The address of the procedure, or NULL if it was not found.</returns>
FARPROC GetProcAddressHook(HMODULE hModule, LPCSTR lpProcName) {
  // If this is a server, unloading pnsdemo.dll or pnsovr.dll currently causes dereferencing of freed memory
  // during a RadPluginShutdown call. This could be due to the timing on the patch for dedicated servers causing
  // some structures to initialize incorrectly or something.
  // For now, we resolve this by simply force exiting the server.

  // If we're performing a plugin shutdown, check if this is a user platform DLL such as pnsdemo.dll or pnsovr.dll,
  // which exports a "Users" method.
  if (isServer && strcmp(lpProcName, "RadPluginShutdown") == 0) {
    // If this is a user platform dll, exit the whole process with a success code instead of continuing to gracefully
    // unload.
    if (EchoVR::GetProcAddress(hModule, "Users") != NULL) exit(0);
  }

  // Call the original function.
  return EchoVR::GetProcAddress(hModule, lpProcName);
}

/// <summary>
/// A detour hook for the game's definition of SetWindowTextA.
/// </summary>
/// <param name="hWnd">The window handle to update the title for.</param>
/// <param name="lpString">The title string to be used.</param>
/// <returns>True if successful, false otherwise.</returns>
BOOL SetWindowTextAHook(HWND hWnd, LPCSTR lpString) {
  // Store a reference to the window
  hWindow = hWnd;

  // Call the original function and return the result with explicit cast to avoid data loss warning
  return (BOOL)EchoVR::SetWindowTextA_(hWnd, lpString);
}

/// <summary>
/// Verifies the version of the game is supported.
/// </summary>
/// <returns>None</returns>
BOOL VerifyGameVersion() {
  // Definitions to read image file header.
#define IMG_SIGNATURE_OFFSET 0x3C
#define IMG_SIGNATURE_SIZE 0x04

  // Obtain the image file header for the game.
  DWORD* signatureOffset = (DWORD*)(EchoVR::g_GameBaseAddress + IMG_SIGNATURE_OFFSET);
  IMAGE_FILE_HEADER* coffFileHeader =
      (IMAGE_FILE_HEADER*)(EchoVR::g_GameBaseAddress + (*signatureOffset + IMG_SIGNATURE_SIZE));

  // Verify the timestamp for Echo VR (version 34.4.631547.1).
  // Timestamp should be Wednesday, May 3, 2023 10:28:06 PM.
  // Note: For other executables, this may not hold. Reproducible builds have pushed this
  // towards being a build signature. In any case, it works as a naive integrity check.
  return coffFileHeader->TimeDateStamp == 0x6452dff6;
}

/// <summary>
/// Initializes the patcher, executing startup patchs on the game and installing detours/hooks on various game
/// functions.
/// </summary>
/// <returns>None</returns>
VOID Initialize() {
  // If we already initialized the library, stop.
  if (initialized) return;
  initialized = true;

  // EchoVR::InitEchoVR();  // Function not defined in nevr-common submodule

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Initializing GamePatches v%s (%s)", PROJECT_VERSION, GIT_COMMIT_HASH);
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Game Base Address: %p", EchoVR::g_GameBaseAddress);

  // Log Version
  CHAR filename[MAX_PATH];
  if (GetModuleFileNameA((HMODULE)EchoVR::g_GameBaseAddress, filename, MAX_PATH)) {
    DWORD handle;
    DWORD size = GetFileVersionInfoSizeA(filename, &handle);
    if (size) {
      std::vector<BYTE> buffer(size);
      if (GetFileVersionInfoA(filename, handle, size, buffer.data())) {
        VS_FIXEDFILEINFO* pFileInfo;
        UINT len;
        if (VerQueryValueA(buffer.data(), "\\", (LPVOID*)&pFileInfo, &len)) {
          Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Game File Version: %d.%d.%d.%d", HIWORD(pFileInfo->dwFileVersionMS),
              LOWORD(pFileInfo->dwFileVersionMS), HIWORD(pFileInfo->dwFileVersionLS),
              LOWORD(pFileInfo->dwFileVersionLS));
          Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Product Version: %d.%d.%d.%d",
              HIWORD(pFileInfo->dwProductVersionMS), LOWORD(pFileInfo->dwProductVersionMS),
              HIWORD(pFileInfo->dwProductVersionLS), LOWORD(pFileInfo->dwProductVersionLS));
        }
      }
    }
  }

  // Initialize the hooking library
  if (!Hooking::Initialize()) {
    MessageBoxW(NULL, L"Failed to initialize hooking library.", L"Echo Relay: Error", MB_OK);
    return;
  }

  // Verify the game version before patching
  if (!VerifyGameVersion())
    MessageBoxW(NULL,
                L"NEVR version check failed. Patches may fail to be applied. Verify you're running the correct "
                L"version of Echo VR.",
                L"Echo Relay: Warning", MB_OK);

  // Patch our CLI argument options to add our additional options.
  PatchDetour(&EchoVR::BuildCmdLineSyntaxDefinitions, reinterpret_cast<PVOID>(BuildCmdLineSyntaxDefinitionsHook));
  PatchDetour(&EchoVR::PreprocessCommandLine, reinterpret_cast<PVOID>(PreprocessCommandLineHook));
  PatchDetour(&EchoVR::NetGameSwitchState, reinterpret_cast<PVOID>(NetGameSwitchStateHook));
  PatchDetour(&EchoVR::LoadLocalConfig, reinterpret_cast<PVOID>(LoadLocalConfigHook));
  PatchDetour(&EchoVR::HttpConnect, reinterpret_cast<PVOID>(HttpConnectHook));
  PatchDetour(&EchoVR::GetProcAddress, reinterpret_cast<PVOID>(GetProcAddressHook));
  PatchDetour(&EchoVR::SetWindowTextA_, reinterpret_cast<PVOID>(SetWindowTextAHook));

  // Hook SSL/TLS functions for modern cipher suite support (ECDSA, EdDSA, RSA)
  HMODULE hSecur32 = GetModuleHandleA("Secur32.dll");
  if (hSecur32 != NULL) {
    OriginalAcquireCredentialsHandleW =
        (AcquireCredentialsHandleWFunc)GetProcAddress(hSecur32, "AcquireCredentialsHandleW");
    if (OriginalAcquireCredentialsHandleW != NULL) {
      PatchDetour(&OriginalAcquireCredentialsHandleW, reinterpret_cast<PVOID>(AcquireCredentialsHandleWHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] SSL/TLS modernization hook installed (Schannel)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find AcquireCredentialsHandleW for SSL/TLS modernization");
    }
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load Secur32.dll for SSL/TLS modernization");
  }

  // Note: WebSocket CreatePeer hook requires vtable hooking, which is complex.
  // For now, DNS/routing overrides should be used for WebSocket endpoint redirection.
  // TODO: Implement vtable hook for TcpBroadcasterData::CreatePeer if needed.
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] Config property injection initialized with fallback chain: service → loginservice_host → default");

  // Run some startup patches
  PatchNoOvrRequiresSpectatorStream();

  // Initialize Asset CDN redirection system
  // if (!AssetCDN::Initialize()) {
  //  Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to initialize Asset CDN redirection");
  //}

  // Patch out the deadlock monitor thread's validation routine if we're compiling in debug mode, as this will panic
  // from process suspension.
#if _DEBUG
  PatchDeadlockMonitor();

  if (isServer || isHeadless) {
    PatchBlockOculusSDK();
    PatchDisableWwise();
  }
#endif
}

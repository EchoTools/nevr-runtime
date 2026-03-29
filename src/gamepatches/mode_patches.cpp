#include "mode_patches.h"

#include <algorithm>
#include <string>

#include <psapi.h>
#include <setjmp.h>

#include "cli.h"
#include "gamepatches_internal.h"
#include "common/globals.h"
#include "common/logging.h"
#include "common/echovr_functions.h"
#include "patch_addresses.h"
#include "process_mem.h"

// ============================================================================
// PatchEnableHeadless — enable headless mode with console window
// ============================================================================

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

  // WriteLog hook removed — log_filter plugin now owns CLog::PrintfImpl.

  // Skip renderer initialization
  const BYTE rendererPatch[] = {0xA8, 0x00};  // TEST al, 0 (always false)
  static_assert(sizeof(rendererPatch) == HEADLESS_RENDERER_SIZE, "HEADLESS_RENDERER patch size mismatch");
  ApplyPatch(HEADLESS_RENDERER, rendererPatch, sizeof(rendererPatch));

  // Skip effects resource loading
  const BYTE effectsPatch[] = {0xEB, 0x41};  // JMP +0x43
  static_assert(sizeof(effectsPatch) == HEADLESS_EFFECTS_SIZE, "HEADLESS_EFFECTS patch size mismatch");
  ApplyPatch(HEADLESS_EFFECTS, effectsPatch, sizeof(effectsPatch));

  // Skip ApplyGraphicsSettings call — it calls ~66 CGRenderer methods that crash without a renderer
  const BYTE graphicsNop[] = {0x90, 0x90, 0x90, 0x90, 0x90};  // 5x NOP over CALL instruction
  static_assert(sizeof(graphicsNop) == HEADLESS_APPLY_GRAPHICS_SIZE, "HEADLESS_APPLY_GRAPHICS patch size mismatch");
  ApplyPatch(HEADLESS_APPLY_GRAPHICS, graphicsNop, sizeof(graphicsNop));

  // Enable fixed timestep if configured
  if (g_headlessTimeStep != 0) {
    UINT64* timestepFlags = reinterpret_cast<UINT64*>(static_cast<CHAR*>(pGame) + GAME_TIMESTEP_FLAGS_OFFSET);
    *timestepFlags |= 0x2000000;  // Set fixed timestep flag
  }

  // Skip console creation if -noconsole was specified
  if (g_noConsole) {
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

// ============================================================================
// PatchBypassOvrPlatform — NOP the OVR conditional branch
// ============================================================================

/// <summary>
/// Patches the OVR platform initialization branch within PlatformModuleDecisionAndInitialize.
/// This function tests platform capability flags (at game_state+0x2da0) and conditionally
/// initializes OVR, RAD, or other platform modules based on bit flags.
///
/// Without this patch, the OVR branch (bit 6, 0x40) causes a crash when OVR DLLs are unavailable.
/// The surgical fix: NOP the conditional jump to the OVR initialization path (offset 0x1580e5),
/// allowing normal RAD platform initialization and broadcaster setup to proceed.
///
/// Assembly context at 0x1401580df-0x1401580eb:
///   1401580df:  shr    $0x6,%cl          # Test bit 6 (OVR platform flag)
///   1401580e2:  test   %cl,%r14b         # Check if OVR flag is set
///   1401580e5:  jne    0x1401581b2       # Jump to OVR initialization (PATCH THIS)
///   1401580eb:  mov    0x30(%rsi),%rcx  # Continue with normal init (broadcaster, etc.)
///
/// By replacing the 6-byte 'jne' (0F 85 C7 00 00 00) with 6 NOPs (90 90 90 90 90 90),
/// the OVR code path is skipped while all other initialization continues normally.
/// </summary>
/// <returns>None</returns>
VOID PatchBypassOvrPlatform() {
  using namespace PatchAddresses;

  // Patch the OVR conditional jump to fall through instead of branching
  // This allows broadcaster initialization and state machine progression
  const BYTE nopPatch[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};  // 6 NOPs to replace JNE instruction
  constexpr uintptr_t OVR_BRANCH_OFFSET = 0x1580e5;              // JNE to OVR initialization path

  ApplyPatch(OVR_BRANCH_OFFSET, nopPatch, sizeof(nopPatch));

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] OVR platform branch bypassed - allowing normal initialization");
}

// ============================================================================
// PatchDisableLoadingTips — RET the loading tip functions
// ============================================================================

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

// ============================================================================
// InitializeGlobalGameSpace hook — skip client-only setup in server mode
// ============================================================================

/// Original function: CR15Game::InitializeGlobalGameSpace @ 0x140110ab0
/// Signature: void(CR15Game* this, void* gamespace, ...)
/// In server mode, the global gamespace has no player actor or CDialogueSceneCS.
/// The original function fatals if either is missing. We set the gamespace pointer
/// (CR15Game+0x7AF0) that downstream code depends on, then return early.
typedef VOID InitializeGlobalGameSpaceFunc(PVOID pGame, PVOID pGameSpace);
static InitializeGlobalGameSpaceFunc* OriginalInitializeGlobalGameSpace = nullptr;

static VOID InitializeGlobalGameSpaceHook(PVOID pGame, PVOID pGameSpace) {
  if (g_isServer) {
    // Set the global gamespace pointer — downstream code reads this
    *(PVOID*)((CHAR*)pGame + 0x7AF0) = pGameSpace;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] InitializeGlobalGameSpace skipped in server mode (no local player actor needed)");
    return;
  }
  OriginalInitializeGlobalGameSpace(pGame, pGameSpace);
}

// ============================================================================
// Engine entity lookup null-check hook — prevent AV in server mode
// ============================================================================

/// Original function: Engine entity lookup @ 0x140f80ed0
/// This function dereferences *(*(int64_t*)arg1 + 0x5e0) which is a hash table
/// pointer. In server mode, this field can be uninitialized (0x10), causing an
/// access violation at address 0x4008. We add a null check and return -1 (the
/// function's default "not found" value) when the pointer is invalid.
typedef INT16 EngineEntityLookupFunc(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5);
static EngineEntityLookupFunc* OriginalEngineEntityLookup = nullptr;

static INT16 EngineEntityLookupHook(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5) {
  if (g_isServer) {
    // Check if the structure pointer chain is valid before calling original
    INT64* outerPtr = (INT64*)arg1;
    if (outerPtr == nullptr) return -1;
    INT64 innerPtr = *outerPtr;
    if (innerPtr == 0) return -1;
    // Check the hash table pointer at +0x5e0
    INT64 hashTablePtr = *(INT64*)(innerPtr + 0x5e0);
    if (hashTablePtr < 0x10000) {
      static volatile LONG guardCount = 0;
      LONG c = InterlockedIncrement(&guardCount);
      if (c <= 3) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] Entity lookup null-guard triggered (ptr+0x5e0=%p, count=%ld)", (void*)hashTablePtr, c);
      }
      return -1;
    }
  }
  return OriginalEngineEntityLookup(arg1, arg2, arg3, arg4, arg5);
}

// ============================================================================
// Engine entity property dispatch null-check hook — prevent AV in server mode
// ============================================================================

/// Original function: fcn.140f87aa0 (580 bytes, 8 callers)
/// Dereferences *(*(int64_t*)arg1 + 0x448) for a flags check. In server mode the
/// inner pointer can be invalid (e.g. 0x10), causing READ AV at low addresses.
typedef VOID EngineEntityPropDispatchFunc(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5);
static EngineEntityPropDispatchFunc* OriginalEngineEntityPropDispatch = nullptr;

static VOID EngineEntityPropDispatchHook(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5) {
  // Skip entirely in server mode — this function dispatches entity property updates
  // for client-side state (rendering, effects) that doesn't exist in headless mode.
  // The internal pointer chain is uninitialized, causing cascading AVs.
  if (g_isServer) return;
  OriginalEngineEntityPropDispatch(arg1, arg2, arg3, arg4, arg5);
}

// ============================================================================
// Game main wrapper hook — restart game loop on crash in server mode
// ============================================================================

/// Original function: Game main wrapper @ 0x1400cd510
/// Calls the game's main loop (fcn.1400cd550). If the game loop returns (which
/// means a fatal error occurred), the original function calls the crash handler.
/// In server mode, we restart the game loop instead so the server stays alive.
typedef VOID GameMainWrapperFunc(INT64 arg1);
static GameMainWrapperFunc* OriginalGameMainWrapper = nullptr;

/// Direct pointer to the game's main function (fcn.1400cd550) so we can call
/// it directly in the restart loop without going through the wrapper.
typedef VOID GameMainFunc(INT64 arg1);
static GameMainFunc* GameMain = nullptr;

/// Jump buffer for recovering from fatal crashes in the game loop.
/// When the VEH catches a null-pointer AV in server mode, it longjmps here
/// to restart the game loop instead of letting the SEH handler terminate.
// Non-static: crash_recovery.cpp's VEH needs extern access for longjmp recovery
jmp_buf g_gameLoopJmpBuf;
volatile bool g_gameLoopJmpBufValid = false;

static VOID GameMainWrapperHook(INT64 arg1) {
  // Always set up the longjmp recovery point — g_isServer isn't set yet when this
  // runs (CLI args haven't been parsed). The VEH checks g_isServer at exception time.
  int crashCount = setjmp(g_gameLoopJmpBuf);
  g_gameLoopJmpBufValid = true;

  if (crashCount > 0) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] Game loop recovered from crash #%d — entering server hold", crashCount);
    // The game loop crashed and can't be safely restarted (internal state is
    // corrupted). Keep the process alive — the broadcaster and game server
    // were already initialized, and the HTTP API may still be listening.
    while (true) {
      Sleep(1000);
    }
  }

  // Run the game main loop
  GameMain(arg1);

  // If we get here, the game loop returned normally (shouldn't happen)
  g_gameLoopJmpBufValid = false;
  Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Game loop exited normally — entering server hold");
  while (true) {
    Sleep(1000);
  }
}

// ============================================================================
// BugSplat crash handler hook — prevent fatal exits in server mode
// ============================================================================

/// Original function: BugSplat crash handler @ 0x1400dbbc0
/// Called from 5 sites when the game encounters a fatal error. Builds an error
/// report, calls ExitProcess(1), then executes int3. In server mode the crash
/// is non-fatal (missing actors, dialogue scenes, etc.) so we log and return.
/// Callers have fallthrough code paths, so returning is safe.
typedef VOID BugSplatCrashHandlerFunc(INT64 exitCode);
static BugSplatCrashHandlerFunc* OriginalBugSplatCrashHandler = nullptr;

static VOID BugSplatCrashHandlerHook(INT64 exitCode) {
  if (g_isServer) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] BugSplat crash handler intercepted (exit code %lld) — suppressed in server mode", exitCode);
    return;
  }
  OriginalBugSplatCrashHandler(exitCode);
}

// ============================================================================
// PatchEnableServer — force dedicated server mode
// ============================================================================

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

  // Force "allow_incoming" to always be true in CBroadcaster::InitializeFromJson
  // (FUN_140f7f8b0, called from CR15NetDedicatedLobby constructor).
  // This reads from netconfig_dedicatedserver.json (game asset), NOT _local/config.json.
  // The byte patch is necessary because _local/config.json doesn't feed this function.
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

// ============================================================================
// PatchEnableOffline — offline client mode
// ============================================================================

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

// ============================================================================
// PatchNoOvrRequiresSpectatorStream — allow -noovr without spectator stream
// ============================================================================

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

// ============================================================================
// PatchDeadlockMonitor — disable deadlock detection for debugging
// ============================================================================

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

static LoadLibraryW_t Original_LoadLibraryW = LoadLibraryW;
static LoadLibraryExW_t Original_LoadLibraryExW = LoadLibraryExW;

static HMODULE WINAPI LoadLibraryW_Hook(LPCWSTR lpLibFileName) {
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

static HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
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
static Wwise_Init_t Original_Wwise_Init = nullptr;

typedef void(WINAPI* Wwise_RenderAudio_t)(PVOID);
static Wwise_RenderAudio_t Original_Wwise_RenderAudio = nullptr;

static int WINAPI Wwise_Init_Hook(PVOID config) {
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Wwise audio initialization blocked (VOIP preserved)");
  return 0;
}

static void WINAPI Wwise_RenderAudio_Hook(PVOID context) {}

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

// ===================================================================================================
// Server Frame Pacing Optimization
// ===================================================================================================

/// Patch CPrecisionSleep::BusyWait to return immediately.
/// The CALL from CPrecisionSleep::Wait still executes normally (clean stack),
/// but the busy-wait function itself is a no-op. The WaitableTimer phase in
/// the caller handles the bulk sleep; we only lose ~250μs of precision per frame.
VOID PatchServerFramePacing() {
  const BYTE ret[] = {0xC3};  // RET
  static_assert(sizeof(ret) == PatchAddresses::PRECISION_SLEEP_BUSYWAIT_SIZE,
                "PRECISION_SLEEP_BUSYWAIT patch size mismatch");
  ApplyPatch(PatchAddresses::PRECISION_SLEEP_BUSYWAIT, ret, sizeof(ret));

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CPrecisionSleep::BusyWait patched to RET (Wine CPU optimization)");
}

// ============================================================================
// PatchDisableServerRendering — disable renderer/effects/audio for servers
// ============================================================================

/// <summary>
/// Disables renderer, effects, and audio for dedicated server mode.
/// These are the same core patches from PatchEnableHeadless() minus console/log hooks.
/// Without a GPU driving vsync, servers also need the fixed timestep flag.
/// </summary>
/// <param name="pGame">The pointer to the instance of the game structure.</param>
VOID PatchDisableServerRendering(PVOID pGame) {
  using namespace PatchAddresses;

  // Disable audio by clearing the audio enable bit (same as `-noaudio` command)
  UINT32* audioFlags = reinterpret_cast<UINT32*>(static_cast<CHAR*>(pGame) + GAME_AUDIO_FLAGS_OFFSET);
  *audioFlags &= 0xFFFFFFFD;  // Clear bit 1 (audio enable)

  // Skip renderer initialization
  const BYTE rendererPatch[] = {0xA8, 0x00};  // TEST al, 0 (always false)
  static_assert(sizeof(rendererPatch) == HEADLESS_RENDERER_SIZE, "HEADLESS_RENDERER patch size mismatch");
  ApplyPatch(HEADLESS_RENDERER, rendererPatch, sizeof(rendererPatch));

  // Skip effects resource loading
  const BYTE effectsPatch[] = {0xEB, 0x41};  // JMP +0x43
  static_assert(sizeof(effectsPatch) == HEADLESS_EFFECTS_SIZE, "HEADLESS_EFFECTS patch size mismatch");
  ApplyPatch(HEADLESS_EFFECTS, effectsPatch, sizeof(effectsPatch));

  // Skip ApplyGraphicsSettings call — it calls ~66 CGRenderer methods that crash without a renderer
  const BYTE graphicsNop[] = {0x90, 0x90, 0x90, 0x90, 0x90};  // 5x NOP over CALL instruction
  static_assert(sizeof(graphicsNop) == HEADLESS_APPLY_GRAPHICS_SIZE, "HEADLESS_APPLY_GRAPHICS patch size mismatch");
  ApplyPatch(HEADLESS_APPLY_GRAPHICS, graphicsNop, sizeof(graphicsNop));

  // Enable fixed timestep if configured
  if (g_headlessTimeStep != 0) {
    UINT64* timestepFlags = reinterpret_cast<UINT64*>(static_cast<CHAR*>(pGame) + GAME_TIMESTEP_FLAGS_OFFSET);
    *timestepFlags |= 0x2000000;  // Set fixed timestep flag
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Server rendering disabled (renderer, effects, audio, graphics settings)");
  if (g_headlessTimeStep != 0) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Fixed timestep flag set (%u ticks/sec)", g_headlessTimeStep);
  }
}

// ============================================================================
// PatchLogServerProfile — log memory and module snapshot
// ============================================================================

/// <summary>
/// Logs a one-time server profile snapshot after all patches are applied.
/// Reports working set, private bytes, and checks whether GPU/audio/OVR DLLs are loaded.
/// </summary>
VOID PatchLogServerProfile() {
  PROCESS_MEMORY_COUNTERS_EX pmc;
  memset(&pmc, 0, sizeof(pmc));
  pmc.cb = sizeof(pmc);
  if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PROFILE] WorkingSet: %llu MB, PrivateBytes: %llu MB",
        pmc.WorkingSetSize / (1024 * 1024), pmc.PrivateUsage / (1024 * 1024));
  }

  const char* checkDlls[] = {"d3d11", "dxgi", "LibOVRPlatform", "AkSoundEngine", NULL};
  for (int i = 0; checkDlls[i]; i++) {
    HMODULE h = GetModuleHandleA(checkDlls[i]);
    Log(EchoVR::LogLevel::Info, "[NEVR.PROFILE] Module %s: %s", checkDlls[i], h ? "LOADED" : "not loaded");
  }
}

// ============================================================================
// Install* wrappers — called from Initialize() to set up detour hooks
// ============================================================================

VOID InstallEntityHooks() {
  // Hook engine entity lookup to prevent null-pointer AV in server mode.
  // The function dereferences a hash table pointer at +0x5e0 that's uninitialized
  // in dedicated server mode (no player actor / client-side state).
  OriginalEngineEntityLookup =
      (EngineEntityLookupFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::ENGINE_ENTITY_LOOKUP);
  PatchDetour(&OriginalEngineEntityLookup, reinterpret_cast<PVOID>(EngineEntityLookupHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Engine entity lookup hook installed (null-pointer guard)");

  OriginalEngineEntityPropDispatch =
      (EngineEntityPropDispatchFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::ENGINE_ENTITY_PROP_DISPATCH);
  PatchDetour(&OriginalEngineEntityPropDispatch, reinterpret_cast<PVOID>(EngineEntityPropDispatchHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Entity prop dispatch hook installed (null-pointer guard)");
}

VOID InstallBugSplatHook() {
  // Hook BugSplat crash handler — prevents fatal exits in server mode.
  // The handler is called from 5 sites for missing actors, dialogue scenes, etc.
  // These are non-fatal in headless dedicated server mode.
  OriginalBugSplatCrashHandler =
      (BugSplatCrashHandlerFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::BUGSPLAT_CRASH_HANDLER);
  PatchDetour(&OriginalBugSplatCrashHandler, reinterpret_cast<PVOID>(BugSplatCrashHandlerHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] BugSplat crash handler hook installed (server crash suppression)");
}

VOID InstallGameSpaceHook() {
  // Hook InitializeGlobalGameSpace to prevent fatal crash in server mode
  // (no local player actor exists in the global gamespace for dedicated servers)
  OriginalInitializeGlobalGameSpace =
      (InitializeGlobalGameSpaceFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::INIT_GLOBAL_GAMESPACE);
  PatchDetour(&OriginalInitializeGlobalGameSpace, reinterpret_cast<PVOID>(InitializeGlobalGameSpaceHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] InitializeGlobalGameSpace hook installed (server crash fix)");
}

VOID InstallGameMainHook() {
  // Hook game main wrapper — longjmp recovery on crash keeps server alive
  GameMain = (GameMainFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::GAME_MAIN);
  OriginalGameMainWrapper =
      (GameMainWrapperFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::GAME_MAIN_WRAPPER);
  PatchDetour(&OriginalGameMainWrapper, reinterpret_cast<PVOID>(GameMainWrapperHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Game main wrapper hook installed (server crash recovery)");
}

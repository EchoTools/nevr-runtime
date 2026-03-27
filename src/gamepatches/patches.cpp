#include "patches.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/globals.h"
#include "common/hooking.h"
#include "common/logging.h"
#include "common/symbols.h"
#include "plugin_loader.h"
#include "process_mem.h"

#ifdef USE_MINHOOK
#include <MinHook.h>
#endif

#include <processthreadsapi.h>
#include <psapi.h>
#include <windows.h>

#include "common/echovr_functions.h"
#include "patch_addresses.h"

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
BOOL g_initialized = FALSE;

/// <summary>
/// Custom config.json path provided via command-line argument. If empty, the default path is used.
/// </summary>
CHAR g_customConfigJsonPath[MAX_PATH] = {0};

/// <summary>
/// A CLI argument flag indicating whether the game is booting as a dedicated server.
/// </summary>
BOOL g_isServer = FALSE;
/// <summary>
/// A CLI argument flag indicating whether the game is booting as an offline client.
/// </summary>
BOOL g_isOffline = FALSE;
/// <summary>
/// A CLI argument flag indicating whether the game is booting in headless mode (no graphics/audio).
/// </summary>
BOOL g_isHeadless = FALSE;
/// <summary>
/// A CLI argument flag used to remove the extra console being added by -headless for running servers on fully headless
/// system.
/// </summary>
BOOL g_noConsole = FALSE;
/// <summary>
/// A CLI argument flag indicating whether the game is booting in a windowed mode, rather than with a VR headset.
/// </summary>
BOOL g_isWindowed = FALSE;

/// <summary>
/// Indicates whether the game was launched with `-noovr`.
/// </summary>
BOOL g_isNoOVR = FALSE;
/// <summary>
/// The window handle for the current game window.
/// </summary>
HWND g_hWindow = NULL;

/// <summary>
/// The local config stored in ./_local/config.json.
/// </summary>
EchoVR::Json* g_localConfig = NULL;

/// <summary>
/// Early-loaded config for URI redirect hooks that fire before the game loads its config.
/// Loaded during Initialize() from _local/config.json using the game's JSON parser.
/// </summary>
static EchoVR::Json g_earlyConfig = {NULL, NULL};
static EchoVR::Json* g_earlyConfigPtr = NULL;

/// <summary>
/// The game instance pointer — stored globally for social message injection.
/// Set during PreprocessCommandLineHook, used to navigate to the broadcaster.
/// Path: pGame + 0x8518 → CR15NetGame → lobby → +0x008 → Broadcaster*
/// </summary>
PVOID g_pGame = NULL;

/// <summary>
/// A timestep value in ticks/updates per second, to be used for headless mode (due to lack of GPU/refresh rate
/// throttling). If non-zero, sets the timestep override by the given tick rate per second. If zero, removes tick rate
/// throttling.
/// </summary>
UINT32 g_headlessTimeStep = 120;

/// Layout must match the declaration in gameserver.cpp (used via GetProcAddress).
struct NevRUPnPConfig {
  BOOL   enabled;
  UINT16 port;
  CHAR   internalIp[46];
  CHAR   externalIp[46];
};

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

// WriteLogHook removed — log filtering, timestamps, and formatting are now
// handled by the log_filter plugin. This unblocks the plugin's MinHook on
// CLog::PrintfImpl which previously failed with MH_ERROR_ALREADY_CREATED.

/// <summary>
/// A wrapper for WriteLog, simplifying logging operations.
/// </summary>
/// <returns>None</returns>
VOID Log(EchoVR::LogLevel level, const CHAR* format, ...) {
  va_list args;
  va_start(args, format);
  EchoVR::WriteLog(level, 0, format, args);
  va_end(args);
}

typedef uint64_t (*CSymbol64_HashFunc)(const char* str, uint64_t seed);
CSymbol64_HashFunc OriginalCSymbol64_Hash = nullptr;

uint64_t CSymbol64_HashHook(const char* str, uint64_t seed) {
  uint64_t result = OriginalCSymbol64_Hash(str, seed);

  if (str && str[0] != '\0') {
    Log(EchoVR::LogLevel::Info, "[HASH] \"%s\" -> 0x%016llx (seed=0x%016llx)", str, result, seed);
  }

  return result;
}

typedef uint64_t (*CMatSym_HashFunc)(const char* str);
CMatSym_HashFunc OriginalCMatSym_Hash = nullptr;

uint64_t CMatSym_HashHook(const char* str) {
  uint64_t intermediate = OriginalCMatSym_Hash(str);

  if (str && str[0] != '\0') {
    Log(EchoVR::LogLevel::Info, "[CMATSYM] \"%s\" -> intermediate=0x%016llx", str, intermediate);
  }

  return intermediate;
}

typedef uint64_t (*SMatSymData_HashAFunc)(uint64_t seed, uint64_t value);
SMatSymData_HashAFunc OriginalSMatSymData_HashA = nullptr;
constexpr uint64_t MATSYM_FINALIZE_SEED = 0x6d451003fb4b172eULL;

uint64_t SMatSymData_HashAHook(uint64_t seed, uint64_t value) {
  uint64_t result = OriginalSMatSymData_HashA(seed, value);

  if (seed == MATSYM_FINALIZE_SEED) {
    Log(EchoVR::LogLevel::Info, "[MATSYM_FINAL] intermediate=0x%016llx -> FINAL=0x%016llx", value, result);
  }

  return result;
}

typedef void (*SnsRegistryInsertSortedFunc)(uint64_t msg_hash, const char* msg_name, uint64_t flags);
SnsRegistryInsertSortedFunc OriginalSnsRegistryInsertSorted = nullptr;

void SnsRegistryInsertSortedHook(uint64_t msg_hash, const char* msg_name, uint64_t flags) {
  if (msg_name && msg_name[0] != '\0') {
    Log(EchoVR::LogLevel::Info, "[MSG_REGISTRY] 0x%016llx = \"%s\" (flags=0x%llx)", msg_hash, msg_name, flags);
  }

  OriginalSnsRegistryInsertSorted(msg_hash, msg_name, flags);
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
InitializeGlobalGameSpaceFunc* OriginalInitializeGlobalGameSpace = nullptr;

VOID InitializeGlobalGameSpaceHook(PVOID pGame, PVOID pGameSpace) {
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
EngineEntityLookupFunc* OriginalEngineEntityLookup = nullptr;

INT16 EngineEntityLookupHook(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5) {
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
EngineEntityPropDispatchFunc* OriginalEngineEntityPropDispatch = nullptr;

VOID EngineEntityPropDispatchHook(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5) {
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
GameMainWrapperFunc* OriginalGameMainWrapper = nullptr;

/// Direct pointer to the game's main function (fcn.1400cd550) so we can call
/// it directly in the restart loop without going through the wrapper.
typedef VOID GameMainFunc(INT64 arg1);
GameMainFunc* GameMain = nullptr;

/// Jump buffer for recovering from fatal crashes in the game loop.
/// When the VEH catches a null-pointer AV in server mode, it longjmps here
/// to restart the game loop instead of letting the SEH handler terminate.
#include <setjmp.h>
static jmp_buf g_gameLoopJmpBuf;
static volatile bool g_gameLoopJmpBufValid = false;

VOID GameMainWrapperHook(INT64 arg1) {
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
BugSplatCrashHandlerFunc* OriginalBugSplatCrashHandler = nullptr;

VOID BugSplatCrashHandlerHook(INT64 exitCode) {
  if (g_isServer) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] BugSplat crash handler intercepted (exit code %lld) — suppressed in server mode", exitCode);
    return;
  }
  OriginalBugSplatCrashHandler(exitCode);
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

// ===================================================================================================
// Social Features — pnsrad SNS ↔ Nakama bridge
// ===================================================================================================

#include "social_messages.h"
#include "nevr_client.h"

namespace SocialSym = EchoVR::Symbols::Social;

// Global Nakama client for social feature bridge
static NevrClient* g_nakamaClient = nullptr;

/// Get the UDP broadcaster from the global game context.
/// Path: g_GameBaseAddress + 0x20a0478 → game_context → +0x8518 → CR15NetGame
///       → +0x28C8 → BroadcasterData → +0x08 → Broadcaster*
/// Offsets from: CR15NetGameLayout.h, telemetry_snapshot.h, echovr.h
/// Returns nullptr if not yet initialized.
EchoVR::Broadcaster* GetBroadcasterFromGame() {
  CHAR* base = EchoVR::g_GameBaseAddress;
  if (!base) return nullptr;

  // Get global game context (DAT_1420a0478)
  VOID** ctxPtr = reinterpret_cast<VOID**>(base + 0x20a0478);
  if (!ctxPtr || !*ctxPtr) return nullptr;

  // CR15NetGame = *(context + 0x8518)
  VOID** netGamePtr = reinterpret_cast<VOID**>(static_cast<CHAR*>(*ctxPtr) + 0x8518);
  if (!netGamePtr || !*netGamePtr) return nullptr;

  // CR15NetGame + 0x28C8 = SBroadcasterData (inline)
  // SBroadcasterData + 0x08 = Broadcaster* owner
  CHAR* netGame = static_cast<CHAR*>(*netGamePtr);
  EchoVR::Broadcaster** ownerPtr = reinterpret_cast<EchoVR::Broadcaster**>(netGame + 0x28C8 + 0x08);
  if (!ownerPtr || !*ownerPtr) return nullptr;

  return *ownerPtr;
}

// Forward declarations for social functions
static void RefreshFriendsList();
VOID SendPlaceholderFriendsList();

/// Helper to convert a 16-byte UUID to a string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
static std::string UuidBytesToString(const uint8_t uuid[16]) {
  CHAR buf[37];
  snprintf(buf, sizeof(buf),
    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
    uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
    uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
  return std::string(buf);
}

/// Forward a party action to Nakama.
static void HandlePartyAction(EchoVR::SymbolId msgId, const void* msg, UINT64 msgSize) {
  if (!g_nakamaClient || !g_nakamaClient->IsAuthenticated()) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.SOCIAL] Not authenticated — party action ignored");
    return;
  }

  std::string partyId = g_nakamaClient->GetCurrentPartyId();

  if (msgId == SocialSym::PartyKickRequest && msgSize == sizeof(SNSPartyTargetPayload)) {
    const auto* p = static_cast<const SNSPartyTargetPayload*>(msg);
    std::string targetId = UuidBytesToString(p->target_user_uuid);
    if (!partyId.empty()) {
      g_nakamaClient->KickMember(partyId, targetId);
    }
  }
  else if (msgId == SocialSym::PartyPassOwnershipRequest && msgSize == sizeof(SNSPartyTargetPayload)) {
    const auto* p = static_cast<const SNSPartyTargetPayload*>(msg);
    std::string targetId = UuidBytesToString(p->target_user_uuid);
    if (!partyId.empty()) {
      g_nakamaClient->PromoteMember(partyId, targetId);
    }
  }
  else if (msgId == SocialSym::PartyRespondToInvite && msgSize == sizeof(SNSPartyTargetPayload)) {
    const auto* p = static_cast<const SNSPartyTargetPayload*>(msg);
    if (p->param != 0 && !partyId.empty()) {
      g_nakamaClient->JoinParty(partyId);
    }
  }
  else if (msgId == SocialSym::PartyMemberUpdate && !partyId.empty()) {
    std::vector<NevrClient::PartyMember> members;
    g_nakamaClient->ListPartyMembers(partyId, members);
  }
}

/// Send an SNS response back to pnsrad via BroadcasterReceiveLocalEvent.
static void SendSocialResponse(EchoVR::SymbolId hash, const char* name, void* payload, uint64_t size) {
  EchoVR::Broadcaster* broadcaster = GetBroadcasterFromGame();
  if (!broadcaster) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] Cannot send %s — broadcaster unavailable", name);
    return;
  }
  EchoVR::BroadcasterReceiveLocalEvent(broadcaster, hash, name, payload, size);
}

/// Forward a friend action to Nakama and send the appropriate SNS response.
static void HandleFriendAction(EchoVR::SymbolId msgId, const SNSFriendsActionPayload* payload) {
  if (!g_nakamaClient || !g_nakamaClient->IsAuthenticated()) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.SOCIAL] Nakama not connected — friend action ignored");
    return;
  }

  std::string targetId = std::to_string(payload->target_user_id);

  if (msgId == SocialSym::FriendSendInviteRequest) {
    // Add friend by ID (or by name if target_user_id == 0)
    bool ok = (payload->target_user_id != 0)
        ? g_nakamaClient->AddFriend(targetId)
        : false;  // TODO: add_by_name needs name string from after payload

    // Send InviteSuccess or InviteFailure
    if (ok) {
      SNSFriendIdPayload resp = {};
      resp.friend_id = payload->target_user_id;
      SendSocialResponse(SocialSym::FriendInviteSuccess, "SNSFriendInviteSuccess", &resp, sizeof(resp));
    } else {
      SNSFriendNotifyPayload resp = {};
      resp.friend_id = payload->target_user_id;
      resp.status_code = static_cast<uint8_t>(EFriendInviteError::NotFound);
      SendSocialResponse(SocialSym::FriendInviteFailure, "SNSFriendInviteFailure", &resp, sizeof(resp));
    }

  } else if (msgId == SocialSym::FriendRemoveRequest) {
    g_nakamaClient->DeleteFriend(targetId);
    SNSFriendIdPayload resp = {};
    resp.friend_id = payload->target_user_id;
    SendSocialResponse(SocialSym::FriendRemoveNotify, "SNSFriendRemoveNotify", &resp, sizeof(resp));

  } else if (msgId == SocialSym::FriendActionRequest) {
    // Accept, reject, or block — server differentiates by state.
    // For now, treat all FriendActionRequest as "accept" (most common UI action).
    // TODO: differentiate based on pnsrad's internal state for the target user.
    bool ok = g_nakamaClient->AddFriend(targetId);
    if (ok) {
      SNSFriendNotifyPayload resp = {};
      resp.friend_id = payload->target_user_id;
      SendSocialResponse(SocialSym::FriendAcceptSuccess, "SNSFriendAcceptSuccess", &resp, sizeof(resp));
    }
  }

  // Refresh friend list after any action
  RefreshFriendsList();
}

/// Fetch friends from Nakama and send updated ListResponse to pnsrad.
static void RefreshFriendsList() {
  if (!g_nakamaClient || !g_nakamaClient->IsAuthenticated()) {
    SendPlaceholderFriendsList();
    return;
  }

  std::vector<NevrFriend> friends;
  if (!g_nakamaClient->ListFriends(-1, friends)) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] Failed to fetch friends from Nakama");
    SendPlaceholderFriendsList();
    return;
  }

  // Count by state and online status
  uint32_t nonline = 0, noffline = 0, nbusy = 0, nsent = 0, nrecv = 0;
  for (const auto& f : friends) {
    switch (f.state) {
      case 0:  // friend
        if (f.online) nonline++;
        else noffline++;
        break;
      case 1: nsent++; break;     // invite sent
      case 2: nrecv++; break;     // invite received
      case 3: break;              // blocked — don't count
    }
  }

  SNSFriendsListResponse response = {};
  response.nonline = nonline;
  response.noffline = noffline;
  response.nbusy = nbusy;
  response.nsent = nsent;
  response.nrecv = nrecv;

  SendSocialResponse(SocialSym::FriendListResponse, "SNSFriendsListResponse", &response, sizeof(response));

  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] Friends list: online=%u offline=%u sent=%u recv=%u (total %zu)",
      nonline, noffline, nsent, nrecv, friends.size());
}

/// Log handler for intercepted social SNS messages from pnsrad.
VOID __fastcall OnSocialMessage(PVOID pGame, EchoVR::SymbolId msgId, PVOID msg, UINT64 msgSize) {
  const char* name = "unknown";

  // Friends outgoing
  if (msgId == SocialSym::FriendSendInviteRequest) name = "FriendSendInviteRequest";
  else if (msgId == SocialSym::FriendRemoveRequest) name = "FriendRemoveRequest";
  else if (msgId == SocialSym::FriendActionRequest) name = "FriendActionRequest";
  // Party outgoing
  else if (msgId == SocialSym::PartyKickRequest) name = "PartyKickRequest";
  else if (msgId == SocialSym::PartyPassOwnershipRequest) name = "PartyPassOwnershipRequest";
  else if (msgId == SocialSym::PartyRespondToInvite) name = "PartyRespondToInvite";
  else if (msgId == SocialSym::PartyMemberUpdate) name = "PartyMemberUpdate";

  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] SNS message: %s (hash=0x%llx, size=%llu)",
      name, (unsigned long long)msgId, (unsigned long long)msgSize);

  // Log hex payload for debugging
  if (msg && msgSize > 0 && msgSize <= 0x30) {
    const BYTE* bytes = static_cast<const BYTE*>(msg);
    CHAR hex[256] = {0};
    for (UINT64 i = 0; i < msgSize && i < 48; i++) {
      sprintf(hex + i * 3, "%02x ", bytes[i]);
    }
    Log(EchoVR::LogLevel::Debug, "[NEVR.SOCIAL]   payload: %s", hex);
  }

  // Forward friend actions to Nakama
  if (msgSize == sizeof(SNSFriendsActionPayload) &&
      (msgId == SocialSym::FriendSendInviteRequest ||
       msgId == SocialSym::FriendRemoveRequest ||
       msgId == SocialSym::FriendActionRequest)) {
    const auto* payload = static_cast<const SNSFriendsActionPayload*>(msg);
    Log(EchoVR::LogLevel::Debug, "[NEVR.SOCIAL]   routing_id=0x%llx target_user_id=%llu",
        (unsigned long long)payload->routing_id, (unsigned long long)payload->target_user_id);
    HandleFriendAction(msgId, payload);
  }

  // Forward party actions to Nakama
  if (msgId == SocialSym::PartyKickRequest ||
      msgId == SocialSym::PartyPassOwnershipRequest ||
      msgId == SocialSym::PartyRespondToInvite ||
      msgId == SocialSym::PartyMemberUpdate) {
    HandlePartyAction(msgId, msg, msgSize);
  }
}

/// Helper: register a broadcaster listener with a callback function.
static UINT16 ListenSocial(EchoVR::Broadcaster* broadcaster, EchoVR::SymbolId msgId, VOID* func) {
  EchoVR::DelegateProxy proxy = {};
  proxy.method[0] = 0xFFFFFFFFFFFFFFFF;  // DELEGATE_PROXY_INVALID_METHOD
  proxy.instance = g_pGame;
  proxy.proxyFunc = func;
  return EchoVR::BroadcasterListen(broadcaster, msgId, TRUE, &proxy, true);
}

/// Register listeners for social SNS messages to log and intercept them.
/// Called when the game enters Lobby state (broadcaster is available by then).
VOID RegisterSocialListeners() {
  EchoVR::Broadcaster* broadcaster = GetBroadcasterFromGame();
  if (!broadcaster) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] Broadcaster not available — social listeners skipped");
    return;
  }

  int count = 0;

  // Register for friends outgoing messages
  if (ListenSocial(broadcaster, SocialSym::FriendSendInviteRequest, reinterpret_cast<VOID*>(OnSocialMessage))) count++;
  if (ListenSocial(broadcaster, SocialSym::FriendRemoveRequest, reinterpret_cast<VOID*>(OnSocialMessage))) count++;
  if (ListenSocial(broadcaster, SocialSym::FriendActionRequest, reinterpret_cast<VOID*>(OnSocialMessage))) count++;

  // Register for party outgoing messages
  if (ListenSocial(broadcaster, SocialSym::PartyKickRequest, reinterpret_cast<VOID*>(OnSocialMessage))) count++;
  if (ListenSocial(broadcaster, SocialSym::PartyPassOwnershipRequest, reinterpret_cast<VOID*>(OnSocialMessage))) count++;
  if (ListenSocial(broadcaster, SocialSym::PartyRespondToInvite, reinterpret_cast<VOID*>(OnSocialMessage))) count++;
  if (ListenSocial(broadcaster, SocialSym::PartyMemberUpdate, reinterpret_cast<VOID*>(OnSocialMessage))) count++;

  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] Registered %d social message listeners", count);
}

/// Send a placeholder friends list response to pnsrad via the broadcaster.
/// This populates pnsrad's internal friend counts so the friends UI renders.
VOID SendPlaceholderFriendsList() {
  EchoVR::Broadcaster* broadcaster = GetBroadcasterFromGame();
  if (!broadcaster) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] Cannot send placeholder friends — broadcaster unavailable");
    return;
  }

  // Send a ListResponse with placeholder counts
  SNSFriendsListResponse response = {};
  response.header = 0;          // SNS correlation
  response.nonline = 3;         // 3 online friends
  response.noffline = 1;        // 1 offline friend
  response.nbusy = 0;
  response.nsent = 0;
  response.nrecv = 1;           // 1 pending friend request
  response.reserved = 0;

  EchoVR::BroadcasterReceiveLocalEvent(
      broadcaster,
      SocialSym::FriendListResponse,
      "SNSFriendsListResponse",
      &response,
      sizeof(response));

  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] Sent placeholder friends list (online=%u, offline=%u, pending=%u)",
      response.nonline, response.noffline, response.nrecv);
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

/// <summary>
/// A detour hook for the game's method it uses to transition from one net game state to another.
/// </summary>
/// <param name="game">A pointer to the game instance.</param>
/// <param name="state">The state to transition to.</param>
/// <returns>None</returns>
/// Tracks whether the server has entered a game session (InGame state).
/// Used to detect session completion when the state returns to Lobby.
static BOOL g_serverWasInGame = FALSE;

VOID NetGameSwitchStateHook(PVOID pGame, EchoVR::NetGameState state) {
  // Notify plugins of state change
  {
    static uint32_t s_prevState = 0;
    NvrGameContext ctx = {};
    ctx.base_addr = (uintptr_t)EchoVR::g_GameBaseAddress;
    ctx.net_game = pGame;
    ctx.game_state = static_cast<uint32_t>(state);
    ctx.flags = NEVR_HOST_HAS_NETGAME;
    if (g_isServer) ctx.flags |= NEVR_HOST_IS_SERVER;
    else ctx.flags |= NEVR_HOST_IS_CLIENT;
    NotifyPluginsStateChange(&ctx, s_prevState, static_cast<uint32_t>(state));
    s_prevState = static_cast<uint32_t>(state);
  }

  if (g_isServer) {
    // Redirect "load level failed" back to lobby instead of getting stuck
    if (state == EchoVR::NetGameState::LoadFailed) {
      Log(EchoVR::LogLevel::Debug,
          "[NEVR.PATCH] Dedicated server failed to load level. Resetting session to keep game server available.");
      EchoVR::NetGameScheduleReturnToLobby(pGame);
      return;
    }

    // Track when we enter a game session
    if (state == EchoVR::NetGameState::InGame) {
      g_serverWasInGame = TRUE;
    }

    // Session ended: we were in-game and now returning to lobby. Exit cleanly
    // so the fleet manager can spawn a fresh instance.
    if (g_serverWasInGame && state == EchoVR::NetGameState::Lobby) {
      Log(EchoVR::LogLevel::Info, "[NEVR] Session ended. Server exiting.");
      ExitProcess(0);
    }
  }

  // Capture the login session GUID when entering the lobby.
  // By this point the Lobby is initialized and localEntrants contains the server's
  // own login session at entrant[0]. The GUID was set by pnsrad.dll's LoginIdResponseCB
  // after the WebSocket login completed. We read it from the Lobby structure.
  if (state == EchoVR::NetGameState::Lobby && g_loginSessionId.Data1 == 0 && g_pGame) {
    // The game's CR15NetGame has a lobby at a known offset. The IServerLib::Initialize
    // already receives the Lobby*. But here we read it from the Lobby's localEntrants
    // pool, which contains LoginSession GUIDs for each entrant.
    // The server's own login session is the first one populated.

    // pnsrad.dll prints "LoginId: <GUID>:" to stdout (bypasses WriteLog), but it
    // also goes to the game's log file in _local/r14logs/. Find the latest log
    // and scan for the LoginId line.
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA("_local\\r14logs\\*.log", &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
      CHAR newestLog[MAX_PATH] = {};
      FILETIME newestTime = {};
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
          if (CompareFileTime(&fd.ftLastWriteTime, &newestTime) > 0) {
            newestTime = fd.ftLastWriteTime;
            snprintf(newestLog, MAX_PATH, "_local\\r14logs\\%s", fd.cFileName);
          }
        }
      } while (FindNextFileA(hFind, &fd));
      FindClose(hFind);

      if (newestLog[0] != '\0') {
        FILE* logFile = fopen(newestLog, "r");
        if (logFile) {
          CHAR line[512];
          while (fgets(line, sizeof(line), logFile)) {
            CHAR* loginIdStr = strstr(line, "LoginId: ");
            if (loginIdStr) {
              loginIdStr += 9;
              unsigned long d1;
              unsigned int d2, d3, d4[8];
              if (sscanf(loginIdStr, "%8lX-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X",
                         &d1, &d2, &d3, &d4[0], &d4[1], &d4[2], &d4[3], &d4[4], &d4[5], &d4[6], &d4[7]) == 11) {
                g_loginSessionId.Data1 = d1;
                g_loginSessionId.Data2 = (USHORT)d2;
                g_loginSessionId.Data3 = (USHORT)d3;
                for (int i = 0; i < 8; i++) g_loginSessionId.Data4[i] = (BYTE)d4[i];
              }
            }
          }
          fclose(logFile);
        }
      }
    }

    if (g_loginSessionId.Data1 != 0) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] Captured login session: %08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
          g_loginSessionId.Data1, g_loginSessionId.Data2, g_loginSessionId.Data3,
          g_loginSessionId.Data4[0], g_loginSessionId.Data4[1], g_loginSessionId.Data4[2],
          g_loginSessionId.Data4[3], g_loginSessionId.Data4[4], g_loginSessionId.Data4[5],
          g_loginSessionId.Data4[6], g_loginSessionId.Data4[7]);
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Login session GUID not found in game log");
    }
  }

  // Initialize social features when entering lobby (broadcaster is available by now)
  static BOOL g_socialInitialized = FALSE;
  if (!g_socialInitialized && state == EchoVR::NetGameState::Lobby) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] Entering lobby — initializing social features");

    // Configure Nakama client from config.json
    if (!g_nakamaClient) {
      g_nakamaClient = new NevrClient();
    }
    if (g_localConfig) {
      CHAR* nakamaUrl = EchoVR::JsonValueAsString(
          const_cast<EchoVR::Json*>(g_localConfig), const_cast<CHAR*>("nevr_url"), nullptr, false);
      CHAR* nevrHttpKey = EchoVR::JsonValueAsString(
          const_cast<EchoVR::Json*>(g_localConfig), const_cast<CHAR*>("nevr_http_key"), nullptr, false);
      CHAR* nevrServerKey = EchoVR::JsonValueAsString(
          const_cast<EchoVR::Json*>(g_localConfig), const_cast<CHAR*>("nevr_server_key"), nullptr, false);

      if (nakamaUrl && nevrHttpKey) {
        g_nakamaClient->Configure(nakamaUrl, nevrHttpKey, nevrServerKey ? nevrServerKey : "", "", "");

        // Clients authenticate via device code flow (Discord OAuth on web).
        // Servers don't need social auth.
        if (!g_isServer) {
          // TODO: check for cached token in _local/auth.json first
          Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] Starting device code authentication (Discord OAuth)...");
          if (g_nakamaClient->RunDeviceAuthFlow()) {
            Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] Authenticated via Discord — social features active");
          } else {
            Log(EchoVR::LogLevel::Warning, "[NEVR.SOCIAL] Auth failed — social features using placeholders");
          }
        }
      } else {
        Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] Nakama not configured (need nevr_url + nevr_http_key in config.json)");
      }
    }

    RegisterSocialListeners();
    RefreshFriendsList();  // Uses Nakama if authenticated, else placeholder
    g_socialInitialized = TRUE;
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

  EchoVR::AddArgSyntax(pArgSyntax, "-noconsole", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-noconsole",
                           "[NEVR] Disable console window creation (must be used with -headless)");

  EchoVR::AddArgSyntax(pArgSyntax, "-config-path", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-config-path", "[NEVR] Specify a custom path to the config.json file");

  EchoVR::AddArgSyntax(pArgSyntax, "-exitonerror", 0, 0, FALSE);
  EchoVR::AddArgHelpString(
      pArgSyntax, "-exitonerror",
      "[NEVR] Exit server when serverdb connection is lost (deferred to end of round + 30s if round is active)");

  EchoVR::AddArgSyntax(pArgSyntax, "-notelemetry", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-notelemetry", "[NEVR] Disable telemetry streaming to telemetry server");

  EchoVR::AddArgSyntax(pArgSyntax, "-telemetryrate", 1, 1, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-telemetryrate",
                           "[NEVR] Set telemetry streaming rate in Hz (default 10)");

  EchoVR::AddArgSyntax(pArgSyntax, "-telemetrydiag", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-telemetrydiag",
                           "[NEVR] Log telemetry snapshot diagnostics (pointer chain, values) every second");

  EchoVR::AddArgSyntax(pArgSyntax, "-timestamps", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-timestamps",
                           "[NEVR] Prefix all log lines with high-resolution timestamps");

  EchoVR::AddArgSyntax(pArgSyntax, "-upnp", 0, 0, FALSE);
  EchoVR::AddArgHelpString(pArgSyntax, "-upnp",
                           "[NEVR] Enable UPnP port forwarding for the broadcaster UDP port");

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
      g_isServer = TRUE;
    } else if (lstrcmpW(arg, L"-offline") == 0) {
      g_isOffline = TRUE;
    } else if (lstrcmpW(arg, L"-noconsole") == 0) {
      g_noConsole = TRUE;
    } else if (lstrcmpW(arg, L"-headless") == 0) {
      g_isHeadless = TRUE;
    } else if (lstrcmpW(arg, L"-windowed") == 0) {
      g_isWindowed = TRUE;
    } else if (lstrcmpW(arg, L"-noovr") == 0) {
      g_isNoOVR = TRUE;
    } else if (lstrcmpW(arg, L"-exitonerror") == 0) {
      g_exitOnError = TRUE;
    } else if (lstrcmpW(arg, L"-notelemetry") == 0) {
      g_telemetryEnabled = FALSE;
    } else if (lstrcmpW(arg, L"-telemetryrate") == 0) {
      if (i + 1 < argc) {
        g_telemetryRateHz = std::wcstoul(argv[i + 1], nullptr, 10);
        if (g_telemetryRateHz == 0) g_telemetryRateHz = 10;
        ++i;
      } else {
        FatalError("Missing argument for -telemetryrate. Provide a rate in Hz (e.g., 10).", NULL);
      }
    } else if (lstrcmpW(arg, L"-telemetrydiag") == 0) {
      g_telemetryDiag = TRUE;
    } else if (lstrcmpW(arg, L"-timestamps") == 0) {
      g_timestampLogs = TRUE;
    } else if (lstrcmpW(arg, L"-upnp") == 0) {
      g_upnpEnabled = TRUE;
    } else if (lstrcmpW(arg, L"-timestep") == 0) {
      if (i + 1 < argc) {
        g_headlessTimeStep = std::wcstoul(argv[i + 1], nullptr, 10);
        ++i;  // Skip the next argument (the value)
      } else {
        FatalError(
            "Missing argument for -timestep. Provide a positive number for fixed tick rate, or zero for unthrottled.",
            NULL);
      }
    } else if (lstrcmpW(arg, L"-config-path") == 0) {
      if (i + 1 < argc) {
        int len = WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, g_customConfigJsonPath, MAX_PATH, NULL, NULL);
        if (len == 0) {
          FatalError("Failed to convert -config-path to multi-byte string.", NULL);
        }
        ++i;  // Skip the next argument (the path value)
      } else {
        FatalError("Missing argument for -config-path. Provide a path to a config.json file.", NULL);
      }
    }
  }

  // -server requires -headless and -noovr — the game's own CLI parser needs these
  // on the command line for VR init bypass and renderer skip. Set them internally
  // for our code, but warn if they're missing from the command line since the game
  // won't have processed them.
  if (g_isServer) {
    if (!g_isHeadless) {
      g_isHeadless = TRUE;
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] -server without -headless — add -headless to the command line");
    }
    if (!g_isNoOVR) {
      g_isNoOVR = TRUE;
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] -server without -noovr — add -noovr to the command line");
    }
  }

  // Auto-enable -noconsole on Wine/Linux
  if (!g_noConsole) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (ntdll && GetProcAddress(ntdll, "wine_get_version") != NULL) {
      g_noConsole = TRUE;
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Wine detected — defaulting to -noconsole");
    }
  }

  // Validate argument combinations
  if (g_isServer && g_isOffline) {
    FatalError("Arguments -server and -offline are mutually exclusive.", NULL);
  }

  // Store the game pointer globally for social feature access
  g_pGame = pGame;

  // Apply patches based on arguments.
  if (g_isOffline) {
    PatchEnableOffline();
  }

  if (g_isHeadless) {
    PatchEnableHeadless(pGame);
  }

  // If the windowed, server, or headless flags were provided, apply the windowed mode patch to not use a VR headset.
  if (g_isWindowed || g_isServer || g_isHeadless) {
    using namespace PatchAddresses;
    // Set windowed mode flag in game structure
    UINT64* windowedFlags = reinterpret_cast<UINT64*>(static_cast<CHAR*>(pGame) + GAME_WINDOWED_FLAGS_OFFSET);
    *windowedFlags |= 0x0100000;  // Enable windowed mode (spectator uses 0x2100000 for additional settings)
  }

  // Apply patches to force the game to load as a server.
  if (g_isServer) {
    PatchDisableServerRendering(pGame);
    PatchEnableServer();
    PatchDisableLoadingTips();
    PatchBypassOvrPlatform();
    PatchBlockOculusSDK();
    PatchDisableWwise();
    PatchLogServerProfile();

    // Replace CPrecisionSleep's QPC busy-wait with Wine-friendly Sleep().
    // Only for headless servers — clients need precise frame timing for VR rendering.
    if (g_isHeadless) {
      PatchServerFramePacing();
    }
  }

  // Update the window title
  if (g_hWindow != NULL && g_isNoOVR) EchoVR::SetWindowTextA_(g_hWindow, "Echo VR - [DEMO]");

  // Load plugins from plugins/ subdirectory (after CLI flags are known)
  LoadPlugins();

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
  if (g_customConfigJsonPath[0] != '\0') {
    // Resolve to full path so the game's loader can find it regardless of CWD
    CHAR resolvedPath[MAX_PATH] = {0};
    DWORD len = GetFullPathNameA(g_customConfigJsonPath, MAX_PATH, resolvedPath, NULL);
    const CHAR* configPath = (len > 0 && len < MAX_PATH) ? resolvedPath : g_customConfigJsonPath;

    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Loading custom config from: %s", configPath);

    // Get the config destination pointer (pGame + 0x63240)
    using namespace PatchAddresses;
    EchoVR::Json* configDest = reinterpret_cast<EchoVR::Json*>(static_cast<CHAR*>(pGame) + GAME_LOCAL_CONFIG_OFFSET);

    // Call the game's internal JSON loader directly with our custom path
    // The third parameter (1) indicates the validation level: 1 = standard validation
    UINT32 loadResult = EchoVR::LoadJsonFromFile(configDest, configPath, 1);

    if (loadResult != 0) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load custom config file: %s (error %u)", configPath,
          loadResult);
      // Fall back to loading the default config
      result = EchoVR::LoadLocalConfig(pGame);
    } else {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Successfully loaded custom config from: %s", configPath);
      result = 0;  // Success
    }
  } else {
    // No custom config specified, use the default loader
    result = EchoVR::LoadLocalConfig(pGame);
  }

  // Configure fixed timestep if specified
  // Note: This is placed here because the required structures must be initialized first
  if ((g_isHeadless || g_isServer) && g_headlessTimeStep != 0) {
    using namespace PatchAddresses;

    // Set the fixed time step value (in microseconds)
    // The timestep is stored in a nested structure accessed via pointer
    UINT32* timeStepPtr = reinterpret_cast<UINT32*>(
        *reinterpret_cast<CHAR**>(EchoVR::g_GameBaseAddress + FIXED_TIMESTEP_PTR) + FIXED_TIMESTEP_OFFSET);
    *timeStepPtr = 1000000 / g_headlessTimeStep;  // Convert Hz to microseconds

    // Fix delta time calculation for fixed timestep mode
    // Changes condition: if (deltaTime > timeStep) to use correct comparison
    const BYTE deltaTimeFix[] = {0x73, 0x7A};  // JAE +0x7A (unsigned comparison)
    static_assert(sizeof(deltaTimeFix) == HEADLESS_DELTATIME_SIZE, "HEADLESS_DELTATIME patch size mismatch");
    ApplyPatch(HEADLESS_DELTATIME, deltaTimeFix, sizeof(deltaTimeFix));
  }

  // Store a reference to the local config from the game structure
  using namespace PatchAddresses;
  g_localConfig = reinterpret_cast<EchoVR::Json*>(static_cast<CHAR*>(pGame) + GAME_LOCAL_CONFIG_OFFSET);

  // Configure Asset CDN URL from config.json if specified
  if (g_localConfig != NULL) {
    CHAR* customCdnUrl = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"asset_cdn_url", NULL, false);
    if (customCdnUrl != NULL && customCdnUrl[0] != '\0') {
      // AssetCDN::SetCustomCdnUrl(customCdnUrl);
      // Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Asset CDN URL set from config: %s", customCdnUrl);
    }

    // exitonerror
    CHAR* exitOnErrorVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"exitonerror", NULL, false);
    if (exitOnErrorVal != NULL && (strcmp(exitOnErrorVal, "true") == 0 || strcmp(exitOnErrorVal, "1") == 0)) {
      g_exitOnError = TRUE;
    }

    // upnp
    CHAR* upnpVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"upnp", NULL, false);
    if (upnpVal != NULL && (strcmp(upnpVal, "true") == 0 || strcmp(upnpVal, "1") == 0)) {
      g_upnpEnabled = TRUE;
    }

    // upnp_port (external port override)
    CHAR* upnpPortVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"upnp_port", NULL, false);
    if (upnpPortVal != NULL && upnpPortVal[0] != '\0') {
      UINT32 port = strtoul(upnpPortVal, nullptr, 10);
      if (port > 0 && port <= 65535) g_upnpPort = (UINT16)port;
    }

    // internal_ip override
    CHAR* internalIpVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"internal_ip", NULL, false);
    if (internalIpVal != NULL && internalIpVal[0] != '\0') {
      strncpy(g_internalIpOverride, internalIpVal, sizeof(g_internalIpOverride) - 1);
    }

    // external_ip override
    CHAR* externalIpVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"external_ip", NULL, false);
    if (externalIpVal != NULL && externalIpVal[0] != '\0') {
      strncpy(g_externalIpOverride, externalIpVal, sizeof(g_externalIpOverride) - 1);
    }

    // Arena rule overrides (float values, 0 = use game default)
    CHAR* arenaRoundTimeVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"arena_round_time", NULL, false);
    if (arenaRoundTimeVal != NULL && arenaRoundTimeVal[0] != '\0') {
      g_arenaRoundTime = (FLOAT)atof(arenaRoundTimeVal);
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Arena round time override: %.0f seconds", g_arenaRoundTime);
    }
    CHAR* arenaCelebrationVal =
        EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"arena_celebration_time", NULL, false);
    if (arenaCelebrationVal != NULL && arenaCelebrationVal[0] != '\0') {
      g_arenaCelebrationTime = (FLOAT)atof(arenaCelebrationVal);
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Arena celebration time override: %.1f seconds", g_arenaCelebrationTime);
    }
    CHAR* arenaMercyVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"arena_mercy_score", NULL, false);
    if (arenaMercyVal != NULL && arenaMercyVal[0] != '\0') {
      g_arenaMercyScore = (FLOAT)atof(arenaMercyVal);
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Arena mercy score override: %.0f", g_arenaMercyScore);
    }
  }

  return result;
}

/// <summary>
/// Hook for CJson_GetFloat to override arena rule config values at load time.
/// Matches specific JSON path suffixes and returns overridden values from config.json.
/// </summary>
FLOAT CJsonGetFloatHook(PVOID root, const CHAR* path, FLOAT defaultValue, INT32 required) {
  FLOAT result = EchoVR::CJsonGetFloat(root, path, defaultValue, required);

  if (path != NULL) {
    // point_score_celebration_time (but not the _private variant)
    if (g_arenaCelebrationTime > 0.0f && strstr(path, "point_score_celebration_time") != NULL &&
        strstr(path, "_private") == NULL) {
      return g_arenaCelebrationTime;
    }
    // round_time (but not round_time_private)
    if (g_arenaRoundTime > 0.0f && strstr(path, "round_time") != NULL && strstr(path, "_private") == NULL &&
        strstr(path, "round_timer") == NULL && strstr(path, "sudden_death_round_time") == NULL) {
      return g_arenaRoundTime;
    }
    // mercy_win_point_spread (but not the _private variant)
    if (g_arenaMercyScore > 0.0f && strstr(path, "mercy_win_point_spread") != NULL &&
        strstr(path, "_private") == NULL) {
      return g_arenaMercyScore;
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
  // Use game config if available, fall back to early-loaded config
  EchoVR::Json* config = g_localConfig ? g_localConfig : g_earlyConfigPtr;
  if (config == NULL) return (CHAR*)defaultUrl;

  // Try primary service key first
  CHAR* host = EchoVR::JsonValueAsString(config, (CHAR*)serviceKey, NULL, false);
  if (host != NULL && host[0] != '\0') {
    Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Service override [%s]: %s", serviceKey, host);
    return host;
  }

  // Fallback to loginservice_host if primary key not found
  host = EchoVR::JsonValueAsString(config, (CHAR*)"loginservice_host", NULL, false);
  if (host != NULL && host[0] != '\0') {
    Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Service fallback [%s → loginservice_host]: %s", serviceKey, host);
    return host;
  }

  // Return default URL
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Service default [%s]: %s", serviceKey, defaultUrl);
  return (CHAR*)defaultUrl;
}

/// <summary>
/// A detour hook for the game's HTTP(S) connect function. Redirects hardcoded endpoints
/// using config.json overrides with fallback chain: service_key → loginservice_host → default.
///
/// The game has NO centralized service endpoint registry. Each service manages its own
/// endpoint independently (CLoginService: loginservice_host, CNSRadMatchmaking:
/// matchingservice_host, CHTTPApi: hardcoded api.readyatdawn.com etc.). URL substring
/// matching is the correct approach given this per-service architecture.
/// </summary>
UINT64 HttpConnectHook(PVOID unk, CHAR* uri) {
  // If we have a local config, check for service overrides with fallback logic
  if (g_localConfig != NULL) {
    CHAR* originalUri = uri;

    // API Service (https://api.*)
    if (!strncmp(uri, "https://api.", 12)) {
      uri = GetServiceHostWithFallback("apiservice_host", uri);
      // Legacy compatibility: also try "api_host"
      if (uri == originalUri) {
        uri = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"api_host", uri, false);
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
      uri = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"graph_host", uri, false);
      if (uri == originalUri) {
        uri = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"graphservice_host", uri, false);
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
// JsonValueAsString Hook (intercepts config lookups to provide early overrides)
// ============================================================================

/// <summary>
/// Hook for JsonValueAsString. When the game's config doesn't have a key (returns the
/// hardcoded default), check our early-loaded _local/config.json for an override.
/// This is necessary because the game reads config values before LoadLocalConfig runs,
/// so hardcoded defaults (readyatdawn.com URLs, "rad15_live", etc.) always win.
/// </summary>
CHAR* JsonValueAsStringHook(EchoVR::Json* root, CHAR* keyName, CHAR* defaultValue, BOOL reportFailure) {
  // Call the original first
  CHAR* result = EchoVR::JsonValueAsString(root, keyName, defaultValue, reportFailure);

  // If we have an early config, check if it has an override for this key.
  // Only override when the result equals the default (meaning the game's config didn't have it).
  // Don't override lookups against our own early config (avoid infinite loop).
  if (g_earlyConfigPtr != NULL && keyName != NULL && root != g_earlyConfigPtr && result == defaultValue) {
    CHAR* override = EchoVR::JsonValueAsString(g_earlyConfigPtr, keyName, NULL, false);
    if (override != NULL && override[0] != '\0') {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Config override [%s]: %s → %s", keyName,
          result ? result : "(null)", override);
      return override;
    }
  }

  return result;
}

// ============================================================================
// SSL/TLS Modernization (Schannel hooks for ECDSA/EdDSA/RSA support)
// WinHTTP is replaced by libcurl (CoCreateInstance hook), but Schannel
// (Windows TLS provider) is still used by game code that makes TLS connections
// directly through the Windows security APIs. This hook enables modern TLS
// protocols and cipher suites for those remaining code paths.
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
/// Intercepts RadPluginShutdown on platform DLLs (pnsdemo/pnsovr) to prevent crash.
///
/// The reconstruction confirms RadPluginShutdown is a DLL export on all platform DLLs
/// (pnsrad.def:7, pnsovr.cpp:147-172, pnsdemo.cpp:140). An alternative approach would
/// be to hook RadPluginShutdown directly on each platform DLL after load, but this
/// GetProcAddress interception catches the exact moment the game resolves the function
/// and is simpler than coordinating with LoadLibrary hooks.
/// </summary>
FARPROC GetProcAddressHook(HMODULE hModule, LPCSTR lpProcName) {
  // Platform DLLs (pnsdemo/pnsovr) crash during RadPluginShutdown due to freed memory.
  // Detect platform DLLs by checking for the "Users" export they all define.
  if (g_isServer && strcmp(lpProcName, "RadPluginShutdown") == 0) {
    if (EchoVR::GetProcAddress(hModule, "Users") != NULL) exit(0);
  }

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
  g_hWindow = hWnd;

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
/// Crash Reporter Suppression (CreateProcessA/W + ExitProcess + TerminateProcess + VEH)
///
/// BugSplat64.dll is a separate third-party DLL imported by echovr.exe that launches
/// BsSndRpt64.exe. The crash reporter launch happens INSIDE BugSplat64.dll, not in game
/// code — there is no single hook point in echovr.exe that controls it. We must intercept
/// at the Windows API level:
///   - CreateProcessA/W: Block BsSndRpt64.exe launch
///   - ExitProcess: Suppress termination after crash reporter block
///   - TerminateProcess: Prevent self-kill after crash reporter block
///   - VEH (BreakpointVEH): Skip int3 padding byte after suppressed ExitProcess return
/// </summary>
typedef BOOL(WINAPI* CreateProcessAFunc)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                         LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
CreateProcessAFunc OriginalCreateProcessA = nullptr;

BOOL WINAPI CreateProcessAHook(LPCSTR lpApplicationName, LPSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
                               LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags,
                               LPVOID lpEnvironment, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo,
                               LPPROCESS_INFORMATION lpProcessInformation) {
  // Block crash reporter executable (BsSndRpt64.exe) to prevent Wine errors
  if (lpApplicationName && strstr(lpApplicationName, "BsSndRpt")) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Blocked crash reporter launch (A): %s", lpApplicationName);
    return FALSE;  // Pretend the process failed to start
  }
  if (lpCommandLine && strstr(lpCommandLine, "BsSndRpt")) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Blocked crash reporter launch (cmdline A): %s", lpCommandLine);
    return FALSE;
  }

  // Allow all other process launches
  return OriginalCreateProcessA(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
                                bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo,
                                lpProcessInformation);
}

/// <summary>
/// Hook for CreateProcessW to disable crash reporter (wide-char version)
/// </summary>
typedef BOOL(WINAPI* CreateProcessWFunc)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                         LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
CreateProcessWFunc OriginalCreateProcessW = nullptr;
static bool g_crashReporterSuppressed = false;
static bool g_justSuppressedCrash = false;

BOOL WINAPI CreateProcessWHook(LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                               LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
                               BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
                               LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
                               LPPROCESS_INFORMATION lpProcessInformation) {
  // Suppress crash reporter executable (BsSndRpt64.exe) by faking successful launch
  if (lpApplicationName && wcsstr(lpApplicationName, L"BsSndRpt")) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Suppressed crash reporter launch (W): %ls", lpApplicationName);
    g_crashReporterSuppressed = true;
    if (lpProcessInformation) {
      ZeroMemory(lpProcessInformation, sizeof(PROCESS_INFORMATION));
      lpProcessInformation->hProcess = (HANDLE)0xDEADBEEF;
      lpProcessInformation->hThread = (HANDLE)0xDEADBEEF;
      lpProcessInformation->dwProcessId = 0xDEADBEEF;
      lpProcessInformation->dwThreadId = 0xDEADBEEF;
    }
    return TRUE;
  }
  if (lpCommandLine && wcsstr(lpCommandLine, L"BsSndRpt")) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Suppressed crash reporter launch (cmdline W): %ls", lpCommandLine);
    g_crashReporterSuppressed = true;
    if (lpProcessInformation) {
      ZeroMemory(lpProcessInformation, sizeof(PROCESS_INFORMATION));
      lpProcessInformation->hProcess = (HANDLE)0xDEADBEEF;
      lpProcessInformation->hThread = (HANDLE)0xDEADBEEF;
      lpProcessInformation->dwProcessId = 0xDEADBEEF;
      lpProcessInformation->dwThreadId = 0xDEADBEEF;
    }
    return TRUE;
  }

  // Allow all other process launches
  return OriginalCreateProcessW(lpApplicationName, lpCommandLine, lpProcessAttributes, lpThreadAttributes,
                                bInheritHandles, dwCreationFlags, lpEnvironment, lpCurrentDirectory, lpStartupInfo,
                                lpProcessInformation);
}

/// <summary>
/// Hook for CreateDirectoryW to fix "_temp" directory creation failure
/// </summary>
typedef BOOL(WINAPI* CreateDirectoryWFunc)(LPCWSTR, LPSECURITY_ATTRIBUTES);
CreateDirectoryWFunc OriginalCreateDirectoryW = nullptr;

BOOL WINAPI CreateDirectoryWHook(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
  if (lpPathName && wcsstr(lpPathName, L"_temp")) {
    wchar_t fixedPath[512];
    const wchar_t* pathToUse = lpPathName;

    if (wcsncmp(lpPathName, L"\\\\?\\", 4) == 0 && lpPathName[4] != L'\\' && lpPathName[5] != L':') {
      WCHAR currentDir[MAX_PATH];
      GetCurrentDirectoryW(MAX_PATH, currentDir);
      _snwprintf(fixedPath, 512, L"%ls\\%ls", currentDir, lpPathName + 4);
      pathToUse = fixedPath;
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Fixed malformed NT path: '%ls' -> '%ls'", lpPathName, fixedPath);
    }

    BOOL result = OriginalCreateDirectoryW(pathToUse, lpSecurityAttributes);
    DWORD lastError = GetLastError();

    if (!result) {
      if (lastError == ERROR_ALREADY_EXISTS) {
        Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Directory '%ls' already exists - returning success", pathToUse);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
      } else if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) {
        Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Parent path missing for '%ls', creating recursively", pathToUse);
        wchar_t parentPath[512];
        wcsncpy(parentPath, pathToUse, 512);
        wchar_t* lastSlash = wcsrchr(parentPath, L'\\');
        if (lastSlash && lastSlash != parentPath) {
          *lastSlash = L'\0';
          CreateDirectoryWHook(parentPath, lpSecurityAttributes);
        }
        result = OriginalCreateDirectoryW(pathToUse, lpSecurityAttributes);
        if (result || GetLastError() == ERROR_ALREADY_EXISTS) {
          Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Successfully created '%ls' after parent creation", pathToUse);
          SetLastError(ERROR_SUCCESS);
          return TRUE;
        }
      }
    } else {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Successfully created directory '%ls'", pathToUse);
    }

    SetLastError(lastError);
    return result;
  }

  return OriginalCreateDirectoryW(lpPathName, lpSecurityAttributes);
}

typedef BOOL(WINAPI* CreateDirectoryAFunc)(LPCSTR, LPSECURITY_ATTRIBUTES);
CreateDirectoryAFunc OriginalCreateDirectoryA = nullptr;

BOOL WINAPI CreateDirectoryAHook(LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes) {
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateDirectoryA('%s') called", lpPathName ? lpPathName : "<null>");

  BOOL result = OriginalCreateDirectoryA(lpPathName, lpSecurityAttributes);
  DWORD lastError = GetLastError();

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateDirectoryA result=%d, lastError=%lu", result, lastError);

  if (!result && lastError == ERROR_ALREADY_EXISTS) {
    if (lpPathName && strstr(lpPathName, "_temp")) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] CreateDirectoryA('%s') failed with ERROR_ALREADY_EXISTS - returning success", lpPathName);
      SetLastError(ERROR_SUCCESS);
      return TRUE;
    }
  }

  SetLastError(lastError);
  return result;
}

/// <summary>
/// Hook for ExitProcess to prevent crash reporter-triggered termination
/// </summary>
typedef VOID(WINAPI* ExitProcessFunc)(UINT);
ExitProcessFunc OriginalExitProcess = nullptr;

VOID WINAPI ExitProcessHook(UINT uExitCode) {
  // In server mode, always suppress ExitProcess — the game's crash reporting
  // chain calls it from multiple places (crash handler, SEH handler, C runtime).
  // We need ALL of them suppressed to keep the server alive.
  if (g_isServer) {
    static volatile LONG exitSuppressCount = 0;
    LONG count = InterlockedIncrement(&exitSuppressCount);
    if (count <= 5) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] ExitProcess(%u) suppressed in server mode (call #%ld)", uExitCode, count);
    }
    g_justSuppressedCrash = true;
    return;
  }

  if (g_crashReporterSuppressed) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] ExitProcess(%u) suppressed after crash reporter block - server continuing", uExitCode);

    void* stack[32];
    USHORT frames = CaptureStackBackTrace(0, 32, stack, NULL);
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Call stack (%u frames):", frames);
    for (USHORT i = 0; i < frames && i < 10; i++) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH]   Frame %u: %p", i, stack[i]);
    }

    g_crashReporterSuppressed = false;
    g_justSuppressedCrash = true;
    return;
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] ExitProcess(%u) called", uExitCode);
  OriginalExitProcess(uExitCode);
}

/// <summary>
/// Vectored Exception Handler to skip the int3 instruction that follows the ExitProcess call site
/// in the game's fatal error handler. After our ExitProcessHook returns (suppressing the exit),
/// the CPU executes the int3 padding byte at the return address, which would kill the process.
/// We advance RIP by 1 to skip it and continue execution.
/// </summary>
/// Counter for access violation recoveries
static volatile LONG g_avRecoveryCount = 0;

/// Log a full crash dump: exception info, registers, stack trace with RVAs.
/// All addresses are logged as RVAs relative to the game base so they match
/// revault / Ghidra / IDA directly.
static void LogCrashDump(PEXCEPTION_POINTERS ex) {
  PEXCEPTION_RECORD rec = ex->ExceptionRecord;
  PCONTEXT ctx = ex->ContextRecord;
  DWORD64 base = (DWORD64)EchoVR::g_GameBaseAddress;

  auto rva = [base](DWORD64 addr) -> INT64 {
    if (addr >= base && addr < base + 0x2000000) return (INT64)(addr - base);
    return -1;
  };

  auto fmtAddr = [base, rva](DWORD64 addr, char* buf, size_t sz) {
    INT64 r = rva(addr);
    if (r >= 0)
      snprintf(buf, sz, "0x%llX (game+0x%llX)", (unsigned long long)addr, (unsigned long long)r);
    else
      snprintf(buf, sz, "0x%llX (external)", (unsigned long long)addr);
  };

  // Exception type
  const char* excName = "Unknown";
  switch (rec->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION: excName = "ACCESS_VIOLATION"; break;
    case EXCEPTION_BREAKPOINT: excName = "BREAKPOINT"; break;
    case EXCEPTION_ILLEGAL_INSTRUCTION: excName = "ILLEGAL_INSTRUCTION"; break;
    case EXCEPTION_STACK_OVERFLOW: excName = "STACK_OVERFLOW"; break;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: excName = "INT_DIVIDE_BY_ZERO"; break;
  }

  char ripStr[80];
  fmtAddr(ctx->Rip, ripStr, sizeof(ripStr));

  Log(EchoVR::LogLevel::Error, "=== CRASH DUMP ===");
  Log(EchoVR::LogLevel::Error, "Exception: %s (0x%08lX)", excName, rec->ExceptionCode);
  Log(EchoVR::LogLevel::Error, "RIP: %s", ripStr);

  // Access violation details
  if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    const char* op = rec->ExceptionInformation[0] == 0 ? "READ"
                   : rec->ExceptionInformation[0] == 1 ? "WRITE"
                                                       : "EXECUTE";
    Log(EchoVR::LogLevel::Error, "Access: %s at 0x%llX", op, (unsigned long long)rec->ExceptionInformation[1]);
  }

  // Register dump
  Log(EchoVR::LogLevel::Error, "RAX=%016llX  RBX=%016llX  RCX=%016llX  RDX=%016llX",
      ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
  Log(EchoVR::LogLevel::Error, "RSI=%016llX  RDI=%016llX  RBP=%016llX  RSP=%016llX",
      ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
  Log(EchoVR::LogLevel::Error, " R8=%016llX   R9=%016llX  R10=%016llX  R11=%016llX",
      ctx->R8, ctx->R9, ctx->R10, ctx->R11);
  Log(EchoVR::LogLevel::Error, "R12=%016llX  R13=%016llX  R14=%016llX  R15=%016llX",
      ctx->R12, ctx->R13, ctx->R14, ctx->R15);

  // Stack scan — x64 doesn't use frame pointers consistently, so scan RSP
  // for return addresses that point into the game's code range.
  Log(EchoVR::LogLevel::Error, "Stack scan (game code return addresses):");
  DWORD64* sp = (DWORD64*)ctx->Rsp;
  int found = 0;
  for (int i = 0; i < 256 && found < 16; i++) {
    if (IsBadReadPtr(sp + i, 8)) break;
    DWORD64 val = sp[i];
    INT64 r = rva(val);
    if (r >= 0 && r < 0x1800000) {
      Log(EchoVR::LogLevel::Error, "  #%d  [RSP+0x%X] game+0x%llX", found, i * 8, (unsigned long long)r);
      found++;
    }
  }
  Log(EchoVR::LogLevel::Error, "=== END CRASH DUMP ===");
}

LONG WINAPI BreakpointVEH(PEXCEPTION_POINTERS pExceptionInfo) {
  if (pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && g_justSuppressedCrash) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] int3 after suppressed ExitProcess at RIP=%p — skipping, server continuing",
        (void*)pExceptionInfo->ContextRecord->Rip);
    pExceptionInfo->ContextRecord->Rip += 1;
    g_justSuppressedCrash = false;
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  // In server mode, catch null-pointer access violations and recover via longjmp
  if (g_isServer && pExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
    DWORD64 target = pExceptionInfo->ExceptionRecord->ExceptionInformation[1];

    if (target < 0x10000 && g_gameLoopJmpBufValid) {
      LONG count = InterlockedIncrement(&g_avRecoveryCount);
      if (count <= 3) LogCrashDump(pExceptionInfo);

      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] Null-ptr AV #%ld — longjmp to server hold", count);
      g_gameLoopJmpBufValid = false;
      longjmp(g_gameLoopJmpBuf, (int)count);
    }
  }

  return EXCEPTION_CONTINUE_SEARCH;
}

typedef BOOL(WINAPI* TerminateProcessFunc)(HANDLE, UINT);
TerminateProcessFunc OriginalTerminateProcess = nullptr;

BOOL WINAPI TerminateProcessHook(HANDLE hProcess, UINT uExitCode) {
  HANDLE currentProcess = GetCurrentProcess();
  if (hProcess == currentProcess || hProcess == (HANDLE)-1) {
    if (g_crashReporterSuppressed) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] TerminateProcess(self, %u) suppressed after crash reporter block - server continuing",
          uExitCode);
      g_crashReporterSuppressed = false;
      return TRUE;
    }
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] TerminateProcess(self, %u) called - allowing", uExitCode);
  }

  return OriginalTerminateProcess(hProcess, uExitCode);
}

/// <summary>
/// WinHTTP CLSID and IID constants from findings document
/// </summary>
static const CLSID CLSID_WinHttpRequest = {
    0x88d96a09, 0xf192, 0x11d4, {0xa6, 0x5f, 0x00, 0x40, 0x96, 0x32, 0x51, 0xe5}};

typedef HRESULT(WINAPI* CoCreateInstanceFunc)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
CoCreateInstanceFunc OriginalCoCreateInstance = nullptr;

HRESULT WINAPI CoCreateInstanceHook(REFCLSID rclsid, LPUNKNOWN pUnkOuter, DWORD dwClsContext, REFIID riid,
                                    LPVOID* ppv) {
  char logBuf[512];
  snprintf(logBuf, sizeof(logBuf),
           "[NEVR.PATCH] CoCreateInstance called: CLSID={%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
           rclsid.Data1, rclsid.Data2, rclsid.Data3, rclsid.Data4[0], rclsid.Data4[1], rclsid.Data4[2], rclsid.Data4[3],
           rclsid.Data4[4], rclsid.Data4[5], rclsid.Data4[6], rclsid.Data4[7]);
  Log(EchoVR::LogLevel::Info, logBuf);

  if (IsEqualCLSID(rclsid, CLSID_WinHttpRequest)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] WinHTTP COM → libcurl bridge");

    // The game stores COM-related data (IID, CLSID, type descriptors) in .rdata
    // which is read-only. Wine's COM implementation writes to these during
    // marshaling/QI, causing an AV. Make the surrounding pages writable.
    // The COM data lives around 0x16E8C88..0x16E9000 in the game's address space.
    static bool s_protectionFixed = false;
    if (!s_protectionFixed) {
      DWORD oldProtect;
      PVOID rdataStart = (PVOID)(EchoVR::g_GameBaseAddress + 0x16E8000);
      if (VirtualProtect(rdataStart, 0x2000, PAGE_READWRITE, &oldProtect)) {
        Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Made COM rdata page writable (was 0x%lX)", oldProtect);
      }
      s_protectionFixed = true;
    }

    extern HRESULT CreateWinHttpRequestStub(REFIID riid, void** ppvObject);
    HRESULT hr = CreateWinHttpRequestStub(riid, ppv);
    if (FAILED(hr)) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] WinHTTP stub creation failed: 0x%08lX", hr);
    }
    return hr;
  }

  return OriginalCoCreateInstance(rclsid, pUnkOuter, dwClsContext, riid, ppv);
}

/// <summary>
/// Initializes the patcher, executing startup patchs on the game and installing detours/hooks on various game
/// functions.
/// </summary>
/// <returns>None</returns>
VOID Initialize() {
  // If we already initialized the library, stop.
  if (g_initialized) return;
  g_initialized = true;

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

  // Early-load _local/config.json so URI redirect hooks work before the game loads its config.
  // The game's JSON loader is available at this point (it's a static function in the EXE).
  {
    CHAR configPath[MAX_PATH] = {0};
    CHAR moduleDir[MAX_PATH] = {0};
    GetModuleFileNameA((HMODULE)EchoVR::g_GameBaseAddress, moduleDir, MAX_PATH);
    // Strip the filename to get the directory
    CHAR* lastSlash = strrchr(moduleDir, '\\');
    if (lastSlash) *(lastSlash + 1) = '\0';
    snprintf(configPath, MAX_PATH, "%s_local\\config.json", moduleDir);

    UINT32 loadResult = EchoVR::LoadJsonFromFile(&g_earlyConfig, configPath, 1);
    if (loadResult == 0 && g_earlyConfig.root != NULL) {
      g_earlyConfigPtr = &g_earlyConfig;
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Early config loaded from: %s", configPath);
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to early-load config from: %s (error %u)", configPath,
          loadResult);
    }
  }

  // Hash discovery hooks (disabled by default - enable for reverse engineering)
  // Uncomment to capture replicated variable names and message type hashes
  /*
  OriginalCSymbol64_Hash = (CSymbol64_HashFunc)(EchoVR::g_GameBaseAddress + PatchAddresses::CSYMBOL64_HASH);
  PatchDetour(&OriginalCSymbol64_Hash, reinterpret_cast<PVOID>(CSymbol64_HashHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CSymbol64_Hash hook installed (variable name discovery)");

  OriginalCMatSym_Hash = (CMatSym_HashFunc)(EchoVR::g_GameBaseAddress + PatchAddresses::CMATSYM_HASH);
  PatchDetour(&OriginalCMatSym_Hash, reinterpret_cast<PVOID>(CMatSym_HashHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CMatSym_Hash hook installed (message type string discovery)");

  OriginalSMatSymData_HashA = (SMatSymData_HashAFunc)(EchoVR::g_GameBaseAddress + PatchAddresses::SMATSYMDATA_HASHA);
  PatchDetour(&OriginalSMatSymData_HashA, reinterpret_cast<PVOID>(SMatSymData_HashAHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] SMatSymData_HashA hook installed (final hash discovery)");

  OriginalSnsRegistryInsertSorted =
      (SnsRegistryInsertSortedFunc)(EchoVR::g_GameBaseAddress + PatchAddresses::SNS_REGISTRY_INSERT_SORTED);
  PatchDetour(&OriginalSnsRegistryInsertSorted, reinterpret_cast<PVOID>(SnsRegistryInsertSortedHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] sns_registry_insert_sorted hook installed (message registry discovery)");
  */

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
  PatchDetour(&EchoVR::CJsonGetFloat, reinterpret_cast<PVOID>(CJsonGetFloatHook));
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

  // Hook CreateProcessA/W to disable crash reporter in Wine/headless mode
  HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
  if (hKernel32 != NULL) {
    OriginalCreateProcessA = (CreateProcessAFunc)GetProcAddress(hKernel32, "CreateProcessA");
    if (OriginalCreateProcessA != NULL) {
      PatchDetour(&OriginalCreateProcessA, reinterpret_cast<PVOID>(CreateProcessAHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateProcessA hook installed (crash reporter disabled)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CreateProcessA");
    }

    OriginalCreateProcessW = (CreateProcessWFunc)GetProcAddress(hKernel32, "CreateProcessW");
    if (OriginalCreateProcessW != NULL) {
      PatchDetour(&OriginalCreateProcessW, reinterpret_cast<PVOID>(CreateProcessWHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateProcessW hook installed (crash reporter disabled)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CreateProcessW");
    }

    OriginalExitProcess = (ExitProcessFunc)GetProcAddress(hKernel32, "ExitProcess");
    if (OriginalExitProcess != NULL) {
      PatchDetour(&OriginalExitProcess, reinterpret_cast<PVOID>(ExitProcessHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] ExitProcess hook installed (prevents crash reporter termination)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find ExitProcess");
    }

    OriginalTerminateProcess = (TerminateProcessFunc)GetProcAddress(hKernel32, "TerminateProcess");
    if (OriginalTerminateProcess != NULL) {
      PatchDetour(&OriginalTerminateProcess, reinterpret_cast<PVOID>(TerminateProcessHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] TerminateProcess hook installed (prevents self-termination)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find TerminateProcess");
    }

    OriginalCreateDirectoryW = (CreateDirectoryWFunc)GetProcAddress(hKernel32, "CreateDirectoryW");
    if (OriginalCreateDirectoryW != NULL) {
      PatchDetour(&OriginalCreateDirectoryW, reinterpret_cast<PVOID>(CreateDirectoryWHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateDirectoryW hook installed (fixes _temp creation)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CreateDirectoryW");
    }

    OriginalCreateDirectoryA = (CreateDirectoryAFunc)GetProcAddress(hKernel32, "CreateDirectoryA");
    if (OriginalCreateDirectoryA != NULL) {
      PatchDetour(&OriginalCreateDirectoryA, reinterpret_cast<PVOID>(CreateDirectoryAHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] CreateDirectoryA hook installed (fixes _temp creation)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CreateDirectoryA");
    }
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load kernel32.dll for crash reporter hooks");
  }

  // Hook CoCreateInstance for WinHTTP replacement
  HMODULE hOle32 = GetModuleHandleA("ole32.dll");
  if (hOle32 != NULL) {
    OriginalCoCreateInstance = (CoCreateInstanceFunc)GetProcAddress(hOle32, "CoCreateInstance");
    if (OriginalCoCreateInstance != NULL) {
      PatchDetour(&OriginalCoCreateInstance, reinterpret_cast<PVOID>(CoCreateInstanceHook));
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] WinHTTP to libcurl hook installed (CoCreateInstance)");
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to find CoCreateInstance for WinHTTP hook");
    }
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load ole32.dll for WinHTTP hook");
  }

  // Hook game main wrapper — longjmp recovery on crash keeps server alive
  GameMain = (GameMainFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::GAME_MAIN);
  OriginalGameMainWrapper =
      (GameMainWrapperFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::GAME_MAIN_WRAPPER);
  PatchDetour(&OriginalGameMainWrapper, reinterpret_cast<PVOID>(GameMainWrapperHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Game main wrapper hook installed (server crash recovery)");

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

  // Hook BugSplat crash handler — prevents fatal exits in server mode.
  // The handler is called from 5 sites for missing actors, dialogue scenes, etc.
  // These are non-fatal in headless dedicated server mode.
  OriginalBugSplatCrashHandler =
      (BugSplatCrashHandlerFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::BUGSPLAT_CRASH_HANDLER);
  PatchDetour(&OriginalBugSplatCrashHandler, reinterpret_cast<PVOID>(BugSplatCrashHandlerHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] BugSplat crash handler hook installed (server crash suppression)");

  // Hook InitializeGlobalGameSpace to prevent fatal crash in server mode
  // (no local player actor exists in the global gamespace for dedicated servers)
  OriginalInitializeGlobalGameSpace =
      (InitializeGlobalGameSpaceFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::INIT_GLOBAL_GAMESPACE);
  PatchDetour(&OriginalInitializeGlobalGameSpace, reinterpret_cast<PVOID>(InitializeGlobalGameSpaceHook));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] InitializeGlobalGameSpace hook installed (server crash fix)");

  PatchDetour(&EchoVR::JsonValueAsString, reinterpret_cast<PVOID>(JsonValueAsStringHook));
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] Service endpoint override hook installed (JsonValueAsString)");

  // Install VEH to handle int3 that fires after our ExitProcess suppression returns
  AddVectoredExceptionHandler(1, BreakpointVEH);
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Breakpoint VEH installed (handles int3 after ExitProcess suppression)");

  // Install console ctrl handler so CTRL+C actually terminates the process.
  // The game registers its own handler that logs "Console close signal received" but doesn't exit.
  // Handlers are called LIFO, so ours runs first and terminates before the game's handler can swallow the signal.
  SetConsoleCtrlHandler(
      [](DWORD dwCtrlType) -> BOOL {
        if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_CLOSE_EVENT || dwCtrlType == CTRL_BREAK_EVENT) {
          Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Console signal %lu received — exiting", dwCtrlType);
          if (OriginalExitProcess)
            OriginalExitProcess(0);
          else
            ExitProcess(0);
          return TRUE;  // unreachable, but satisfies the signature
        }
        return FALSE;
      },
      TRUE);
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Console ctrl handler installed (CTRL+C will terminate)");

  // Run some startup patches
  PatchNoOvrRequiresSpectatorStream();

  // Initialize Asset CDN redirection system
  // if (!AssetCDN::Initialize()) {
  //  Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to initialize Asset CDN redirection");
  //}

  // Patch out the deadlock monitor thread's validation routine.
  // In debug mode: prevents deadlock panic from debugger breakpoint suspension.
  // In server/headless mode: prevents false deadlock detection during level transitions
  // (no GPU to keep frames flowing, causing the monitor thread to see stalls).
  // NOTE: Applied unconditionally because Initialize() runs before command-line parsing
  // (PreprocessCommandLineHook), so g_isServer/g_isHeadless aren't set yet. The deadlock
  // monitor is harmful in all Wine/headless scenarios and benign to disable on clients.
  PatchDeadlockMonitor();
}

// ============================================================================
// Cross-DLL exports (called by gameserver.dll via GetProcAddress)
// ============================================================================

/// Schedules a return to lobby via the game engine. No-op if pGame is not yet set.
extern "C" __declspec(dllexport) void NEVR_ScheduleReturnToLobby() {
  if (g_pGame) EchoVR::NetGameScheduleReturnToLobby(g_pGame);
}

/// Fills *out with the current UPnP configuration globals.
/// Layout must match NevRUPnPConfig in gameserver.cpp.
extern "C" __declspec(dllexport) void NEVR_GetUPnPConfig(NevRUPnPConfig* out) {
  if (!out) return;
  out->enabled = g_upnpEnabled;
  out->port    = g_upnpPort;
  memcpy(out->internalIp, g_internalIpOverride, sizeof(out->internalIp));
  memcpy(out->externalIp, g_externalIpOverride, sizeof(out->externalIp));
}

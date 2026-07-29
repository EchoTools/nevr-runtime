#include "runtime/patch/mode_patches.h"

#include <algorithm>
#include <string>

#include <psapi.h>
#include <setjmp.h>

#include "runtime/lifecycle/cli.h"
#include "runtime/hook/patching.h"
#include "core/globals.h"
#include "core/logging.h"
#include "abi/echovr_functions.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/hook_liveness.h"
#include "runtime/hook/process_memory.h"

// ============================================================================
// Headless graphics-gate skip helper
// ============================================================================

/// The CEngine init function (FUN_14154a950) enables graphics subsystems one by
/// one, each gated by a bit in the graphics-enable word at CEngine+0x2cfec. Each
/// gate is a `je <skip>` taken when the bit is clear (the game's own device-free
/// branch). In server mode the bits are set, so the game runs the GPU init and
/// asserts. Forcing `je`(0x74)->`jmp`(0xEB) — same rel8 displacement — makes the
/// game take its native graphics-disabled branch. Prologue-VALIDATED: patches
/// only when the current byte is the expected conditional-jump opcode (never
/// blind-write). `expectedOpcode` is the game's conditional jump — `je` (0x74)
/// for a direct bit test, `jne` (0x75) where the game inverts the flag first
/// (`not`/`test`/`jne`). Both become an unconditional `jmp` (0xEB) with the same
/// rel8 displacement, forcing the skip branch.
static VOID ForceHeadlessSkip(uintptr_t offset, BYTE expectedOpcode, const char* what) {
  const BYTE* site = reinterpret_cast<const BYTE*>(EchoVR::g_GameBaseAddress + offset);
  if (*site == expectedOpcode) {
    const BYTE jmp = 0xEB;
    ProcessMemcpy(EchoVR::g_GameBaseAddress + offset, const_cast<BYTE*>(&jmp), 1);
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.HEADLESS] %s — forced device-free branch (0x%02x->jmp) at +0x%lx",
        what, static_cast<unsigned>(expectedOpcode), static_cast<unsigned long>(offset));
  } else {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.HEADLESS] %s — prologue mismatch at +0x%lx (got 0x%02x, want 0x%02x) — NOT patched",
        what, static_cast<unsigned long>(offset), static_cast<unsigned>(*site),
        static_cast<unsigned>(expectedOpcode));
  }
}

// ============================================================================
// CEngineConfig copy hook — root-cause bit-0x1 clear for headless render
// ============================================================================

/// CEngineConfig::operator= (FUN_141547360) copies the source engine config
/// struct into CEngine+0x2cf98. Offset 0x54 in the copy is the 32-bit
/// graphics-enable word at CEngine+0x2cfec, which includes renderer-enable
/// bit 0x1. In server mode the source config has bit 0x1 SET; clearing it
/// AFTER the copy makes the ENTIRE render subsystem take the game's native
/// device-free path uniformly — covering render-worker threads that per-gate
/// branch-force byte patches cannot reach. Single caller (ReVault-verified:
/// CRenderPipeline::InitStages), so hooking here is safe and targeted.
typedef VOID CEngineConfigCopyFunc(PVOID dst, PVOID src);
static CEngineConfigCopyFunc* OriginalCEngineConfigCopy = nullptr;

static VOID CEngineConfigCopyHook(PVOID dst, PVOID src) {
  // Let the original copy complete — all fields including the graphics-enable
  // word are written normally.
  OriginalCEngineConfigCopy(dst, src);

  if (g_isServer) {
    // Clear renderer-enable bit 0x1 at [dst+0x54] = [CEngine+0x2cfec].
    // The copy destination (dst = CEngine+0x2cf98) receives the full config
    // struct from the source; offset 0x54 is the graphics-enable dword.
    // Clearing bit 0x1 makes ALL downstream bit-0x1 gates in the CEngine
    // init and render-worker threads take their native je device-free branches.
    uint32_t* enableWord = reinterpret_cast<uint32_t*>(static_cast<char*>(dst) + 0x54);
    uint32_t before = *enableWord;
    *enableWord &= ~0x1u;
    uint32_t after = *enableWord;
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.HEADLESS] CEngine config copy — cleared renderer bit 0x1: "
        "0x%08x -> 0x%08x",
        before, after);
  }
}

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

  // Hook CEngineConfig::operator= (FUN_141547360) to clear renderer-enable
  // bit 0x1 at [CEngine+0x2cfec] AFTER the config copy. Single-caller hook,
  // so MinHook installed once during init and fires during CEngine boot.
  // The bit-clear makes ALL downstream bit-0x1 gates (including on render-
  // worker threads) take their native je device-free branches.  The per-gate
  // branch-force byte patches below are redundant once this hook fires, but
  // they are harmless (je->jmp when the native je is already taken) and
  // serve as a defense-in-depth fallback.
  OriginalCEngineConfigCopy =
      reinterpret_cast<CEngineConfigCopyFunc*>(EchoVR::g_GameBaseAddress + CENGINE_CONFIG_COPY);
  PatchDetour(&OriginalCEngineConfigCopy, reinterpret_cast<PVOID>(CEngineConfigCopyHook), "CEngineConfigCopy");
  Log(EchoVR::LogLevel::Debug,
      "[NEVR.HEADLESS] CEngineConfig copy hook installed — bit-0x1 will be cleared after config copy");

  // Engine flags word at pGame+0x1D4 (N99).
  //
  // `-headless` is NOT a NEVR flag — it is native to echovr.exe, and its entire
  // effect in the whole binary is the single mask at 0x140504566, applied by
  // the game's own arg handler only when that literal token is present.
  // `-server` is NEVR-invented (there is no standalone "-server" string in
  // echovr.exe), so the game's parser never acted on it and NOTHING applied
  // that mask. g_isHeadless gated OUR patches only. This block wrote just
  // ENGINE_FLAGS_NOAUDIO_MASK, so bit 0 — the render/window master bit — stayed
  // SET on every `-server`-only run. Measured before this fix, `-server` alone:
  //   engine flags 0x00000137 -> 0x00000135 (bit0_render=SET(WINDOWED))
  // with one game window present for the entire run.
  //
  // Ordering is what makes applying it here correct: this function runs from
  // boot.cpp's `if (g_isHeadless)` block, strictly after `-server` sets that
  // global in the same hook and strictly before the trailing
  // EchoVR::PreprocessCommandLine(pGame) — which is where the game applies this
  // identical mask when the token IS present. So our write lands earlier in the
  // same call chain than the game's own known-sufficient write, and precedes
  // every consumer that write precedes. AND-masking is idempotent, so passing
  // the token as well remains a no-op.
  UINT32* engineFlags = reinterpret_cast<UINT32*>(static_cast<CHAR*>(pGame) + GAME_ENGINE_FLAGS_OFFSET);
  const UINT32 flagsBefore = *engineFlags;
  *engineFlags &= ENGINE_FLAGS_NOAUDIO_MASK & ENGINE_FLAGS_HEADLESS_MASK;
  const UINT32 flagsAfter = *engineFlags;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.HEADLESS] engine flags 0x%08X -> 0x%08X (bit0_render=%s)",
      static_cast<unsigned>(flagsBefore), static_cast<unsigned>(flagsAfter),
      (flagsAfter & ENGINE_FLAGS_RENDER_BIT) ? "SET(WINDOWED)" : "CLEAR(HEADLESS)");
  if (flagsAfter & ENGINE_FLAGS_RENDER_BIT) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.HEADLESS] render bit still SET after masking — a window will open. "
        "The engine-flags offset or mask no longer matches this build of echovr.exe.");
  }

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

  // Kill DirectInput8Create call — prevents HID enumeration thread from spinning
  const BYTE dinputNop[] = {0x90, 0x90, 0x90, 0x90, 0x90};
  static_assert(sizeof(dinputNop) == HEADLESS_DINPUT_SIZE, "HEADLESS_DINPUT patch size mismatch");
  ApplyPatch(HEADLESS_DINPUT, dinputNop, sizeof(dinputNop));

  // Skip cgs_dx12 D3D12 device/factory/adapter init (fcn.14058e7f0). Its single
  // call site is gated by the graphics-enable bit (0x100 at CEngine+0x2cfec); in
  // server mode the bit is re-set by the config copy at function entry (measured,
  // N6), so the game runs full D3D12 init and asserts ("Unknown error while
  // loading the game") with no GPU. Force the game's own device-free branch
  // (je -> jmp) so it takes its native graphics-disabled path instead of creating
  // a device. Prologue-VALIDATED (never blind-write): only patch if the current
  // byte is the expected `je` opcode.
  {
    const BYTE* gate = reinterpret_cast<const BYTE*>(EchoVR::g_GameBaseAddress + HEADLESS_DX12_INIT);
    if (*gate == HEADLESS_DX12_INIT_EXPECT) {
      const BYTE jmp = HEADLESS_DX12_INIT_PATCH;
      ProcessMemcpy(EchoVR::g_GameBaseAddress + HEADLESS_DX12_INIT, const_cast<BYTE*>(&jmp), 1);
      Log(EchoVR::LogLevel::Debug,
          "[NEVR.HEADLESS] D3D12 device init skipped — forced device-free branch at +0x%lx",
          static_cast<unsigned long>(HEADLESS_DX12_INIT));
    } else {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.HEADLESS] D3D12-skip prologue mismatch at +0x%lx (got 0x%02x, want 0x74) — NOT patched",
          static_cast<unsigned long>(HEADLESS_DX12_INIT), static_cast<unsigned>(*gate));
    }
  }

  // Install the remaining 4 headless render gates via the production gate table.
  // N65: HEADLESS_GATE_TABLE is the single source of truth — adding a gate means
  // adding an entry to the table; the loop mechanically installs it. Call sites
  // CANNOT drift from the table.
  for (const auto& gate : PatchAddresses::HEADLESS_GATE_TABLE) {
    // Skip the D3D12 init gate — it has its own expect/patch constants and
    // prologue-validated install above.
    if (gate.rva == HEADLESS_DX12_INIT) continue;
    ForceHeadlessSkip(gate.rva, gate.expected_opcode, gate.description);
  }

  // Hook the SYSNET internet-connectivity check to always return TRUE in server
  // mode.  SYSNET (fcn.1401f6fa0) is called from fcn.140157fb0 (the multiplayer
  // init orchestrator) to verify internet connectivity before calling
  // LoadServerSupport / setting login_host / TcpBroadcasterListen.  Under Wine
  // with no real network manager (NLM), CoCreateInstance(INetworkListManager)
  // returns a stub, InternetGetConnectedState reports no internet, and the game
  // transitions to NoNetwork — which interrupts BeginMultiplayer before it
  // reaches LoadServerSupport and the login/transaction host setters.  Hooking
  // SYSNET to always return TRUE lets the orchestrator proceed through the full
  // multiplayer init chain (LoadServerSupport → ServerLib factory → ServerDB
  // registration) without the NoNetwork detour.
  {
    typedef int64_t (*SysNetCheckFn)();
    static const auto kSysNetHook = +[]() -> int64_t {
      Log(EchoVR::LogLevel::Debug,
          "[NEVR.HEADLESS] SYSNET check — returning TRUE (internet-connected) for server mode");
      return 1;
    };
    SysNetCheckFn target =
        reinterpret_cast<SysNetCheckFn>(EchoVR::g_GameBaseAddress + PatchAddresses::SYSNET_CHECK);
    PatchDetour(&target, reinterpret_cast<PVOID>(static_cast<SysNetCheckFn>(kSysNetHook)), "SysNetCheck");
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.HEADLESS] SYSNET check hooked — will always report internet-connected");
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

  // CR15NetGame::LogInSuccess checks a platform capability flag before calling the
  // login state update (fcn.1406157c0). The code loads flags from game+0x2DA0, shifts
  // right 9, then tests bit 0 — effectively testing bit 9 of the original value.
  // pnsovr sets this bit; pnsrad does not. NOP the JE so the state update always runs.
  //   14017f810:  shr rcx, 0x9     ; shift platform flags right 9
  //   14017f814:  test cl, 0x1     ; test bit 0 of shifted value (= original bit 9)
  //   14017f817:  je   +0x1e       ; skip login state update
  // NOTE: Same address as OFFLINE_TRANSACTION_1 — both patches NOP the same JE for
  // different reasons. The NOP is idempotent.
  {
    constexpr uintptr_t LOGIN_CAP_CHECK = PatchAddresses::OFFLINE_TRANSACTION_1;
    auto* site = (const BYTE*)(EchoVR::g_GameBaseAddress + LOGIN_CAP_CHECK);
    if (site[0] == 0x74 && site[1] == 0x1E) {
      const BYTE nop2[] = {0x90, 0x90};
      ApplyPatch(LOGIN_CAP_CHECK, nop2, sizeof(nop2));
    } else if (site[0] != 0x90) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] Unexpected bytes at LogInSuccess cap check +0x%x: %02x %02x",
          (unsigned)LOGIN_CAP_CHECK, site[0], site[1]);
    }
  }

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
    Log(EchoVR::LogLevel::Debug,
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

// N83: this hooks CBroadcaster::Listen (0xF80ED0), NOT an entity lookup — see
// hook/addresses.h. What that means for the code below:
//
//   - The guard's ARITHMETIC IS CORRECT and empirically derived. [inner+0x5e0]
//     really is this function's first dereference, and 0x10 + 0x3ff8 = 0x4008 is
//     exactly the AV address in the original annotation. Do not remove it on the
//     grounds that the name was wrong.
//   - But `return -1` is not "skip a lookup". It is a FAILED LISTENER
//     REGISTRATION, and -1 collides with 0xFFFF, this function's own native
//     failure sentinel. Callers do not check it: RegisterClientCallbacks
//     (0x14060e780) stores the value straight into its listener-id array with no
//     CMP AX,0xFFFF in between, and CR15NetDedicatedLobby::SetupDedicatedMode
//     (0x14019c280) discards EAX after each of its five calls. So a trip here is
//     silent to the game and only visible in the Warning below (first 3 only).
//
// Whether it ever trips on a real server is THE open question for N83, and the
// log line below is the only instrument that answers it. If you are debugging a
// server that registers but receives nothing, grep the log for it first.
static volatile LONG g_listenHookEntries = 0;
static volatile LONG g_dispatchHookEntries = 0;

// N83/N84 exit condition: "if the guard does not trip across representative runs,
// remove the detour." That inference is only valid if the hook RAN. A guard that
// never trips because its function is never called is not evidence of anything.
// These counters make the two states distinguishable.
void LogBroadcasterHookStats() {
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] broadcaster hook stats listen_entries=%ld dispatch_entries=%ld "
      "(N83/N84 evidence — zero entries means idle runs prove nothing)",
      g_listenHookEntries, g_dispatchHookEntries);
}

static INT16 EngineEntityLookupHook(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5) {
  InterlockedIncrement(&g_listenHookEntries);
  HookLiveness::Mark(HookLiveness::kBroadcasterListen);
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

// N83 (2026-07-26): re-implemented as the null-guard it was always described as.
//
// ORIGIN, corrected: 7beccee (2026-03-24, Andrew Bates) added this and stated its
// purpose in the commit message — "Entity property dispatch null-guard
// (fcn.140f87aa0) to prevent AV in server mode from uninitialized client-side
// state". An earlier pass here claimed there was no recorded justification; that
// was a bad search (git log -S scoped to a file created by a later refactor, so it
// could not have found the originating commit). The fence had a builder and a reason.
//
// What was actually wrong was the GAP between that intent and the code. The stated
// intent is a null-guard. The implementation was `if (g_isServer) return;` — an
// unconditional skip with no null check at all. Because 0xF87AA0 is
// CBroadcaster::ReceiveLocalEvent (the listener dispatcher, not entity property
// dispatch), that skip suppressed all message delivery on a server, including our
// own 15 ServerLib injections which reach this VA via
// EchoVR::BroadcasterReceiveLocalEvent (echovr_functions.cpp:87).
//
// The AV is guarded precisely instead. Disassembly gives the exact fault chain:
//   0x140f87b81  MOV R8, qword ptr [RDI]          ; inner = *arg1
//   0x140f87b8d  MOV RAX, qword ptr [R8 + 0x5e0]  ; listener index table
//   0x140f87b94  MOV EDX, dword ptr [RAX+RCX*4]   ; <-- AV when table is garbage
// Same +0x5e0 structure the sibling Listen guard checks, which is consistent: one
// broadcaster, one table.
//
// So: skip only when that chain is actually unsafe; dispatch whenever it is valid.
// The original protection is preserved; the collateral severance is not.
static VOID EngineEntityPropDispatchHook(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5) {
  InterlockedIncrement(&g_dispatchHookEntries);
  HookLiveness::Mark(HookLiveness::kBroadcasterReceiveLocal);
  if (g_isServer) {
    // Exact AV condition from the disassembly above — nothing broader.
    if (arg1 == 0) return;
    const INT64 inner = *reinterpret_cast<INT64*>(arg1);
    if (inner == 0) return;
    const INT64 table = *reinterpret_cast<INT64*>(inner + 0x5e0);
    if (table < 0x10000) {
      static volatile LONG guardCount = 0;
      const LONG c = InterlockedIncrement(&guardCount);
      if (c <= 3) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] broadcaster dispatch guard tripped table=0x%llX count=%ld "
            "va=0x140F87AA0 fn=CBroadcaster::ReceiveLocalEvent (N83)",
            static_cast<unsigned long long>(table), c);
      }
      return;  // the AV 7beccee was written to prevent
    }
  }
  OriginalEngineEntityPropDispatch(arg1, arg2, arg3, arg4, arg5);
}

// Historical record — the body as originally written, and why it was replaced.
[[maybe_unused]] static VOID EngineEntityPropDispatchHook_Original(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5) {
  // !! N83 — THE COMMENT THAT USED TO BE HERE WAS FALSE. Preserved verbatim so
  // !! the next reader can recognise the shape of the mistake:
  // !!
  // !!   "Skip entirely in server mode — this function dispatches entity property
  // !!    updates for client-side state (rendering, effects) that doesn't exist in
  // !!    headless mode. The internal pointer chain is uninitialized, causing
  // !!    cascading AVs."
  //
  // This is not entity property dispatch. 0xF87AA0 is
  // CBroadcaster::ReceiveLocalEvent — the broadcaster's LISTENER DISPATCHER. The
  // early return below therefore does not skip rendering work; it skips message
  // delivery. ReVault's caller list is FinalizeEntrant, InitEntrantSlots,
  // CommitPlaceholder, CreateAndJoinOfflineSession — entrant and session
  // lifecycle, not rendering. The stated justification was falsified by its own
  // callers; nobody re-checked because the constant was named ENGINE_ENTITY_*.
  //
  // Worse, src/abi/echovr_functions.cpp:87 points
  // EchoVR::BroadcasterReceiveLocalEvent at this same RVA, so all 15 injection
  // sites in gameserver/gameserver.cpp re-enter THIS hook and hit THIS return.
  // That is the entire ServerDB→game path: LobbyRegistrationSuccess/Failure,
  // LobbyStartSessionV4, LobbyAcceptPlayersSuccess/FailureV2,
  // LobbySessionSuccessV5, LobbySmiteEntrant.
  //
  // The `return` is LEFT IN PLACE deliberately. Removing it is a behaviour change
  // on a path that was added in response to a real AV, and whether that AV still
  // occurs is not statically determinable. Procedure to settle it:
  // docs/primers/2026-07-26-n83-broadcaster-severance.md. Do not "fix" this by
  // deleting the line because the name was wrong — that is the same error in the
  // opposite direction.
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

static LoadLibraryW_t Original_LoadLibraryW = nullptr;
static LoadLibraryExW_t Original_LoadLibraryExW = nullptr;

static HMODULE WINAPI LoadLibraryW_Hook(LPCWSTR lpLibFileName) {
  if (lpLibFileName != nullptr) {
    std::wstring dllName(lpLibFileName);
    std::transform(dllName.begin(), dllName.end(), dllName.begin(), ::tolower);

    if (dllName.find(L"libovrplatform") != std::wstring::npos || dllName.find(L"ovrplatform") != std::wstring::npos) {
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Blocked Oculus Platform SDK load: %S", lpLibFileName);
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
      Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Blocked Oculus Platform SDK load: %S", lpLibFileName);
      SetLastError(ERROR_MOD_NOT_FOUND);
      return NULL;
    }
  }
  return Original_LoadLibraryExW(lpLibFileName, hFile, dwFlags);
}

VOID PatchBlockOculusSDK() {
  Original_LoadLibraryW = LoadLibraryW;
  Original_LoadLibraryExW = LoadLibraryExW;
  PatchDetour(&Original_LoadLibraryW, reinterpret_cast<PVOID>(LoadLibraryW_Hook), "LoadLibraryW");
  PatchDetour(&Original_LoadLibraryExW, reinterpret_cast<PVOID>(LoadLibraryExW_Hook), "LoadLibraryExW");

  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Installed Oculus Platform SDK blocking hooks");
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Expected savings: 50-80MB RAM, 8-12%% CPU per instance");
}

// ===================================================================================================
// Wwise Audio Optimization Hooks
// ===================================================================================================

typedef int(WINAPI* Wwise_Init_t)(PVOID);
static Wwise_Init_t Original_Wwise_Init = nullptr;

typedef void(WINAPI* Wwise_RenderAudio_t)(PVOID);
static Wwise_RenderAudio_t Original_Wwise_RenderAudio = nullptr;

static int WINAPI Wwise_Init_Hook(PVOID config) {
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Wwise audio initialization blocked (VOIP preserved)");
  return 0;
}

static void WINAPI Wwise_RenderAudio_Hook(PVOID context) {}

VOID PatchDisableWwise() {
  PVOID base = GetModuleHandleA(NULL);

  Original_Wwise_Init = (Wwise_Init_t)((uintptr_t)base + PatchAddresses::WWISE_INIT);
  PatchDetour(&Original_Wwise_Init, (PVOID)Wwise_Init_Hook, "AK::SoundEngine::Init");

  Original_Wwise_RenderAudio = (Wwise_RenderAudio_t)((uintptr_t)base + PatchAddresses::WWISE_RENDERAUDIO);
  PatchDetour(&Original_Wwise_RenderAudio, (PVOID)Wwise_RenderAudio_Hook, "AK::SoundEngine::RenderAudio");

  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Installed Wwise audio blocking hooks");
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Expected savings: 20-30MB RAM, 5-8%% CPU per instance");
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] VOIP components preserved for multiplayer");
}

// ===================================================================================================
// Server Frame Pacing Optimization
// ===================================================================================================

// PatchServerFramePacing was removed 2026-07-29 (N113). It wrote 0xC3 to
// CPrecisionSleep::BusyWait via ApplyPatch with NO prologue validation and no
// original-byte save — both of which the canonical site in
// patch/binary_bug_fixes.cpp does (ResolveVA_Checked, then memcpy the original
// into s_busywait_original_byte for the N33 shutdown restore). It was marked
// DEPRECATED by N25 with the exit condition "remove once all paths route
// through BinaryBugFixes::Init"; that condition was already met, since Init
// patches unconditionally while this copy was server-gated.

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
    Log(EchoVR::LogLevel::Debug, "[NEVR.PROFILE] WorkingSet: %llu MB, PrivateBytes: %llu MB",
        pmc.WorkingSetSize / (1024 * 1024), pmc.PrivateUsage / (1024 * 1024));
  }

  const char* checkDlls[] = {"d3d11", "dxgi", "LibOVRPlatform", "AkSoundEngine", NULL};
  for (int i = 0; checkDlls[i]; i++) {
    HMODULE h = GetModuleHandleA(checkDlls[i]);
    Log(EchoVR::LogLevel::Debug, "[NEVR.PROFILE] Module %s: %s", checkDlls[i], h ? "LOADED" : "not loaded");
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
  PatchDetour(&OriginalEngineEntityLookup, reinterpret_cast<PVOID>(EngineEntityLookupHook), "EngineEntityLookup");
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Engine entity lookup hook installed (null-pointer guard)");

  // N83: re-installed 2026-07-26 with a real null-guard body (see above). The
  // hook now preserves the AV protection 7beccee intended while allowing the
  // broadcaster's listener dispatch through when the structure is valid.
  OriginalEngineEntityPropDispatch =
      (EngineEntityPropDispatchFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::ENGINE_ENTITY_PROP_DISPATCH);
  PatchDetour(&OriginalEngineEntityPropDispatch, reinterpret_cast<PVOID>(EngineEntityPropDispatchHook), "EngineEntityPropDispatch");
  Log(EchoVR::LogLevel::Debug,
      "[NEVR.PATCH] hooked name=CBroadcaster::ReceiveLocalEvent va=0x140F87AA0 mode=null_guard");
}

VOID InstallBugSplatHook() {
  // Hook BugSplat crash handler — prevents fatal exits in server mode.
  // The handler is called from 5 sites for missing actors, dialogue scenes, etc.
  // These are non-fatal in headless dedicated server mode.
  OriginalBugSplatCrashHandler =
      (BugSplatCrashHandlerFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::BUGSPLAT_CRASH_HANDLER);
  PatchDetour(&OriginalBugSplatCrashHandler, reinterpret_cast<PVOID>(BugSplatCrashHandlerHook), "BugSplatCrashHandler");
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] BugSplat crash handler hook installed (server crash suppression)");
}

VOID InstallGameSpaceHook() {
  // Hook InitializeGlobalGameSpace to prevent fatal crash in server mode
  // (no local player actor exists in the global gamespace for dedicated servers)
  OriginalInitializeGlobalGameSpace =
      (InitializeGlobalGameSpaceFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::INIT_GLOBAL_GAMESPACE);
  PatchDetour(&OriginalInitializeGlobalGameSpace, reinterpret_cast<PVOID>(InitializeGlobalGameSpaceHook), "InitializeGlobalGameSpace");
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] InitializeGlobalGameSpace hook installed (server crash fix)");
}

VOID InstallGameMainHook() {
  // Hook game main wrapper — longjmp recovery on crash keeps server alive
  GameMain = (GameMainFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::GAME_MAIN);
  OriginalGameMainWrapper =
      (GameMainWrapperFunc*)(EchoVR::g_GameBaseAddress + PatchAddresses::GAME_MAIN_WRAPPER);
  PatchDetour(&OriginalGameMainWrapper, reinterpret_cast<PVOID>(GameMainWrapperHook), "GameMainWrapper");
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Game main wrapper hook installed (server crash recovery)");
}

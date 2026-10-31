#include "runtime/patch/mode_patches.h"
#include "runtime/hook/dll_load_hook.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
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
#include "runtime/patch/broadcaster_pointer_guard.h"
#include "runtime/patch/checked_patch.h"
#include "runtime/patch/headless_gfx_settings.h"

// ============================================================================
// Headless graphics-gate skip helper
// ============================================================================

/// CEngine enables graphics subsystems one by one, each gated, directly or through
/// a flag derived from it, by a bit in the graphics-enable word at CEngine+0x2cfec.
/// The gates are in FUN_14154a950 (three), FUN_14154b670 and FUN_14154d6c0; the
/// comment on each HEADLESS_* gate constant in hook/addresses.h names its
/// function. Each gate is a conditional jump to the skip taken when the bit is clear (the game's own
/// device-free branch). In server mode the bits are set, so the game runs the GPU init and
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
    DWORD error = ERROR_SUCCESS;
    if (ProcessMemcpy(const_cast<BYTE*>(site), &jmp, 1, &error)) {
      Log(EchoVR::LogLevel::Debug,
          "[NEVR.HEADLESS] %s — forced device-free branch (0x%02x->jmp) at +0x%lx",
          what, static_cast<unsigned>(expectedOpcode), static_cast<unsigned long>(offset));
    } else {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.HEADLESS] patch failed name=%s va=0x%llX error=%lu",
          what, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(site)),
          static_cast<unsigned long>(error));
    }
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
  using namespace nevr_patch_addresses;

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
  // SET on every `-server`-only run. Without this write, `-server` alone gives:
  //   engine flags 0x00000137 -> 0x00000135 (bit0_render=SET(WINDOWED))
  // with one game window present for the entire run.
  //
  // Ordering: this function runs from RunDeferredRuntimeBootstrap's
  // `if (g_isHeadless)` block (boot.cpp), after `-server` or `-headless` has set that global
  // during the preflight pass and AFTER PreprocessCommandLineHook's original
  // EchoVR::PreprocessCommandLine(pGame) call has returned. That original call
  // is where the game applies this identical mask when the token IS present, so
  // on a `-server`-only run our write lands after it, not before it. Whether any
  // consumer inside that call needs the mask earlier is not established.
  // AND-masking is idempotent, so passing the token as well remains a no-op.
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

  // No WriteLog hook here — the built-in filter (runtime/log/builtin_filter.cpp) hooks CLog::PrintfImpl.

  // Skip renderer initialization
  const BYTE rendererPatch[] = {0xA8, 0x00};  // TEST al, 0 (always false)
  static_assert(sizeof(rendererPatch) == HEADLESS_RENDERER_SIZE, "HEADLESS_RENDERER patch size mismatch");
  ApplyPatch(HEADLESS_RENDERER, rendererPatch, sizeof(rendererPatch));

  // Skip effects resource loading
  const BYTE effectsPatch[] = {0xEB, 0x41};  // JMP +0x43
  static_assert(sizeof(effectsPatch) == HEADLESS_EFFECTS_SIZE, "HEADLESS_EFFECTS patch size mismatch");
  ApplyPatch(HEADLESS_EFFECTS, effectsPatch, sizeof(effectsPatch));

  // ApplyRuntimeGfxSettings calls ~66 CGRenderer methods that crash without a renderer. Its prologue is validated and
  // detoured here; both game callers get 0 back (patch/headless_gfx_settings.h).
  nevr_headless_gfx_settings::Install(reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress));

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
      DWORD error = ERROR_SUCCESS;
      if (ProcessMemcpy(const_cast<BYTE*>(gate), &jmp, 1, &error)) {
        Log(EchoVR::LogLevel::Debug,
            "[NEVR.HEADLESS] D3D12 device init skipped — forced device-free branch at +0x%lx",
            static_cast<unsigned long>(HEADLESS_DX12_INIT));
      } else {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.HEADLESS] patch failed name=D3D12-device-init va=0x%llX error=%lu",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(gate)),
            static_cast<unsigned long>(error));
      }
    } else {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.HEADLESS] D3D12-skip prologue mismatch at +0x%lx (got 0x%02x, want 0x%02x) — NOT patched",
          static_cast<unsigned long>(HEADLESS_DX12_INIT), static_cast<unsigned>(*gate),
          static_cast<unsigned>(HEADLESS_DX12_INIT_EXPECT));
    }
  }

  // Install the remaining 4 headless render gates via the production gate table.
  // N65: HEADLESS_GATE_TABLE is the single source of truth — adding a gate means
  // adding an entry to the table; the loop mechanically installs it. Call sites
  // CANNOT drift from the table.
  for (const auto& gate : nevr_patch_addresses::HEADLESS_GATE_TABLE) {
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
      // No per-call log: this hook has no condition (always returns 1), so a
      // per-invocation line would just restate the standing fact the install
      // log below already states once. If SYSNET_CHECK is polled/retried, a
      // per-call line here would also be Category B (identical content on
      // every call, no state change to observe).
      return 1;
    };
    SysNetCheckFn target =
        reinterpret_cast<SysNetCheckFn>(EchoVR::g_GameBaseAddress + nevr_patch_addresses::SYSNET_CHECK);
    PatchDetour(&target, reinterpret_cast<PVOID>(static_cast<SysNetCheckFn>(kSysNetHook)), "SysNetCheck");
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.HEADLESS] SYSNET check hooked — will always report internet-connected");
  }

  // Skip console creation if -no-console was specified
  if (g_noConsole) {
    return;
  }

  // Create a console window for headless mode
  // Note: We create a new console because the parent console is already detached
  // due to /SUBSYSTEM:WINDOWS. Attaching multiple processes would be problematic.
  AllocConsole();

  // Log() writes UTF-8 (its em dashes are E2 80 94). A console's output code
  // page defaults to the OEM page (437), which draws those bytes as "ΓÇö", so
  // set the page to UTF-8 once, here, where the console is made (#1295).
  SetConsoleOutputCP(CP_UTF8);

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
  using namespace nevr_patch_addresses;

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
    constexpr uintptr_t LOGIN_CAP_CHECK = nevr_patch_addresses::OFFLINE_TRANSACTION_1;
    auto* site = (const BYTE*)(EchoVR::g_GameBaseAddress + LOGIN_CAP_CHECK);
    if (site[0] == 0x74 && site[1] == 0x1E) {
      const BYTE nop2[] = {0x90, 0x90};
      ApplyPatch(LOGIN_CAP_CHECK, nop2, sizeof(nop2));
    } else if (site[0] != 0x90) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] unexpected bytes at LogInSuccess cap check +0x%x: got=%02x%02x want=%02x%02x — NOT patched",
          (unsigned)LOGIN_CAP_CHECK, site[0], site[1], 0x74, 0x1E);
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
  using namespace nevr_patch_addresses;

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
// CBroadcaster::Listen null-check hook — prevent AV in server mode
// ============================================================================

/// CBroadcaster::Listen @ 0x140f80ed0 reads the listener table through arg1.
/// A low pointer in either link can fault before the table-value guard runs.
typedef INT16 CBroadcasterListenFunc(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5);
static CBroadcasterListenFunc* OriginalCBroadcasterListen = nullptr;

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
// Whether it ever trips on a real server is THE open question for N83. The instrument that answers
// it is HookLiveness (hook/hook_liveness.h): kBroadcasterListen and kBroadcasterReceiveLocal count
// every entry into the two hooks below, and tick.cpp reports them periodically. "Guard never
// tripped" is evidence only when those counts are non-zero; a guard that never trips because its
// function is never called proves nothing. If you are debugging a server that registers but
// receives nothing, grep the log for `hook_liveness name=CBroadcaster::Listen` first.

static INT16 CBroadcasterListenHook(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5) {
  nevr_hook_liveness::Mark(nevr_hook_liveness::kBroadcasterListen);
  if (g_isServer) {
    INT64 hashTablePtr = 0;
    const bool chainReadable = nevr_broadcaster_guard::TryReadListenerTable(
        static_cast<std::uintptr_t>(arg1), hashTablePtr);
    if (!chainReadable || hashTablePtr < 0x10000 ||
        hashTablePtr > static_cast<INT64>(nevr_broadcaster_guard::kMaximumUserAddress)) {
      static volatile LONG guardCount = 0;
      const LONG c = InterlockedIncrement(&guardCount);
      if (c <= 3) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] broadcaster listen guard rejected reason=%s table=0x%llX count=%ld "
            "va=0x140F80ED0 fn=CBroadcaster::Listen",
            chainReadable ? "table_range" : "pointer_chain",
            static_cast<unsigned long long>(hashTablePtr), c);
      } else if ((c % 100) == 0) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] broadcaster listen guard rejected count=%ld va=0x140F80ED0 "
            "fn=CBroadcaster::Listen",
            c);
      }
      return -1;
    }
  }
  return OriginalCBroadcasterListen(arg1, arg2, arg3, arg4, arg5);
}

// ============================================================================
// CBroadcaster::ReceiveLocalEvent null-check hook — prevent AV in server mode
// ============================================================================

/// CBroadcaster::ReceiveLocalEvent @ 0x140f87aa0 reads its broadcaster through
/// arg1. A low pointer in either link can fault before dispatch reaches the
/// listener table.
typedef VOID CBroadcasterReceiveLocalEventFunc(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5);
static CBroadcasterReceiveLocalEventFunc* OriginalCBroadcasterReceiveLocalEvent = nullptr;

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
// EchoVR::BroadcasterReceiveLocalEvent (echovr_functions.cpp).
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
static VOID CBroadcasterReceiveLocalEventHook(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5) {
  nevr_hook_liveness::Mark(nevr_hook_liveness::kBroadcasterReceiveLocal);
  if (g_isServer) {
    INT64 table = 0;
    const bool chainReadable = nevr_broadcaster_guard::TryReadListenerTable(
        static_cast<std::uintptr_t>(arg1), table);
    if (!chainReadable || table < 0x10000 ||
        table > static_cast<INT64>(nevr_broadcaster_guard::kMaximumUserAddress)) {
      static volatile LONG guardCount = 0;
      const LONG c = InterlockedIncrement(&guardCount);
      if (c <= 3) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] broadcaster dispatch guard rejected reason=%s table=0x%llX count=%ld "
            "va=0x140F87AA0 fn=CBroadcaster::ReceiveLocalEvent",
            chainReadable ? "table_range" : "pointer_chain",
            static_cast<unsigned long long>(table), c);
      } else if ((c % 100) == 0) {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.PATCH] broadcaster dispatch guard rejected count=%ld va=0x140F87AA0 "
            "fn=CBroadcaster::ReceiveLocalEvent",
            c);
      }
      return;  // the AV 7beccee was written to prevent
    }
  }
  OriginalCBroadcasterReceiveLocalEvent(arg1, arg2, arg3, arg4, arg5);
}

// The unconditional server-mode skip of the broadcaster's listener dispatcher, and why its justification is false.
[[maybe_unused]] static VOID CBroadcasterReceiveLocalEventHook_Original(INT64 arg1, INT64 arg2, INT64 arg3, INT64 arg4, INT64 arg5) {
  // !! N83 — the justification below is FALSE. It is quoted so the next reader
  // !! can recognise the shape of the mistake:
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
  // Worse, src/abi/echovr_functions.cpp points
  // EchoVR::BroadcasterReceiveLocalEvent at this same RVA, so every injection
  // site in gameserver/gameserver.cpp re-enters THIS hook and hit THIS return.
  // That is the entire ServerDB→game path: LobbyRegistrationSuccess/Failure,
  // LobbyStartSessionV4, LobbyAcceptPlayersSuccess/FailureV2,
  // LobbySessionSuccessV5, LobbySmiteEntrant.
  //
  // The `return` is LEFT IN PLACE deliberately. Removing it is a behaviour change
  // on a path that was added in response to a real AV, and whether that AV still
  // occurs is not statically determinable. Procedure to settle it:
  // N83 (self-collision — two code paths detour the same address). Do not "fix" this by
  // deleting the line because the name was wrong — that is the same error in the
  // opposite direction.
  if (g_isServer) return;
  OriginalCBroadcasterReceiveLocalEvent(arg1, arg2, arg3, arg4, arg5);
}

// ============================================================================
// Game main wrapper hook — restart game loop on crash in server mode
// ============================================================================

/// Original function: Game main wrapper @ 0x1400cd510
/// Calls the game's main loop (fcn.1400cd550). If the game loop returns (which
/// means a fatal error occurred), the original function calls the crash handler.
/// In server mode, we restart the game loop instead so the server stays alive.
// N125: GameMainWrapperHook, RuntimeGameMain, ArmGameMain, GameMain/OriginalGameMainWrapper and
// the g_gameLoopJmpBuf definition live in lifecycle/crash_recovery.cpp — that
// file owns the longjmp side, so the setjmp side sits beside it rather than
// coupled across an extern.

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
    // This one hook is reached from 5 different game call sites (missing
    // actors, dialogue scenes, etc.) — the caller return address distinguishes
    // which, since exitCode alone does not. Rate-limited the same way as the
    // other repeat-prone guards in this file (the CBroadcaster::Listen and
    // ReceiveLocalEvent guards above): first 3 in full, then a periodic total so a retriggering
    // condition doesn't flood identical WARNINGs forever.
    static volatile LONG count = 0;
    const LONG c = InterlockedIncrement(&count);
    if (c <= 3) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] BugSplat crash handler intercepted — suppressed in server mode: "
          "exit_code=%lld count=%ld caller=%p",
          exitCode, c, __builtin_return_address(0));
    } else if ((c % 100) == 0) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] BugSplat crash handler: %ld total intercepts this session "
          "(still occurring, all suppressed)",
          c);
    }
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
  using namespace nevr_patch_addresses;

  // Patch the server flag checks inside fcn.140157fb0 (SERVER_FLAGS_CHECK; BeginMultiplayer calls it)
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
  // This pass runs after the first original PreprocessCommandLine call has returned, so it is the
  // only write of the site and is expected to report applied. It is never made before that call:
  // there the NOPped check sets the spectator flow, which begins multiplayer a second time
  // (PreprocessCommandLineHook in boot.cpp). Whether this later write has any effect is not
  // established.
  PatchSpectatorStreamCheck("PatchEnableServer");
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
  using namespace nevr_patch_addresses;

  // N17: every site reports its outcome by name, and one aggregate line at Info says how many were written.
  // Before this, a failed write was silent here (only ApplyPatch's offset-only warning), so a boot could not
  // show which offline patches were in effect.
  int attempted = 0;
  int written = 0;
  const auto apply = [&attempted, &written](const char* site, uintptr_t offset, const BYTE* patch, size_t size) {
    ++attempted;
    const bool ok = ApplyPatch(offset, patch, size);
    if (ok) ++written;
    Log(ok ? EchoVR::LogLevel::Debug : EchoVR::LogLevel::Warning, "[NEVR.PATCH] offline site=%s result=%s", site,
        ok ? "applied" : "failed");
  };

  // Patch multiplayer initialization for offline mode
  const BYTE multiplayerPatch[] = {0xE8, 0xCD, 0x02, 0x00, 0x00};  // CALL +0x2CD
  static_assert(sizeof(multiplayerPatch) == OFFLINE_MULTIPLAYER_SIZE, "OFFLINE_MULTIPLAYER patch size mismatch");
  apply("multiplayer", OFFLINE_MULTIPLAYER, multiplayerPatch, sizeof(multiplayerPatch));

  // Patch incident reporting
  const BYTE incidentsPatch[] = {0x75, 0x0A};  // JNZ +0x0A
  static_assert(sizeof(incidentsPatch) == OFFLINE_INCIDENTS_SIZE, "OFFLINE_INCIDENTS patch size mismatch");
  apply("incidents", OFFLINE_INCIDENTS, incidentsPatch, sizeof(incidentsPatch));

  // Patch title/session checks
  const BYTE titlePatch[] = {0x74, 0x12};  // JZ +0x12
  static_assert(sizeof(titlePatch) == OFFLINE_TITLE_SIZE, "OFFLINE_TITLE patch size mismatch");
  apply("title", OFFLINE_TITLE, titlePatch, sizeof(titlePatch));

  // Force transaction service to load (two conditional jumps to NOP)
  // Both patches use the same 2-byte NOP pattern and share the same SIZE constant
  const BYTE nopConditionalJump[] = {0x90, 0x90};  // 2x NOP
  static_assert(sizeof(nopConditionalJump) == OFFLINE_TRANSACTION_SIZE, "OFFLINE_TRANSACTION patch size mismatch");
  apply("transaction_1", OFFLINE_TRANSACTION_1, nopConditionalJump, sizeof(nopConditionalJump));
  apply("transaction_2", OFFLINE_TRANSACTION_2, nopConditionalJump, sizeof(nopConditionalJump));

  // Skip failed logon service code
  const BYTE skipLogon[] = {0xE9, 0x92, 0x00, 0x00, 0x00, 0x00};  // JMP +0x97
  static_assert(sizeof(skipLogon) == OFFLINE_LOGON_SIZE, "OFFLINE_LOGON patch size mismatch");
  apply("logon", OFFLINE_LOGON, skipLogon, sizeof(skipLogon));

  // Redirect tutorial beginning
  const BYTE tutorialRedirect[] = {0xE8, 0xD6, 0x17, 0x68, 0xFF};  // CALL relative
  static_assert(sizeof(tutorialRedirect) == OFFLINE_TUTORIAL_SIZE, "OFFLINE_TUTORIAL patch size mismatch");
  apply("tutorial", OFFLINE_TUTORIAL, tutorialRedirect, sizeof(tutorialRedirect));

  // The aggregate line is always written, so an offline boot that wrote nothing still says so (N122/N124).
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] offline mode: %d of %d patch sites written", written, attempted);
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
  using namespace nevr_patch_addresses;

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
  using namespace nevr_patch_addresses;

  // Disable the deadlock monitor's panic condition check
  // This allows debugging with breakpoints without triggering a timeout
  const BYTE deadlockPatch[] = {0x90, 0x90};  // 2x NOP (replace JLE instruction)
  static_assert(sizeof(deadlockPatch) == DEADLOCK_MONITOR_SIZE, "DEADLOCK_MONITOR patch size mismatch");
  ApplyPatch(DEADLOCK_MONITOR, deadlockPatch, sizeof(deadlockPatch));
}

// =============================================================================
// Oculus Platform SDK Blocking
// =============================================================================

// The Oculus Platform SDK is refused through DllLoadHook, which owns the LoadLibraryA/W/ExA/ExW detours.
// It installs no detour of its own: MinHook allows one per target, so a second installer on
// LoadLibraryW/ExW fails with MH_ERROR_ALREADY_CREATED and the block never takes effect (#361).
VOID PatchBlockOculusSDK() {
  nevr_dll_load_hook::AddLoadFilter("oculus-platform-sdk", nevr_dll_load_hook::IsOculusPlatformPath);
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Oculus Platform SDK blocking registered with the DLL load hook");
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

  Original_Wwise_Init = (Wwise_Init_t)((uintptr_t)base + nevr_patch_addresses::WWISE_INIT);
  PatchDetour(&Original_Wwise_Init, (PVOID)Wwise_Init_Hook, "CWwiseEngine::Init");

  Original_Wwise_RenderAudio = (Wwise_RenderAudio_t)((uintptr_t)base + nevr_patch_addresses::WWISE_RENDERAUDIO);
  PatchDetour(&Original_Wwise_RenderAudio, (PVOID)Wwise_RenderAudio_Hook, "CWwiseEngine::RenderAudio");

  Log(EchoVR::LogLevel::Debug,
      "[NEVR.PATCH] Wwise audio blocking hooks installed — VOIP preserved "
      "(est. savings 20-30MB RAM, 5-8%% CPU/instance)");
}

// Patch the PreprocessCommandLine spectator-stream check so the game acts as if
// -spectatorstream was provided. The site (FUN_140116720 + 0x81D) is inside
// PreprocessCommandLine; its one caller, PatchEnableServer, runs after the first original call
// has returned. Written before that call it puts a server in the spectator flow, which begins
// multiplayer a second time (PreprocessCommandLineHook in boot.cpp). Validated against
// kSpectatorStreamExpected, written only on a match, and recorded as one key=value line on every
// call. Log() routes the record by phase itself (the boot tee, then the game's CLog).
namespace {
// The writer is a plain function pointer, so a failed ProcessMemcpy's Win32 error is kept here. It is reset and read
// within one PatchSpectatorStreamCheck call (logging Rule 10).
nevr_checked_patch::WriteErrors g_spectatorWriteErrors;
}  // namespace

nevr_checked_patch::Result PatchSpectatorStreamCheck(const char* phase) {
  uint8_t* const site = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress) + nevr_patch_addresses::SPECTATORSTREAM_CHECK;
  static_assert(sizeof(nevr_checked_patch::kSpectatorStreamExpected) == nevr_patch_addresses::SPECTATORSTREAM_CHECK_SIZE,
                "SPECTATORSTREAM_CHECK expected-bytes size mismatch");
  static_assert(sizeof(nevr_checked_patch::kSpectatorStreamPatched) == nevr_patch_addresses::SPECTATORSTREAM_CHECK_SIZE,
                "SPECTATORSTREAM_CHECK patch size mismatch");

  uint8_t before[nevr_patch_addresses::SPECTATORSTREAM_CHECK_SIZE];
  std::memcpy(before, site, sizeof(before));
  const nevr_checked_patch::Site spec = nevr_checked_patch::SpectatorStreamSite();
  g_spectatorWriteErrors = {};
  const nevr_checked_patch::Result result = nevr_checked_patch::Apply(
      site, spec, [](void* dest, const void* src, size_t n) {
        DWORD error = 0;
        return g_spectatorWriteErrors.Note(ProcessMemcpy(dest, src, n, &error), error);
      });

  char record[192];
  nevr_checked_patch::FormatSpectatorStreamRecord(record, sizeof(record), phase,
                                                  reinterpret_cast<uintptr_t>(site), before,
                                                  g_spectatorWriteErrors.last, result);
  const bool ok = result == nevr_checked_patch::Result::Applied ||
                  result == nevr_checked_patch::Result::AlreadyApplied;
  Log(ok ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning, "%s", record);
  return result;
}

// ===================================================================================================
// Server Frame Pacing Optimization
// ===================================================================================================

// Server frame pacing is patched only by patch/binary_bug_fixes.cpp, which validates
// the address (ResolveVA_Checked) and saves the original byte
// (s_busywait_original_byte) for the N33 shutdown restore. A second writer of 0xC3 to
// CPrecisionSleep::BusyWait here, without validation or a saved byte, would race
// that restore.

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
    Log(h ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Debug,
        "[NEVR.PROFILE] module %s: %s%s", checkDlls[i],
        h ? "unexpectedly LOADED" : "not loaded",
        h ? " — headless patches may not have taken effect" : "");
  }
}

// ============================================================================
// Install* wrappers — called from Initialize() to set up detour hooks
// ============================================================================

VOID InstallEntityHooks() {
  // Hook CBroadcaster::Listen to prevent a null-pointer AV in server mode.
  // It dereferences the listener table pointer at +0x5e0, which can be
  // uninitialized in dedicated server mode.
  OriginalCBroadcasterListen =
      (CBroadcasterListenFunc*)(EchoVR::g_GameBaseAddress + nevr_patch_addresses::BROADCASTER_LISTEN);
  PatchDetour(&OriginalCBroadcasterListen, reinterpret_cast<PVOID>(CBroadcasterListenHook), "CBroadcaster::Listen");
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] hooked name=CBroadcaster::Listen va=0x140F80ED0 mode=null_guard");

  // N83: re-installed 2026-07-26 with a real null-guard body (see above). The
  // hook now preserves the AV protection 7beccee intended while allowing the
  // broadcaster's listener dispatch through when the structure is valid.
  OriginalCBroadcasterReceiveLocalEvent =
      (CBroadcasterReceiveLocalEventFunc*)(EchoVR::g_GameBaseAddress + nevr_patch_addresses::BROADCASTER_RECEIVE_LOCAL_EVENT);
  PatchDetour(&OriginalCBroadcasterReceiveLocalEvent, reinterpret_cast<PVOID>(CBroadcasterReceiveLocalEventHook), "CBroadcaster::ReceiveLocalEvent");
  Log(EchoVR::LogLevel::Debug,
      "[NEVR.PATCH] hooked name=CBroadcaster::ReceiveLocalEvent va=0x140F87AA0 mode=null_guard");
}

VOID InstallBugSplatHook() {
  // Hook BugSplat crash handler — prevents fatal exits in server mode.
  // The handler is called from 5 sites for missing actors, dialogue scenes, etc.
  // These are non-fatal in headless dedicated server mode.
  OriginalBugSplatCrashHandler =
      (BugSplatCrashHandlerFunc*)(EchoVR::g_GameBaseAddress + nevr_patch_addresses::BUGSPLAT_CRASH_HANDLER);
  PatchDetour(&OriginalBugSplatCrashHandler, reinterpret_cast<PVOID>(BugSplatCrashHandlerHook), "BugSplatCrashHandler");
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] BugSplat crash handler hook installed (server crash suppression)");
}

VOID InstallGameSpaceHook() {
  // Hook InitializeGlobalGameSpace to prevent fatal crash in server mode
  // (no local player actor exists in the global gamespace for dedicated servers)
  OriginalInitializeGlobalGameSpace =
      (InitializeGlobalGameSpaceFunc*)(EchoVR::g_GameBaseAddress + nevr_patch_addresses::INIT_GLOBAL_GAMESPACE);
  PatchDetour(&OriginalInitializeGlobalGameSpace, reinterpret_cast<PVOID>(InitializeGlobalGameSpaceHook), "InitializeGlobalGameSpace");
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] InitializeGlobalGameSpace hook installed (server crash fix)");
}

#pragma once

#include <cstdint>

/// <summary>
/// Memory patch addresses for EchoVR game modifications.
/// These are relative offsets from the game's base address.
/// Source: Reverse-engineered from echovr.exe
/// Validated against: echovr-reconstruction (struct/function names, offsets)
///                    ReVault (Windows PE virtual addresses, decompilation)
/// </summary>
namespace PatchAddresses {

// ============================================================================
// Server Mode Patches (PatchEnableServer)
// ============================================================================

/// Address: FUN_140116720 (PreprocessCommandLine, 808+ lines)
/// Forces bits in game state flags at CR15NetGame + 0x2DA0:
///   bit 1, bit 2 (loadout_save_allowed), bit 3 (loadout_action_enabled),
///   bit 6 (LAN), bit 14 (game_mode_active)
/// The game's CLI registry has NO -server/-dedicated argument — this NOP sled
/// is the correct approach. See: MatchLifecycle.cpp, CR15NetDedicatedLobby.cpp
/// Original: Conditional checks for server mode
/// Patched: OR QWORD ptr[rax], 0x6 followed by NOPs
constexpr uintptr_t SERVER_FLAGS_CHECK = 0x1580C3;
constexpr size_t SERVER_FLAGS_CHECK_SIZE = 40;

/// Address: FUN_1400ff4b0 (Logging initialization)
/// Reference: String "r14netserver" at 0x1416d2bb0
/// Prevents enabling "r14netserver" logging which requires missing files
/// Original: Comparison and move operations
/// Patched: MOV RBX, RAX; NOP
constexpr uintptr_t NETSERVER_LOGGING = 0xFFA58;
constexpr size_t NETSERVER_LOGGING_SIZE = 4;

/// Address: FUN_1400ff4b0 (Logging subject update)
/// Updates logging subject to "r14(server)" instead of "r14netserver"
/// Original: Conditional jump
/// Patched: JMP short (unconditional)
constexpr uintptr_t LOGGING_SUBJECT = 0xFFB0E;
constexpr size_t LOGGING_SUBJECT_SIZE = 2;

/// Address: FUN_140f7f8b0 (CBroadcaster::InitializeFromJson, 1440 bytes)
/// Called from FUN_140145b30 (CR15NetDedicatedLobby constructor)
/// Reference: String "|allow_incoming" at 0x141cd0480
/// Forces "allow_incoming" key in netconfig_dedicatedserver.json to always be true.
/// NOTE: _local/config.json does NOT feed this function (only overrides dedicated_port
/// and port_retries). The game asset already has allow_incoming: true but this patch
/// ensures it regardless of asset state or parse failure.
/// Original: Complex parsing logic
/// Patched: MOV eax, 1
constexpr uintptr_t ALLOW_INCOMING = 0xF7F904;
constexpr size_t ALLOW_INCOMING_SIZE = 5;

/// Address: FUN_140116720 + 0x81D (Within PreprocessCommandLine)
/// Reference: String "-spectatorstream" at 0x1416d27b8
/// -spectatorstream IS a registered CLI argument in BuildCmdLineSyntaxDefinitions.
/// The isspectatorstream game expression variable (int32, symbol 0x993e022a8336e85a)
/// exists on R15NETGAMEEXPRESSION. Alternative: inject -spectatorstream into command line
/// or set the expression variable directly. NOP is simplest for fixed binary.
/// Original: JZ (conditional jump if arg not provided)
/// Patched: 6x NOP (fall through)
constexpr uintptr_t SPECTATORSTREAM_CHECK = 0x116F3D;
constexpr size_t SPECTATORSTREAM_CHECK_SIZE = 6;

// ============================================================================
// Offline Mode Patches (PatchEnableOffline)
// ============================================================================

/// Patches multiplayer initialization for offline mode
constexpr uintptr_t OFFLINE_MULTIPLAYER = 0xFDE0E;
constexpr size_t OFFLINE_MULTIPLAYER_SIZE = 5;

/// Patches incident reporting for offline mode
constexpr uintptr_t OFFLINE_INCIDENTS = 0x17F0B1;
constexpr size_t OFFLINE_INCIDENTS_SIZE = 2;

/// Patches title/session checks for offline mode
constexpr uintptr_t OFFLINE_TITLE = 0x17F77B;
constexpr size_t OFFLINE_TITLE_SIZE = 2;

/// Forces transaction service to load in offline mode (first patch)
constexpr uintptr_t OFFLINE_TRANSACTION_1 = 0x17F817;
constexpr size_t OFFLINE_TRANSACTION_SIZE = 2;

/// Forces transaction service to load in offline mode (second patch)
constexpr uintptr_t OFFLINE_TRANSACTION_2 = 0x17F823;

/// Skips failed logon service code in offline mode
constexpr uintptr_t OFFLINE_LOGON = 0x1AC83E;
constexpr size_t OFFLINE_LOGON_SIZE = 6;

/// Redirects tutorial beginning for offline mode
constexpr uintptr_t OFFLINE_TUTORIAL = 0xA7C685;
constexpr size_t OFFLINE_TUTORIAL_SIZE = 5;

// ============================================================================
// Headless Mode Patches (PatchEnableHeadless)
// ============================================================================

/// ReVault VA 0x1400ff4b0: Game initialization / logging setup (252+ lines)
/// Quest name: CGRenderer::Initialize @ Quest:0x188e278
/// Patch at +0xD1 skips renderer initialization
constexpr uintptr_t HEADLESS_RENDERER = 0xFF581;
constexpr size_t HEADLESS_RENDERER_SIZE = 2;

/// ReVault VA 0x14062c940: CLevel::Load (d:\projects\rad\dev\src\engine\libs\nodes\clevel.cpp, 447 bytes)
/// Patch at +0x151 skips effects resource loading
constexpr uintptr_t HEADLESS_EFFECTS = 0x62CA91;
constexpr size_t HEADLESS_EFFECTS_SIZE = 2;

/// Fixes delta time calculation when using fixed timestep
/// Legitimate bug fix: JLE (signed) → JAE (unsigned comparison)
constexpr uintptr_t HEADLESS_DELTATIME = 0xCF46D;
constexpr size_t HEADLESS_DELTATIME_SIZE = 2;

/// ReVault VA 0x140109209: CALL to FUN_140c31870 (ApplyGraphicsSettings) inside FUN_1401090c0
/// FUN_140c31870 calls ~66 CGRenderer methods (MSAA, TAA, resolution scale, etc.)
/// that assume a live renderer. Crashes on headless with no GPU.
/// Patch: NOP the 5-byte CALL instruction
constexpr uintptr_t HEADLESS_APPLY_GRAPHICS = 0x109209;
constexpr size_t HEADLESS_APPLY_GRAPHICS_SIZE = 5;

/// Address: DirectInput8Create call in CSceneRenderPipeline input init (0x141055DBB)
/// Source: ReVault — fcn.141055d90 calls DirectInput8Create via delay-load thunk.
///   CSceneRenderPipeline::Create → fcn.141056090 → fcn.141055d90 → DirectInput8Create
/// The call spawns a DInput Enumeration Thread that polls HID devices indefinitely.
/// Patch: NOP the 5-byte CALL instruction to prevent DirectInput initialization.
constexpr uintptr_t HEADLESS_DINPUT = 0x1055DBB;
constexpr size_t HEADLESS_DINPUT_SIZE = 5;

/// Ground-truth VA 0x14154af7f (RVA 0x154af7f): the ONLY caller of the cgs_dx12
/// D3D12 device-init fcn.14058e7f0 (verified: single direct E8 call @ 0x14154af86).
/// The call block is gated by graphics-enable bit 0x100 at CEngine+0x2cfec:
///   14154af74: testl $0x100,0x2cfec(%r14)
///   14154af7f: je    0x14154afde        ; flag clear -> game's own device-free init
///   14154af81: call  0x1405977f0        ; flag set  -> graphics init prelude
///   14154af86: call  0x14058e7f0        ; flag set  -> D3D12 device create (asserts, no GPU)
///   14154afde: call  0x140599000        ; device-free init variant (je target)
///   14154afe3: call  0x140565320        ; both paths converge here
/// In server/headless the bit is re-set by the CEngineConfig copy at function
/// entry (so clearing it at entry is ineffective — measured, N6), and the game
/// runs full D3D12 init and aborts ("Unknown error while loading the game").
/// Force je(0x74) -> jmp(0xEB), same rel8 displacement 0x5d, so the game takes
/// its native graphics-disabled path (fcn.140599000) unconditionally. Skip, not
/// stub — reproduces the game's own device-free branch. Prologue-VALIDATED.
constexpr uintptr_t HEADLESS_DX12_INIT = 0x154AF7F;
constexpr unsigned char HEADLESS_DX12_INIT_EXPECT = 0x74;  // je rel8
constexpr unsigned char HEADLESS_DX12_INIT_PATCH = 0xEB;   // jmp rel8 (same displacement)

/// Ground-truth VA 0x14154b0e4 (RVA 0x154b0e4): the renderer-init gate in the same
/// CEngine init (FUN_14154a950), gated by graphics-enable bit 0x1 at CEngine+0x2cfec:
///   14154b0dc: testb $0x1,0x2cfec(%r14)
///   14154b0e4: je    0x14154b117        ; bit clear -> skip renderer init entirely
///   14154b0e6: lea   "Initializing renderer" ; log
///   14154b0fd: call  0x141549ec0        ; bit set  -> renderer init (asserts, no GPU)
///   14154b117: (success continuation)
/// The renderer init (fcn.141549ec0) sets CEngine+0x2cec0; the success path at
/// 0x14154b11c re-checks that field and BACKFILLS it (call 0x14154a1f0) when the
/// init was skipped — i.e. the game natively supports the renderer-disabled path.
/// Force je(0x74)->jmp(0xEB), same rel8 displacement 0x31, so headless takes it.
constexpr uintptr_t HEADLESS_ENGINE_RENDER_INIT = 0x154B0E4;

/// Ground-truth VA 0x14154b38f (RVA 0x154b38f): the GUI-subsystem-init gate in the
/// same CEngine init (FUN_14154a950). GUI Initialize (0x140f92b70, single caller
/// here) creates GPU resources (fcn.1405933c0 -> the crashing fcn.14053f170) and
/// sets the "GUI ready" flag DAT_1420db9b1. The call is natively gated by r15d,
/// which is set to 1 (0x14154b345) ONLY when the renderer-enable bit 0x1 at
/// CEngine+0x2cfec AND renderer-status bit 16 at CEngine+0x2cff0 are both set:
///   14154b38c: test %r15d,%r15d
///   14154b38f: je   0x14154b3d6        ; r15d==0 -> game's own "no renderer -> skip GUI init" branch
///   14154b3d1: call 0x140f92b70        ; r15d!=0 -> GUI init (GPU resource create, asserts no GPU)
/// Skipping GUI init keeps DAT_1420db9b1=0, so ALL font/GUI resource loads
/// (fcn.140f90990, gui.cpp) early-return instead of creating GPU resources against
/// the null device (N7 crash @ 0x1413581E4). A dedicated server has no UI, so this
/// reproduces the game's own headless GUI-absent path. Force je(0x74)->jmp(0xEB),
/// same rel8 displacement 0x45. Prologue-VALIDATED.
constexpr uintptr_t HEADLESS_GUI_INIT = 0x154B38F;

/// Ground-truth VA 0x14154d7e4 (RVA 0x154d7e4): the render-submit-context init gate,
/// the LAST bit-0x1 gate in the same CEngine init (FUN_14154a950), tail block:
///   14154d7dd: testb $0x1,0x2cfec(%rbx)   ; renderer-enable bit 0x1 at CEngine+0x2cfec
///   14154d7e4: je    0x14154d7ef          ; bit clear -> game's own "no renderer -> skip" branch
///   14154d7e6: lea   0x8(%rbx),%rcx
///   14154d7ea: call  0x14102f630          ; bit set  -> render-submit-context init cluster
///   14154d7ef: (ret)
/// FUN_14102f630 initializes render-submit contexts; its chain
/// FUN_14054b5e0 -> FUN_14059dcb0 -> CRenderSubmitContext::RequestBuffer @ 0x1405bc4d0
/// computes align_up(size, granularity) via `div r15` @ 0x1405bc52a with granularity
/// (the buffer descriptor's uint16 field +0x5c) = 0 when no GPU device exists ->
/// INT_DIVIDE_BY_ZERO (N8). We force-skip the renderer INIT (0x14154b0e4) but never
/// clear bit 0x1, so it stays SET and this later gate's je is not taken -> the init
/// runs -> div-by-zero. Verified by the N8 crash stack: frame #7 return = 0x14154d7ef
/// (right after this call), frame #6 inside FUN_14102f630, frame #5 inside
/// FUN_14054b5e0, frame #2 inside FUN_14059dcb0. Force je(0x74)->jmp(0xEB), same rel8
/// displacement 0x09, so headless takes the game's native renderer-disabled branch and
/// the render-submit path never runs. Skip, not stub. Prologue-VALIDATED.
constexpr uintptr_t HEADLESS_RENDER_SUBMIT_INIT = 0x154D7E4;

/// Ground-truth VA 0x14154b683 (RVA 0x154b683): the renderer-enable gate in
/// FUN_14154b670, using the inverted-test idiom (opcode `jne` 0x75, not `je`):
///   14154b676: mov  0x2cfec(%rcx),%eax   ; graphics-enable word [CEngine+0x2cfec]
///   14154b67f: not  %eax
///   14154b681: test $0x1,%al             ; renderer-enable bit 0x1
///   14154b683: jne  0x14154b6dd          ; bit CLEAR -> skip renderer work (native branch)
///   14154b6a9: call 0x14154da10          ; bit SET  -> runs -> fcn.14056d560 null deref
/// fcn.14056d560 is a float getter (`movss 0x1258(%rcx),%xmm0`) that dereferences a
/// null render object (RCX from [CEngine+0x2cee0]) when no device exists ->
/// ACCESS_VIOLATION @ 0x14056d560 (N9). Same root cause as N8: bit 0x1 is left SET by
/// the branch-force strategy, so this renderer-gated function runs. Verified by the N9
/// crash stack: frame #2 return = 0x14154b6ae (right after `call 0x14154da10`), #13 in
/// GAME_MAIN. Force jne(0x75)->jmp(0xEB), same rel8 displacement 0x58, so headless takes
/// the game's native renderer-disabled branch. Skip, not stub. Prologue-VALIDATED.
constexpr uintptr_t HEADLESS_RENDER_SETUP = 0x154B683;

/// Address: CEngineConfig::operator= (0x141547360, FUN_141547360)
/// Called from CRenderPipeline::InitStages — single caller (ReVault-verified)
/// to copy the source engine config struct into CEngine+0x2cf98. Copies ~0x98
/// bytes via MOVUPS/MOV/MOVSD, including the 32-bit graphics-enable word at
/// offset 0x54 into the copy (== CEngine+0x2cfec). Hooked in server mode to
/// clear renderer-enable bit 0x1 AFTER the copy, so ALL downstream bit-0x1
/// tests in the CEngine init take their native je device-free branches
/// uniformly — covering render-worker threads that per-gate branch-force
/// patches cannot reach.
/// Prologue: 48 89 5c 24 08 57 48 83 ec 20 (MinHook-safe: 10 bytes before
/// first branch). RCX at entry = CEngine+0x2cf98 (dest); bit clear is at
/// [RCX+0x54] = [CEngine+0x2cfec].
constexpr uintptr_t CENGINE_CONFIG_COPY = 0x1547360;

/// Address: SYSNET internet-connectivity check (fcn.1401f6fa0, 124 bytes)
/// Called from fcn.140157fb0 (multiplayer init orchestrator) to verify
/// internet connectivity before loading server support and setting service hosts.
/// Uses CoCreateInstance(INetworkListManager) + InternetGetConnectedState.
/// In headless/server mode under Wine, this fails → NoNetwork transition →
/// BeginMultiplayer never completes. Hook to always return TRUE.
/// Return type: int64_t (1 = connected, 0 = not connected)
/// Prologue: 48 89 5c 24 08 57 48 83 ec 20 (MinHook-safe: 10 bytes)
constexpr uintptr_t SYSNET_CHECK = 0x1F6FA0;

// ============================================================================
// Server Global GameSpace Crash Fix
// ============================================================================

/// Address: CR15Game::InitializeGlobalGameSpace (0x140110ab0, 450 bytes)
/// This client-side function searches for a player actor and CDialogueSceneCS
/// in the global (menu) gamespace. In dedicated server mode there is no local
/// player, so the actor lookup fails and the function calls the fatal error
/// handler (ExitProcess(1) + int3). Hook this to return early in server mode,
/// setting only the gamespace pointer (CR15Game+0x7AF0) which downstream code
/// needs.
constexpr uintptr_t INIT_GLOBAL_GAMESPACE = 0x110ab0;

/// Address: Game main wrapper (0x1400cd510, 62 bytes)
/// Called from WinMain. Calls the game's main loop function (fcn.1400cd550) then
/// calls the BugSplat crash handler if it returns. Hook this to restart the game
/// loop on crash instead of exiting.
constexpr uintptr_t GAME_MAIN_WRAPPER = 0x0CD510;

/// Address: Game main function (0x1400cd550, 676 bytes)
/// The actual game execution — creates CR15Game, enters the game loop via vtable
/// calls. Returns when the game encounters a fatal error or shuts down.
constexpr uintptr_t GAME_MAIN = 0x0CD550;

/// Address: Engine entity lookup function (0x140f80ed0, 555 bytes)
/// Called from 109+ sites. Accesses *(int64_t*)(arg1->ptr + 0x5e0) which is a
/// hash table pointer. In server mode this can be 0x10 (uninitialized), causing
/// a null-pointer AV at offset 0x3ff8 → target 0x4008. Hook to add null check.
constexpr uintptr_t ENGINE_ENTITY_LOOKUP = 0xF80ED0;

/// Address: Engine entity property dispatch (0x140f87aa0, 580 bytes)
/// Called from 8 sites. Dereferences *(int64_t*)arg1 + 0x448 for a flags check,
/// then accesses deeper offsets. In server mode the inner pointer can be invalid
/// (0x10 or similar), causing an AV. Hook to add null check.
constexpr uintptr_t ENGINE_ENTITY_PROP_DISPATCH = 0xF87AA0;

/// Address: BugSplat crash handler (0x1400dbbc0, 141 bytes)
/// Fatal error handler called from 5 sites in the game. Builds an error report,
/// calls ExitProcess(1), then executes int3. In server mode we hook this to log
/// the error and return — callers have fallthrough paths so execution continues.
/// Xrefs: 0x1400cd53f, 0x140110b60, 0x1401d3e31, 0x1401d4052, 0x1401d77af
constexpr uintptr_t BUGSPLAT_CRASH_HANDLER = 0x0DBBC0;

// ============================================================================
// Other Patches
// ============================================================================

/// Allows -noovr without requiring -spectatorstream
constexpr uintptr_t NOOVR_SPECTATOR = 0x11690D;
constexpr size_t NOOVR_SPECTATOR_SIZE = 2;

/// ReVault VA 0x1401d3850: Deadlock monitor thread — loops Sleep(1000), logs "Deadlock detected!"
/// Patch at +0x31 disables the panic condition check
constexpr uintptr_t DEADLOCK_MONITOR = 0x1D3881;
constexpr size_t DEADLOCK_MONITOR_SIZE = 2;

// ============================================================================
// Oculus Platform SDK Optimization (PatchBlockOculusSDK)
// ============================================================================

/// Import table entries for DLL loading functions
/// These are used to hook LoadLibraryW/LoadLibraryExW to block Oculus Platform SDK
/// Original analysis: OPTIMIZATION_FINAL_ADDRESS_MAP.md, Category 2
/// Expected savings: 50-80MB RAM, 8-12% CPU per server instance
/// Reference strings:
///   - "Disable OVR platform features" @ 0x1416d3418
///   - "Failed to initialize the Oculus VR Platform SDK" @ 0x14171dcf8
///   - "OVRPlatformInitFail" @ 0x14171dcd7
/// Import table locations:
constexpr uintptr_t IMPORT_LOADLIBRARYW = 0x1FFA742;    // LoadLibraryW import
constexpr uintptr_t IMPORT_LOADLIBRARYEXW = 0x1FFAFFA;  // LoadLibraryExW import

// ============================================================================
// Wwise Audio System Optimization (PatchDisableWwise)
// ============================================================================

/// Wwise audio system addresses for non-VOIP audio disabling
/// Original analysis: OPTIMIZATION_FINAL_ADDRESS_MAP.md, Category 3
/// Expected savings: 20-30MB RAM, 5-8% CPU per server instance
/// CRITICAL: VOIP components (CR15NetVoipBroadcasterCS @ 0x141208348,
///           CR15NetVoipReceiverCS @ 0x1412084a5) must remain functional
/// Reference strings:
///   - "Wwise Initialization Successful" @ various
///   - Wwise SDK version strings
/// Function locations:
constexpr uintptr_t WWISE_INIT = 0x209920;         // Wwise initialization
constexpr uintptr_t WWISE_RENDERAUDIO = 0xFA5610;  // Audio rendering loop

// ============================================================================
// Loading Tips Patches (PatchDisableLoadingTips)
// ============================================================================

/// Address: R15PickLoadingTipNode (0x140bd9670)
/// Entry point for picking a random loading tip during loading screens
/// Patched to immediately return in server mode to avoid unnecessary processing
constexpr uintptr_t LOADING_TIP_PICK = 0xBD9670;
constexpr size_t LOADING_TIP_PICK_SIZE = 1;

/// Address: R15SelectLoadingTipNode (0x140be6d10)
/// Core logic for selecting a loading tip
/// Patched to immediately return in server mode
constexpr uintptr_t LOADING_TIP_SELECT = 0xBE6D10;
constexpr size_t LOADING_TIP_SELECT_SIZE = 1;

/// Address: R15SelectLoadingTipNode_2 (0x140be7c90)
/// Secondary implementation of tip selection logic
/// Patched to immediately return in server mode
constexpr uintptr_t LOADING_TIP_SELECT_2 = 0xBE7C90;
constexpr size_t LOADING_TIP_SELECT_2_SIZE = 1;

// ============================================================================
// Frame Pacing / Timing (PatchServerFramePacing)
// ============================================================================

/// Address: CPrecisionSleep::BusyWait (0x1401ce4c0, 112 bytes)
/// Tight QPC + SwitchToThread loop for sub-ms frame timing precision.
/// On Wine, SwitchToThread doesn't yield efficiently, burning ~35% CPU.
/// Patch the first byte to RET (0xC3) — function returns immediately.
/// The WaitableTimer phase in the caller still handles the bulk of the sleep;
/// we only lose the final ~250μs of busy-wait precision per frame.
constexpr uintptr_t PRECISION_SLEEP_BUSYWAIT = 0x1CE4C0;
constexpr size_t PRECISION_SLEEP_BUSYWAIT_SIZE = 1;

// ============================================================================
// Game Structure Offsets
// ============================================================================

/// Offset within game instance structure for audio flags
constexpr uintptr_t GAME_AUDIO_FLAGS_OFFSET = 468;

/// Offset within game instance structure for fixed timestep flag
constexpr uintptr_t GAME_TIMESTEP_FLAGS_OFFSET = 2088;

/// Offset within game instance structure for windowed mode flag
constexpr uintptr_t GAME_WINDOWED_FLAGS_OFFSET = 31456;

/// Offset within game instance structure for local config JSON
constexpr uintptr_t GAME_LOCAL_CONFIG_OFFSET = 0x63240;

// ============================================================================
// Arena Rules Override (CJson_GetFloat hook)
// ============================================================================

/// Address: CJson_GetFloat (0x1405fca60, 22 bytes)
/// Thunk that calls CJson::Real and casts to float.
/// 45 direct callers, 623 callers via inspector ReadFloat.
/// Hooked to override arena rule config values (celebration time, round time).
constexpr uintptr_t CJSON_GET_FLOAT = 0x5FCA60;

// ============================================================================
// XPID Provider String Patches (PSN- → DSC-)
// ============================================================================

/// Platform prefix string table entries for PSN (provider_id 1 in Nakama enum).
/// Patched to "DSC" / "DSC-" so the game formats/parses Discord-based XPIDs.
/// All three are in .rdata — ProcessMemcpy handles VirtualProtect.
///
/// Source: ReVault search_strings + read_memory on echovr.exe
/// Region 0x1416D0ED0: "OlPrEfIx" struct array — short platform names
/// Region 0x1416D0F5C: Dash-suffixed prefix table (SNSUserID StartsWith targets)
/// Region 0x1416D7130: Compact name table feeding "%s-%llu" format string

/// VA 0x1416D0EE0: "PSN\0" (4 bytes) — short platform name in OlPrEfIx struct
constexpr uintptr_t XPID_PLATFORM_SHORT_NAME = 0x16D0EE0;
constexpr size_t XPID_PLATFORM_SHORT_NAME_SIZE = 4;

/// VA 0x1416D0F64: "PSN-" (4 bytes) — dash-prefixed string for CSysString::StartsWith
constexpr uintptr_t XPID_PLATFORM_DASH_PREFIX = 0x16D0F64;
constexpr size_t XPID_PLATFORM_DASH_PREFIX_SIZE = 4;

/// VA 0x1416D7138: "PSN\0" (4 bytes) — compact name feeding "%s-%llu" sprintf
constexpr uintptr_t XPID_PLATFORM_COMPACT_NAME = 0x16D7138;
constexpr size_t XPID_PLATFORM_COMPACT_NAME_SIZE = 4;

/// VA 0x1416D0F9C: "???-" (4 bytes) — catch-all fallback for unrecognized provider_code.
/// When CNSUser state isn't set yet during early init (login_state=0), the game's
/// platform-name lookup hits this fallback and formats "[NSUSER] Creating user ???-1".
/// Patched to "DSC-" so the early-init fallback matches the EULA-cache directory prefix
/// (N14: EULA acceptance cached per-platform-prefix, so a ???- prefix breaks cache lookup).
constexpr uintptr_t XPID_PLATFORM_FALLBACK_PREFIX = 0x16D0F9C;
constexpr size_t XPID_PLATFORM_FALLBACK_PREFIX_SIZE = 4;

// ============================================================================
// Asset CDN / Loadout Hooks
// ============================================================================

/// Address: Loadout_ResolveDataFromId (RVA 0x004F37A0, 87 bytes)
/// Signature: void* __fastcall (void* context, int64_t loadout_id)
/// Resolves loadout cosmetic data from a 64-bit loadout ID.
/// Two-tier lookup: binary search first, linear scan fallback.
/// Returns NULL on miss. 261+ call sites — hook must be O(1).
/// Return value + 0x370 -> resource table; +0xB8 primary, +0x80 fallback.
/// Prologue: 48 89 5C 24 08 57 48 83 EC 20 (safe for MinHook)
constexpr uintptr_t LOADOUT_RESOLVE_DATA_FROM_ID = 0x004F37A0;

// ============================================================================
// Global Data Addresses
// ============================================================================

/// Pointer to fixed timestep value (in microseconds)
constexpr uintptr_t FIXED_TIMESTEP_PTR = 0x020A00E8;
constexpr uintptr_t FIXED_TIMESTEP_OFFSET = 0x90;

// ============================================================================
// Wave 0 Instrumentation Addresses
// ============================================================================

/// Address: GetTimeMicroseconds (0x1400d00c0, 68 bytes)
/// QPC-to-microseconds conversion. 12 callers (physics, network, rendering).
/// Signature: uint64_t __fastcall() — no parameters, returns microseconds since boot.
constexpr uintptr_t GET_TIME_MICROSECONDS = 0xD00C0;

/// Address: CTimer_GetMilliSeconds (0x1400d0110, 95 bytes)
/// QPC-to-milliseconds conversion. 11 callers including CleanupPeers.
/// Signature: uint64_t __fastcall() — no parameters, returns milliseconds since boot.
constexpr uintptr_t GET_TIME_MILLISECONDS = 0xD0110;

/// Address: EndMultiplayer (0x140162450, 1196 bytes)
/// Session teardown. Dereferences *(*(arg1+0x2DA0)) without null check.
/// Signature: void __fastcall(int64_t game_object, int64_t arg2)
constexpr uintptr_t END_MULTIPLAYER = 0x162450;

// GetTimeMicroseconds global data addresses (offsets from ImageBase)
// Used by the overflow-safe replacement hook to replicate the original's
// paused/fixed time check without calling the buggy original function.

/// Global flag: non-zero means time is paused/fixed, use cached value
constexpr uintptr_t GET_TIME_OVERRIDE_FLAG = 0x2099038;

/// Cached microsecond value returned when override flag is set
constexpr uintptr_t GET_TIME_OVERRIDE_VALUE = 0x209CB00;

// ============================================================================
// Future Wave 1 Hook Targets (validated, not yet hooked)
// ============================================================================

/// Address: CleanupPeers (0x140F76500, 1939 bytes, 1 caller)
/// Network peer timeout check. BUG #2: unsigned subtraction of overflowed
/// timestamp wraps to massive value, disconnecting all peers.
/// Fixed by GetTimeMilliseconds overflow-safe replacement (N22).
constexpr uintptr_t CLEANUP_PEERS = 0xF76500;

/// Address: CSpinWait::WaitForValue (0x141500ED8, 115 bytes, 3 callers)
/// BUG #14: Inverted backoff — starts Sleep(1), degrades to Sleep(0).
/// Maximum CPU consumption at peak contention.
constexpr uintptr_t SPINWAIT_WAIT_FOR_VALUE = 0x1500ED8;

/// Global spin iteration limit read by CSpinWait::WaitForValue
constexpr uintptr_t SPINWAIT_SPIN_LIMIT = 0x2034500;

}  // namespace PatchAddresses

#pragma once

#include <cstdint>

/// <summary>
/// Memory patch addresses for EchoVR game modifications.
/// These are relative offsets from the game's base address.
/// Source: Reverse-engineered from echovr.exe
/// </summary>
namespace PatchAddresses {

// ============================================================================
// Server Mode Patches (PatchEnableServer)
// ============================================================================

/// Address: FUN_140116720 (Command line processing function in cr15game.cpp)
/// Patches server flag checks to permanently enable dedicated server mode
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

/// Address: Network configuration parser
/// Reference: String "|allow_incoming" at 0x141cd0480
/// Forces "allow_incoming" key in netconfig_*.json to always be true
/// This is necessary for game servers to accept player connections
/// Original: Complex parsing logic
/// Patched: MOV eax, 1
constexpr uintptr_t ALLOW_INCOMING = 0xF7F904;
constexpr size_t ALLOW_INCOMING_SIZE = 5;

/// Address: FUN_140116720 + 0x81D (Within command line processing)
/// Reference: String "-spectatorstream" at 0x1416d27b8
/// Bypasses check for "-spectatorstream" argument
/// Causes game to enter "load lobby" state and start server automatically
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

/// Skips renderer initialization for headless mode
constexpr uintptr_t HEADLESS_RENDERER = 0xFF581;
constexpr size_t HEADLESS_RENDERER_SIZE = 2;

/// Skips effects resource loading for headless mode
constexpr uintptr_t HEADLESS_EFFECTS = 0x62CA91;
constexpr size_t HEADLESS_EFFECTS_SIZE = 2;

/// Fixes delta time calculation when using fixed timestep
constexpr uintptr_t HEADLESS_DELTATIME = 0xCF46D;
constexpr size_t HEADLESS_DELTATIME_SIZE = 2;

// ============================================================================
// Other Patches
// ============================================================================

/// Allows -noovr without requiring -spectatorstream
constexpr uintptr_t NOOVR_SPECTATOR = 0x11690D;
constexpr size_t NOOVR_SPECTATOR_SIZE = 2;

/// Disables deadlock monitor for debugging
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
constexpr uintptr_t IMPORT_LOADLIBRARYW = 0x1FFA742;   // LoadLibraryW import
constexpr uintptr_t IMPORT_LOADLIBRARYEXW = 0x1FFAFFA; // LoadLibraryExW import

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
// Global Data Addresses
// ============================================================================

/// Pointer to fixed timestep value (in microseconds)
constexpr uintptr_t FIXED_TIMESTEP_PTR = 0x020A00E8;
constexpr uintptr_t FIXED_TIMESTEP_OFFSET = 0x90;

}  // namespace PatchAddresses

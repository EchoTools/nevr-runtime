#!/bin/bash
# Script to create game start hooks files in DbgHooks directory

cd ~/src/nevr-server/DbgHooks || exit 1

echo "Creating game start hooks files..."

# Create gamestart_hooks.h
cat > gamestart_hooks.h << 'HEADER_EOF'
#pragma once

#include "pch.h"

/// <summary>
/// Initializes game start debugging hooks to investigate the bit 43 trigger.
/// 
/// Background:
/// - Clients stuck on black loading screen waiting for state 8 → 9 transition
/// - Binary analysis shows transition requires bit 43 set at NetGame+0x2da0
/// - Custom servers don't set this bit, causing soft-lock
/// 
/// Hooks installed:
/// 1. State transition function @ 0x1401bc800 - logs state changes and bit 43 status
/// 
/// Output: gamestart_debug.log in game directory
/// </summary>
VOID InitializeGameStartHooks();

/// <summary>
/// Shuts down game start hooks and flushes log file.
/// </summary>
VOID ShutdownGameStartHooks();
HEADER_EOF

echo "✓ Created gamestart_hooks.h"

# Create gamestart_hooks.cpp (this is a long file, using heredoc)
cat > gamestart_hooks.cpp << 'CPP_EOF'
#include "gamestart_hooks.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <chrono>

#include "common/hooking.h"
#include "common/logging.h"
#include "echovrInternal.h"

// =============================================================================
// Game Start State Transition Debugging
// =============================================================================
// This hooks the game state update function to investigate why clients get
// stuck on black loading screen (state 8 → 9 transition blocked).
//
// Binary analysis shows the transition requires bit 43 set at NetGame+0x2da0.
// Custom servers don't set this bit, causing infinite ping/pong loop.
//
// Hook target: 0x1401bc800 (state transition check function)
// =============================================================================

// Log file for game start debugging
static FILE* g_gamestartLog = nullptr;
static std::mutex g_logMutex;
static BOOL g_hooksInitialized = FALSE;

// =============================================================================
// Function Type Definitions
// =============================================================================

// UpdateGameState or state transition check function
// Based on binary analysis @ 0x1401bc800:
//   if (state == 8 && (flags >> 0x2b & 1) != 0) newState = 9
//
// Signature is estimated - may need adjustment based on actual decompilation
typedef void (*pStateTransitionFunc)(void* netGameContext);
pStateTransitionFunc orig_StateTransitionFunc = nullptr;

// =============================================================================
// Hook Addresses (from Ghidra analysis)
// =============================================================================
// NOTE: These addresses are for Echo VR version 34.4.631547.1
// From evr-reconstruction/docs/kb/agent_state.yaml line 228

// State transition check function (checks bit 43 for state 8→9)
#define ADDR_StateTransitionFunc 0x001bc800  // RVA: 0x1401bc800 - 0x140000000

// NetGame context offsets
#define OFFSET_NETGAME_FLAGS 0x2da0  // Confirmed from binary analysis

// =============================================================================
// Hook: State Transition Function
// =============================================================================

void hook_StateTransitionFunc(void* netGameContext) {
    // Log state and bit 43 status BEFORE calling original
    if (g_gamestartLog && netGameContext) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        
        // Read flags from context + 0x2da0
        uint64_t* flagsPtr = reinterpret_cast<uint64_t*>(
            reinterpret_cast<char*>(netGameContext) + OFFSET_NETGAME_FLAGS
        );
        uint64_t flags = *flagsPtr;
        
        // Check bit 43 (0x2b in hex)
        bool bit43 = (flags >> 43) & 1;
        
        // Get timestamp
        auto now = std::chrono::system_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();
        
        // Log every transition
        fprintf(g_gamestartLog, 
                "[%lld] STATE_CHECK context=%p flags=0x%016llx bit43=%d\n",
                ms, netGameContext, flags, bit43 ? 1 : 0);
        fflush(g_gamestartLog);
        
        // If bit 43 just got set, log prominently
        static bool lastBit43 = false;
        if (bit43 && !lastBit43) {
            fprintf(g_gamestartLog, 
                    "[%lld] *** BIT 43 SET! Transition should happen now ***\n", ms);
            fflush(g_gamestartLog);
        }
        lastBit43 = bit43;
    }
    
    // Call original
    orig_StateTransitionFunc(netGameContext);
}

// =============================================================================
// Initialization and Shutdown
// =============================================================================

VOID InitializeGameStartHooks() {
    using namespace EchoVR;
    
    // Check if hooking library is initialized
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) {
        Log(LogLevel::Error, "[GameStartHooks] Failed to initialize MinHook");
        return;
    }
    
    // Open log file
    const char* logPath = "gamestart_debug.log";
    g_gamestartLog = fopen(logPath, "w");
    if (!g_gamestartLog) {
        Log(LogLevel::Error, "[GameStartHooks] Failed to open log file: %s", logPath);
        return;
    }
    
    // Write log header
    fprintf(g_gamestartLog, "# Game Start State Transition Debug Log\n");
    fprintf(g_gamestartLog, "# Investigating bit 43 trigger for state 8->9 transition\n");
    fprintf(g_gamestartLog, "#\n");
    fprintf(g_gamestartLog, "# Problem: Clients stuck on black screen after level load\n");
    fprintf(g_gamestartLog, "# Cause: Bit 43 at NetGame+0x2da0 not set by custom server\n");
    fprintf(g_gamestartLog, "#\n");
    fprintf(g_gamestartLog, "# Binary location: 0x1401bc800 (state transition check)\n");
    fprintf(g_gamestartLog, "# Logic: if (state == 8 && (flags >> 0x2b & 1) != 0) -> state = 9\n");
    fprintf(g_gamestartLog, "#\n\n");
    fflush(g_gamestartLog);
    
    Log(LogLevel::Info, "[GameStartHooks] Log file opened: %s", logPath);
    Log(LogLevel::Info, "[GameStartHooks] Game base address: %p", g_GameBaseAddress);
    
    // Check if addresses are configured
    if (ADDR_StateTransitionFunc == 0x0) {
        Log(LogLevel::Warning, "[GameStartHooks] Hook address not configured!");
        Log(LogLevel::Warning, "[GameStartHooks] Update ADDR_StateTransitionFunc from Ghidra");
        fprintf(g_gamestartLog, "# WARNING: Hook address not configured (0x0)\n\n");
        fflush(g_gamestartLog);
        return;
    }
    
    // Install state transition hook
    if (g_GameBaseAddress) {
        void* targetAddr = g_GameBaseAddress + ADDR_StateTransitionFunc;
        
        Log(LogLevel::Info, "[GameStartHooks] Targeting StateTransitionFunc @ %p (RVA 0x%llX)",
            targetAddr, ADDR_StateTransitionFunc);
        
        MH_STATUS status = MH_CreateHook(
            targetAddr,
            &hook_StateTransitionFunc,
            reinterpret_cast<LPVOID*>(&orig_StateTransitionFunc)
        );
        
        if (status == MH_OK) {
            status = MH_EnableHook(targetAddr);
            if (status == MH_OK) {
                Log(LogLevel::Info, "[GameStartHooks] ✓ Installed hook: StateTransitionFunc");
                fprintf(g_gamestartLog, "[INIT] Hook installed @ %p\n\n", targetAddr);
                fflush(g_gamestartLog);
                g_hooksInitialized = TRUE;
            } else {
                Log(LogLevel::Error, "[GameStartHooks] ✗ Failed to enable hook: %d", status);
            }
        } else {
            Log(LogLevel::Error, "[GameStartHooks] ✗ Failed to create hook: %d", status);
        }
    } else {
        Log(LogLevel::Warning, "[GameStartHooks] Game base address not available");
    }
    
    if (g_hooksInitialized) {
        Log(LogLevel::Info, "[GameStartHooks] Initialization complete");
        Log(LogLevel::Info, "[GameStartHooks] Monitoring state transitions and bit 43 status");
    }
}

VOID ShutdownGameStartHooks() {
    if (g_gamestartLog) {
        fprintf(g_gamestartLog, "\n# Hook shutdown\n");
        fclose(g_gamestartLog);
        g_gamestartLog = nullptr;
        
        using namespace EchoVR;
        Log(LogLevel::Info, "[GameStartHooks] Shutdown complete");
    }
}
CPP_EOF

echo "✓ Created gamestart_hooks.cpp"

# Update CMakeLists.txt
echo "Updating CMakeLists.txt..."
if ! grep -q "gamestart_hooks.cpp" CMakeLists.txt; then
    sed -i 's/  "gun2cr_hook.cpp"/  "gun2cr_hook.cpp"\n  "gamestart_hooks.cpp"/' CMakeLists.txt
    echo "✓ Added gamestart_hooks.cpp to DBGHOOKS_SOURCES"
else
    echo "⚠ gamestart_hooks.cpp already in CMakeLists.txt"
fi

if ! grep -q "gamestart_hooks.h" CMakeLists.txt; then
    sed -i 's/  "gun2cr_hook.h"/  "gun2cr_hook.h"\n  "gamestart_hooks.h"/' CMakeLists.txt
    echo "✓ Added gamestart_hooks.h to DBGHOOKS_HEADERS"
else
    echo "⚠ gamestart_hooks.h already in CMakeLists.txt"
fi

# Update dllmain.cpp
echo "Updating dllmain.cpp..."
if ! grep -q "gamestart_hooks.h" dllmain.cpp; then
    sed -i '/#include "hash_hooks.h"/a #include "gamestart_hooks.h"' dllmain.cpp
    echo "✓ Added #include to dllmain.cpp"
else
    echo "⚠ gamestart_hooks.h already included in dllmain.cpp"
fi

if ! grep -q "InitializeGameStartHooks" dllmain.cpp; then
    sed -i '/InitializeGun2CRHook();/a \      InitializeGameStartHooks();' dllmain.cpp
    echo "✓ Added InitializeGameStartHooks() call"
else
    echo "⚠ InitializeGameStartHooks() already called in dllmain.cpp"
fi

if ! grep -q "ShutdownGameStartHooks" dllmain.cpp; then
    sed -i '/ShutdownGun2CRHook();/i \      ShutdownGameStartHooks();' dllmain.cpp
    echo "✓ Added ShutdownGameStartHooks() call"
else
    echo "⚠ ShutdownGameStartHooks() already called in dllmain.cpp"
fi

echo ""
echo "====================================="
echo "✓ Game start hooks files created!"
echo "====================================="
echo ""
echo "Next steps:"
echo "1. cd ~/src/nevr-server"
echo "2. cmake --preset linux-wine-release"
echo "3. cmake --build --preset linux-wine-release --target DbgHooks"
echo "4. Copy build/mingw-release/bin/DbgHooks.dll to Echo VR directory"
echo "5. Launch game and check gamestart_debug.log"
echo ""

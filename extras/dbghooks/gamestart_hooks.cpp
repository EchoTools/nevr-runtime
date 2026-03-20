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
    
    // ALWAYS create diagnostic log first, before any early returns
    FILE* diagLog = fopen("gamestart_init_diag.log", "w");
    if (diagLog) {
        fprintf(diagLog, "=== GameStart Hooks Initialization Diagnostics ===\n\n");
        fflush(diagLog);
    }
    
    // Try to log to console (might not work if no console attached)
    Log(LogLevel::Info, "[GameStartHooks] === INITIALIZATION STARTING ===");
    if (diagLog) {
        fprintf(diagLog, "Step 1: Called Log() for initialization start\n");
        fflush(diagLog);
    }
    
    // Check if hooking library is initialized
    MH_STATUS mhStatus = MH_Initialize();
    if (diagLog) {
        fprintf(diagLog, "Step 2: MH_Initialize() returned %d (OK=0, ALREADY_INIT=5)\n", mhStatus);
        fflush(diagLog);
    }
    
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        Log(LogLevel::Error, "[GameStartHooks] Failed to initialize MinHook: %d", mhStatus);
        if (diagLog) {
            fprintf(diagLog, "FAILED: MinHook initialization failed with status %d\n", mhStatus);
            fclose(diagLog);
        }
        return;
    }
    
    Log(LogLevel::Info, "[GameStartHooks] MinHook initialized (status: %d)", mhStatus);
    
    // Open main log file
    const char* logPath = "gamestart_debug.log";
    g_gamestartLog = fopen(logPath, "w");
    if (diagLog) {
        fprintf(diagLog, "Step 3: fopen('%s') returned %p\n", logPath, g_gamestartLog);
        fflush(diagLog);
    }
    
    if (!g_gamestartLog) {
        Log(LogLevel::Error, "[GameStartHooks] Failed to open log file: %s", logPath);
        if (diagLog) {
            fprintf(diagLog, "FAILED: Could not open main log file\n");
            fclose(diagLog);
        }
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
    
    // CRITICAL: Get the actual game base address
    // g_GameBaseAddress might be the DLL's base, not the EXE's
    HMODULE gameModule = GetModuleHandleA("echovr.exe");
    if (!gameModule) {
        // Fallback to GetModuleHandle(NULL) which should be the EXE
        gameModule = GetModuleHandleA(NULL);
    }
    
    CHAR* gameBase = reinterpret_cast<CHAR*>(gameModule);
    
    if (diagLog) {
        fprintf(diagLog, "Step 4: Game base address determination:\n");
        fprintf(diagLog, "  - g_GameBaseAddress (from echovrInternal.cpp) = %p\n", g_GameBaseAddress);
        fprintf(diagLog, "  - GetModuleHandle(\"echovr.exe\") = %p\n", gameModule);
        fprintf(diagLog, "  - GetModuleHandle(NULL) = %p\n", GetModuleHandleA(NULL));
        fprintf(diagLog, "  - Using gameBase = %p\n", gameBase);
        fflush(diagLog);
    }
    
    Log(LogLevel::Info, "[GameStartHooks] g_GameBaseAddress = %p", g_GameBaseAddress);
    Log(LogLevel::Info, "[GameStartHooks] GetModuleHandle(NULL) = %p", GetModuleHandleA(NULL));
    Log(LogLevel::Info, "[GameStartHooks] GetModuleHandle(\"echovr.exe\") = %p", gameModule);
    Log(LogLevel::Info, "[GameStartHooks] Using gameBase = %p", gameBase);
    
    // Check if addresses are configured
    if (ADDR_StateTransitionFunc == 0x0) {
        Log(LogLevel::Warning, "[GameStartHooks] Hook address not configured!");
        if (diagLog) {
            fprintf(diagLog, "FAILED: ADDR_StateTransitionFunc is 0x0\n");
            fclose(diagLog);
        }
        fprintf(g_gamestartLog, "# WARNING: Hook address not configured (0x0)\n\n");
        fflush(g_gamestartLog);
        return;
    }
    
    Log(LogLevel::Info, "[GameStartHooks] ADDR_StateTransitionFunc = 0x%llX", (unsigned long long)ADDR_StateTransitionFunc);
    
    // Install state transition hook
    if (gameBase) {
        void* targetAddr = gameBase + ADDR_StateTransitionFunc;
        
        if (diagLog) {
            fprintf(diagLog, "Step 5: Installing hook:\n");
            fprintf(diagLog, "  - Target address = %p (base %p + RVA 0x%llX)\n", 
                    targetAddr, gameBase, (unsigned long long)ADDR_StateTransitionFunc);
            fflush(diagLog);
        }
        
        Log(LogLevel::Info, "[GameStartHooks] Targeting StateTransitionFunc @ %p (RVA 0x%llX)",
            targetAddr, (unsigned long long)ADDR_StateTransitionFunc);
        
        MH_STATUS status = MH_CreateHook(
            targetAddr,
            reinterpret_cast<LPVOID>(hook_StateTransitionFunc),
            reinterpret_cast<LPVOID*>(&orig_StateTransitionFunc)
        );
        
        if (diagLog) {
            fprintf(diagLog, "  - MH_CreateHook returned: %d\n", status);
            fflush(diagLog);
        }
        
        Log(LogLevel::Info, "[GameStartHooks] MH_CreateHook returned: %d", status);
        
        if (status == MH_OK) {
            status = MH_EnableHook(targetAddr);
            
            if (diagLog) {
                fprintf(diagLog, "  - MH_EnableHook returned: %d\n", status);
                fflush(diagLog);
            }
            
            Log(LogLevel::Info, "[GameStartHooks] MH_EnableHook returned: %d", status);
            
            if (status == MH_OK) {
                Log(LogLevel::Info, "[GameStartHooks] ✓ Installed hook: StateTransitionFunc");
                fprintf(g_gamestartLog, "[INIT] Hook installed @ %p\n\n", targetAddr);
                fflush(g_gamestartLog);
                g_hooksInitialized = TRUE;
                
                if (diagLog) {
                    fprintf(diagLog, "SUCCESS: Hook installed and enabled\n");
                    fclose(diagLog);
                }
            } else {
                Log(LogLevel::Error, "[GameStartHooks] ✗ Failed to enable hook: %d", status);
                if (diagLog) {
                    fprintf(diagLog, "FAILED: MH_EnableHook failed with status %d\n", status);
                    fclose(diagLog);
                }
            }
        } else {
            Log(LogLevel::Error, "[GameStartHooks] ✗ Failed to create hook: %d", status);
            if (diagLog) {
                fprintf(diagLog, "FAILED: MH_CreateHook failed with status %d\n", status);
                fclose(diagLog);
            }
        }
    } else {
        Log(LogLevel::Warning, "[GameStartHooks] Game base address is NULL!");
        if (diagLog) {
            fprintf(diagLog, "FAILED: gameBase is NULL\n");
            fclose(diagLog);
        }
    }
    
    if (g_hooksInitialized) {
        Log(LogLevel::Info, "[GameStartHooks] === INITIALIZATION COMPLETE ===");
    } else {
        Log(LogLevel::Warning, "[GameStartHooks] === INITIALIZATION FAILED ===");
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

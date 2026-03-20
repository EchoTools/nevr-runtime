#include "booster_test_hook.h"

#include <cstdio>
#include <windows.h>

#include "common/hooking.h"
#include "common/logging.h"
#include "echovrInternal.h"

// =============================================================================
// Booster Test Hook - Implementation
// =============================================================================
// This hook provides diagnostic logging for booster (X key) activation
// to verify our hook mechanism works correctly.
//
// KEY INSIGHT:
//   - We don't need to find the exact "ActivateBooster" function
//   - InputState is called EVERY frame to check key states
//   - When X key is queried and returns > 0.0, booster is activating
//   - Log at that moment → proves hook works
//
// Phase 1 Goal: See "[BOOSTER_TEST] X key active" in log when pressing X
// Phase 2 Goal: Correlate log timestamp with HTTP API player velocity change
// Phase 3 Goal: Apply same pattern to weapon fire
// =============================================================================

// Original function pointer
static pInputStateFuncBooster orig_InputState_Booster = nullptr;

// State tracking
static BOOL g_boosterHookInitialized = FALSE;
static FILE* g_boosterTestLog = nullptr;
static UINT64 g_boosterActivationCount = 0;
static UINT64 g_lastBoosterLogTime = 0;

// Throttling: Log max once per 100ms to avoid spam
#define BOOSTER_LOG_THROTTLE_MS 100

// =============================================================================
// Hook: CKeyboardInputDevice::InputState (Booster Test)
// =============================================================================
// Intercepts all key state queries, specifically watching for X key (0x58)
//
// When X key returns > 0.0:
//   - Booster is being activated
//   - Log timestamp + diagnostic info
//   - NO game modification (pure observation)
// =============================================================================

float __fastcall hook_InputState_Booster(void* device, uint64_t keyCode) {
  // Call original function first
  float result = orig_InputState_Booster(device, keyCode);

  // Watch for X key (booster) activation
  if (keyCode == VK_X && result > 0.0f) {
    UINT64 currentTime = GetTickCount64();

    // Throttle logging to avoid spam (log max once per 100ms)
    if (currentTime - g_lastBoosterLogTime >= BOOSTER_LOG_THROTTLE_MS) {
      g_lastBoosterLogTime = currentTime;
      g_boosterActivationCount++;

      if (g_boosterTestLog) {
        fprintf(g_boosterTestLog,
                "[BOOSTER_TEST] X key active (state=%.2f) | "
                "Activation #%llu | Timestamp: %llu ms\n",
                result, g_boosterActivationCount, currentTime);
        fflush(g_boosterTestLog);
      }

      // Also log to game console
      Log(EchoVR::LogLevel::Info, "[BoosterTest] X key pressed (activation #%llu)", g_boosterActivationCount);
    }
  }

  return result;
}

// =============================================================================
// Initialization
// =============================================================================

VOID InitializeBoosterTestHook() {
  if (g_boosterHookInitialized) {
    Log(EchoVR::LogLevel::Warning, "[BoosterTest] Already initialized");
    return;
  }

  Log(EchoVR::LogLevel::Info, "[BoosterTest] Initializing booster test hook...");

  // Open log file
  g_boosterTestLog = fopen("booster_test.log", "w");
  if (!g_boosterTestLog) {
    Log(EchoVR::LogLevel::Error, "[BoosterTest] Failed to open booster_test.log");
    return;
  }

  fprintf(g_boosterTestLog, "# Booster Test Log\n");
  fprintf(g_boosterTestLog, "# Testing hook mechanism by monitoring X key (booster) activation\n");
  fprintf(g_boosterTestLog, "# Format: [BOOSTER_TEST] X key active (state=<value>) | Activation #<N> | Timestamp: <ms>\n");
  fprintf(g_boosterTestLog, "#\n");
  fprintf(g_boosterTestLog, "# Expected behavior:\n");
  fprintf(g_boosterTestLog, "#   - Press X in-game → See log entry\n");
  fprintf(g_boosterTestLog, "#   - HTTP API shows velocity increase within 50ms\n");
  fprintf(g_boosterTestLog, "#   - If this works → Hook mechanism is good, apply to weapon fire\n");
  fprintf(g_boosterTestLog, "#\n");
  fprintf(g_boosterTestLog, "# Log throttled to max 1 entry per 100ms to avoid spam\n");
  fprintf(g_boosterTestLog, "========================================\n\n");
  fflush(g_boosterTestLog);

  // Get base address of echovr.exe
  HMODULE hModule = GetModuleHandleA(NULL);
  if (!hModule) {
    Log(EchoVR::LogLevel::Error, "[BoosterTest] Failed to get module handle");
    fclose(g_boosterTestLog);
    g_boosterTestLog = nullptr;
    return;
  }

  // Calculate absolute address of InputState
  void* targetAddr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hModule) + ADDR_InputState_Booster);

  Log(EchoVR::LogLevel::Info, "[BoosterTest] Hooking InputState @ %p (RVA 0x%08X)", targetAddr,
      ADDR_InputState_Booster);

  if (g_boosterTestLog) {
    fprintf(g_boosterTestLog, "Target: CKeyboardInputDevice::InputState @ RVA 0x%08X\n", ADDR_InputState_Booster);
    fprintf(g_boosterTestLog, "Absolute Address: %p\n", targetAddr);
    fprintf(g_boosterTestLog, "Watching for: VK_X (0x%02X)\n\n", VK_X);
    fflush(g_boosterTestLog);
  }

  // Install hook using MinHook
  MH_STATUS status =
      MH_CreateHook(targetAddr, reinterpret_cast<void*>(&hook_InputState_Booster),
                    reinterpret_cast<void**>(&orig_InputState_Booster));

  if (status != MH_OK) {
    Log(EchoVR::LogLevel::Error, "[BoosterTest] Failed to create hook: %s", MH_StatusToString(status));
    fclose(g_boosterTestLog);
    g_boosterTestLog = nullptr;
    return;
  }

  status = MH_EnableHook(targetAddr);
  if (status != MH_OK) {
    Log(EchoVR::LogLevel::Error, "[BoosterTest] Failed to enable hook: %s", MH_StatusToString(status));
    MH_RemoveHook(targetAddr);
    fclose(g_boosterTestLog);
    g_boosterTestLog = nullptr;
    return;
  }

  g_boosterHookInitialized = TRUE;
  Log(EchoVR::LogLevel::Info, "[BoosterTest] ✓ Hook installed successfully");
  Log(EchoVR::LogLevel::Info, "[BoosterTest] Press X in-game to test booster detection");

  if (g_boosterTestLog) {
    fprintf(g_boosterTestLog, "✓ Hook installed successfully\n");
    fprintf(g_boosterTestLog, "Monitoring started at: %llu ms\n\n", GetTickCount64());
    fflush(g_boosterTestLog);
  }
}

// =============================================================================
// Shutdown
// =============================================================================

VOID ShutdownBoosterTestHook() {
  if (!g_boosterHookInitialized) {
    return;
  }

  Log(EchoVR::LogLevel::Info, "[BoosterTest] Shutting down...");

  // Get base address for hook removal
  HMODULE hModule = GetModuleHandleA(NULL);
  if (hModule) {
    void* targetAddr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hModule) + ADDR_InputState_Booster);
    MH_DisableHook(targetAddr);
    MH_RemoveHook(targetAddr);
  }

  // Close log file
  if (g_boosterTestLog) {
    fprintf(g_boosterTestLog, "\n========================================\n");
    fprintf(g_boosterTestLog, "Shutdown at: %llu ms\n", GetTickCount64());
    fprintf(g_boosterTestLog, "Total booster activations logged: %llu\n", g_boosterActivationCount);
    fclose(g_boosterTestLog);
    g_boosterTestLog = nullptr;
  }

  g_boosterHookInitialized = FALSE;
  Log(EchoVR::LogLevel::Info, "[BoosterTest] Shutdown complete");
}

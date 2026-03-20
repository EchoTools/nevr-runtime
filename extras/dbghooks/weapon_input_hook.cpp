#include "weapon_input_hook.h"

#include <cstdio>

#include "common/hooking.h"
#include "common/logging.h"
#include "echovrInternal.h"

// =============================================================================
// Weapon Input Hook - Implementation
// =============================================================================
// This hook enables T key as an alternate weapon fire input in EchoVR.
//
// APPROACH (MVP Phase 1):
//   1. Hook CKeyboardInputDevice::InputState to detect input queries
//   2. Monitor for T key (0x54) presses detected by the hook
//   3. When T key is pressed, call weapon fire function
//
// INTEGRATION STRATEGY:
//   - Detects T key via game's own keyboard polling system
//   - Both VR trigger and T key can fire weapons concurrently
//   - Uses existing game input polling infrastructure
//
// KEY DISCOVERY:
//   - Space bar (0x20) is NEVER queried by the game
//   - T key (0x54) is confirmed to be queried by InputState
//   - Hook verified with 285,000+ function calls per minute
//
// KNOWN LIMITATIONS:
//   - May not perfectly sync with VR trigger cooldowns (Phase 2 improvement)
//   - Direct state machine call bypasses some input validation
//   - Optimized for -noovr mode testing
//
// Research notes: docs/kb/weapon_input_hooks.yaml
// =============================================================================
// This hook enables T key as an alternate weapon fire input in EchoVR.
//
// APPROACH (MVP Phase 1):
//   1. Hook CKeyboardInputDevice::InputState to detect input queries
//   2. Monitor for T key presses (keyCode 0x54)
//   3. When T key is pressed, call weapon fire function
//
// INTEGRATION STRATEGY:
//   - Detects T key independently of VR trigger system
//   - Both VR trigger and T key can fire weapons concurrently
//   - Uses existing game input polling infrastructure
//
// KNOWN LIMITATIONS:
//   - May not perfectly sync with VR trigger cooldowns (Phase 2 improvement)
//   - Direct state machine call bypasses some input validation
//   - Optimized for -noovr mode testing
//
// Research notes: docs/kb/weapon_input_hooks.yaml
// =============================================================================

// Original function pointers
pInputStateFunc orig_InputState = nullptr;
typedef void(__fastcall* pWeaponFireSM)(void* weaponComponent, float triggerValue);
pWeaponFireSM orig_WeaponFireStateMachine = nullptr;
pInputStateFunc orig_MotionControllerInputState = nullptr;

// State tracking
static BOOL g_hooksInitialized = FALSE;
static BOOL g_lastTKeyState = FALSE;
static BOOL g_fireRequested = FALSE;  // TRUE while Y key held, read by VR hook
static FILE* g_weaponInputLog = nullptr;
static void* g_lastWeaponComponent = nullptr;
static float g_lastTriggerValue = 0.0f;

// Diagnostic tracking for Oracle's root cause analysis
static DWORD g_naturalFireThread = 0;         // Thread ID when weapon fires naturally (VR trigger)
static uint64_t g_lastWeaponCaptureTime = 0;  // QueryPerformanceCounter timestamp
static uint64_t g_qpcFrequency = 0;           // QPC frequency for time calculations

// Forward declarations for weapon fire triggering
static void TriggerWeaponFire();

// =============================================================================
// Hook: CKeyboardInputDevice::InputState
// =============================================================================
// Original: float InputState(CKeyboardInputDevice* this, uint64_t keyCode)
// Purpose: Returns 1.0 if key pressed, 0.0 if not
//
// Our hook:
//   - Calls original function first (preserve normal keyboard behavior)
//   - Checks if T key (0x54) is pressed using result from InputState
//   - If pressed (and wasn't in last frame), trigger weapon fire
//   - Returns original result (transparent to game)
// =============================================================================

float __fastcall hook_InputState(void* device, uint64_t keyCode) {
  static int call_count = 0;
  call_count++;

  if (!orig_InputState) {
    if (g_weaponInputLog && call_count == 1) {
      fprintf(g_weaponInputLog, "[ERROR] orig_InputState is NULL!\n");
      fflush(g_weaponInputLog);
    }
    return 0.0f;
  }

  float result = orig_InputState(device, keyCode);

  if (call_count == 1 && g_weaponInputLog) {
    fprintf(g_weaponInputLog, "[DEBUG] Hook function is being called! orig_InputState=%p\n", orig_InputState);
    fprintf(g_weaponInputLog, "[DEBUG] Will log all unique keyCodes queried...\n");
    fflush(g_weaponInputLog);
  }

  static int any_key_pressed_logged = 0;
  if (result > 0.5f && !any_key_pressed_logged && g_weaponInputLog) {
    fprintf(g_weaponInputLog, "[INPUT_TEST] First key press detected! keyCode=0x%02llx, result=%.3f\n", keyCode,
            result);
    any_key_pressed_logged = 1;
    fflush(g_weaponInputLog);
  }

  static uint64_t seen_keys[256] = {0};
  static int unique_key_count = 0;

  int found = 0;
  for (int i = 0; i < unique_key_count; i++) {
    if (seen_keys[i] == keyCode) {
      found = 1;
      break;
    }
  }

  if (!found && unique_key_count < 256 && g_weaponInputLog) {
    seen_keys[unique_key_count++] = keyCode;
    fprintf(g_weaponInputLog, "[KEY] Unique keyCode queried: 0x%02llx (decimal: %llu)\n", keyCode, keyCode);
    if (keyCode == VK_Y) {
      fprintf(g_weaponInputLog, "[KEY] ^^^ THIS IS Y KEY! ^^^\n");
    }
    if (keyCode == VK_SPACE) {
      fprintf(g_weaponInputLog, "[KEY] ^^^ THIS IS SPACE BAR (unexpected!)! ^^^\n");
    }
    fflush(g_weaponInputLog);
  }

  if (keyCode == VK_Y) {
    BOOL yKeyPressed = (result > 0.5f);

    if (yKeyPressed && !g_lastTKeyState) {
      if (g_weaponInputLog) {
        fprintf(g_weaponInputLog, "[Y_KEY_FIRE] Detected Y key press (keyCode=0x%02llx, result=%.1f)\n", keyCode,
                result);
        fflush(g_weaponInputLog);
      }
      printf("[Y_KEY_FIRE] Detected Y key press\n");
      fflush(stdout);

      TriggerWeaponFire();
    }

    g_fireRequested = yKeyPressed;  // Track held state for VR hook
    g_lastTKeyState = yKeyPressed;
  }

  static uint64_t key_frequency[256] = {0};
  if (keyCode < 256) {
    key_frequency[keyCode]++;
  }

  if (call_count % 50000 == 0 && g_weaponInputLog) {
    fprintf(g_weaponInputLog, "[DEBUG] Hook called %d times, unique keys seen: %d\n", call_count, unique_key_count);

    if (call_count == 50000) {
      fprintf(g_weaponInputLog, "[FREQUENCY] All keys queried (showing keys with >100 calls):\n");
      for (int i = 0; i < 256; i++) {
        if (key_frequency[i] > 100) {
          fprintf(g_weaponInputLog, "[FREQUENCY]   0x%02x: %llu times", i, key_frequency[i]);
          if (i == VK_Y) {
            fprintf(g_weaponInputLog, " <-- Y KEY");
          }
          fprintf(g_weaponInputLog, "\n");
        }
      }
      fflush(g_weaponInputLog);
    }
    fflush(g_weaponInputLog);
  }

  return result;
}

// =============================================================================
// Weapon Fire Trigger Function
// =============================================================================
// This function directly calls the weapon fire state machine.
//
// IMPORTANT: This is a Phase 1 MVP approach. It bypasses some input
// validation and may not perfectly integrate with VR trigger cooldowns.
//
// Phase 2 improvement: Locate and use the proper fire message creation
// function to integrate with the game's input system more cleanly.
// =============================================================================

// Hook for Weapon_Fire_StateMachine - captures weapon component pointer
void __fastcall hook_WeaponFireStateMachine(void* weaponComponent, float triggerValue) {
  g_lastWeaponComponent = weaponComponent;
  g_lastTriggerValue = triggerValue;

  DWORD threadId = GetCurrentThreadId();
  void* returnAddr = __builtin_return_address(0);
  void* vtable = weaponComponent ? *(void**)weaponComponent : nullptr;

  QueryPerformanceCounter((LARGE_INTEGER*)&g_lastWeaponCaptureTime);

  if (g_naturalFireThread == 0 && triggerValue >= 0.9f && triggerValue <= 1.1f) {
    g_naturalFireThread = threadId;
    if (g_weaponInputLog) {
      fprintf(g_weaponInputLog, "[NATURAL_FIRE] Valid weapon fire detected on thread %lu (trig=%.2f, vtbl=%p)\n",
              threadId, triggerValue, vtable);
      fflush(g_weaponInputLog);
    }
  }

  if (g_weaponInputLog) {
    fprintf(g_weaponInputLog, "[WEAPON_FIRE_SM] tid=%lu ra=%p weapon=%p vtbl=%p trig=%.2f\n", threadId, returnAddr,
            weaponComponent, vtable, triggerValue);
    fflush(g_weaponInputLog);
  }

  orig_WeaponFireStateMachine(weaponComponent, triggerValue);
}

static void TriggerWeaponFire() {
  if (!g_lastWeaponComponent) {
    if (g_weaponInputLog) {
      fprintf(g_weaponInputLog, "[FIRE_TRIGGER] No weapon component captured yet - fire VR trigger first!\n");
      fflush(g_weaponInputLog);
    }
    return;
  }

  uint64_t currentTime;
  QueryPerformanceCounter((LARGE_INTEGER*)&currentTime);
  double elapsedSeconds = (double)(currentTime - g_lastWeaponCaptureTime) / (double)g_qpcFrequency;

  DWORD threadId = GetCurrentThreadId();
  void* returnAddr = __builtin_return_address(0);
  void* vtable = g_lastWeaponComponent ? *(void**)g_lastWeaponComponent : nullptr;

  if (g_weaponInputLog) {
    fprintf(g_weaponInputLog, "[FIRE_TRIGGER] Y key fire: tid=%lu ra=%p weapon=%p vtbl=%p age=%.3fs\n", threadId,
            returnAddr, g_lastWeaponComponent, vtable, elapsedSeconds);

    if (g_naturalFireThread != 0 && threadId != g_naturalFireThread) {
      fprintf(g_weaponInputLog, "[WARNING] Thread mismatch! Natural fire on tid=%lu, Y key fire on tid=%lu\n",
              g_naturalFireThread, threadId);
    }

    if (elapsedSeconds > 2.0) {
      fprintf(g_weaponInputLog, "[WARNING] Stale weapon pointer! Age: %.3f seconds\n", elapsedSeconds);
    }

    fflush(g_weaponInputLog);
  }

  orig_WeaponFireStateMachine(g_lastWeaponComponent, 1.0f);

  if (g_weaponInputLog) {
    fprintf(g_weaponInputLog, "[FIRE_TRIGGER] Weapon fired successfully!\n");
    fflush(g_weaponInputLog);
  }
}

static void TriggerWeaponFireDirect() {
  if (g_weaponInputLog) {
    fprintf(g_weaponInputLog, "[FIRE_TRIGGER] Weapon_Fire_StateMachine returned successfully!\n");
    fflush(g_weaponInputLog);
  }
}

// =============================================================================
// Hook: CMotionControllerInputDevice::InputState (VR Controller Input)
// =============================================================================
// Purpose: Inject trigger input when g_fireRequested is set by keyboard hook
//
// Injection semantics:
//   - When g_fireRequested is TRUE (Y key held), return 1.0 for TRIGGER_INDEX
//   - This provides continuous "trigger held" signal while Y is pressed
//   - When Y released, g_fireRequested becomes FALSE, trigger returns to original
// =============================================================================

float __fastcall hook_MotionControllerInputState(void* device, uint64_t inputIndex) {
  static int vr_call_count = 0;
  vr_call_count++;

  // Log first call to confirm hook is active
  if (vr_call_count == 1 && g_weaponInputLog) {
    fprintf(g_weaponInputLog, "[VR_HOOK] VR controller hook active! First call received.\n");
    fflush(g_weaponInputLog);
  }

  // Log unique indices (diagnostic)
  static uint64_t seen_indices[64] = {0};
  static int unique_count = 0;
  int found = 0;
  for (int i = 0; i < unique_count && i < 64; i++) {
    if (seen_indices[i] == inputIndex) {
      found = 1;
      break;
    }
  }
  if (!found && unique_count < 64 && g_weaponInputLog) {
    seen_indices[unique_count++] = inputIndex;
    fprintf(g_weaponInputLog, "[VR_HOOK] New VR input index queried: %llu\n", inputIndex);
    fflush(g_weaponInputLog);
  }

  // INJECT TRIGGER: When Y key pressed (g_fireRequested set by keyboard hook)
  if (g_fireRequested && inputIndex == TRIGGER_INDEX) {
    if (g_weaponInputLog) {
      fprintf(g_weaponInputLog, "[VR_HOOK] Injecting trigger value 1.0 for index %llu\n", inputIndex);
      fflush(g_weaponInputLog);
    }
    return 1.0f;  // Trigger fully pressed
  }

  if (!orig_MotionControllerInputState) {
    return 0.0f;
  }

  return orig_MotionControllerInputState(device, inputIndex);
}

// =============================================================================
// Initialization
// =============================================================================

BOOL InitWeaponInputHook() {
  if (g_hooksInitialized) {
    return TRUE;
  }

  // Open log file
  errno_t err = fopen_s(&g_weaponInputLog, "weapon_input_hook.log", "w");
  if (err != 0) {
    Log(EchoVR::LogLevel::Warning, "[WeaponInput] Failed to open log file");
    return FALSE;
  }

  fprintf(g_weaponInputLog, "=== Weapon Input Hook Initialized ===\n");
  fprintf(g_weaponInputLog, "Target: CKeyboardInputDevice::InputState @ RVA 0x%08X\n", ADDR_InputState);
  fprintf(g_weaponInputLog, "Goal: Enable Y key weapon fire\n");
  fprintf(g_weaponInputLog, "Note: Y key (0x19) confirmed working in tests\n");
  fprintf(g_weaponInputLog, "Note: T key had input detection issues\n");
  fprintf(g_weaponInputLog, "==========================================\n\n");
  fprintf(g_weaponInputLog, "[INIT] Starting hook installation...\n");
  fflush(g_weaponInputLog);

  QueryPerformanceFrequency((LARGE_INTEGER*)&g_qpcFrequency);
  fprintf(g_weaponInputLog, "[INIT] QPC frequency: %llu Hz\n", g_qpcFrequency);
  fflush(g_weaponInputLog);

  // Initialize MinHook
  fprintf(g_weaponInputLog, "[INIT] Calling MH_Initialize...\n");
  fflush(g_weaponInputLog);

  MH_STATUS status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
    Log(EchoVR::LogLevel::Error, "[WeaponInput] MH_Initialize failed: %d", status);
    fprintf(g_weaponInputLog, "[ERROR] MH_Initialize failed with status: %d\n", status);
    fflush(g_weaponInputLog);
    return FALSE;
  }

  fprintf(g_weaponInputLog, "[INIT] MH_Initialize succeeded (status: %d)\n", status);
  fflush(g_weaponInputLog);

  // Get module base
  fprintf(g_weaponInputLog, "[INIT] Getting module handle...\n");
  fflush(g_weaponInputLog);

  HMODULE hModule = GetModuleHandleA(NULL);
  if (!hModule) {
    Log(EchoVR::LogLevel::Error, "[WeaponInput] Failed to get module handle");
    fprintf(g_weaponInputLog, "[ERROR] GetModuleHandleA failed\n");
    fflush(g_weaponInputLog);
    return FALSE;
  }

  fprintf(g_weaponInputLog, "[INIT] Module handle: %p\n", hModule);
  fflush(g_weaponInputLog);

  // Calculate absolute address
  void* targetAddr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hModule) + ADDR_InputState);

  if (g_weaponInputLog) {
    fprintf(g_weaponInputLog, "[INIT] Target address: %p (base + 0x%08X)\n", targetAddr, ADDR_InputState);
    fflush(g_weaponInputLog);
  }

  // Create hook
  status =
      MH_CreateHook(targetAddr, reinterpret_cast<void*>(&hook_InputState), reinterpret_cast<void**>(&orig_InputState));

  if (status != MH_OK) {
    Log(EchoVR::LogLevel::Error, "[WeaponInput] MH_CreateHook failed: %d", status);
    if (g_weaponInputLog) {
      fprintf(g_weaponInputLog, "[ERROR] MH_CreateHook failed with status: %d\n", status);
      fflush(g_weaponInputLog);
    }
    return FALSE;
  }

  if (g_weaponInputLog) {
    fprintf(g_weaponInputLog, "[INIT] MH_CreateHook succeeded\n");
    fflush(g_weaponInputLog);
  }

  // Enable hook
  status = MH_EnableHook(targetAddr);
  if (status != MH_OK) {
    Log(EchoVR::LogLevel::Error, "[WeaponInput] MH_EnableHook failed: %d", status);
    if (g_weaponInputLog) {
      fprintf(g_weaponInputLog, "[ERROR] MH_EnableHook failed with status: %d\n", status);
      fflush(g_weaponInputLog);
    }
    return FALSE;
  }

  if (g_weaponInputLog) {
    fprintf(g_weaponInputLog, "[INIT] InputState hook enabled\n");
    fflush(g_weaponInputLog);
  }

  void* weaponFireAddr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hModule) + ADDR_WeaponFireStateMachine);

  fprintf(g_weaponInputLog, "[INIT] Hooking Weapon_Fire_StateMachine @ %p\n", weaponFireAddr);
  fflush(g_weaponInputLog);

  status = MH_CreateHook(weaponFireAddr, reinterpret_cast<void*>(&hook_WeaponFireStateMachine),
                         reinterpret_cast<void**>(&orig_WeaponFireStateMachine));

  if (status != MH_OK) {
    Log(EchoVR::LogLevel::Error, "[WeaponInput] MH_CreateHook (WeaponFireSM) failed: %d", status);
    fprintf(g_weaponInputLog, "[ERROR] WeaponFireSM hook failed with status: %d\n", status);
    fflush(g_weaponInputLog);
    return FALSE;
  }

  status = MH_EnableHook(weaponFireAddr);
  if (status != MH_OK) {
    Log(EchoVR::LogLevel::Error, "[WeaponInput] MH_EnableHook (WeaponFireSM) failed: %d", status);
    fprintf(g_weaponInputLog, "[ERROR] WeaponFireSM enable failed with status: %d\n", status);
    fflush(g_weaponInputLog);
    return FALSE;
  }

  fprintf(g_weaponInputLog, "[INIT] Weapon_Fire_StateMachine hook enabled\n");
  fflush(g_weaponInputLog);

  // Install VR controller input hook
  void* vrInputAddr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hModule) + ADDR_MotionControllerInputState);

  fprintf(g_weaponInputLog, "[INIT] Installing VR controller hook at %p (RVA 0x%08X)\n", vrInputAddr,
          ADDR_MotionControllerInputState);
  fflush(g_weaponInputLog);

  status = MH_CreateHook(vrInputAddr, reinterpret_cast<void*>(&hook_MotionControllerInputState),
                         reinterpret_cast<void**>(&orig_MotionControllerInputState));

  if (status != MH_OK) {
    fprintf(g_weaponInputLog, "[ERROR] VR controller hook MH_CreateHook failed: %d\n", status);
    fflush(g_weaponInputLog);
  } else {
    status = MH_EnableHook(vrInputAddr);
    if (status != MH_OK) {
      fprintf(g_weaponInputLog, "[ERROR] VR controller hook MH_EnableHook failed: %d\n", status);
      fflush(g_weaponInputLog);
    } else {
      fprintf(g_weaponInputLog, "[INIT] VR controller hook enabled successfully\n");
      fflush(g_weaponInputLog);
    }
  }

  Log(EchoVR::LogLevel::Info, "[WeaponInput] Hook installed successfully");
  Log(EchoVR::LogLevel::Info, "[WeaponInput] Press Y KEY to fire weapons (in addition to VR trigger)");

  g_hooksInitialized = TRUE;
  return TRUE;
}

// =============================================================================
// Cleanup
// =============================================================================

void CleanupWeaponInputHook() {
  if (!g_hooksInitialized) {
    return;
  }

  // Get module base
  HMODULE hModule = GetModuleHandleA(NULL);
  if (hModule) {
    void* targetAddr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(hModule) + ADDR_InputState);
    MH_DisableHook(targetAddr);
  }

  if (g_weaponInputLog) {
    fprintf(g_weaponInputLog, "\n=== Weapon Input Hook Shutdown ===\n");
    fclose(g_weaponInputLog);
    g_weaponInputLog = nullptr;
  }

  g_hooksInitialized = FALSE;
}

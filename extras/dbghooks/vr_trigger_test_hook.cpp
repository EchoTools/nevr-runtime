#include "vr_trigger_test_hook.h"

#include <cstdio>
#include <windows.h>

#include "common/hooking.h"
#include "common/logging.h"
#include "echovrInternal.h"

static pInputStateFunc_VRTrigger orig_InputState_VRTrigger = nullptr;
static WeaponFireSM_VRTrigger_t g_weaponFireSM = nullptr;

static BOOL g_vrTriggerHookInitialized = FALSE;
static FILE* g_vrTriggerLog = nullptr;
static UINT64 g_triggerAttemptCount = 0;
static UINT64 g_lastTriggerLogTime = 0;
static BOOL g_lastTKeyState = FALSE;

#define TRIGGER_LOG_THROTTLE_MS 200

float __fastcall hook_InputState_VRTrigger(void* device, uint64_t keyCode) {
  float result = orig_InputState_VRTrigger(device, keyCode);

  if (keyCode == VK_T && result > 0.0f) {
    if (!g_lastTKeyState) {
      g_lastTKeyState = TRUE;
      
      UINT64 currentTime = GetTickCount64();
      if (currentTime - g_lastTriggerLogTime >= TRIGGER_LOG_THROTTLE_MS) {
        g_lastTriggerLogTime = currentTime;
        TriggerWeaponFireTest();
      }
    }
  } else if (keyCode == VK_T && result == 0.0f) {
    g_lastTKeyState = FALSE;
  }

  return result;
}

VOID TriggerWeaponFireTest() {
  g_triggerAttemptCount++;
  
  UINT64 timestamp = GetTickCount64();
  
  if (g_vrTriggerLog) {
    fprintf(g_vrTriggerLog,
            "[VR_TRIGGER_TEST] Attempt #%llu | Timestamp: %llu ms\n",
            g_triggerAttemptCount, timestamp);
    fprintf(g_vrTriggerLog,
            "[VR_TRIGGER_TEST] Calling Weapon_Fire_StateMachine @ 0x%p with NULL context\n",
            g_weaponFireSM);
    fflush(g_vrTriggerLog);
  }

  Log(EchoVR::LogLevel::Info, "[VRTriggerTest] Attempt #%llu - Firing weapon", g_triggerAttemptCount);

  if (!g_weaponFireSM) {
    if (g_vrTriggerLog) {
      fprintf(g_vrTriggerLog,
              "[VR_TRIGGER_TEST] ERROR: g_weaponFireSM is NULL! Cannot call function.\n");
      fflush(g_vrTriggerLog);
    }
    Log(EchoVR::LogLevel::Error, "[VRTriggerTest] Weapon fire function pointer is NULL");
    return;
  }

  uint64_t result = g_weaponFireSM(nullptr);
  
  if (g_vrTriggerLog) {
    fprintf(g_vrTriggerLog,
            "[VR_TRIGGER_TEST] Weapon_Fire_StateMachine returned: 0x%llx\n",
            result);
    fprintf(g_vrTriggerLog,
            "[VR_TRIGGER_TEST] SUCCESS - Check weapon_system_trace.log for full trace\n\n");
    fflush(g_vrTriggerLog);
  }
  
  Log(EchoVR::LogLevel::Info, "[VRTriggerTest] Weapon fire returned 0x%llx", result);
}

VOID InitializeVRTriggerTestHook() {
  if (g_vrTriggerHookInitialized) {
    Log(EchoVR::LogLevel::Warning, "[VRTriggerTest] Already initialized");
    return;
  }

  Log(EchoVR::LogLevel::Info, "[VRTriggerTest] Initializing VR trigger test hook...");

  g_vrTriggerLog = fopen("vr_trigger_test.log", "w");
  if (!g_vrTriggerLog) {
    Log(EchoVR::LogLevel::Error, "[VRTriggerTest] Failed to open vr_trigger_test.log");
    return;
  }

  fprintf(g_vrTriggerLog, "# VR Trigger Test Log\n");
  fprintf(g_vrTriggerLog, "# Testing programmatic weapon fire via T key\n\n");
  fflush(g_vrTriggerLog);

  HMODULE hModule = GetModuleHandleA(NULL);
  if (!hModule) {
    Log(EchoVR::LogLevel::Error, "[VRTriggerTest] Failed to get module handle");
    fclose(g_vrTriggerLog);
    g_vrTriggerLog = nullptr;
    return;
  }

  void* inputStateAddr = reinterpret_cast<void*>(
      reinterpret_cast<uintptr_t>(hModule) + ADDR_InputState_VRTrigger);
  
  g_weaponFireSM = reinterpret_cast<WeaponFireSM_VRTrigger_t>(
      reinterpret_cast<uintptr_t>(hModule) + ADDR_WeaponFireSM_VRTrigger);

  Log(EchoVR::LogLevel::Info, "[VRTriggerTest] InputState @ %p, WeaponFireSM @ %p",
      inputStateAddr, g_weaponFireSM);

  if (g_vrTriggerLog) {
    fprintf(g_vrTriggerLog, "InputState @ %p (RVA 0x%08X)\n", inputStateAddr, ADDR_InputState_VRTrigger);
    fprintf(g_vrTriggerLog, "WeaponFireSM @ %p (RVA 0x%08X)\n", g_weaponFireSM, ADDR_WeaponFireSM_VRTrigger);
    fprintf(g_vrTriggerLog, "Watching for: VK_T (0x%02X)\n\n", VK_T);
    fflush(g_vrTriggerLog);
  }

  MH_STATUS status = MH_CreateHook(
      inputStateAddr,
      reinterpret_cast<void*>(&hook_InputState_VRTrigger),
      reinterpret_cast<void**>(&orig_InputState_VRTrigger));

  if (status != MH_OK) {
    Log(EchoVR::LogLevel::Error, "[VRTriggerTest] Failed to create hook: %s", MH_StatusToString(status));
    fclose(g_vrTriggerLog);
    g_vrTriggerLog = nullptr;
    return;
  }

  status = MH_EnableHook(inputStateAddr);
  if (status != MH_OK) {
    Log(EchoVR::LogLevel::Error, "[VRTriggerTest] Failed to enable hook: %s", MH_StatusToString(status));
    MH_RemoveHook(inputStateAddr);
    fclose(g_vrTriggerLog);
    g_vrTriggerLog = nullptr;
    return;
  }

  g_vrTriggerHookInitialized = TRUE;
  Log(EchoVR::LogLevel::Info, "[VRTriggerTest] Hook installed successfully");
  Log(EchoVR::LogLevel::Info, "[VRTriggerTest] Press T in-game to test weapon fire");

  if (g_vrTriggerLog) {
    fprintf(g_vrTriggerLog, "Hook installed successfully\n");
    fprintf(g_vrTriggerLog, "Monitoring started at: %llu ms\n\n", GetTickCount64());
    fflush(g_vrTriggerLog);
  }
}

VOID ShutdownVRTriggerTestHook() {
  if (!g_vrTriggerHookInitialized) {
    return;
  }

  Log(EchoVR::LogLevel::Info, "[VRTriggerTest] Shutting down...");

  HMODULE hModule = GetModuleHandleA(NULL);
  if (hModule) {
    void* inputStateAddr = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(hModule) + ADDR_InputState_VRTrigger);
    MH_DisableHook(inputStateAddr);
    MH_RemoveHook(inputStateAddr);
  }

  if (g_vrTriggerLog) {
    fprintf(g_vrTriggerLog, "\nShutdown at: %llu ms\n", GetTickCount64());
    fprintf(g_vrTriggerLog, "Total trigger attempts: %llu\n", g_triggerAttemptCount);
    fclose(g_vrTriggerLog);
    g_vrTriggerLog = nullptr;
  }

  g_vrTriggerHookInitialized = FALSE;
  Log(EchoVR::LogLevel::Info, "[VRTriggerTest] Shutdown complete");
}

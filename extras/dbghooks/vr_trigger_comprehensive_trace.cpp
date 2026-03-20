#include "vr_trigger_comprehensive_trace.h"
#include <MinHook.h>
#include <string>
#include <vector>

namespace VRTriggerTrace {

std::chrono::steady_clock::time_point g_startTime;
FILE* g_traceLog = nullptr;

// X key simulation globals
static DWORD g_lastXKeyPress = 0;
static bool g_xKeyWasPressed = false;
static HANDLE g_xKeyThread = nullptr;
static bool g_xKeyThreadRunning = false;

uint32_t GetElapsedMs() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_startTime);
    return static_cast<uint32_t>(elapsed.count());
}

void StartTimer() {
    g_startTime = std::chrono::steady_clock::now();
}

void TriggerWeaponFireFromXKey() {
    fprintf(g_traceLog, "[X_KEY] Attempting to trigger weapon fire...\n");
    fflush(g_traceLog);
    
    if (orig_InputState) {
        void* triggerInputId = (void*)0x141c8d4c0;
        
        fprintf(g_traceLog, "[X_KEY] Calling InputState with RightIndexTriggerAnalog (0x%p)...\n", triggerInputId);
        float result = orig_InputState(nullptr, (uint64_t)triggerInputId);
        fprintf(g_traceLog, "[X_KEY] InputState returned: %f\n", result);
    }
    
    if (orig_FireGunInputMsg_Handler) {
        fprintf(g_traceLog, "[X_KEY] Calling FireGunInputMsg_Handler...\n");
        orig_FireGunInputMsg_Handler(nullptr, nullptr, nullptr);
        fprintf(g_traceLog, "[X_KEY] FireGunInputMsg_Handler returned\n");
    }
    
    if (orig_Weapon_Fire_StateMachine) {
        fprintf(g_traceLog, "[X_KEY] Calling Weapon_Fire_StateMachine...\n");
        orig_Weapon_Fire_StateMachine();
        fprintf(g_traceLog, "[X_KEY] Weapon_Fire_StateMachine returned\n");
    }
    
    fprintf(g_traceLog, "[X_KEY] All trigger attempts completed\n");
    fprintf(g_traceLog, "========================================\n\n");
    fflush(g_traceLog);
}

void CheckForXKeyPress() {
    static DWORD lastCheck = 0;
    DWORD now = GetTickCount();
    
    if (now - lastCheck < 50) return;
    lastCheck = now;
    
    SHORT xKeyState = GetAsyncKeyState('X');
    bool xPressed = (xKeyState & 0x8000) != 0;
    
    if (xPressed && !g_xKeyWasPressed) {
        g_lastXKeyPress = now;
        
        fprintf(g_traceLog, "\n");
        fprintf(g_traceLog, "========================================\n");
        fprintf(g_traceLog, "[X_KEY] X KEY PRESSED - SIMULATING VR TRIGGER\n");
        fprintf(g_traceLog, "========================================\n");
        fflush(g_traceLog);
        
        TriggerWeaponFireFromXKey();
    }
    
    g_xKeyWasPressed = xPressed;
}

DWORD WINAPI XKeyMonitorThread(LPVOID param) {
    while (g_xKeyThreadRunning) {
        CheckForXKeyPress();
        Sleep(50);
    }
    return 0;
}

bool InitializeLogging(const char* logPath) {
    errno_t err = fopen_s(&g_traceLog, logPath, "w");
    if (err != 0 || !g_traceLog) {
        return false;
    }
    
    fprintf(g_traceLog, "=== VR TRIGGER COMPREHENSIVE TRACE START ===\n");
    fprintf(g_traceLog, "Strategy: Blanket coverage of trigger->weapon fire pipeline\n");
    fprintf(g_traceLog, "Instructions: Put on VR headset, enter combat, fire weapon ONCE\n");
    fprintf(g_traceLog, "Alternative: Press X key on keyboard to simulate trigger pull\n");
    fprintf(g_traceLog, "============================================\n\n");
    fflush(g_traceLog);
    
    StartTimer();
    
    g_xKeyThreadRunning = true;
    g_xKeyThread = CreateThread(nullptr, 0, XKeyMonitorThread, nullptr, 0, nullptr);
    if (g_xKeyThread) {
        fprintf(g_traceLog, "[INIT] X key monitor thread started\n");
    } else {
        fprintf(g_traceLog, "[INIT] WARNING: Failed to start X key monitor thread\n");
    }
    fflush(g_traceLog);
    
    return true;
}

void CloseLogging() {
    if (g_xKeyThread) {
        g_xKeyThreadRunning = false;
        WaitForSingleObject(g_xKeyThread, 1000);
        CloseHandle(g_xKeyThread);
        g_xKeyThread = nullptr;
    }
    
    if (g_traceLog) {
        fprintf(g_traceLog, "\n=== VR TRIGGER COMPREHENSIVE TRACE END ===\n");
        fclose(g_traceLog);
        g_traceLog = nullptr;
    }
}

InputState_t orig_InputState = nullptr;
float hook_InputState(void* thisptr, uint64_t inputId) {
    float result = orig_InputState(thisptr, inputId);
    
    if (result > 0.01f) {
        LOG_ENTRY("InputState");
        LOG_PTR("thisptr", thisptr);
        LOG_PARAM("inputId", "0x%llx", inputId);
        LOG_PARAM("result", "%f", result);
        LOG_EXIT("InputState");
    }
    
    return result;
}

SetIndex_t orig_SetIndex = nullptr;
void hook_SetIndex(void* thisptr, uint64_t index) {
    LOG_ENTRY("SetIndex");
    LOG_PTR("thisptr", thisptr);
    LOG_PARAM("index", "%llu", index);
    
    orig_SetIndex(thisptr, index);
    
    LOG_EXIT("SetIndex");
}

FireGunInputMsg_Handler_t orig_FireGunInputMsg_Handler = nullptr;
void hook_FireGunInputMsg_Handler(void* param1, void* param2, void* param3) {
    LOG_ENTRY("FireGunInputMsg_Handler [SR15NetGameInputFireGunMsg]");
    LOG_PTR("param1", param1);
    LOG_PTR("param2", param2);
    LOG_PTR("param3", param3);
    
    orig_FireGunInputMsg_Handler(param1, param2, param3);
    
    LOG_EXIT("FireGunInputMsg_Handler");
}

FireGunReplicateMsg_Handler_t orig_FireGunReplicateMsg_Handler = nullptr;
void hook_FireGunReplicateMsg_Handler(void* param1, void* param2, void* param3) {
    LOG_ENTRY("FireGunReplicateMsg_Handler [SR15NetGameReplicateFireGunMsg]");
    LOG_PTR("param1", param1);
    LOG_PTR("param2", param2);
    LOG_PTR("param3", param3);
    
    orig_FireGunReplicateMsg_Handler(param1, param2, param3);
    
    LOG_EXIT("FireGunReplicateMsg_Handler");
}

Weapon_Fire_StateMachine_t orig_Weapon_Fire_StateMachine = nullptr;
void hook_Weapon_Fire_StateMachine() {
    LOG_ENTRY("Weapon_Fire_StateMachine @ 0x1410b3000");
    
    orig_Weapon_Fire_StateMachine();
    
    LOG_EXIT("Weapon_Fire_StateMachine");
}

FUN_1400c29b0_t orig_FUN_1400c29b0 = nullptr;
void hook_FUN_1400c29b0(void* param1) {
    LOG_ENTRY("FUN_1400c29b0 [LeftIndexTriggerAnalog ref]");
    LOG_PTR("param1", param1);
    
    orig_FUN_1400c29b0(param1);
    
    LOG_EXIT("FUN_1400c29b0");
}

FUN_1400c7d46_t orig_FUN_1400c7d46 = nullptr;
void hook_FUN_1400c7d46(void* param1) {
    LOG_ENTRY("FUN_1400c7d46 [LeftIndexTriggerAnalog ref]");
    LOG_PTR("param1", param1);
    
    orig_FUN_1400c7d46(param1);
    
    LOG_EXIT("FUN_1400c7d46");
}

GetConnectedControllerTypes_t orig_GetConnectedControllerTypes = nullptr;
void* hook_GetConnectedControllerTypes() {
    LOG_ENTRY("GetConnectedControllerTypes [ovr_GetConnectedControllerTypes]");
    
    void* result = orig_GetConnectedControllerTypes();
    
    LOG_PTR("result", result);
    LOG_EXIT("GetConnectedControllerTypes");
    return result;
}

SetControllerVibration_t orig_SetControllerVibration = nullptr;
int hook_SetControllerVibration(void* controller, float frequency, float amplitude) {
    LOG_ENTRY("SetControllerVibration [ovr_SetControllerVibration]");
    LOG_PTR("controller", controller);
    LOG_PARAM("frequency", "%f", frequency);
    LOG_PARAM("amplitude", "%f", amplitude);
    
    int result = orig_SetControllerVibration(controller, frequency, amplitude);
    
    LOG_PARAM("result", "%d", result);
    LOG_EXIT("SetControllerVibration");
    return result;
}

UpdatePlayerInput_t orig_UpdatePlayerInput = nullptr;
void hook_UpdatePlayerInput(void* thisptr, void* state, float deltaTime) {
    LOG_ENTRY("UpdatePlayerInput");
    LOG_PTR("thisptr", thisptr);
    LOG_PTR("state", state);
    LOG_PARAM("deltaTime", "%f", deltaTime);
    
    orig_UpdatePlayerInput(thisptr, state, deltaTime);
    
    LOG_EXIT("UpdatePlayerInput");
}

FireStart_Delegate_t orig_FireStart_Delegate = nullptr;
void hook_FireStart_Delegate(void* thisptr) {
    LOG_ENTRY("FireStart_Delegate [delegate_FireStart]");
    LOG_PTR("thisptr", thisptr);
    
    orig_FireStart_Delegate(thisptr);
    
    LOG_EXIT("FireStart_Delegate");
}

FireStop_Delegate_t orig_FireStop_Delegate = nullptr;
void hook_FireStop_Delegate(void* thisptr) {
    LOG_ENTRY("FireStop_Delegate [delegate_FireStop]");
    LOG_PTR("thisptr", thisptr);
    
    orig_FireStop_Delegate(thisptr);
    
    LOG_EXIT("FireStop_Delegate");
}

Fired_Delegate_t orig_Fired_Delegate = nullptr;
void hook_Fired_Delegate(void* thisptr) {
    LOG_ENTRY("Fired_Delegate [delegate_Fired]");
    LOG_PTR("thisptr", thisptr);
    
    orig_Fired_Delegate(thisptr);
    
    LOG_EXIT("Fired_Delegate");
}

BulletFired_Delegate_t orig_BulletFired_Delegate = nullptr;
void hook_BulletFired_Delegate(void* thisptr) {
    LOG_ENTRY("BulletFired_Delegate [delegate_BulletFired]");
    LOG_PTR("thisptr", thisptr);
    
    orig_BulletFired_Delegate(thisptr);
    
    LOG_EXIT("BulletFired_Delegate");
}

NetworkFired_Delegate_t orig_NetworkFired_Delegate = nullptr;
void hook_NetworkFired_Delegate(void* thisptr) {
    LOG_ENTRY("NetworkFired_Delegate [delegate_NetworkFired]");
    LOG_PTR("thisptr", thisptr);
    
    orig_NetworkFired_Delegate(thisptr);
    
    LOG_EXIT("NetworkFired_Delegate");
}

CPlayerInputCS_Initialize_t orig_CPlayerInputCS_Initialize = nullptr;
void hook_CPlayerInputCS_Initialize(void* thisptr) {
    LOG_ENTRY("CPlayerInputCS_Initialize");
    LOG_PTR("thisptr", thisptr);
    
    orig_CPlayerInputCS_Initialize(thisptr);
    
    LOG_EXIT("CPlayerInputCS_Initialize");
}

CInputCS_Initialize_t orig_CInputCS_Initialize = nullptr;
void hook_CInputCS_Initialize(void* thisptr) {
    LOG_ENTRY("CInputCS_Initialize");
    LOG_PTR("thisptr", thisptr);
    
    orig_CInputCS_Initialize(thisptr);
    
    LOG_EXIT("CInputCS_Initialize");
}

FUN_14127ed30_t orig_FUN_14127ed30 = nullptr;
void hook_FUN_14127ed30(void* param1) {
    LOG_ENTRY("FUN_14127ed30 [Weapon_Fire_StateMachine ref in RemotePlayerCS]");
    LOG_PTR("param1", param1);
    
    orig_FUN_14127ed30(param1);
    
    LOG_EXIT("FUN_14127ed30");
}

struct HookEntry {
    const char* name;
    void* targetAddress;
    void* hookFunction;
    void** originalFunction;
};

bool InstallAllHooks() {
    std::vector<HookEntry> hooks = {
        {"InputState", (void*)0x140f9b2f0, (void*)hook_InputState, (void**)&orig_InputState},
        {"SetIndex", (void*)0x140f9c310, (void*)hook_SetIndex, (void**)&orig_SetIndex},
        {"FireGunInputMsg_Handler", (void*)0x1400ab740, (void*)hook_FireGunInputMsg_Handler, (void**)&orig_FireGunInputMsg_Handler},
        {"FireGunReplicateMsg_Handler", (void*)0x1400ab860, (void*)hook_FireGunReplicateMsg_Handler, (void**)&orig_FireGunReplicateMsg_Handler},
        {"Weapon_Fire_StateMachine", (void*)0x1410b3000, (void*)hook_Weapon_Fire_StateMachine, (void**)&orig_Weapon_Fire_StateMachine},
        {"FUN_1400c29b0", (void*)0x1400c29b0, (void*)hook_FUN_1400c29b0, (void**)&orig_FUN_1400c29b0},
        {"FUN_1400c7d46", (void*)0x1400c7d46, (void*)hook_FUN_1400c7d46, (void**)&orig_FUN_1400c7d46},
        {"FUN_14127ed30", (void*)0x14127ed30, (void*)hook_FUN_14127ed30, (void**)&orig_FUN_14127ed30},
    };
    
    uintptr_t baseAddress = (uintptr_t)GetModuleHandleA(nullptr);
    
    if (MH_Initialize() != MH_OK) {
        return false;
    }
    
    int successCount = 0;
    for (const auto& hook : hooks) {
        void* actualTarget = (void*)((uintptr_t)hook.targetAddress - 0x140000000 + baseAddress);
        
        MH_STATUS status = MH_CreateHook(actualTarget, hook.hookFunction, hook.originalFunction);
        if (status == MH_OK) {
            status = MH_EnableHook(actualTarget);
            if (status == MH_OK) {
                successCount++;
                if (g_traceLog) {
                    fprintf(g_traceLog, "[INIT] Hooked %s at 0x%p\n", hook.name, actualTarget);
                }
            }
        }
        
        if (status != MH_OK) {
            if (g_traceLog) {
                fprintf(g_traceLog, "[INIT] FAILED to hook %s at 0x%p (status: %d)\n", 
                        hook.name, actualTarget, status);
            }
        }
    }
    
    if (g_traceLog) {
        fprintf(g_traceLog, "[INIT] Successfully installed %d/%zu hooks\n", successCount, hooks.size());
        fprintf(g_traceLog, "\n=== WAITING FOR VR TRIGGER PULL ===\n\n");
        fflush(g_traceLog);
    }
    
    return successCount > 0;
}

void UninstallAllHooks() {
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
}

}

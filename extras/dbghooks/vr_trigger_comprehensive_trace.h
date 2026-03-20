#pragma once
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <chrono>

// VR Trigger Comprehensive Trace
// Strategy: Hook EVERYTHING that could possibly be involved in trigger -> weapon fire pipeline

namespace VRTriggerTrace {

// Timing
extern std::chrono::steady_clock::time_point g_startTime;
uint32_t GetElapsedMs();
void StartTimer();

// Logging
extern FILE* g_traceLog;
bool InitializeLogging(const char* logPath);
void CloseLogging();

// Hook installation
bool InstallAllHooks();
void UninstallAllHooks();

// Hook function declarations
// Category 1: VR Input System (CRITICAL)
typedef float (*InputState_t)(void* thisptr, uint64_t inputId);
extern InputState_t orig_InputState;
float hook_InputState(void* thisptr, uint64_t inputId);

typedef void (*SetIndex_t)(void* thisptr, uint64_t index);
extern SetIndex_t orig_SetIndex;
void hook_SetIndex(void* thisptr, uint64_t index);

// Category 2: Fire Messages (CRITICAL)
typedef void (*FireGunInputMsg_Handler_t)(void* param1, void* param2, void* param3);
extern FireGunInputMsg_Handler_t orig_FireGunInputMsg_Handler;
void hook_FireGunInputMsg_Handler(void* param1, void* param2, void* param3);

typedef void (*FireGunReplicateMsg_Handler_t)(void* param1, void* param2, void* param3);
extern FireGunReplicateMsg_Handler_t orig_FireGunReplicateMsg_Handler;
void hook_FireGunReplicateMsg_Handler(void* param1, void* param2, void* param3);

// Category 3: Weapon State Machine (CRITICAL)
typedef void (*Weapon_Fire_StateMachine_t)();
extern Weapon_Fire_StateMachine_t orig_Weapon_Fire_StateMachine;
void hook_Weapon_Fire_StateMachine();

// Category 4: Input Component Functions (HIGH PRIORITY)
typedef void (*FUN_1400c29b0_t)(void* param1);
extern FUN_1400c29b0_t orig_FUN_1400c29b0;
void hook_FUN_1400c29b0(void* param1);

typedef void (*FUN_1400c7d46_t)(void* param1);
extern FUN_1400c7d46_t orig_FUN_1400c7d46;
void hook_FUN_1400c7d46(void* param1);

// Category 5: Controller Input System (HIGH PRIORITY)
typedef void* (*GetConnectedControllerTypes_t)();
extern GetConnectedControllerTypes_t orig_GetConnectedControllerTypes;
void* hook_GetConnectedControllerTypes();

typedef int (*SetControllerVibration_t)(void* controller, float frequency, float amplitude);
extern SetControllerVibration_t orig_SetControllerVibration;
int hook_SetControllerVibration(void* controller, float frequency, float amplitude);

// Category 6: Player Input / Equipment (MEDIUM PRIORITY)
typedef void (*UpdatePlayerInput_t)(void* thisptr, void* state, float deltaTime);
extern UpdatePlayerInput_t orig_UpdatePlayerInput;
void hook_UpdatePlayerInput(void* thisptr, void* state, float deltaTime);

// Category 7: Fire Delegates (MEDIUM PRIORITY)
// These are likely called when weapon fires
typedef void (*FireStart_Delegate_t)(void* thisptr);
extern FireStart_Delegate_t orig_FireStart_Delegate;
void hook_FireStart_Delegate(void* thisptr);

typedef void (*FireStop_Delegate_t)(void* thisptr);
extern FireStop_Delegate_t orig_FireStop_Delegate;
void hook_FireStop_Delegate(void* thisptr);

typedef void (*Fired_Delegate_t)(void* thisptr);
extern Fired_Delegate_t orig_Fired_Delegate;
void hook_Fired_Delegate(void* thisptr);

typedef void (*BulletFired_Delegate_t)(void* thisptr);
extern BulletFired_Delegate_t orig_BulletFired_Delegate;
void hook_BulletFired_Delegate(void* thisptr);

typedef void (*NetworkFired_Delegate_t)(void* thisptr);
extern NetworkFired_Delegate_t orig_NetworkFired_Delegate;
void hook_NetworkFired_Delegate(void* thisptr);

// Category 8: Input Action System (MEDIUM PRIORITY)
typedef void (*CPlayerInputCS_Initialize_t)(void* thisptr);
extern CPlayerInputCS_Initialize_t orig_CPlayerInputCS_Initialize;
void hook_CPlayerInputCS_Initialize(void* thisptr);

typedef void (*CInputCS_Initialize_t)(void* thisptr);
extern CInputCS_Initialize_t orig_CInputCS_Initialize;
void hook_CInputCS_Initialize(void* thisptr);

// Category 9: Wide Net - Exploratory Functions
typedef void (*FUN_14127ed30_t)(void* param1);
extern FUN_14127ed30_t orig_FUN_14127ed30;
void hook_FUN_14127ed30(void* param1);

// Add more hooks as needed from Ghidra analysis

// Helper macros
#define LOG_ENTRY(name, ...) \
    fprintf(g_traceLog, "[%06u] >>> " name "\n", GetElapsedMs(), ##__VA_ARGS__); \
    fflush(g_traceLog);

#define LOG_EXIT(name, ...) \
    fprintf(g_traceLog, "[%06u] <<< " name "\n", GetElapsedMs(), ##__VA_ARGS__); \
    fflush(g_traceLog);

#define LOG_PARAM(name, format, value) \
    fprintf(g_traceLog, "[%06u]     %s=" format "\n", GetElapsedMs(), name, value); \
    fflush(g_traceLog);

#define LOG_PTR(name, ptr) \
    fprintf(g_traceLog, "[%06u]     %s=0x%p\n", GetElapsedMs(), name, ptr); \
    fflush(g_traceLog);

} // namespace VRTriggerTrace

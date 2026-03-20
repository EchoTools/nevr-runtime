#pragma once

#include <cstdint>
#include <windows.h>

// =============================================================================
// VR Trigger Test Hook - Header
// =============================================================================
// This hook simulates VR trigger input using keyboard T key for testing.
//
// APPROACH:
//   1. Monitor keyboard for T key press (VK_T = 0x54)
//   2. When T is pressed, directly call Weapon_Fire_StateMachine
//   3. Log all attempts and results
//   4. Verify weapon system trace hooks fire
//
// INTEGRATION:
//   - Uses same InputState hook as booster test
//   - Calls weapon fire function directly (bypass VR input path)
//   - Works with weapon_system_trace.cpp hooks
//
// GOAL:
//   Prove we can trigger weapon fire programmatically, then find real VR path
// =============================================================================

// =============================================================================
// Hook Addresses
// =============================================================================

// CKeyboardInputDevice::InputState
#define ADDR_InputState_VRTrigger 0x0f9b330  // RVA: 0x140f9b330

// Weapon_Fire_StateMachine (from weapon_system_trace.h)
#define ADDR_WeaponFireSM_VRTrigger 0x10b3000  // RVA: 0x1410b3000

// =============================================================================
// Virtual Key Codes
// =============================================================================

#define VK_T 0x54  // T key (trigger test)

// =============================================================================
// Function Type Definitions
// =============================================================================

// InputState signature
typedef float(__fastcall* pInputStateFunc_VRTrigger)(void* device, uint64_t keyCode);

// Weapon_Fire_StateMachine signature
// Based on weapon_system_trace.h: uint64_t Weapon_Fire_StateMachine(int64_t* context)
typedef uint64_t (*WeaponFireSM_VRTrigger_t)(int64_t* context);

// =============================================================================
// Public API
// =============================================================================

/// <summary>
/// Initializes the VR trigger test hook.
/// Opens log file and installs InputState hook.
/// </summary>
VOID InitializeVRTriggerTestHook();

/// <summary>
/// Shuts down the VR trigger test hook and closes log file.
/// </summary>
VOID ShutdownVRTriggerTestHook();

/// <summary>
/// Attempts to fire the weapon programmatically.
/// Called when T key is detected.
/// </summary>
VOID TriggerWeaponFireTest();

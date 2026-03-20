#pragma once

// =============================================================================
// Weapon Input Hook - Space Bar Weapon Fire
// =============================================================================
// Purpose: Enable space bar as alternate weapon fire input (alongside VR trigger)
//
// Architecture:
//   - Hooks CKeyboardInputDevice::InputState @ 0x140f9b330
//   - Detects space bar press
//   - Triggers weapon fire via state machine
//
// Research: docs/kb/weapon_input_hooks.yaml
// =============================================================================

#include <intrin.h>
#include <windows.h>

#include <cstdint>

// Function type for CKeyboardInputDevice::InputState
// Signature: float InputState(CKeyboardInputDevice* this, uint64_t keyCode)
typedef float(__fastcall* pInputStateFunc)(void* device, uint64_t keyCode);

// Original function pointer (populated during initialization)
extern pInputStateFunc orig_InputState;

// Hook functions
float __fastcall hook_InputState(void* device, uint64_t keyCode);

// VR Controller Input Hook
extern pInputStateFunc orig_MotionControllerInputState;
float __fastcall hook_MotionControllerInputState(void* device, uint64_t inputIndex);

// Initialization/cleanup
BOOL InitWeaponInputHook();
void CleanupWeaponInputHook();

// Y key code (confirmed working in tests)
#define VK_Y 0x19

// Addresses (from Ghidra analysis @ port 8192)
// Binary: echovr.exe v34.4.631547-final
#define ADDR_InputState 0x0f9b330              // RVA: 0x140f9b330 - 0x140000000
#define ADDR_WeaponFireStateMachine 0x10b3000  // RVA: 0x1410b3000 - 0x140000000

// VR Controller Input Hook (added for weapon fire via trigger injection)
#define ADDR_MotionControllerInputState 0x0f9b2f0  // CMotionControllerInputDevice::InputState
#define TRIGGER_INDEX \
  1  // Right-hand trigger (empirical test candidate - index 1 is common VR trigger)
     // NOTE: This is INITIAL GUESS. Will be updated after empirical testing confirms correct index.

#pragma once

#include <cstdint>
#include <windows.h>

// =============================================================================
// Weapon System Trace - Comprehensive Function Hooks
// =============================================================================
// This module hooks ALL weapon-related functions to create complete visibility
// into weapon fire system behavior.
//
// OBJECTIVE:
//   Hook every weapon function to understand the complete call sequence
//   from trigger press to bullet spawn.
//
// PRIORITY 1 - CORE FIRE SYSTEM:
//   1. Weapon_Fire_StateMachine   - Main state machine (0x10b3000)
//   2. SpawnBulletFromPool        - Bullet pooling system (0x0cd5a10)
//   3. FUN_1400d45a0              - Fire setup/initialization (0x00d45a0)
//   4. FUN_140532220              - Event initialization (0x0532220)
//
// PRIORITY 2 - BULLET SYSTEM:
//   5. SetBulletVisualProperties  - Visual/effect setup (0x0ce2020)
//   6. InitBulletCI               - Bullet collision init (0x0f991e0)
//   7. DeferredHandleBulletCollision - Collision handling (0x15bc8b0)
//
// PRIORITY 3 - REGISTRATION:
//   8. FUN_1400a3bb0              - FireGun registration (0x00a3bb0)
//
// LOG FORMAT:
//   [%06ums] FUNCTION_NAME: param1=value param2=value
//   [%06ums] FUNCTION_NAME: returned 0xvalue
//
// USAGE:
//   1. Call InitWeaponSystemTrace() during DLL init
//   2. Fire weapon in-game
//   3. Check weapon_system_trace.log for complete call sequence
//   4. Call CleanupWeaponSystemTrace() during DLL shutdown
// =============================================================================

// =============================================================================
// Hook Addresses (RVAs from Ghidra analysis)
// =============================================================================

// Priority 1 - Core Fire System
#define ADDR_WeaponFireSM          0x10b3000  // Weapon_Fire_StateMachine
#define ADDR_SpawnBulletFromPool   0x0cd5a10  // SpawnBulletFromPool
#define ADDR_FireSetup             0x00d45a0  // FUN_1400d45a0 (Fire setup)
#define ADDR_EventInit             0x0532220  // FUN_140532220 (Event init)

// Priority 2 - Bullet System
#define ADDR_SetBulletVisuals      0x0ce2020  // SetBulletVisualProperties
#define ADDR_InitBulletCI          0x0f991e0  // InitBulletCI
#define ADDR_HandleBulletCollision 0x15bc8b0  // DeferredHandleBulletCollision

// Priority 3 - Registration
#define ADDR_FireGunRegistration   0x00a3bb0  // FUN_1400a3bb0 (FireGun registration)

// =============================================================================
// Function Type Definitions
// =============================================================================
// These signatures are based on Ghidra analysis and may need adjustment
// after runtime observation.

// 1. Weapon_Fire_StateMachine
// Signature: uint64_t Weapon_Fire_StateMachine(int64_t* context)
typedef uint64_t (*WeaponFireSM_t)(int64_t* context);

// 2. SpawnBulletFromPool
// Signature: void* SpawnBulletFromPool(void* pool, void* params)
typedef void* (*SpawnBulletFromPool_t)(void* pool, void* params);

// 3. FUN_1400d45a0 (Fire Setup)
// Signature: uint64_t FUN_1400d45a0(void* a1, void* a2)
typedef uint64_t (*FireSetup_t)(void* a1, void* a2);

// 4. FUN_140532220 (Event Init)
// Signature: void FUN_140532220(void* event)
typedef void (*EventInit_t)(void* event);

// 5. SetBulletVisualProperties
// Signature: void SetBulletVisualProperties(void* bullet, void* properties)
typedef void (*SetBulletVisuals_t)(void* bullet, void* properties);

// 6. InitBulletCI
// Signature: void InitBulletCI(void* bulletCI)
typedef void (*InitBulletCI_t)(void* bulletCI);

// 7. DeferredHandleBulletCollision
// Signature: void DeferredHandleBulletCollision(void* collision)
typedef void (*HandleBulletCollision_t)(void* collision);

// 8. FUN_1400a3bb0 (FireGun Registration)
// Signature: void FUN_1400a3bb0(void* registration)
typedef void (*FireGunReg_t)(void* registration);

// =============================================================================
// Public API
// =============================================================================

/// <summary>
/// Initializes weapon system trace hooks.
/// Opens log file and installs all 8 weapon function hooks.
/// Returns TRUE on success, FALSE on failure.
/// </summary>
BOOL InitWeaponSystemTrace();

/// <summary>
/// Cleans up weapon system trace hooks.
/// Removes all hooks and closes log file.
/// </summary>
VOID CleanupWeaponSystemTrace();

// =============================================================================
// Global State (declared in .cpp)
// =============================================================================

extern FILE* g_weaponTraceLog;
extern DWORD g_weaponTraceStartTick;
extern BOOL g_weaponTraceInitialized;

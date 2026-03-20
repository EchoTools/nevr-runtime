#pragma once

#include <cstdint>

#include "pch.h"

// =============================================================================
// Gun2CR Visual Effects Fix - Runtime Hook
// =============================================================================
// This implements the hook from:
//   - /home/andrew/src/evr-reconstruction/docs/hooks/gun2cr_visual_fix_hook.md
//
// **Problem:** Gun2CR bullets have no trails, muzzle flashes, or impact
//              particles due to zero-valued visual parameters in asset files.
//
// **Solution:** Hook InitBulletCI to detect Gun2CR bullets and override zero
//              values with working values from GunCR weapons.
//
// **Impact:** Enables Gun2CR visual effects without modifying game assets.
// =============================================================================

// =============================================================================
// Component Type Hashes
// =============================================================================

// Known bullet component type hashes (CSymbol64 lower 32 bits)
#define COMPONENT_CR15NetBulletCR 0x754402a4   // GunCR bullet (working)
#define COMPONENT_CR15NetBullet2CR 0x1e5be8ae  // Gun2CR bullet (broken)

// Full 64-bit hashes for reference
#define COMPONENT_CR15NetBulletCR_FULL 0x16bafee3754402a4ULL
#define COMPONENT_CR15NetBullet2CR_FULL 0xf4ba53ab1e5be8aeULL

// =============================================================================
// Visual Effect Flags
// =============================================================================

// Bitfield flags controlling visual rendering behavior
#define FLAG_DISABLE_COMPONENT_ON_INIT 0x0001     // Disable component after init
#define FLAG_EDITOR_ONLY 0x0002                   // Editor-only component
#define FLAG_PIE_COMPLIANT 0x0004                 // Play-in-editor compatible
#define FLAG_NEEDS_ACTOR_DATA 0x0008              // Requires actor data injection
#define FLAG_USE_WHIZZBY 0x0010                   // Enable whizz-by sounds
#define FLAG_DISABLE_TEAMMATE_WHIZZBY 0x0020      // Mute friendly fire sounds
#define FLAG_USE_IMPACT 0x0040                    // Enable impact particles
#define FLAG_EXPLODE_ON_COLLISION 0x0080          // Spawn explosions on hit
#define FLAG_EXPLODE_ON_DAMAGEABLE 0x0100         // Explosions only on players/objects
#define FLAG_EXPLOSION_SCALES_WITH_DAMAGE 0x0200  // Scale explosion size with damage
#define FLAG_TRAIL_SCALES_WITH_DAMAGE 0x0400      // Scale trail with damage
#define FLAG_UPDATE_TRAIL_LENGTH 0x0800           // Dynamic trail length
#define FLAG_USE_TEAM_PARTICLES 0x1000            // Team-colored particles
#define FLAG_KILL_TRAIL_ON_COLLISION 0x2000       // Stop trail on impact

// Recommended Gun2CR flags (if currently zero)
#define GUN2CR_REQUIRED_FLAGS \
  (FLAG_USE_IMPACT | FLAG_TRAIL_SCALES_WITH_DAMAGE | FLAG_UPDATE_TRAIL_LENGTH | FLAG_KILL_TRAIL_ON_COLLISION)

// =============================================================================
// Structure Definitions
// =============================================================================

// Component ID structure (8 bytes)
struct SComponentID {
  uint32_t index;      // Component instance index in pool
  uint32_t type_hash;  // CSymbol64 lower 32 bits of component type
};

// Compact pool handle (opaque)
struct SCompactPoolHandle {
  uint32_t handle;
};

// Forward declaration of component system classes
struct CR15NetBullet2CS;
struct SR15NetBullet2CI;

// SR15NetBullet2CD::SProperties - Complete structure layout
// Total size: 0x88 bytes (136 bytes)
struct SR15NetBullet2CD_SProperties {
  // Base Component Header (inherited from CComponentData)
  uint8_t base_header[0x18];  // +0x00 to +0x17

  // Visual & Gameplay Flags
  uint32_t flags;       // +0x18 - Bitfield (see flags above)
  uint32_t padding_1c;  // +0x1C - Alignment padding

  // Physics & Behavior Parameters
  float distance;              // +0x20 - Max bullet travel distance (meters)
  float lifetime;              // +0x24 - Bullet lifetime (seconds)
  float whizzbyradius;         // +0x28 - Whizz-by sound trigger radius
  float explosionoffsetscale;  // +0x2C - Explosion spawn offset multiplier

  // Visual Scaling Parameters
  float decalscale;        // +0x30 - Impact decal size multiplier
  float decalfadetime;     // +0x34 - Decal fade duration (seconds)
  float trailduration;     // +0x38 - **CRITICAL** Trail lifetime (seconds)
  float damagescalestart;  // +0x3C - Damage-based visual scaling (min)
  float damagescaleend;    // +0x40 - Damage-based visual scaling (max)
  uint32_t padding_44;     // +0x44 - Alignment padding

  // Ricochet Count
  int32_t ricochets;    // +0x48 - Number of bounces allowed
  uint32_t padding_4c;  // +0x4C - Alignment padding

  // Particle Effect Resource IDs (CSymbol64 hashes)
  uint64_t whizzbystartkey;  // +0x50 - Whizz-by sound effect ID
  uint64_t collisionpfx;     // +0x58 - **CRITICAL** Impact particle effect
  uint64_t trailpfx;         // +0x60 - **CRITICAL** Primary bullet trail
  uint64_t explosionpfx;     // +0x68 - Explosion particle effect
  uint64_t collisionpfx_b;   // +0x70 - Secondary impact particles
  uint64_t trailpfx_b;       // +0x78 - **CRITICAL** Secondary trail overlay
  uint64_t explosionpfx_b;   // +0x80 - Secondary explosion particles

  // Total size: 0x88 bytes (136 bytes)
};

// GunCR Reference Values (loaded from config)
struct GunCRReferenceValues {
  float trailduration;
  uint64_t trailpfx;
  uint64_t trailpfx_b;
  uint64_t collisionpfx;
  uint64_t collisionpfx_b;
  uint32_t flags;
  bool enabled;  // Master enable/disable flag
};

// =============================================================================
// Function Type Definitions
// =============================================================================

// InitBulletCI function signature
// Address: 0x140f991e0 (RVA in echovr.exe)
// Calling Convention: Microsoft x64 (fastcall)
typedef int(__fastcall* pInitBulletCI)(CR15NetBullet2CS* this_ptr,                 // RCX
                                       SR15NetBullet2CI* bullet_instance,          // RDX
                                       const SR15NetBullet2CD_SProperties* props,  // R8
                                       SComponentID component_id,                  // R9
                                       SCompactPoolHandle pool_handle,             // Stack+0x28
                                       uint64_t flags,                             // Stack+0x30
                                       void* user_data);                           // Stack+0x38

// =============================================================================
// Hook Addresses (from Ghidra analysis)
// =============================================================================
// NOTE: These addresses are for Echo VR version 34.4.631547.1

// CR15NetBullet2CS::InitBulletCI
#define ADDR_InitBulletCI 0xF991E0  // RVA: 0x140f991e0

// =============================================================================
// Public API
// =============================================================================

/// <summary>
/// Initializes the Gun2CR visual fix hook.
/// Loads config and installs the InitBulletCI hook.
/// </summary>
VOID InitializeGun2CRHook();

/// <summary>
/// Shuts down the Gun2CR hook and flushes log file.
/// </summary>
VOID ShutdownGun2CRHook();

/// <summary>
/// Loads GunCR reference values from gun2cr_config.ini
/// </summary>
/// <param name="configPath">Path to the config file</param>
/// <returns>TRUE if loaded successfully, FALSE otherwise</returns>
BOOL LoadGun2CRConfig(const char* configPath);

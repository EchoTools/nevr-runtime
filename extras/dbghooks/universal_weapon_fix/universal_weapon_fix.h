#pragma once

#include <cstdint>

// Forward declaration of the weapon properties struct.
// The actual definition is unknown, but we know the fields we need to patch.
struct WeaponProperties {
    // Inferred from the bug description.
    // There are likely many other fields before and after these.
    char unknown_data[0x100]; // Placeholder for unknown data at the start.
    float trailduration;
    uint64_t collisionpfx;
    uint64_t trailpfx;
    // ... other fields
};

// Holds the known-good reference values for weapon VFX.
struct WeaponReferenceValues {
    float trailduration;
    uint64_t collisionpfx;
    uint64_t trailpfx;
};

// Function signature for the target function FUN_140784bb0
// It is assumed to take a pointer to the properties struct and a buffer.
using pDeserializeWeaponProperties = void (*)(WeaponProperties* properties, void* buffer);

// Public API for the universal weapon fix.
void InitializeUniversalWeaponFix();
void ShutdownUniversalWeaponFix();

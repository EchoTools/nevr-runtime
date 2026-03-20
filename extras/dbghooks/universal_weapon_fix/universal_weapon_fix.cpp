#include "universal_weapon_fix.h"
#include <windows.h>
#include <MinHook.h>
#include <iostream>

// Target function address
#define TARGET_FUNCTION_ADDRESS 0x140784bb0

// Original function pointer
pDeserializeWeaponProperties oDeserializeWeaponProperties = nullptr;

// Known-good VFX values for broken weapons.
static const WeaponReferenceValues g_reference_values = {
    1.0f,    // trailduration
    0x12345, // collisionpfx (placeholder)
    0x6789A  // trailpfx (placeholder)
};

// Hooked function to patch weapon properties.
void hkDeserializeWeaponProperties(WeaponProperties* properties, void* buffer) {
    // Call the original function first.
    oDeserializeWeaponProperties(properties, buffer);

    // Check if the weapon is broken (zeroed VFX properties).
    if (properties->trailduration == 0.0f && properties->collisionpfx == 0) {
        std::cout << "Patching broken weapon properties..." << std::endl;

        // Copy the known-good values.
        properties->trailduration = g_reference_values.trailduration;
        properties->collisionpfx = g_reference_values.collisionpfx;
        properties->trailpfx = g_reference_values.trailpfx;
    }
}

// Initializes and enables the hook.
void InitializeUniversalWeaponFix() {
    if (MH_Initialize() != MH_OK) {
        std::cerr << "Failed to initialize MinHook." << std::endl;
        return;
    }

    if (MH_CreateHook((LPVOID)TARGET_FUNCTION_ADDRESS, &hkDeserializeWeaponProperties,
                      reinterpret_cast<LPVOID*>(&oDeserializeWeaponProperties)) != MH_OK) {
        std::cerr << "Failed to create hook for FUN_140784bb0." << std::endl;
        return;
    }

    if (MH_EnableHook((LPVOID)TARGET_FUNCTION_ADDRESS) != MH_OK) {
        std::cerr << "Failed to enable hook for FUN_140784bb0." << std::endl;
        return;
    }

    std::cout << "Universal weapon fix hook initialized." << std::endl;
}

// Disables and removes the hook.
void ShutdownUniversalWeaponFix() {
    if (MH_DisableHook((LPVOID)TARGET_FUNCTION_ADDRESS) != MH_OK) {
        std::cerr << "Failed to disable hook for FUN_140784bb0." << std::endl;
    }

    if (MH_RemoveHook((LPVOID)TARGET_FUNCTION_ADDRESS) != MH_OK) {
        std::cerr << "Failed to remove hook for FUN_140784bb0." << std::endl;
    }

    if (MH_Uninitialize() != MH_OK) {
        std::cerr << "Failed to uninitialize MinHook." << std::endl;
    }

    std::cout << "Universal weapon fix hook shut down." << std::endl;
}

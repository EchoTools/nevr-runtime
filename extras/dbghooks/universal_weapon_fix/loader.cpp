#include "loader.h"
#include "universal_weapon_fix.h"

namespace dbghooks::universal_weapon_fix {
    void Load() {
        InitializeUniversalWeaponFix();
    }

    void Unload() {
        ShutdownUniversalWeaponFix();
    }
}

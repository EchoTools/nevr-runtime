#pragma once
#include <Windows.h>

namespace dbghooks::gun2cr_fix {
  // Load and install Gun2CR visual effects patch
  bool LoadGun2CRFix();
  
  // Uninstall and unload patch
  bool UnloadGun2CRFix();
  
  // Get status
  bool IsGun2CRFixActive();
}

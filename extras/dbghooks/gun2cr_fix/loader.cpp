#include "loader.h"
#include <cstdio>

namespace dbghooks::gun2cr_fix {
  static bool g_is_active = false;
  
  // External functions declared in gun2cr_hook.cpp
  extern void InitializeGun2CRHook();
  extern void ShutdownGun2CRHook();
  
  bool LoadGun2CRFix() {
    if (g_is_active) return true; // Already loaded
    
    try {
      InitializeGun2CRHook();
      g_is_active = true;
      printf("[Gun2CR] Patch installed successfully\n");
      return true;
    } catch (...) {
      printf("[Gun2CR] Failed to install patch\n");
      return false;
    }
  }
  
  bool UnloadGun2CRFix() {
    if (!g_is_active) return true; // Already unloaded
    
    try {
      ShutdownGun2CRHook();
      g_is_active = false;
      printf("[Gun2CR] Patch uninstalled\n");
      return true;
    } catch (...) {
      printf("[Gun2CR] Failed to uninstall patch\n");
      return false;
    }
  }
  
  bool IsGun2CRFixActive() {
    return g_is_active;
  }
}

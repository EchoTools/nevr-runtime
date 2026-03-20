#include "static_entry.h"

#include <cstdio>

#include "gamestart_hooks.h"
#include "gun2cr_fix/weapon_enabler.h"
#include "gun2cr_hook.h"
#include "hash_hooks.h"
#include "mesh_dump_hooks.h"
#include "pch.h"
#include "weapon_input_hook.h"

using namespace dbghooks::weapon_system;

namespace dbghooks {

void InitializeStatic(HMODULE hModule) {
  // Print version info before anything else
  printf("[DbgHooks] Version: %s\n", GIT_DESCRIBE);

  // Write diagnostic file IMMEDIATELY to prove DllMain is called
  FILE* diag = fopen("dbghooks_static_trace.txt", "w");
  if (diag) {
    fprintf(diag, "[DbgHooks] Version: %s\n", GIT_DESCRIBE);
    fprintf(diag, "InitializeStatic called with hModule=%p\n", hModule);
    fflush(diag);
  }

  // Try to install hooks immediately (for early injection)

  // DISABLED: Hash hooks require game base address (set after game DLL loads)
  // try {
  //   if (diag) fprintf(diag, "Calling InitializeHashHooks\n"), fflush(diag);
  //   InitializeHashHooks();
  //   if (diag) fprintf(diag, "InitializeHashHooks returned OK\n"), fflush(diag);
  // } catch (...) {
  //   if (diag) fprintf(diag, "EXCEPTION in InitializeHashHooks\n"), fflush(diag);
  // }

  // DISABLED: Gun2CR accesses g_GameBaseAddress which is not valid until game DLL initializes
  // The symbol exists but points to uninitialized memory, causing access violations
  // Will be initialized via callback after game startup
  // try {
  //   if (diag) fprintf(diag, "Calling InitializeGun2CRHook\n"), fflush(diag);
  //   InitializeGun2CRHook();
  //   if (diag) fprintf(diag, "InitializeGun2CRHook returned OK\n"), fflush(diag);
  // } catch (...) {
  //   if (diag) fprintf(diag, "EXCEPTION in InitializeGun2CRHook\n"), fflush(diag);
  // }

  // Skip GameStartHooks for now due to potential crashes
  // try {
  //   if (diag) fprintf(diag, "Calling InitializeGameStartHooks\n"), fflush(diag);
  //   InitializeGameStartHooks();
  //   if (diag) fprintf(diag, "InitializeGameStartHooks returned OK\n"), fflush(diag);
  // } catch (...) {
  //   if (diag) fprintf(diag, "EXCEPTION in InitializeGameStartHooks\n"), fflush(diag);
  // }

  try {
    if (diag) fprintf(diag, "Calling InitWeaponInputHook\n"), fflush(diag);
    InitWeaponInputHook();
    if (diag) fprintf(diag, "InitWeaponInputHook returned OK\n"), fflush(diag);
  } catch (...) {
    if (diag) fprintf(diag, "EXCEPTION in InitWeaponInputHook\n"), fflush(diag);
  }

  try {
    if (diag) fprintf(diag, "Calling InitializeMeshDumpHooks\n"), fflush(diag);
    InitializeMeshDumpHooks();
    if (diag) fprintf(diag, "InitializeMeshDumpHooks returned OK\n"), fflush(diag);
  } catch (...) {
    if (diag) fprintf(diag, "EXCEPTION in InitializeMeshDumpHooks\n"), fflush(diag);
  }

  try {
    if (diag) fprintf(diag, "Calling WeaponSystemHook::Initialize\n"), fflush(diag);
    if (WeaponSystemHook::Initialize()) {
      if (diag) fprintf(diag, "WeaponSystemHook::Initialize returned OK\n"), fflush(diag);
      printf("[DbgHooks] Weapon system hook initialized successfully\n");
    } else {
      if (diag) fprintf(diag, "WeaponSystemHook::Initialize returned FALSE\n"), fflush(diag);
      printf("[DbgHooks] WARNING: Failed to initialize weapon system hook\n");
    }
  } catch (...) {
    if (diag) fprintf(diag, "EXCEPTION in WeaponSystemHook::Initialize\n"), fflush(diag);
    printf("[DbgHooks] Exception during weapon system initialization\n");
  }

  if (diag) {
    fprintf(diag, "InitializeStatic complete\n");
    fclose(diag);
  }
}

void ShutdownStatic() {
  // Shutdown weapon input hook
  try {
    CleanupWeaponInputHook();
    printf("[DbgHooks] Weapon input hook shut down\n");
  } catch (...) {
    printf("[DbgHooks] Exception during weapon input hook shutdown\n");
  }

  // Shutdown weapon system first
  try {
    WeaponSystemHook::Shutdown();
    printf("[DbgHooks] Weapon system shut down\n");
  } catch (...) {
    printf("[DbgHooks] Exception during weapon system shutdown\n");
  }

  // Shutdown and flush logs
  ShutdownMeshDumpHooks();
  ShutdownGameStartHooks();
  ShutdownGun2CRHook();
  ShutdownHashHooks();
}

}  // namespace dbghooks

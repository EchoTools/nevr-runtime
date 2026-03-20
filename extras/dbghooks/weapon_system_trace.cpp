#include "weapon_system_trace.h"

#include <cstdio>
#include <windows.h>

#include "common/hooking.h"
#include "common/logging.h"
#include "echovrInternal.h"

FILE* g_weaponTraceLog = nullptr;
DWORD g_weaponTraceStartTick = 0;
BOOL g_weaponTraceInitialized = FALSE;

static WeaponFireSM_t orig_WeaponFireSM = nullptr;
static SpawnBulletFromPool_t orig_SpawnBulletFromPool = nullptr;
static FireSetup_t orig_FireSetup = nullptr;
static EventInit_t orig_EventInit = nullptr;
static SetBulletVisuals_t orig_SetBulletVisuals = nullptr;
static InitBulletCI_t orig_InitBulletCI = nullptr;
static HandleBulletCollision_t orig_HandleBulletCollision = nullptr;
static FireGunReg_t orig_FireGunReg = nullptr;

uint64_t hook_WeaponFireSM(int64_t* context) {
    DWORD elapsed = GetTickCount() - g_weaponTraceStartTick;
    int64_t state = context ? context[3] : -1;
    
    fprintf(g_weaponTraceLog, "[%06ums] FIRE_SM: ctx=0x%llx state=%lld\n", 
            elapsed, (uint64_t)context, state);
    fflush(g_weaponTraceLog);
    
    uint64_t result = orig_WeaponFireSM(context);
    
    fprintf(g_weaponTraceLog, "[%06ums] FIRE_SM: returned 0x%llx\n", 
            elapsed, result);
    fflush(g_weaponTraceLog);
    
    return result;
}

void* hook_SpawnBulletFromPool(void* pool, void* params) {
    DWORD elapsed = GetTickCount() - g_weaponTraceStartTick;
    
    fprintf(g_weaponTraceLog, "[%06ums] SPAWN_BULLET: pool=0x%llx params=0x%llx\n", 
            elapsed, (uint64_t)pool, (uint64_t)params);
    fflush(g_weaponTraceLog);
    
    void* result = orig_SpawnBulletFromPool(pool, params);
    
    fprintf(g_weaponTraceLog, "[%06ums] SPAWN_BULLET: returned 0x%llx\n", 
            elapsed, (uint64_t)result);
    fflush(g_weaponTraceLog);
    
    return result;
}

uint64_t hook_FireSetup(void* a1, void* a2) {
    DWORD elapsed = GetTickCount() - g_weaponTraceStartTick;
    
    fprintf(g_weaponTraceLog, "[%06ums] FIRE_SETUP: a1=0x%llx a2=0x%llx\n", 
            elapsed, (uint64_t)a1, (uint64_t)a2);
    fflush(g_weaponTraceLog);
    
    uint64_t result = orig_FireSetup(a1, a2);
    
    fprintf(g_weaponTraceLog, "[%06ums] FIRE_SETUP: returned 0x%llx\n", 
            elapsed, result);
    fflush(g_weaponTraceLog);
    
    return result;
}

void hook_EventInit(void* event) {
    DWORD elapsed = GetTickCount() - g_weaponTraceStartTick;
    
    fprintf(g_weaponTraceLog, "[%06ums] EVENT_INIT: event=0x%llx\n", 
            elapsed, (uint64_t)event);
    fflush(g_weaponTraceLog);
    
    orig_EventInit(event);
    
    fprintf(g_weaponTraceLog, "[%06ums] EVENT_INIT: returned\n", elapsed);
    fflush(g_weaponTraceLog);
}

void hook_SetBulletVisuals(void* bullet, void* properties) {
    DWORD elapsed = GetTickCount() - g_weaponTraceStartTick;
    
    fprintf(g_weaponTraceLog, "[%06ums] SET_BULLET_VIS: bullet=0x%llx props=0x%llx\n", 
            elapsed, (uint64_t)bullet, (uint64_t)properties);
    fflush(g_weaponTraceLog);
    
    orig_SetBulletVisuals(bullet, properties);
    
    fprintf(g_weaponTraceLog, "[%06ums] SET_BULLET_VIS: returned\n", elapsed);
    fflush(g_weaponTraceLog);
}

void hook_InitBulletCI(void* bulletCI) {
    DWORD elapsed = GetTickCount() - g_weaponTraceStartTick;
    
    fprintf(g_weaponTraceLog, "[%06ums] INIT_BULLET_CI: bulletCI=0x%llx\n", 
            elapsed, (uint64_t)bulletCI);
    fflush(g_weaponTraceLog);
    
    orig_InitBulletCI(bulletCI);
    
    fprintf(g_weaponTraceLog, "[%06ums] INIT_BULLET_CI: returned\n", elapsed);
    fflush(g_weaponTraceLog);
}

void hook_HandleBulletCollision(void* collision) {
    DWORD elapsed = GetTickCount() - g_weaponTraceStartTick;
    
    fprintf(g_weaponTraceLog, "[%06ums] HANDLE_COLLISION: collision=0x%llx\n", 
            elapsed, (uint64_t)collision);
    fflush(g_weaponTraceLog);
    
    orig_HandleBulletCollision(collision);
    
    fprintf(g_weaponTraceLog, "[%06ums] HANDLE_COLLISION: returned\n", elapsed);
    fflush(g_weaponTraceLog);
}

void hook_FireGunReg(void* registration) {
    DWORD elapsed = GetTickCount() - g_weaponTraceStartTick;
    
    fprintf(g_weaponTraceLog, "[%06ums] FIREGUN_REG: registration=0x%llx\n", 
            elapsed, (uint64_t)registration);
    fflush(g_weaponTraceLog);
    
    orig_FireGunReg(registration);
    
    fprintf(g_weaponTraceLog, "[%06ums] FIREGUN_REG: returned\n", elapsed);
    fflush(g_weaponTraceLog);
}

BOOL InitWeaponSystemTrace() {
    if (g_weaponTraceInitialized) {
        Log(EchoVR::LogLevel::Warning, "[WeaponTrace] Already initialized");
        return FALSE;
    }

    Log(EchoVR::LogLevel::Info, "[WeaponTrace] Initializing weapon system trace...");

    g_weaponTraceStartTick = GetTickCount();
    
    errno_t err = fopen_s(&g_weaponTraceLog, "weapon_system_trace.log", "w");
    if (err != 0 || !g_weaponTraceLog) {
        Log(EchoVR::LogLevel::Error, "[WeaponTrace] Failed to open log file");
        return FALSE;
    }

    fprintf(g_weaponTraceLog, "=== Weapon System Trace Started ===\n");
    fprintf(g_weaponTraceLog, "Timestamp: %u ms\n", g_weaponTraceStartTick);
    fprintf(g_weaponTraceLog, "\nTarget Functions:\n");
    fprintf(g_weaponTraceLog, "  1. Weapon_Fire_StateMachine   @ RVA 0x%08X\n", ADDR_WeaponFireSM);
    fprintf(g_weaponTraceLog, "  2. SpawnBulletFromPool        @ RVA 0x%08X\n", ADDR_SpawnBulletFromPool);
    fprintf(g_weaponTraceLog, "  3. FUN_1400d45a0 (Fire Setup) @ RVA 0x%08X\n", ADDR_FireSetup);
    fprintf(g_weaponTraceLog, "  4. FUN_140532220 (Event Init) @ RVA 0x%08X\n", ADDR_EventInit);
    fprintf(g_weaponTraceLog, "  5. SetBulletVisualProperties  @ RVA 0x%08X\n", ADDR_SetBulletVisuals);
    fprintf(g_weaponTraceLog, "  6. InitBulletCI               @ RVA 0x%08X\n", ADDR_InitBulletCI);
    fprintf(g_weaponTraceLog, "  7. DeferredHandleBulletCollision @ RVA 0x%08X\n", ADDR_HandleBulletCollision);
    fprintf(g_weaponTraceLog, "  8. FUN_1400a3bb0 (FireGun Reg) @ RVA 0x%08X\n", ADDR_FireGunRegistration);
    fprintf(g_weaponTraceLog, "\n");
    fflush(g_weaponTraceLog);

    try {
        volatile PVOID testPtr = EchoVR::g_GameBaseAddress;
        if (testPtr == nullptr || (uintptr_t)testPtr < 0x1000) {
            fprintf(g_weaponTraceLog, "✗ Base address not ready\n");
            Log(EchoVR::LogLevel::Error, "[WeaponTrace] Base address not ready");
            fclose(g_weaponTraceLog);
            g_weaponTraceLog = nullptr;
            return FALSE;
        }
    } catch (...) {
        fprintf(g_weaponTraceLog, "✗ Exception checking base address\n");
        fclose(g_weaponTraceLog);
        g_weaponTraceLog = nullptr;
        return FALSE;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        fprintf(g_weaponTraceLog, "✗ MH_Initialize failed: %d\n", status);
        Log(EchoVR::LogLevel::Error, "[WeaponTrace] MH_Initialize failed: %d", status);
        fclose(g_weaponTraceLog);
        g_weaponTraceLog = nullptr;
        return FALSE;
    }

    HMODULE hGame = GetModuleHandleA(NULL);
    uintptr_t base = (uintptr_t)hGame;

    int successCount = 0;
    int failCount = 0;

    status = MH_CreateHook((LPVOID)(base + ADDR_WeaponFireSM), 
                           (LPVOID)hook_WeaponFireSM, 
                           (LPVOID*)&orig_WeaponFireSM);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)(base + ADDR_WeaponFireSM));
        if (status == MH_OK) {
            fprintf(g_weaponTraceLog, "✓ Hooked Weapon_Fire_StateMachine @ 0x%llx\n", 
                    base + ADDR_WeaponFireSM);
            successCount++;
        } else {
            fprintf(g_weaponTraceLog, "✗ Failed to enable Weapon_Fire_StateMachine: %d\n", status);
            failCount++;
        }
    } else {
        fprintf(g_weaponTraceLog, "✗ Failed to hook Weapon_Fire_StateMachine: %d\n", status);
        failCount++;
    }

    status = MH_CreateHook((LPVOID)(base + ADDR_SpawnBulletFromPool), 
                           (LPVOID)hook_SpawnBulletFromPool, 
                           (LPVOID*)&orig_SpawnBulletFromPool);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)(base + ADDR_SpawnBulletFromPool));
        if (status == MH_OK) {
            fprintf(g_weaponTraceLog, "✓ Hooked SpawnBulletFromPool @ 0x%llx\n", 
                    base + ADDR_SpawnBulletFromPool);
            successCount++;
        } else {
            fprintf(g_weaponTraceLog, "✗ Failed to enable SpawnBulletFromPool: %d\n", status);
            failCount++;
        }
    } else {
        fprintf(g_weaponTraceLog, "✗ Failed to hook SpawnBulletFromPool: %d\n", status);
        failCount++;
    }

    status = MH_CreateHook((LPVOID)(base + ADDR_FireSetup), 
                           (LPVOID)hook_FireSetup, 
                           (LPVOID*)&orig_FireSetup);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)(base + ADDR_FireSetup));
        if (status == MH_OK) {
            fprintf(g_weaponTraceLog, "✓ Hooked FUN_1400d45a0 (Fire Setup) @ 0x%llx\n", 
                    base + ADDR_FireSetup);
            successCount++;
        } else {
            fprintf(g_weaponTraceLog, "✗ Failed to enable Fire Setup: %d\n", status);
            failCount++;
        }
    } else {
        fprintf(g_weaponTraceLog, "✗ Failed to hook Fire Setup: %d\n", status);
        failCount++;
    }

    status = MH_CreateHook((LPVOID)(base + ADDR_EventInit), 
                           (LPVOID)hook_EventInit, 
                           (LPVOID*)&orig_EventInit);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)(base + ADDR_EventInit));
        if (status == MH_OK) {
            fprintf(g_weaponTraceLog, "✓ Hooked FUN_140532220 (Event Init) @ 0x%llx\n", 
                    base + ADDR_EventInit);
            successCount++;
        } else {
            fprintf(g_weaponTraceLog, "✗ Failed to enable Event Init: %d\n", status);
            failCount++;
        }
    } else {
        fprintf(g_weaponTraceLog, "✗ Failed to hook Event Init: %d\n", status);
        failCount++;
    }

    status = MH_CreateHook((LPVOID)(base + ADDR_SetBulletVisuals), 
                           (LPVOID)hook_SetBulletVisuals, 
                           (LPVOID*)&orig_SetBulletVisuals);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)(base + ADDR_SetBulletVisuals));
        if (status == MH_OK) {
            fprintf(g_weaponTraceLog, "✓ Hooked SetBulletVisualProperties @ 0x%llx\n", 
                    base + ADDR_SetBulletVisuals);
            successCount++;
        } else {
            fprintf(g_weaponTraceLog, "✗ Failed to enable SetBulletVisualProperties: %d\n", status);
            failCount++;
        }
    } else {
        fprintf(g_weaponTraceLog, "✗ Failed to hook SetBulletVisualProperties: %d\n", status);
        failCount++;
    }

    status = MH_CreateHook((LPVOID)(base + ADDR_InitBulletCI), 
                           (LPVOID)hook_InitBulletCI, 
                           (LPVOID*)&orig_InitBulletCI);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)(base + ADDR_InitBulletCI));
        if (status == MH_OK) {
            fprintf(g_weaponTraceLog, "✓ Hooked InitBulletCI @ 0x%llx\n", 
                    base + ADDR_InitBulletCI);
            successCount++;
        } else {
            fprintf(g_weaponTraceLog, "✗ Failed to enable InitBulletCI: %d\n", status);
            failCount++;
        }
    } else {
        fprintf(g_weaponTraceLog, "✗ Failed to hook InitBulletCI: %d\n", status);
        failCount++;
    }

    status = MH_CreateHook((LPVOID)(base + ADDR_HandleBulletCollision), 
                           (LPVOID)hook_HandleBulletCollision, 
                           (LPVOID*)&orig_HandleBulletCollision);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)(base + ADDR_HandleBulletCollision));
        if (status == MH_OK) {
            fprintf(g_weaponTraceLog, "✓ Hooked DeferredHandleBulletCollision @ 0x%llx\n", 
                    base + ADDR_HandleBulletCollision);
            successCount++;
        } else {
            fprintf(g_weaponTraceLog, "✗ Failed to enable DeferredHandleBulletCollision: %d\n", status);
            failCount++;
        }
    } else {
        fprintf(g_weaponTraceLog, "✗ Failed to hook DeferredHandleBulletCollision: %d\n", status);
        failCount++;
    }

    status = MH_CreateHook((LPVOID)(base + ADDR_FireGunRegistration), 
                           (LPVOID)hook_FireGunReg, 
                           (LPVOID*)&orig_FireGunReg);
    if (status == MH_OK) {
        status = MH_EnableHook((LPVOID)(base + ADDR_FireGunRegistration));
        if (status == MH_OK) {
            fprintf(g_weaponTraceLog, "✓ Hooked FUN_1400a3bb0 (FireGun Reg) @ 0x%llx\n", 
                    base + ADDR_FireGunRegistration);
            successCount++;
        } else {
            fprintf(g_weaponTraceLog, "✗ Failed to enable FireGun Registration: %d\n", status);
            failCount++;
        }
    } else {
        fprintf(g_weaponTraceLog, "✗ Failed to hook FireGun Registration: %d\n", status);
        failCount++;
    }

    fprintf(g_weaponTraceLog, "\n=== Hook Installation Summary ===\n");
    fprintf(g_weaponTraceLog, "Success: %d / 8\n", successCount);
    fprintf(g_weaponTraceLog, "Failed:  %d / 8\n", failCount);
    fprintf(g_weaponTraceLog, "\n=== Monitoring Started ===\n");
    fprintf(g_weaponTraceLog, "Press trigger/fire button in-game to see weapon system calls\n\n");
    fflush(g_weaponTraceLog);

    g_weaponTraceInitialized = TRUE;
    
    Log(EchoVR::LogLevel::Info, "[WeaponTrace] ✓ Initialized: %d/8 hooks installed", successCount);
    
    if (failCount > 0) {
        Log(EchoVR::LogLevel::Warning, "[WeaponTrace] %d/8 hooks failed to install", failCount);
    }

    return TRUE;
}

VOID CleanupWeaponSystemTrace() {
    if (!g_weaponTraceInitialized) {
        return;
    }

    Log(EchoVR::LogLevel::Info, "[WeaponTrace] Cleaning up...");

    HMODULE hGame = GetModuleHandleA(NULL);
    if (hGame) {
        uintptr_t base = (uintptr_t)hGame;
        
        MH_DisableHook((LPVOID)(base + ADDR_WeaponFireSM));
        MH_RemoveHook((LPVOID)(base + ADDR_WeaponFireSM));
        
        MH_DisableHook((LPVOID)(base + ADDR_SpawnBulletFromPool));
        MH_RemoveHook((LPVOID)(base + ADDR_SpawnBulletFromPool));
        
        MH_DisableHook((LPVOID)(base + ADDR_FireSetup));
        MH_RemoveHook((LPVOID)(base + ADDR_FireSetup));
        
        MH_DisableHook((LPVOID)(base + ADDR_EventInit));
        MH_RemoveHook((LPVOID)(base + ADDR_EventInit));
        
        MH_DisableHook((LPVOID)(base + ADDR_SetBulletVisuals));
        MH_RemoveHook((LPVOID)(base + ADDR_SetBulletVisuals));
        
        MH_DisableHook((LPVOID)(base + ADDR_InitBulletCI));
        MH_RemoveHook((LPVOID)(base + ADDR_InitBulletCI));
        
        MH_DisableHook((LPVOID)(base + ADDR_HandleBulletCollision));
        MH_RemoveHook((LPVOID)(base + ADDR_HandleBulletCollision));
        
        MH_DisableHook((LPVOID)(base + ADDR_FireGunRegistration));
        MH_RemoveHook((LPVOID)(base + ADDR_FireGunRegistration));
    }

    if (g_weaponTraceLog) {
        fprintf(g_weaponTraceLog, "\n========================================\n");
        fprintf(g_weaponTraceLog, "Weapon System Trace Shutdown\n");
        fprintf(g_weaponTraceLog, "End Time: %u ms\n", GetTickCount());
        fprintf(g_weaponTraceLog, "Duration: %u ms\n", GetTickCount() - g_weaponTraceStartTick);
        fclose(g_weaponTraceLog);
        g_weaponTraceLog = nullptr;
    }

    g_weaponTraceInitialized = FALSE;
    Log(EchoVR::LogLevel::Info, "[WeaponTrace] Cleanup complete");
}

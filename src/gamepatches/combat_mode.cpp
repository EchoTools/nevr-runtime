/*
 * combat_mode.cpp — Force combat game type in arena mode.
 *
 * Hooks IsCombatGameType/IsArenaGameType to make the engine think
 * it's in combat mode, enabling weapon systems. Also patches the
 * chassis equip check and weapon guard conditionals.
 *
 * RVAs from echovr.exe v34.4.631547-final.
 */

#include "combat_mode.h"

#include <cstring>
#include <windows.h>
#include <MinHook.h>

#include "common/globals.h"
#include "common/logging.h"
#include "common/echovr_functions.h"
#include "process_mem.h"

namespace {

/* RVAs */
constexpr uintptr_t RVA_IS_COMBAT    = 0x15C170;
constexpr uintptr_t RVA_IS_ARENA     = 0x155B20;
constexpr uintptr_t RVA_PLAYER_INIT  = 0xD625C0;
constexpr uintptr_t RVA_EQUIP_CHECK  = 0xD0D1FB;  /* JZ -> JMP */
constexpr uintptr_t RVA_WEAPON_JZ    = 0xCF5F62;  /* NOP */
constexpr uintptr_t RVA_WEAPON_JNZ   = 0xCF5F6B;  /* NOP */

constexpr ptrdiff_t IS_COMBAT_OFFSET = 360;  /* player + 0x168 */

/* Original function pointers */
typedef int64_t (__fastcall *GameTypeCheck_fn)(int64_t);
typedef void    (__fastcall *PlayerInit_fn)(uintptr_t);

static GameTypeCheck_fn g_origIsCombat = nullptr;
static GameTypeCheck_fn g_origIsArena  = nullptr;
static PlayerInit_fn    g_origPlayerInit = nullptr;

/* --- Hooks --- */

static int64_t __fastcall Hook_IsCombatGameType(int64_t a1) {
    return 1;  /* Always combat */
}

static int64_t __fastcall Hook_IsArenaGameType(int64_t a1) {
    return 0;  /* Never arena */
}

static void __fastcall Hook_PlayerInit(uintptr_t a1) {
    g_origPlayerInit(a1);
    /* Set isCombat flag on player component */
    *reinterpret_cast<int32_t*>(a1 + IS_COMBAT_OFFSET) = 1;
    Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] Player init @ 0x%llX, combat flag set",
        (unsigned long long)a1);
}

/* --- Memory patches --- */

static void PatchEquipBypass(bool enable) {
    uintptr_t addr = (uintptr_t)EchoVR::g_GameBaseAddress + RVA_EQUIP_CHECK;
    BYTE val = enable ? 0xEB : 0x74;  /* JMP (always) vs JZ (original) */
    DWORD oldProt;
    if (VirtualProtect((void*)addr, 1, PAGE_EXECUTE_READWRITE, &oldProt)) {
        *reinterpret_cast<BYTE*>(addr) = val;
        VirtualProtect((void*)addr, 1, oldProt, &oldProt);
        Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] Equip bypass: %s",
            enable ? "JZ->JMP" : "restored");
    }
}

static void PatchWeaponGuard(bool enable) {
    uintptr_t base = (uintptr_t)EchoVR::g_GameBaseAddress;
    BYTE nop2[2] = {0x90, 0x90};
    BYTE origJZ[2] = {0x74, 0x00};   /* Will read original for restore */
    BYTE origJNZ[2] = {0x75, 0x00};

    DWORD oldProt;
    if (enable) {
        /* NOP out both conditional jumps */
        if (VirtualProtect((void*)(base + RVA_WEAPON_JZ), 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy((void*)(base + RVA_WEAPON_JZ), nop2, 2);
            VirtualProtect((void*)(base + RVA_WEAPON_JZ), 2, oldProt, &oldProt);
        }
        if (VirtualProtect((void*)(base + RVA_WEAPON_JNZ), 2, PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy((void*)(base + RVA_WEAPON_JNZ), nop2, 2);
            VirtualProtect((void*)(base + RVA_WEAPON_JNZ), 2, oldProt, &oldProt);
        }
        Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] Weapon guards NOPed");
    }
}

} // anonymous namespace

void InstallCombatMode() {
    uintptr_t base = (uintptr_t)EchoVR::g_GameBaseAddress;

    /* Hook IsCombatGameType */
    void* pIsCombat = (void*)(base + RVA_IS_COMBAT);
    if (MH_CreateHook(pIsCombat, (void*)Hook_IsCombatGameType, (void**)&g_origIsCombat) == MH_OK &&
        MH_EnableHook(pIsCombat) == MH_OK) {
        Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] IsCombatGameType hooked -> always true");
    } else {
        Log(EchoVR::LogLevel::Warning, "[NEVR.COMBAT] IsCombatGameType hook FAILED");
    }

    /* Hook IsArenaGameType */
    void* pIsArena = (void*)(base + RVA_IS_ARENA);
    if (MH_CreateHook(pIsArena, (void*)Hook_IsArenaGameType, (void**)&g_origIsArena) == MH_OK &&
        MH_EnableHook(pIsArena) == MH_OK) {
        Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] IsArenaGameType hooked -> always false");
    } else {
        Log(EchoVR::LogLevel::Warning, "[NEVR.COMBAT] IsArenaGameType hook FAILED");
    }

    /* Hook PlayerInit */
    void* pPlayerInit = (void*)(base + RVA_PLAYER_INIT);
    if (MH_CreateHook(pPlayerInit, (void*)Hook_PlayerInit, (void**)&g_origPlayerInit) == MH_OK &&
        MH_EnableHook(pPlayerInit) == MH_OK) {
        Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] PlayerInit hooked");
    } else {
        Log(EchoVR::LogLevel::Warning, "[NEVR.COMBAT] PlayerInit hook FAILED");
    }

    /* Patch equip bypass and weapon guards */
    PatchEquipBypass(true);
    PatchWeaponGuard(true);

    Log(EchoVR::LogLevel::Info, "[NEVR.COMBAT] Combat mode active");
}

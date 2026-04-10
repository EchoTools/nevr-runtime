/*
 * combat_system.cpp — Merged combatpatch + swaptoggle functionality.
 *
 * Handles body swap script discovery+hooking, weapon enable events,
 * SendComponentEvent hook for network replication, HandleRespawn teleport,
 * team tracking, PlayerInit spawn detection, F9 toggle, sublevel offset.
 *
 * All __try/__except blocks replaced with nevr::SafeRead* / SafeMemcmp.
 */

#include "combat_system.h"

#include <windows.h>
#include <tlhelp32.h>
#include <MinHook.h>
#include <cstdint>
#include <cstring>
#include <atomic>

#include "safe_memory.h"
#include "combat_log.h"
#include "nevr_common.h"
#include "address_registry.h"

namespace {

/* ── Event struct ──────────────────────────────────────────────── */

#pragma pack(push, 1)
struct SComponentEvent {
    uint64_t name;
    uint64_t actor;
    uint64_t nodeid;
    uint16_t poolidx;
    uint8_t  pad[6];
    uint64_t gamespace;
    uint64_t component;
    uint64_t userdata;
};
#pragma pack(pop)
static_assert(sizeof(SComponentEvent) == 56, "SComponentEvent must be 56 bytes");

/* ── Event hashes ──────────────────────────────────────────────── */

constexpr uint64_t EVT_TOGGLE_PLAYER_ACTOR = 0x5FB56A55E9D1D227ULL;
constexpr uint64_t EVT_ENTERED_COMBAT_LAND = 0xE0ED102827299A0AULL;
constexpr uint64_t EVT_ENTERED_ARENA_LAND  = 0x003AECFF85E4E60AULL;
constexpr uint64_t EVT_DISABLE_CEASE_FIRE  = 0x0EA31347147AD3C3ULL;
constexpr uint64_t EVT_ENABLE_GUN          = 0x3A6D0346A13117DEULL;
constexpr uint64_t EVT_ENABLE_TAC          = 0x3A6D0346A12203D3ULL;
constexpr uint64_t EVT_ENABLE_GRENADE      = 0x0064DC7FC234322FULL;
constexpr uint64_t EVT_ENABLE_GEAR         = 0xBFCE97C3DF1F3716ULL;

constexpr uint64_t NEVT_ENTERED_COMBAT_LAND      = 0x0206FE71903C19B2ULL;
constexpr uint64_t NEVT_TOGGLE_PLAYER_ACTOR_TYPE = 0x8C0B17FA5669CE85ULL;
constexpr uint64_t NEVT_ENTERED_ARENA_LAND       = 0x2C448E7060E7E332ULL;
constexpr uint64_t NEVT_COMBAT_ZONE_ENTER_A      = 0x635D09DEFF7164C0ULL;
constexpr uint64_t NEVT_COMBAT_ZONE_ENTER_B      = 0xFBC0427AC211919EULL;
constexpr uint64_t NEVT_COMBAT_ZONE_LEAVE_A      = 0x635D09DEFF7164C1ULL;
constexpr uint64_t NEVT_COMBAT_ZONE_LEAVE_B      = 0xFBC0427AC211919FULL;
constexpr uint64_t EVT_ENABLE_CEASE_FIRE         = 0x0EA31347147AD3C3ULL;

constexpr uint64_t PEER_TARGET_ALL = 0xFFFFFFFFFFFFFFFEULL;
constexpr uint64_t CUSTOM_SUBLEVEL_HASH = 0x813EDECF5228A2BAULL;
constexpr float    SUBLEVEL_Y_OFFSET = -100.0f;

/* ── State ─────────────────────────────────────────────────────── */

std::atomic<bool> g_InCombat{true};
std::atomic<bool> g_PendingToggle{false};
std::atomic<void*> g_NetGame{nullptr};
std::atomic<uint16_t> g_LocalTeamId{0xFFFF};
std::atomic<int> g_RespawnCounter{0};
std::atomic<bool> g_PlayerSpawned{false};
std::atomic<int64_t> g_PendingOffsetScene{0};

uintptr_t g_GameBase = 0;
bool g_ScriptDLLsHooked = false;
bool g_WeaponsDLLsHooked = false;
bool g_HealScriptHooked = false;
int g_FrameCounter = 0;
int g_DLLSearchAttempts = 0;

/* ── Net event payload ─────────────────────────────────────────── */

#pragma pack(push, 1)
struct NetComponentEventPayload {
    uint64_t event_hash;
    uint64_t target;
    int32_t  flags;
    int32_t  sender_slot;
};
#pragma pack(pop)

/* ── Teleport request ──────────────────────────────────────────── */

#pragma pack(push, 1)
struct UserID { uint16_t slot; uint16_t gen; };
struct CTransQ { float qx, qy, qz, qw, px, py, pz, scale; };
struct SR15NetGameplayTeleportPlayerRequest { UserID player; CTransQ pos; };
#pragma pack(pop)

const float TEAM1_SPAWNS[][3] = {
    { 0.0f, -2.5f,  74.0f}, { 2.0f, -2.5f,  74.0f}, {-2.0f, -2.5f,  74.0f},
};
const float TEAM2_SPAWNS[][3] = {
    { 0.0f, -2.5f, -74.0f}, { 2.0f, -2.5f, -74.0f}, {-2.0f, -2.5f, -74.0f},
};

/* ── Function types ────────────────────────────────────────────── */

using SendComponentEvent_t = void(__fastcall*)(void*, void*, int64_t, int64_t, uint64_t, int64_t);
using HandleRespawn_t = void(__fastcall*)(int64_t, int64_t);
using TeleportPlayerRequest_t = void(__fastcall*)(void*, void*, void*, int64_t, uint64_t, uint64_t);
using TeamChange_t = int64_t(__fastcall*)(int64_t, int64_t);
using PlayerInit_t = void(__fastcall*)(uintptr_t);
using ScriptTick_t = int(__fastcall*)(int64_t);
using ScriptHandler_t = void(__fastcall*)(int64_t, SComponentEvent*);
using FnLevelOffset = void(__fastcall*)(int64_t, int64_t);
using HealScriptInit_t = int64_t(__fastcall*)(int64_t);

static SendComponentEvent_t g_OrigSCE = nullptr;
static HandleRespawn_t g_OrigRespawn = nullptr;
static TeleportPlayerRequest_t g_FnTeleport = nullptr;
static TeamChange_t g_OrigTeamChange = nullptr;
static PlayerInit_t g_OrigPlayerInit = nullptr;
static FnLevelOffset g_OrigLevelOffset = nullptr;
static HealScriptInit_t g_OrigHealInit = nullptr;

/* ── Script DLL instance tracking ──────────────────────────────── */

struct ScriptInstance {
    HMODULE hModule;
    ScriptTick_t tickFn;
    ScriptHandler_t handlerFn;
    ScriptHandler_t originalHandler;
    int64_t lastCtx;
    bool ctxValid;
};

constexpr int kMaxInstances = 8;
ScriptInstance g_BodySwap[kMaxInstances] = {};
int g_BodySwapCount = 0;
ScriptTick_t g_BodySwapOrigTicks[kMaxInstances] = {};

ScriptInstance g_Weapons[kMaxInstances] = {};
int g_WeaponsCount = 0;
ScriptTick_t g_WeaponsOrigTicks[kMaxInstances] = {};

/* ── Utility: send net event ───────────────────────────────────── */

bool SendNetEvent(uint64_t eventHash) {
    void* netgame = g_NetGame.load(std::memory_order_acquire);
    if (!netgame || !g_OrigSCE) return false;
    NetComponentEventPayload payload = {};
    payload.event_hash = eventHash;
    payload.target = 0xFFFFFFFFFFFFFFFFULL;
    payload.flags = 0x50FFF;
    payload.sender_slot = 0xFFFFFFFF;
    g_OrigSCE(netgame, &payload, 0, 0, PEER_TARGET_ALL, -1);
    return true;
}

/* ── Send combat/weapon events ─────────────────────────────────── */

void SendCombatEvents() {
    for (int i = 0; i < g_BodySwapCount; i++) {
        if (!g_BodySwap[i].ctxValid || !g_BodySwap[i].handlerFn) continue;
        ScriptHandler_t handler = g_BodySwap[i].originalHandler
            ? g_BodySwap[i].originalHandler : g_BodySwap[i].handlerFn;
        SComponentEvent evt = {};
        evt.name = EVT_ENTERED_COMBAT_LAND;
        handler(g_BodySwap[i].lastCtx, &evt);
        memset(&evt, 0, sizeof(evt));
        evt.name = EVT_TOGGLE_PLAYER_ACTOR;
        handler(g_BodySwap[i].lastCtx, &evt);
    }
    SendNetEvent(NEVT_ENTERED_COMBAT_LAND);
    SendNetEvent(NEVT_TOGGLE_PLAYER_ACTOR_TYPE);
    SendNetEvent(NEVT_COMBAT_ZONE_ENTER_A);
    SendNetEvent(NEVT_COMBAT_ZONE_ENTER_B);
}

void SendWeaponEvents() {
    for (int i = 0; i < g_WeaponsCount; i++) {
        if (!g_Weapons[i].ctxValid || !g_Weapons[i].handlerFn) continue;
        ScriptHandler_t handler = g_Weapons[i].originalHandler
            ? g_Weapons[i].originalHandler : g_Weapons[i].handlerFn;
        uint64_t events[] = {
            EVT_DISABLE_CEASE_FIRE, EVT_ENABLE_GUN, EVT_ENABLE_TAC,
            EVT_ENABLE_GRENADE, EVT_ENABLE_GEAR,
        };
        for (uint64_t evtHash : events) {
            SComponentEvent evt = {};
            evt.name = evtHash;
            handler(g_Weapons[i].lastCtx, &evt);
        }
    }
}

/* ── Hooks ─────────────────────────────────────────────────────── */

bool ShouldBlockEvent(uint64_t name) {
    return name == EVT_ENTERED_ARENA_LAND
        || name == EVT_ENABLE_CEASE_FIRE
        || name == NEVT_COMBAT_ZONE_LEAVE_A
        || name == NEVT_COMBAT_ZONE_LEAVE_B;
}

void __fastcall Hook_SendComponentEvent(
    void* netgame, void* payload, int64_t extra1, int64_t extra2,
    uint64_t peer_target, int64_t timeout)
{
    if (netgame && !g_NetGame.load(std::memory_order_relaxed)) {
        g_NetGame.store(netgame, std::memory_order_release);
        combat_mod::PluginLog( "Captured CR15NetGame: 0x%llX",
            (unsigned long long)(uintptr_t)netgame);
    }
    if (payload && g_InCombat.load(std::memory_order_relaxed)) {
        uint64_t evtHash = *reinterpret_cast<uint64_t*>(payload);
        if (evtHash == NEVT_COMBAT_ZONE_LEAVE_A || evtHash == NEVT_COMBAT_ZONE_LEAVE_B ||
            evtHash == EVT_ENTERED_ARENA_LAND || evtHash == NEVT_ENTERED_ARENA_LAND) {
            return;
        }
    }
    g_OrigSCE(netgame, payload, extra1, extra2, peer_target, timeout);
}

int64_t __fastcall Hook_TeamChange(int64_t a1, int64_t a2) {
    /* SEH replaced with SafeReadU64/SafeReadU16 */
    int64_t data = 0;
    if (nevr::SafeReadU64(static_cast<uintptr_t>(a2 + 16), reinterpret_cast<uint64_t*>(&data)) && data) {
        uint64_t entrant_id = 0;
        uint16_t new_team = 0;
        nevr::SafeReadU64(static_cast<uintptr_t>(data), &entrant_id);
        nevr::SafeReadU16(static_cast<uintptr_t>(data + 8), &new_team);

        void* ng = g_NetGame.load(std::memory_order_relaxed);
        uint16_t local_slot = 0;
        if (ng) nevr::SafeReadU16(static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(ng) + 376), &local_slot);
        local_slot &= 0x1F;

        if (entrant_id == local_slot) {
            g_LocalTeamId.store(new_team, std::memory_order_release);
            combat_mod::PluginLog( "Local team changed to %hu", new_team);
        }
    }
    return g_OrigTeamChange(a1, a2);
}

void __fastcall Hook_HandleRespawn(int64_t a1, int64_t a2) {
    /* SEH replaced with SafeReadU64 */
    int64_t msg_data = 0;
    if (nevr::SafeReadU64(static_cast<uintptr_t>(a2 + 16), reinterpret_cast<uint64_t*>(&msg_data)) && msg_data) {
        uint64_t packed = 0;
        nevr::SafeReadU64(static_cast<uintptr_t>(msg_data + 8), &packed);
        uint16_t slot_index = static_cast<uint16_t>(packed & 0xFFFF);
        uint16_t gen = static_cast<uint16_t>(packed >> 16);

        void* ng = g_NetGame.load(std::memory_order_acquire);
        uint16_t local_slot = 0xFFFF;
        if (ng) {
            nevr::SafeReadU16(static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(ng) + 376), &local_slot);
            local_slot &= 0x1F;
        }

        if (ng && g_FnTeleport && slot_index < 16 && slot_index == local_slot) {
            uint16_t team_id = g_LocalTeamId.load(std::memory_order_acquire);
            int idx = g_RespawnCounter.fetch_add(1) % 3;
            const float* spawn = (team_id == 1) ? TEAM1_SPAWNS[idx] : TEAM2_SPAWNS[idx];

            SR15NetGameplayTeleportPlayerRequest req = {};
            req.player.slot = slot_index;
            req.player.gen = gen;
            req.pos.px = spawn[0];
            req.pos.py = spawn[1];
            req.pos.pz = spawn[2];
            req.pos.qw = (team_id == 1) ? 0.0f : 1.0f;
            req.pos.qy = (team_id == 1) ? 1.0f : 0.0f;
            req.pos.scale = 1.0f;

            g_FnTeleport(ng, &req, nullptr, 0, 0xFFFFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFFFFFULL);
            combat_mod::PluginLog( "Teleported slot=%d team=%d to (%.0f,%.1f,%.0f)",
                slot_index, team_id, spawn[0], spawn[1], spawn[2]);
        }
    }
    g_OrigRespawn(a1, a2);
}

void __fastcall Hook_PlayerInit(uintptr_t a1) {
    g_OrigPlayerInit(a1);
    bool was = g_PlayerSpawned.exchange(true, std::memory_order_release);
    if (!was)
        combat_mod::PluginLog( "Player spawned");
}

/* ── Level offset hook (from swaptoggle) ───────────────────────── */

void __fastcall Hook_LevelOffset(int64_t a1, int64_t a2) {
    int64_t scene = 0;
    nevr::SafeReadU64(static_cast<uintptr_t>(a1 + 0x10), reinterpret_cast<uint64_t*>(&scene));

    g_OrigLevelOffset(a1, a2);

    if (!scene) return;
    uint64_t levelHash = 0;
    if (nevr::SafeReadU64(static_cast<uintptr_t>(scene + 0xB10), &levelHash)) {
        if (levelHash == CUSTOM_SUBLEVEL_HASH) {
            g_PendingOffsetScene.store(scene);
            combat_mod::PluginLog( "Sub-level detected — queuing offset");
        }
    }
}

void ApplyPendingOffset() {
    int64_t scene = g_PendingOffsetScene.exchange(0);
    if (!scene) return;

    uint64_t rootNode = 0;
    if (!nevr::SafeReadU64(static_cast<uintptr_t>(scene + 0x60), &rootNode) || !rootNode) return;

    uint64_t vtable = 0;
    if (!nevr::SafeReadU64(static_cast<uintptr_t>(rootNode), &vtable) || !vtable) return;

    uint64_t addTransAddr = 0;
    if (!nevr::SafeReadU64(static_cast<uintptr_t>(vtable + 0x80), &addTransAddr) || !addTransAddr) return;

    float offset[3] = {0.0f, SUBLEVEL_Y_OFFSET, 0.0f};
    using AddTranslation_t = void(__fastcall*)(int64_t, float*);
    auto addTrans = reinterpret_cast<AddTranslation_t>(addTransAddr);
    addTrans(static_cast<int64_t>(rootNode), offset);
    combat_mod::PluginLog( "Applied Y=%.0f offset to sub-level", SUBLEVEL_Y_OFFSET);
}

/* ── Heal script crash fix ─────────────────────────────────────── */

static const char g_EmptyStr[3] = {0, 0, 0};

int64_t __fastcall Hook_HealScriptInit(int64_t a1) {
    uint64_t emptyPtr = reinterpret_cast<uint64_t>(&g_EmptyStr);
    memset(reinterpret_cast<void*>(a1), 0, 0x244);
    *reinterpret_cast<uint64_t*>(a1 + 0x08) = 0xFFFFFFFFFFFFFFFFULL;
    *reinterpret_cast<uint32_t*>(a1 + 0x10) = 0x3F800000;
    *reinterpret_cast<uint16_t*>(a1 + 0x14) = 1;
    *reinterpret_cast<uint32_t*>(a1 + 0x18) = 0x40A00000;
    *reinterpret_cast<uint64_t*>(a1 + 0x28) = 0xFFFFFFFFFFFFFFFFULL;
    *reinterpret_cast<uint32_t*>(a1 + 0x30) = 0xFFFFFFFF;
    *reinterpret_cast<uint32_t*>(a1 + 0x34) = 0x0000FFFF;
    *reinterpret_cast<uint64_t*>(a1 + 0x38) = 0xFFFFFFFFFFFFFFFFULL;
    *reinterpret_cast<uint64_t*>(a1 + 0x40) = 0xFFFFFFFFFFFFFFFFULL;
    *reinterpret_cast<uint64_t*>(a1 + 0x48) = 0xFFFFFFFFFFFFFFFFULL;
    *reinterpret_cast<uint32_t*>(a1 + 0x6C) = 0x3F800000;
    *reinterpret_cast<uint32_t*>(a1 + 0x70) = 0x3F800000;
    *reinterpret_cast<uint32_t*>(a1 + 0x74) = 0x3F800000;
    *reinterpret_cast<uint8_t*>(a1 + 0x78) = 1;
    *reinterpret_cast<uint32_t*>(a1 + 0x7C) = 0xFF;
    for (int i = 0; i < 8; i++)
        *reinterpret_cast<uint64_t*>(a1 + 0x80 + i * 0x38) = emptyPtr;
    return a1;
}

/* ── Body swap handler/tick hooks (template array) ─────────────── */

template<int IDX>
void __fastcall HookBodySwapHandler(int64_t ctx, SComponentEvent* evt) {
    if (evt && g_InCombat.load(std::memory_order_relaxed)) {
        if (ShouldBlockEvent(evt->name)) return;
    }
    g_BodySwap[IDX].originalHandler(ctx, evt);
}

static void (__fastcall *HookBodySwapHandlerFns[])(int64_t, SComponentEvent*) = {
    HookBodySwapHandler<0>, HookBodySwapHandler<1>, HookBodySwapHandler<2>, HookBodySwapHandler<3>,
    HookBodySwapHandler<4>, HookBodySwapHandler<5>, HookBodySwapHandler<6>, HookBodySwapHandler<7>,
};

template<int IDX>
int __fastcall HookBodySwapTick(int64_t ctx) {
    g_BodySwap[IDX].lastCtx = ctx;
    g_BodySwap[IDX].ctxValid = true;

    if (g_PendingToggle.exchange(false)) {
        bool newState = !g_InCombat.load(std::memory_order_relaxed);
        g_InCombat.store(newState, std::memory_order_release);
        combat_mod::PluginLog( "F9 toggle -> %s", newState ? "COMBAT" : "ARENA");

        if (newState) {
            SendCombatEvents();
            SendWeaponEvents();
        } else {
            for (int i = 0; i < g_BodySwapCount; i++) {
                if (!g_BodySwap[i].ctxValid) continue;
                ScriptHandler_t handler = g_BodySwap[i].originalHandler
                    ? g_BodySwap[i].originalHandler : g_BodySwap[i].handlerFn;
                SComponentEvent evt = {};
                evt.name = EVT_ENTERED_ARENA_LAND;
                handler(g_BodySwap[i].lastCtx, &evt);
                memset(&evt, 0, sizeof(evt));
                evt.name = EVT_TOGGLE_PLAYER_ACTOR;
                handler(g_BodySwap[i].lastCtx, &evt);
            }
        }
    }

    ApplyPendingOffset();

    int result = g_BodySwapOrigTicks[IDX](ctx);
    return result > 0 ? result : 1;
}

static int (__fastcall *HookBodySwapTickFns[])(int64_t) = {
    HookBodySwapTick<0>, HookBodySwapTick<1>, HookBodySwapTick<2>, HookBodySwapTick<3>,
    HookBodySwapTick<4>, HookBodySwapTick<5>, HookBodySwapTick<6>, HookBodySwapTick<7>,
};

template<int IDX>
int __fastcall HookWeaponsTick(int64_t ctx) {
    g_Weapons[IDX].lastCtx = ctx;
    g_Weapons[IDX].ctxValid = true;
    int result = g_WeaponsOrigTicks[IDX](ctx);
    return result > 0 ? result : 1;
}

static int (__fastcall *HookWeaponsTickFns[])(int64_t) = {
    HookWeaponsTick<0>, HookWeaponsTick<1>, HookWeaponsTick<2>, HookWeaponsTick<3>,
    HookWeaponsTick<4>, HookWeaponsTick<5>, HookWeaponsTick<6>, HookWeaponsTick<7>,
};

/* ── Script DLL discovery ──────────────────────────────────────── */

const uint8_t kBodySwapPattern[] = {0x27, 0xD2, 0xD1, 0xE9, 0x55, 0x6A, 0xB5, 0x5F};
const uint8_t kWeaponsPattern[]  = {0xDE, 0x17, 0x31, 0xA1, 0x46, 0x03, 0x6D, 0x3A};

constexpr uintptr_t kScriptTickRVA    = 0x67B0;
constexpr uintptr_t kScriptHandlerRVA = 0xAFA0;
constexpr uintptr_t kWeaponsTickRVA   = 0x39C0;

bool FindAndHookBodySwapDLLs(nevr::HookManager& hooks) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32 me = {};
    me.dwSize = sizeof(me);
    g_BodySwapCount = 0;

    if (Module32First(snap, &me)) {
        do {
            if (g_BodySwapCount >= kMaxInstances) break;
            FARPROC sb = GetProcAddress(me.hModule, "setup_bindings");
            if (!sb) continue;

            /* SEH replaced with SafeMemcmp */
            uint8_t* base = me.modBaseAddr;
            DWORD size = me.modBaseSize;
            bool found = false;
            for (DWORD i = 0; i + 8 <= size; i++) {
                if (nevr::SafeMemcmp(base + i, kBodySwapPattern, 8)) {
                    found = true;
                    break;
                }
            }
            if (!found) continue;

            int idx = g_BodySwapCount;
            uintptr_t dllBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);

            g_BodySwap[idx].hModule = me.hModule;
            g_BodySwap[idx].tickFn = reinterpret_cast<ScriptTick_t>(dllBase + kScriptTickRVA);
            g_BodySwap[idx].handlerFn = reinterpret_cast<ScriptHandler_t>(dllBase + kScriptHandlerRVA);
            g_BodySwap[idx].originalHandler = nullptr;
            g_BodySwap[idx].ctxValid = false;

            void* target = reinterpret_cast<void*>(g_BodySwap[idx].tickFn);
            if (MH_CreateHook(target, reinterpret_cast<void*>(HookBodySwapTickFns[idx]),
                               reinterpret_cast<void**>(&g_BodySwapOrigTicks[idx])) != MH_OK) continue;
            MH_EnableHook(target);
            hooks.Track(target);

            g_BodySwap[idx].originalHandler = g_BodySwap[idx].handlerFn;
            void* hTarget = reinterpret_cast<void*>(g_BodySwap[idx].handlerFn);
            if (MH_CreateHook(hTarget, reinterpret_cast<void*>(HookBodySwapHandlerFns[idx]),
                               reinterpret_cast<void**>(&g_BodySwap[idx].originalHandler)) == MH_OK) {
                MH_EnableHook(hTarget);
                hooks.Track(hTarget);
            }

            g_BodySwapCount++;
            combat_mod::PluginLog( "Body swap DLL #%d hooked @ 0x%llX",
                idx, (unsigned long long)dllBase);
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return g_BodySwapCount > 0;
}

bool FindAndHookWeaponsDLLs(nevr::HookManager& hooks) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32 me = {};
    me.dwSize = sizeof(me);
    g_WeaponsCount = 0;

    if (Module32First(snap, &me)) {
        do {
            if (g_WeaponsCount >= kMaxInstances) break;
            FARPROC sb = GetProcAddress(me.hModule, "setup_bindings");
            if (!sb) continue;

            uint8_t* base = me.modBaseAddr;
            DWORD size = me.modBaseSize;
            bool found = false;
            for (DWORD i = 0; i + 8 <= size; i++) {
                if (nevr::SafeMemcmp(base + i, kWeaponsPattern, 8)) {
                    found = true;
                    break;
                }
            }
            if (!found) continue;

            int idx = g_WeaponsCount;
            uintptr_t dllBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);

            g_Weapons[idx].hModule = me.hModule;
            g_Weapons[idx].tickFn = reinterpret_cast<ScriptTick_t>(dllBase + kWeaponsTickRVA);
            g_Weapons[idx].ctxValid = false;

            void* target = reinterpret_cast<void*>(g_Weapons[idx].tickFn);
            if (MH_CreateHook(target, reinterpret_cast<void*>(HookWeaponsTickFns[idx]),
                               reinterpret_cast<void**>(&g_WeaponsOrigTicks[idx])) == MH_OK) {
                MH_EnableHook(target);
                hooks.Track(target);
                g_WeaponsCount++;
                combat_mod::PluginLog( "Weapons DLL #%d hooked @ 0x%llX",
                    idx, (unsigned long long)dllBase);
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return g_WeaponsCount > 0;
}

bool FindAndHookHealScript(nevr::HookManager& hooks) {
    if (g_HealScriptHooked) return true;

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    MODULEENTRY32 me = {};
    me.dwSize = sizeof(me);
    bool found = false;

    if (Module32First(snap, &me)) {
        do {
            if (strstr(me.szModule, "6a0f689e5b91c666")) {
                uintptr_t dllBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                void* target = reinterpret_cast<void*>(dllBase + 0x38D0);
                void* origPtr = nullptr;
                if (MH_CreateHook(target, reinterpret_cast<void*>(Hook_HealScriptInit), &origPtr) == MH_OK) {
                    g_OrigHealInit = reinterpret_cast<HealScriptInit_t>(origPtr);
                    MH_EnableHook(target);
                    hooks.Track(target);
                    g_HealScriptHooked = true;
                    found = true;
                    combat_mod::PluginLog( "Heal script init hooked @ 0x%llX+0x38D0",
                        (unsigned long long)dllBase);
                }
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

/* Track hooks for deferred DLL discovery */
nevr::HookManager* g_DeferredHooks = nullptr;

} // anonymous namespace

/* ── Public API ────────────────────────────────────────────────── */

namespace combat_mod {

void InstallCombatEarly(uintptr_t base, nevr::HookManager& hooks) {
    g_GameBase = base;
    g_DeferredHooks = &hooks;

    /* Team change hook — fires during lobby, before level loads */
    void* tc = nevr::ResolveVA(base, nevr::addresses::VA_TEAM_CHANGE);
    void* tcOrig = nullptr;
    if (MH_CreateHook(tc, reinterpret_cast<void*>(Hook_TeamChange), &tcOrig) == MH_OK) {
        g_OrigTeamChange = reinterpret_cast<TeamChange_t>(tcOrig);
        MH_EnableHook(tc);
        hooks.Track(tc);
        combat_mod::PluginLog( "TeamChange hooked (early)");
    }
}

void InstallCombatLevelHooks(uintptr_t base, nevr::HookManager& hooks) {
    /* PlayerInit — spawn detection */
    void* pi = nevr::ResolveVA(base, nevr::addresses::VA_PLAYER_INIT);
    void* piOrig = nullptr;
    if (MH_CreateHook(pi, reinterpret_cast<void*>(Hook_PlayerInit), &piOrig) == MH_OK) {
        g_OrigPlayerInit = reinterpret_cast<PlayerInit_t>(piOrig);
        MH_EnableHook(pi);
        hooks.Track(pi);
        combat_mod::PluginLog( "PlayerInit hooked");
    }

    /* HandleRespawn — teleport on death */
    g_FnTeleport = reinterpret_cast<TeleportPlayerRequest_t>(
        nevr::ResolveVA(base, nevr::addresses::VA_TELEPORT_PLAYER));

    void* hr = nevr::ResolveVA(base, nevr::addresses::VA_HANDLE_RESPAWN);
    void* hrOrig = nullptr;
    if (MH_CreateHook(hr, reinterpret_cast<void*>(Hook_HandleRespawn), &hrOrig) == MH_OK) {
        g_OrigRespawn = reinterpret_cast<HandleRespawn_t>(hrOrig);
        MH_EnableHook(hr);
        hooks.Track(hr);
        combat_mod::PluginLog( "HandleRespawn hooked");
    }

    /* SendComponentEvent — net replication + event blocking */
    void* sce = nevr::ResolveVA(base, nevr::addresses::VA_SEND_COMPONENT_EVENT);
    void* sceOrig = nullptr;
    if (MH_CreateHook(sce, reinterpret_cast<void*>(Hook_SendComponentEvent), &sceOrig) == MH_OK) {
        g_OrigSCE = reinterpret_cast<SendComponentEvent_t>(sceOrig);
        MH_EnableHook(sce);
        hooks.Track(sce);
        combat_mod::PluginLog( "SendComponentEvent hooked");
    }
}

void InstallLevelOffset(uintptr_t base, nevr::HookManager& hooks) {
    void* lo = nevr::ResolveVA(base, nevr::addresses::VA_LEVEL_OFFSET_HOOK);
    void* loOrig = nullptr;
    if (MH_CreateHook(lo, reinterpret_cast<void*>(Hook_LevelOffset), &loOrig) == MH_OK) {
        g_OrigLevelOffset = reinterpret_cast<FnLevelOffset>(loOrig);
        MH_EnableHook(lo);
        hooks.Track(lo);
        combat_mod::PluginLog( "LevelOffset hooked");
    }
}

bool CombatOnFrame() {
    g_FrameCounter++;

    /* F9 polling — throttled to ~20Hz */
    if (g_FrameCounter % 3 == 0) {
        static bool keyWasDown = false;
        bool key = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        if (key && !keyWasDown)
            g_PendingToggle.store(true, std::memory_order_release);
        keyWasDown = key;
    }

    /* Deferred script DLL discovery (every 120 frames ≈ 2s) */
    if (!g_ScriptDLLsHooked && g_DeferredHooks && g_FrameCounter % 120 == 0
        && g_DLLSearchAttempts < 60) {
        g_DLLSearchAttempts++;
        g_ScriptDLLsHooked = FindAndHookBodySwapDLLs(*g_DeferredHooks);
        if (!g_WeaponsDLLsHooked)
            g_WeaponsDLLsHooked = FindAndHookWeaponsDLLs(*g_DeferredHooks);
        if (!g_HealScriptHooked)
            FindAndHookHealScript(*g_DeferredHooks);
    }

    return g_InCombat.load(std::memory_order_relaxed);
}

bool IsCombatActive() {
    return g_InCombat.load(std::memory_order_relaxed);
}

} // namespace combat_mod

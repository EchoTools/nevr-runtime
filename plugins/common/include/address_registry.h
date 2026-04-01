/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

/*
 * Central registry of ALL verified virtual addresses used across nEVR plugins.
 * Every address listed here has been verified in the echovr-reconstruction source.
 * Do NOT add speculative or unverified addresses.
 */
namespace nevr::addresses {

// Source: src/NRadEngine/Game/CBalanceConfig.h in echovr-reconstruction
static constexpr uint64_t VA_BALANCE_CONFIG = 0x1420d3450;

// Source: src/NRadEngine/Game/CR15NetCombatHelpers.cpp in echovr-reconstruction
static constexpr uint64_t VA_RESOLVE_CONFIG_HASH = 0x140d0b170;

// Source: src/NRadEngine/Game/CR15NetCombatAccessors5.cpp in echovr-reconstruction
static constexpr uint64_t VA_CAN_PLAYER_TAKE_DAMAGE = 0x140cb1040;

// Source: src/NRadEngine/Network/CBroadcaster.cpp in echovr-reconstruction
static constexpr uint64_t VA_BROADCASTER_SEND = 0x140f89af0;

// Source: src/NRadEngine/Network/CBroadcaster.cpp in echovr-reconstruction
static constexpr uint64_t VA_BROADCASTER_RECEIVE_LOCAL = 0x140f87aa0;

// Source: src/NRadEngine/Network/CBroadcaster.cpp in echovr-reconstruction
static constexpr uint64_t VA_BROADCASTER_LISTEN = 0x140f80ed0;

// Source: streamed_audio_injector/hook_manager.cpp in echovr-reconstruction
static constexpr uint64_t VA_VOIP_FRAME_PROCESSOR = 0x140d7bd90;

// Source: streamed_audio_injector/hook_manager.cpp in echovr-reconstruction
static constexpr uint64_t VA_LOBBY_JOIN_HANDLER = 0x14000b020;

// Source: streamed_audio_injector/hook_manager.cpp in echovr-reconstruction
static constexpr uint64_t VA_LOBBY_LEFT_HANDLER = 0x14000b160;

// Source: streamed_audio_injector/voip_broadcaster.cpp in echovr-reconstruction
static constexpr uint64_t VA_VOIP_ROUTER = 0x140132dc0;

// Source: GameStateAPI.h in echovr-reconstruction
static constexpr uint64_t VA_HTTP_EXPORT_GAME_STATE = 0x140155c80;

// Source: src/NRadEngine/Game/CR15NetReplicatedEventFire.cpp in echovr-reconstruction
static constexpr uint64_t VA_CSYMBOL64_FROM_STRING = 0x140107F80;

// Source: src/NRadEngine/Game/CR15NetWeaponTypeChange.cpp in echovr-reconstruction
static constexpr uint64_t VA_WEAPON_TYPE_TABLE = 0x1420D3850;

// Source: src/NRadEngine/Game/CR15NetEquipmentConfig.cpp in echovr-reconstruction
static constexpr uint64_t VA_GEAR_WEAPONS_TABLE = 0x1420D3420;

// Source: src/NRadEngine/Game/CR15NetCombatHelpers.cpp in echovr-reconstruction
static constexpr uint64_t VA_CONFIG_TABLE = 0x1420d3a68;

// Source: src/NRadEngine/Game/CR15NetBalanceSettingsCS.cpp in echovr-reconstruction
static constexpr uint64_t VA_BALANCE_SETTINGS_TABLE = 0x1420d3a68;

// --- Pass Frenzy / Core gameplay ---

// Source: CR15NetGameplay::IncrementScore (PC binary analysis)
static constexpr uint64_t VA_INCREMENT_SCORE = 0x140172990;

// Source: CR15NetGame::UpdateAfterTick (PC binary analysis)
static constexpr uint64_t VA_UPDATE_AFTER_TICK = 0x1401BBDB0;

// Source: CJson::Int (PC binary analysis)
static constexpr uint64_t VA_CJSON_INT = 0x1405fd4f0;

// --- Super Hyper Turbo ---

// Source: CR15NetGame::CreateSession (PC binary analysis)
static constexpr uint64_t VA_CREATE_SESSION = 0x14015e920;

// --- Server Timing ---

// Source: src/gamepatches/patch_addresses.h in nevr-runtime
// CPrecisionSleep::BusyWait — tight QPC+SwitchToThread loop for sub-ms frame timing.
// On Wine, SwitchToThread doesn't yield, burning ~35% CPU. Patch to RET.
// Source: echovr-reconstruction/src/NRadEngine/Core/CTiming.h
// CPrecisionSleep::Wait — hybrid WaitableTimer + BusyWait frame pacer (361 bytes).
// Signature: void(int64_t microseconds, int64_t unknown, LARGE_INTEGER* unknown2)
// On Wine, the WaitableTimer phase spins instead of sleeping.
static constexpr uint64_t VA_PRECISION_SLEEP_WAIT = 0x1401CE0B0;

static constexpr uint64_t VA_PRECISION_SLEEP_BUSYWAIT = 0x1401CE4C0;

// Source: objdump disassembly, echovr-reconstruction/src/NRadEngine/Core/CTiming.h
// SwitchToThread IAT thunk — indirect jump to kernel32!SwitchToThread.
// 10 call sites, all yield/wait/backoff paths (none per-work-item):
//   DrainWorkQueue idle poll, SRWLock contention, spinlock backoff,
//   fence sync, fiber scheduler, memory allocator contention.
// On Wine, maps to sched_yield() which returns immediately → spin loops.
static constexpr uint64_t VA_SWITCH_TO_THREAD = 0x1401CE4B0;

// Source: src/gamepatches/patch_addresses.h in nevr-runtime
// Delta time comparison — signed JLE should be unsigned JAE
static constexpr uint64_t VA_HEADLESS_DELTATIME = 0x1400CF46D;

// --- Arena Rules (CJson_GetFloat hook) ---

// Source: src/gamepatches/patch_addresses.h in nevr-runtime
// CJson_GetFloat thunk — 45 direct callers, 623 via inspector ReadFloat.
// Signature: float(void* root, const char* path, float defaultValue, int32_t required)
static constexpr uint64_t VA_CJSON_GET_FLOAT = 0x1405FCA60;

// --- Grab Hands ---

// Source: CR15TouchInteractCS::SetCanBeGrabbed (PC binary analysis)
// Setter controlling whether a touch-interact component can be grabbed by other players.
// Signature: void __fastcall (void* this, bool canBeGrabbed)
static constexpr uint64_t VA_SET_CAN_BE_GRABBED = 0x140909f10;

// --- Log Filter ---

// Source: src/NRadEngine/Core/CLog.h in echovr-reconstruction
// CLog::PrintfImpl — real log dispatcher. Non-variadic 4-arg __fastcall:
//   void(uint32_t level, int64_t category, const char* fmt, int64_t* varargs)
// Hook this for log filtering (NOT the variadic wrapper at 0x1400ebe50).
static constexpr uint64_t VA_CLOG_PRINTF_IMPL = 0x1400ebe70;

// --- Filesystem Loader ---

// Source: src/NRadEngine/Core/CResource.h in echovr-reconstruction
// Resource_InitFromBuffers — stores data buffers into CResource and calls
// vtable[7] (DeserializeAndUpload). 5 parameters:
//   void __fastcall (CResource* resource, void* buf1, uint64_t size1,
//                    void* buf2, uint64_t size2)
static constexpr uint64_t VA_RESOURCE_INIT_FROM_BUFFERS = 0x140fa2510;

// --- Combat Patch ---

// Source: combatpatch.h / PC binary analysis
// IsCombatGameType — returns 1 if current game type is combat
static constexpr uint64_t VA_IS_COMBAT_GAME_TYPE = 0x14015C170;

// Source: combatpatch.h / PC binary analysis
// IsArenaGameType — returns 1 if current game type is arena
static constexpr uint64_t VA_IS_ARENA_GAME_TYPE = 0x140155B20;

// Source: combatpatch.h / PC binary analysis
// PlayerInit — player component initialization
static constexpr uint64_t VA_PLAYER_INIT = 0x140D625C0;

// Source: combatpatch.h / PC binary analysis
// EquipmentInit — R14NetPlayerEquipment component init
static constexpr uint64_t VA_EQUIPMENT_INIT = 0x140D02390;

// Source: combatpatch.h / PC binary analysis
// WeaponActorExpression — animation system weapon entity lookup
static constexpr uint64_t VA_WEAPON_ACTOR_EXPR = 0x140F0BCF0;

// Source: combatpatch.h / PC binary analysis
// EntityLookup — gamespace entity hash lookup (hot path)
static constexpr uint64_t VA_ENTITY_LOOKUP = 0x1404F3700;

// Source: combatpatch.h / PC binary analysis
// ApplyDefaultLoadout — reapplies full default loadout
//   void __fastcall (R15NetGame*, uint16_t userIndex)
static constexpr uint64_t VA_APPLY_DEFAULT_LOADOUT = 0x1401544A0;

// Source: combatpatch.h / PC binary analysis
// EquipChassisCheck — JZ at this RVA skips weapon item substitution
// for net_standard_chassis actors. Patch JZ (0x74) -> JMP (0xEB).
static constexpr uint64_t VA_EQUIP_CHASSIS_CHECK = 0x140D0D1FB;

// Source: combatpatch.h / PC binary analysis
// WeaponGuardJZ — vtable actor-type check in WeaponSlotSetup
static constexpr uint64_t VA_WEAPON_GUARD_JZ = 0x140CF5F62;

// Source: combatpatch.h / PC binary analysis
// WeaponGuardJNZ — server_mode check in WeaponSlotSetup
static constexpr uint64_t VA_WEAPON_GUARD_JNZ = 0x140CF5F6B;

// Source: combatpatch.h / PC binary analysis
// HashFunc — CRC64 hash from entity name string
//   uint64_t __fastcall (char* name, int64_t seed, int registerFlag, int64_t maxLen, int)
static constexpr uint64_t VA_HASH_FUNC = 0x1400CE120;

// Source: combatpatch.h / PC binary analysis
// SymTableInsert — registers (hash, name) in gear symbol table
static constexpr uint64_t VA_SYM_TABLE_INSERT = 0x140C4C050;

// Source: combatpatch.h / PC binary analysis
// SetWeaponEntities — writes weapon entity names to data tree
static constexpr uint64_t VA_SET_WEAPON_ENTITIES = 0x140D42E00;

// --- Social Plugin DLL Selection ---

// Source: revault string search "pnsovr"/"pnsdemo", commit e1803da
// String data for social platform DLL selection at ~0x140109xxx.
// The game passes these to CModuleLoader (fcn.140606690).
// Overwriting forces all paths to load pnsrad.dll.
static constexpr uint64_t VA_SOCIAL_PLUGIN_STR_PNSOVR  = 0x1416d35c4;  // xref 0x140109993
static constexpr uint64_t VA_SOCIAL_PLUGIN_STR_PNSDEMO = 0x1416d35e8;  // xref 0x140109a2b

// Source: mode_patches.cpp PatchBypassOvrPlatform, commit e1803da
// OVR conditional branch in PlatformModuleDecisionAndInitialize.
// 6-byte JNE (0F 85 C7 00 00 00). NOP to skip OVR init and take Path 2.
static constexpr uint64_t VA_OVR_PLATFORM_BRANCH = 0x1401580e5;

} // namespace nevr::addresses

// Struct offsets within CR15NetGame
namespace nevr::net_game_offsets {
static constexpr uintptr_t kGameplayMT = 0x2aa8;
static constexpr uintptr_t kLobbyObj = 0x647c8;
static constexpr uintptr_t kSlotTable = 0x138;
} // namespace nevr::net_game_offsets

// Struct offsets within CR15NetGameplay
namespace nevr::gameplay_offsets {
static constexpr uintptr_t kMatchPhase = 0x0C;
static constexpr uintptr_t kLastScoreTeam = 0x10;
static constexpr uintptr_t kLastScorePoints = 0x12;
static constexpr uintptr_t kOwnerId = 0x5c;
static constexpr uintptr_t kAssistUserId = 0x68;
static constexpr uintptr_t kLastPersonToScore = 0x6c;
static constexpr uintptr_t kUserStatsArray = 0x498;
static constexpr uintptr_t kUserStatsCount = 0x4c8;
} // namespace nevr::gameplay_offsets

// SR15NetUserId sentinel
static constexpr uint32_t kInvalidUserId = 0xFFFFFFFF;

/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <atomic>
#include <string>

#include "address_registry.h"

namespace nevr::combat_patch {

// ===================================================================
// Combat Patch — enables combat weapons in arena mode
//
// Ported from standalone combatpatch.dll (v7 entity stubs) into
// the nevr-runtime plugin system.
//
// Strategy:
//   1. Hook IsCombatGameType/IsArenaGameType to lie about game mode
//   2. Patch JZ->JMP at chassis weapon-stripping check
//   3. NOP weapon guard conditional jumps
//   4. Hook PlayerInit/EquipmentInit to capture component pointers
//   5. Hook EntityLookup to inject weapon entity stubs in arena
//   6. Hook WeaponActorExpression with SEH crash protection
//   7. Hook body swap + combat weapons script DLLs for event dispatch
//   8. Reapply loadout after toggle via sub_1401544A0
// ===================================================================

// ---------------------------------------------------------------
// Plugin configuration
// ---------------------------------------------------------------
struct CombatPatchConfig {
    bool enabled     = true;
    bool auto_toggle = false;  // if true, enable combat on load without F9
};

CombatPatchConfig ParseConfig(const std::string& json_text);

// ---------------------------------------------------------------
// echovr.exe RVAs (relative to image base)
// ---------------------------------------------------------------
static constexpr uintptr_t kIsCombatGameTypeRVA   = 0x15C170;
static constexpr uintptr_t kIsArenaGameTypeRVA    = 0x155B20;
static constexpr uintptr_t kPlayerInitRVA         = 0xD625C0;
static constexpr uintptr_t kEquipChassisCheckRVA  = 0xD0D1FB;
static constexpr uintptr_t kWeaponGuardJZ_RVA     = 0xCF5F62;
static constexpr uintptr_t kWeaponGuardJNZ_RVA    = 0xCF5F6B;
static constexpr uintptr_t kEquipmentInitRVA      = 0xD02390;
static constexpr uintptr_t kWeaponActorExprRVA    = 0xF0BCF0;
static constexpr uintptr_t kEntityLookupRVA       = 0x4F3700;
static constexpr uintptr_t kApplyDefaultLoadoutRVA = 0x1544A0;
static constexpr uintptr_t kHashFuncRVA           = 0xCE120;
static constexpr uintptr_t kSymTableInsertRVA     = 0xC4C050;
static constexpr uintptr_t kSetWeaponEntitiesRVA  = 0xD42E00;

// Equip bypass byte values
static constexpr uint8_t kOrigByte_JZ  = 0x74;
static constexpr uint8_t kPatchByte_JMP = 0xEB;

// Weapon entity hashes (lobby gamespace)
static constexpr uint64_t kWeaponEntityHash1 = 0xCB9977F7FC2B4526ULL;
static constexpr uint64_t kWeaponEntityHash2 = 0xD09AFD15B1C75C04ULL;
static constexpr uint64_t kWeaponEntityHash3 = 0x6DAA00A6D33D44B7ULL;

// Player component offsets
static constexpr ptrdiff_t kIsCombatOffset = 360;  // 0x168

// Equipment component slot offsets
static constexpr ptrdiff_t kEquipWeaponHashOff  = 11016;
static constexpr ptrdiff_t kEquipAbilityHashOff = 11024;
static constexpr ptrdiff_t kEquipGrenadeHashOff = 11032;

// R15NetGame offsets
static constexpr ptrdiff_t kPlayerNetGameOffset    = 184;
static constexpr ptrdiff_t kNetGameUserCountOffset  = 222;
static constexpr ptrdiff_t kNetGameUserArrayBase    = 960;
static constexpr ptrdiff_t kNetGameUserEntryStride  = 592;
static constexpr ptrdiff_t kNetGameServerFlagOffset = 11680;

// Equipment slot navigation
static constexpr ptrdiff_t kNetGameEquipDataBase  = 256;
static constexpr ptrdiff_t kNetGameUserMapArray   = 208;
static constexpr int       kUserDataBlockStride   = 448;
static constexpr ptrdiff_t kUserDataSlotContainer = 368;
static constexpr ptrdiff_t kSlotContainerCountOff = 48;
static constexpr int       kSlotEntrySize         = 48;
static constexpr ptrdiff_t kSlotItemHash          = 16;
static constexpr ptrdiff_t kSlotFlagsOff          = 8;

// Entity stub constants
static constexpr size_t    kEntityStubSize       = 8192;
static constexpr ptrdiff_t kCompsysmapArrayOff   = 1120;
static constexpr ptrdiff_t kCompsysmapCountOff   = 1168;
static constexpr ptrdiff_t kCompsysmapDirtyOff   = 1176;
static constexpr size_t    kCompsysmapEntrySize  = 64;

// Body swap script DLL
static constexpr uint64_t  kBodySwapScriptHash      = 0x92481bf498600a78ULL;
static constexpr uintptr_t kScriptTickRVA            = 0x67B0;
static constexpr uintptr_t kScriptHandlerRVA         = 0xAFA0;

// Event hashes (body swap)
static constexpr uint64_t EVT_TOGGLE_PLAYER_ACTOR  = 0x5FB56A55E9D1D227ULL;
static constexpr uint64_t EVT_ENTERED_COMBAT_LAND  = 0xE0ED102827299A0AULL;
static constexpr uint64_t EVT_ENTERED_ARENA_LAND   = 0x003AECFF85E4E60AULL;

// Combat weapons script DLL
static constexpr uint64_t  kCombatWeaponsScriptHash  = 0xd44f62ba114f0cdeULL;
static constexpr uintptr_t kCombatWeaponsTickRVA     = 0x39C0;
static constexpr uintptr_t kCombatWeaponsHandlerRVA  = 0x5AC0;

// Event hashes (combat weapons)
static constexpr uint64_t EVT_DISABLE_CEASE_FIRE = 0x0EA31347147AD3C3ULL;
static constexpr uint64_t EVT_ENABLE_GUN         = 0x3A6D0346A13117DEULL;
static constexpr uint64_t EVT_ENABLE_TAC         = 0x3A6D0346A12203D3ULL;
static constexpr uint64_t EVT_ENABLE_GRENADE     = 0x0064DC7FC234322FULL;
static constexpr uint64_t EVT_ENABLE_GEAR        = 0xBFCE97C3DF1F3716ULL;

// ---------------------------------------------------------------
// SComponentEvent layout (56 bytes)
// ---------------------------------------------------------------
#pragma pack(push, 1)
struct SComponentEvent {
    uint64_t name;       // +0  event hash
    uint64_t actor;      // +8  actor entity symbol
    uint64_t nodeid;     // +16 node ID
    uint16_t poolidx;    // +24 pool index
    uint8_t  pad[6];     // +26 padding
    uint64_t gamespace;  // +32 gamespace symbol
    uint64_t component;  // +40 component symbol
    uint64_t userdata;   // +48 user data
};
#pragma pack(pop)
static_assert(sizeof(SComponentEvent) == 56, "SComponentEvent must be 56 bytes");

// ---------------------------------------------------------------
// Weapon entity stub
// ---------------------------------------------------------------
struct WeaponEntityStub {
    uint64_t  hash;
    void*     stubData;
    void*     compsysmapCopy;
    uint64_t  compsysmapCount;
    uintptr_t vtable;
    bool      created;
    bool      deepCopied;
};

// ---------------------------------------------------------------
// Function typedefs
// ---------------------------------------------------------------
typedef int64_t (__fastcall *GameTypeCheck_t)(int64_t);
typedef void    (__fastcall *PlayerInit_t)(uintptr_t);
typedef int     (__fastcall *ScriptTick_t)(int64_t);
typedef void    (__fastcall *ScriptHandler_t)(int64_t, SComponentEvent*);
typedef void    (__fastcall *ApplyDefaultLoadout_t)(int64_t, uint16_t);
typedef int     (__fastcall *EquipmentInit_t)(int64_t);
typedef uint64_t (__fastcall *HashFunc_t)(const char*, int64_t, int, int64_t, int);
typedef int64_t (__fastcall *SymTableInsert_t)(uint64_t, const char*);
typedef void*   (__fastcall *WeaponActorExpr_t)(void*, void*, int64_t);
typedef int64_t (__fastcall *EntityLookup_t)(int64_t, int64_t);

// ---------------------------------------------------------------
// Entity/gamespace tracking limits
// ---------------------------------------------------------------
static constexpr int kMaxTrackedGamespaces = 8;
struct EntityCacheEntry { uint64_t hash; int64_t ptr; };
static constexpr int kEntityCacheMax = 64;

// ---------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------
int  Initialize(uintptr_t base_addr, const char* config_path);
void OnFrame();
void Shutdown();

} // namespace nevr::combat_patch

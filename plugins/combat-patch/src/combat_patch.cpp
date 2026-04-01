/* SYNTHESIS -- custom tool code, not from binary */

#include "combat_patch.h"
#include "nevr_common.h"

#include <windows.h>
#include <MinHook.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
// setjmp.h removed — VEH+longjmp corrupts MinHook trampoline stacks

namespace nevr::combat_patch {

// ---------------------------------------------------------------
// Safe memory read helpers (replaces MSVC __try/__except)
//
// MinGW doesn't support __try/__except and longjmp from VEH
// corrupts the stack (causes STATUS_STACK_BUFFER_OVERRUN).
// Instead we validate pointers before reading.
// ---------------------------------------------------------------
static bool IsSafePtr(const void* ptr, size_t size = 8)
{
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr < 0x10000) return false;  // low addresses are never valid
    return !IsBadReadPtr(ptr, size);
}

static bool SafeMemcpy(void* dst, const void* src, size_t size)
{
    if (!IsSafePtr(src, size)) return false;
    memcpy(dst, src, size);
    return true;
}

// ---------------------------------------------------------------
// Logging — [NEVR.COMBAT] prefix
// ---------------------------------------------------------------
static void Log(const char* fmt, ...)
{
    char buf[512];
    int prefix_len = snprintf(buf, sizeof(buf), "[NEVR.COMBAT] ");

    va_list args;
    va_start(args, fmt);
    vsnprintf(buf + prefix_len, sizeof(buf) - prefix_len, fmt, args);
    va_end(args);

    fprintf(stderr, "%s", buf);
    fflush(stderr);
    OutputDebugStringA(buf);
}

// ---------------------------------------------------------------
// State
// ---------------------------------------------------------------
static uintptr_t g_GameBase = 0;
static std::atomic<bool> g_ForceCombat{false};
static std::atomic<uintptr_t> g_LocalPlayerPtr{0};
static std::atomic<int> g_PendingToggle{0};
static std::atomic<bool> g_ScriptHooked{false};
static CombatPatchConfig g_Config;

// Script DLL hook state
static GameTypeCheck_t Original_IsCombatGameType = nullptr;
static GameTypeCheck_t Original_IsArenaGameType  = nullptr;
static PlayerInit_t    Original_PlayerInit       = nullptr;
static ScriptTick_t    Original_ScriptTick       = nullptr;
static ScriptHandler_t g_ScriptHandler           = nullptr;

// Combat weapons script DLL state
static ScriptTick_t    Original_CombatWeaponsTick = nullptr;
static ScriptHandler_t g_CombatWeaponsHandler     = nullptr;
static std::atomic<bool> g_CombatWeaponsHooked{false};
static std::atomic<int>  g_PendingWeaponsToggle{0};

// Loadout reapplication
static ApplyDefaultLoadout_t g_ApplyDefaultLoadout = nullptr;
static std::atomic<int> g_PendingLoadoutReapply{0};

// Equipment reinit
static EquipmentInit_t Original_EquipmentInit = nullptr;
static std::atomic<int64_t> g_EquipmentComponentPtr{0};
static std::atomic<int> g_PendingEquipReinit{0};

// Data tree access functions
static HashFunc_t       g_HashFunc       = nullptr;
static SymTableInsert_t g_SymTableInsert = nullptr;

// WeaponActorExpression diagnostics
static WeaponActorExpr_t Original_WeaponActorExpr = nullptr;
static std::atomic<uint32_t> g_WeaponExprCallCount{0};

// EntityLookup
static EntityLookup_t Original_EntityLookup = nullptr;
static int64_t g_TrackedGamespaces[kMaxTrackedGamespaces] = {};
static int     g_TrackedGamespaceCount = 0;
static std::atomic<bool> g_PendingEntityDump{false};

// Entity cache
static EntityCacheEntry g_EntityCache[kEntityCacheMax] = {};
static int              g_EntityCacheCount = 0;

// Weapon entity stubs
static WeaponEntityStub g_WeaponStubs[3] = {};
static uintptr_t g_CapturedEntityVtable = 0;

// Script DLL hook retry state (for OnFrame)
static bool g_ScriptDLLSearchDone = false;
static int  g_ScriptDLLRetryCount = 0;
static DWORD g_ScriptDLLLastRetryTick = 0;
static constexpr int kScriptDLLMaxRetries = 60;
static constexpr DWORD kScriptDLLRetryIntervalMs = 2000;

// Hotkey state
static bool g_KeyWasDown9  = false;
static bool g_KeyWasDown10 = false;

// ---------------------------------------------------------------
// Config parsing (minimal JSON — matches other plugins)
// ---------------------------------------------------------------
CombatPatchConfig ParseConfig(const std::string& json_text)
{
    CombatPatchConfig cfg;
    if (json_text.empty()) return cfg;

    // Simple key-value extraction
    auto find_bool = [&](const char* key, bool& out) {
        auto pos = json_text.find(key);
        if (pos == std::string::npos) return;
        pos = json_text.find(':', pos);
        if (pos == std::string::npos) return;
        auto rest = json_text.substr(pos + 1, 20);
        if (rest.find("true") != std::string::npos) out = true;
        else if (rest.find("false") != std::string::npos) out = false;
    };

    find_bool("\"enabled\"", cfg.enabled);
    find_bool("\"auto_toggle\"", cfg.auto_toggle);
    return cfg;
}

// ---------------------------------------------------------------
// Utility: MinHook detour installer
// ---------------------------------------------------------------
static bool PatchDetour(PVOID* ppPointer, PVOID pDetour)
{
    void* pOriginal = nullptr;
    MH_STATUS status = MH_CreateHook(*ppPointer, pDetour, &pOriginal);
    if (status == MH_OK)
    {
        MH_EnableHook(*ppPointer);
        *ppPointer = pOriginal;
        return true;
    }
    Log("MH_CreateHook FAILED (status %d) for %p\n",
        (int)status, *ppPointer);
    return false;
}

// ---------------------------------------------------------------
// Hook 1 & 2: Game type classification overrides
// ---------------------------------------------------------------
static int64_t __fastcall Hook_IsCombatGameType(int64_t a1)
{
    if (g_ForceCombat.load(std::memory_order_relaxed))
        return 1;
    return Original_IsCombatGameType(a1);
}

static int64_t __fastcall Hook_IsArenaGameType(int64_t a1)
{
    if (g_ForceCombat.load(std::memory_order_relaxed))
        return 0;
    return Original_IsArenaGameType(a1);
}

// ---------------------------------------------------------------
// Hook 3: Player initialization — captures player component ptr
// ---------------------------------------------------------------
static void __fastcall Hook_PlayerInit(uintptr_t a1)
{
    Original_PlayerInit(a1);
    uintptr_t prev = g_LocalPlayerPtr.exchange(a1, std::memory_order_relaxed);

    if (g_ForceCombat.load(std::memory_order_relaxed))
        *reinterpret_cast<int32_t*>(a1 + kIsCombatOffset) = 1;

    if (prev != a1)
        Log("Player init @ 0x%llX\n", (unsigned long long)a1);
}

// ---------------------------------------------------------------
// Patch/unpatch the "net_standard_chassis" item substitution check
// ---------------------------------------------------------------
static void PatchEquipBypass(bool enable)
{
    if (!g_GameBase) return;

    uint8_t* addr = reinterpret_cast<uint8_t*>(
        g_GameBase + kEquipChassisCheckRVA);
    uint8_t target = enable ? kPatchByte_JMP : kOrigByte_JZ;

    if (*addr == target) return;

    DWORD oldProt;
    if (VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &oldProt))
    {
        *addr = target;
        VirtualProtect(addr, 1, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), addr, 1);
        Log("Equip chassis check %s (0x%llX: 0x%02X)\n",
            enable ? "BYPASSED" : "RESTORED",
            (unsigned long long)(uintptr_t)addr, target);
    }
    else
    {
        Log("VirtualProtect FAILED for equip patch (err=%u)\n",
            GetLastError());
    }
}

// ---------------------------------------------------------------
// Patch/unpatch the WeaponSlotSetup guard check
// ---------------------------------------------------------------
static void PatchWeaponGuard(bool enable)
{
    if (!g_GameBase) return;

    uint8_t* addrJZ  = reinterpret_cast<uint8_t*>(
        g_GameBase + kWeaponGuardJZ_RVA);
    uint8_t* addrJNZ = reinterpret_cast<uint8_t*>(
        g_GameBase + kWeaponGuardJNZ_RVA);

    DWORD oldProt;
    if (VirtualProtect(addrJZ, 16, PAGE_EXECUTE_READWRITE, &oldProt))
    {
        if (enable)
        {
            addrJZ[0]  = 0x90; addrJZ[1]  = 0x90;
            addrJNZ[0] = 0x90; addrJNZ[1] = 0x90;
        }
        else
        {
            addrJZ[0]  = 0x74; addrJZ[1]  = 0x16;
            addrJNZ[0] = 0x75; addrJNZ[1] = 0x0D;
        }
        VirtualProtect(addrJZ, 16, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), addrJZ, 16);
        Log("Weapon guard %s\n",
            enable ? "BYPASSED (NOP)" : "RESTORED");
    }
    else
    {
        Log("VirtualProtect FAILED for weapon guard (err=%u)\n",
            GetLastError());
    }
}

// ---------------------------------------------------------------
// Clear equipment slots to force full model reload
// ---------------------------------------------------------------
static void ClearEquipmentSlots(int64_t netGame, uint16_t userIndex)
{
    int64_t equipBase = *reinterpret_cast<int64_t*>(
        netGame + kNetGameEquipDataBase);
    int64_t userMap = *reinterpret_cast<int64_t*>(
        netGame + kNetGameUserMapArray);
    if (!equipBase || !userMap)
    {
        Log("ClearSlots: null equipBase or userMap\n");
        return;
    }

    uint16_t mappedIdx = *reinterpret_cast<uint16_t*>(
        userMap + 4 * userIndex);
    if (mappedIdx == 0xFFFF)
    {
        Log("ClearSlots: unmapped user %u\n", userIndex);
        return;
    }

    int64_t userData = equipBase +
        (int64_t)kUserDataBlockStride * mappedIdx;

    int64_t containerBase = userData + kUserDataSlotContainer;
    int64_t slotArray = *reinterpret_cast<int64_t*>(containerBase);
    uint64_t slotCount = *reinterpret_cast<uint64_t*>(
        containerBase + kSlotContainerCountOff);

    if (!slotArray || slotCount == 0 || slotCount > 64)
    {
        Log("ClearSlots: bad slot data (ptr=0x%llX, count=%llu)\n",
            (unsigned long long)slotArray, slotCount);
        return;
    }

    int cleared = 0;
    for (uint64_t i = 0; i < slotCount; i++)
    {
        int64_t entry = slotArray + kSlotEntrySize * (int64_t)i;
        int64_t curItem = *reinterpret_cast<int64_t*>(
            entry + kSlotItemHash);
        if (curItem != -1 && curItem != 0)
        {
            *reinterpret_cast<int64_t*>(entry + kSlotItemHash) = -1;
            *reinterpret_cast<int32_t*>(entry + kSlotFlagsOff) &= ~3;
            cleared++;
        }
    }

    Log("Cleared %d/%llu equipment slot(s) for user %u\n",
        cleared, slotCount, userIndex);
}

// ---------------------------------------------------------------
// Hook 6: EquipmentInit — captures R14NetPlayerEquipment ptr
// ---------------------------------------------------------------
static int __fastcall Hook_EquipmentInit(int64_t a1)
{
    int64_t prev = g_EquipmentComponentPtr.exchange(
        a1, std::memory_order_relaxed);
    if (prev != a1)
        Log("Equipment component captured: 0x%llX\n",
            (unsigned long long)a1);
    return Original_EquipmentInit(a1);
}

// ---------------------------------------------------------------
// Weapon entity stub creation
// ---------------------------------------------------------------
static int64_t GetOrCreateWeaponStub(int ci, uint64_t hash)
{
    WeaponEntityStub& stub = g_WeaponStubs[ci];

    if (stub.created && stub.stubData)
        return reinterpret_cast<int64_t>(stub.stubData);

    void* mem = VirtualAlloc(NULL, kEntityStubSize,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem)
    {
        Log("VirtualAlloc FAILED for stub hash 0x%016llX\n",
            (unsigned long long)hash);
        return 0;
    }
    memset(mem, 0, kEntityStubSize);

    if (g_CapturedEntityVtable)
    {
        *reinterpret_cast<uintptr_t*>(mem) = g_CapturedEntityVtable;
        Log("Created stub with captured vtable 0x%llX for hash 0x%016llX\n",
            (unsigned long long)g_CapturedEntityVtable,
            (unsigned long long)hash);
    }
    else
    {
        Log("Created zeroed stub (no vtable) for hash 0x%016llX\n",
            (unsigned long long)hash);
    }

    stub.hash = hash;
    stub.stubData = mem;
    stub.created = true;
    return reinterpret_cast<int64_t>(mem);
}

// ---------------------------------------------------------------
// Deep-copy weapon entity data from a live gamespace entity
// ---------------------------------------------------------------
static void DeepCopyWeaponEntity(int ci, int64_t entityPtr, uint64_t hash)
{
    WeaponEntityStub& stub = g_WeaponStubs[ci];

    // Capture vtable (first 8 bytes, always in .rdata)
    uintptr_t vtable = *reinterpret_cast<uintptr_t*>(entityPtr);
    if (vtable > g_GameBase && vtable < g_GameBase + 0x20000000)
    {
        g_CapturedEntityVtable = vtable;
    }

    if (!stub.stubData)
    {
        stub.stubData = VirtualAlloc(NULL, kEntityStubSize,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!stub.stubData) return;
    }

    // Copy main entity data block (validated pointer)
    if (!SafeMemcpy(stub.stubData, reinterpret_cast<void*>(entityPtr), kEntityStubSize))
    {
        Log("Failed to copy entity data for 0x%016llX (bad ptr)\n",
            (unsigned long long)hash);
        memset(stub.stubData, 0, kEntityStubSize);
        if (g_CapturedEntityVtable)
            *reinterpret_cast<uintptr_t*>(stub.stubData) =
                g_CapturedEntityVtable;
        stub.hash = hash;
        stub.created = true;
        stub.deepCopied = false;
        return;
    }

    // Copy the compsysmap array
    uint8_t* base = reinterpret_cast<uint8_t*>(stub.stubData);
    int64_t cmArrayPtr = *reinterpret_cast<int64_t*>(
        base + kCompsysmapArrayOff);
    uint64_t cmCount = *reinterpret_cast<uint64_t*>(
        base + kCompsysmapCountOff);

    if (cmArrayPtr && cmCount > 0 && cmCount < 1024)
    {
        size_t cmSize = cmCount * kCompsysmapEntrySize;
        void* cmCopy = VirtualAlloc(NULL, cmSize,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (cmCopy)
        {
            bool copyOk = SafeMemcpy(cmCopy,
                reinterpret_cast<void*>(cmArrayPtr), cmSize);

            if (copyOk)
            {
                *reinterpret_cast<int64_t*>(
                    base + kCompsysmapArrayOff) =
                    reinterpret_cast<int64_t>(cmCopy);

                if (stub.compsysmapCopy)
                    VirtualFree(stub.compsysmapCopy, 0, MEM_RELEASE);

                stub.compsysmapCopy = cmCopy;
                stub.compsysmapCount = cmCount;
                stub.deepCopied = true;

                Log("Deep-copied entity 0x%016llX: %llu compsysmap entries (%zu bytes)\n",
                    (unsigned long long)hash, cmCount, cmSize);
            }
            else
            {
                VirtualFree(cmCopy, 0, MEM_RELEASE);
                *reinterpret_cast<int64_t*>(
                    base + kCompsysmapArrayOff) = 0;
                *reinterpret_cast<uint64_t*>(
                    base + kCompsysmapCountOff) = 0;
                stub.deepCopied = false;
                Log("Compsysmap copy failed for 0x%016llX (zeroed out)\n",
                    (unsigned long long)hash);
            }
        }
    }
    else
    {
        Log("Entity 0x%016llX has %llu compsysmap entries (ptr=0x%llX)\n",
            (unsigned long long)hash, cmCount,
            (unsigned long long)cmArrayPtr);
    }

    // Clear dirty flag
    *reinterpret_cast<uint32_t*>(base + kCompsysmapDirtyOff) &= ~1u;

    stub.hash = hash;
    stub.created = true;

    uint64_t* d = reinterpret_cast<uint64_t*>(base);
    Log("Entity 0x%016llX data: [0]=0x%llX [1]=0x%llX\n",
        (unsigned long long)hash,
        (unsigned long long)d[0],
        (unsigned long long)d[1]);
}

// ---------------------------------------------------------------
// Hook 7: EntityLookup — hot-path gamespace capture + stub injection
// ---------------------------------------------------------------
static int64_t __fastcall Hook_EntityLookup(int64_t a1, int64_t a2)
{
    if (!a1) return Original_EntityLookup(a1, a2);

    int64_t gs = *reinterpret_cast<int64_t*>(a1 + 8);
    if (gs > 0)
    {
        bool found = false;
        for (int i = 0; i < g_TrackedGamespaceCount; i++)
            if (g_TrackedGamespaces[i] == gs) { found = true; break; }
        if (!found && g_TrackedGamespaceCount < kMaxTrackedGamespaces)
            g_TrackedGamespaces[g_TrackedGamespaceCount++] = gs;
    }

    int64_t  result = Original_EntityLookup(a1, a2);
    uint64_t hash   = static_cast<uint64_t>(a2);

    if (result != 0)
    {
        // Cache successful lookups
        bool found = false;
        for (int i = 0; i < g_EntityCacheCount; i++)
        {
            if (g_EntityCache[i].hash == hash)
            {
                g_EntityCache[i].ptr = result;
                found = true;
                break;
            }
        }
        if (!found && g_EntityCacheCount < kEntityCacheMax)
        {
            g_EntityCache[g_EntityCacheCount].hash = hash;
            g_EntityCache[g_EntityCacheCount].ptr  = result;
            g_EntityCacheCount++;
        }

        // Entity deep-copy and stub injection DISABLED
        // The deep copy + stub system uses memory layouts that are fragile
        // under Wine. Commented out for stability — re-enable once basic
        // combat toggle (IsCombatGameType + equip bypass) is verified.
        //
        // TODO: re-enable DeepCopyWeaponEntity + GetOrCreateWeaponStub
    }
    else
    {

        // Log remaining misses (throttled)
        static uint32_t s_MissCount = 0;
        uint32_t m = ++s_MissCount;
        if (m <= 30 || m % 1000 == 0)
        {
            const bool isKnown =
                hash == kWeaponEntityHash1 ||
                hash == kWeaponEntityHash2 ||
                hash == kWeaponEntityHash3;
            const bool inCombat =
                g_ForceCombat.load(std::memory_order_relaxed);
            if (isKnown || inCombat)
                Log("MISS #%u gs=0x%llX hash=0x%016llX%s\n",
                    m, (unsigned long long)gs,
                    (unsigned long long)hash,
                    isKnown ? " <-- WEAPON ENTITY" : "");
        }
    }

    return result;
}

// ---------------------------------------------------------------
// Hook 8: WeaponActorExpression with SEH crash protection
// ---------------------------------------------------------------
static void* __fastcall Hook_WeaponActorExpr(void* a1, void* a2, int64_t a3)
{
    uint64_t entityHash = *reinterpret_cast<uint64_t*>(a1);

    uint32_t count = g_WeaponExprCallCount.fetch_add(1,
        std::memory_order_relaxed) + 1;

    if (count <= 5 || count % 300 == 0)
    {
        Log("WeaponExpr #%u hash=0x%llX\n",
            count, (unsigned long long)entityHash);
    }

    // Call original — no crash guarding (VEH+longjmp corrupts stack)
    return Original_WeaponActorExpr(a1, a2, a3);
}

// ---------------------------------------------------------------
// Dump all entities in tracked gamespaces (F10 diagnostic)
// ---------------------------------------------------------------
static void DumpGamespaceEntities()
{
    if (g_TrackedGamespaceCount == 0)
    {
        Log("No gamespaces tracked yet. WeaponExpr calls: %u\n",
            g_WeaponExprCallCount.load(std::memory_order_relaxed));
        return;
    }

    Log("=== GAMESPACE DUMP (%d unique seen) ===\n",
        g_TrackedGamespaceCount);

    int64_t equipComp = g_EquipmentComponentPtr.load(
        std::memory_order_relaxed);
    if (equipComp)
    {
        int64_t wh = *reinterpret_cast<int64_t*>(
            equipComp + kEquipWeaponHashOff);
        int64_t ah = *reinterpret_cast<int64_t*>(
            equipComp + kEquipAbilityHashOff);
        int64_t gh = *reinterpret_cast<int64_t*>(
            equipComp + kEquipGrenadeHashOff);
        Log("EquipComp weapon=0x%llX ability=0x%llX grenade=0x%llX\n",
            (unsigned long long)wh,
            (unsigned long long)ah,
            (unsigned long long)gh);
    }

    for (int ci = 0; ci < 3; ci++)
    {
        const auto& stub = g_WeaponStubs[ci];
        Log("WeaponStub[%d] hash=0x%016llX created=%d deep=%d data=%p cmCount=%llu\n",
            ci, (unsigned long long)stub.hash,
            (int)stub.created, (int)stub.deepCopied,
            stub.stubData, stub.compsysmapCount);
    }
    Log("CapturedVtable=0x%llX\n",
        (unsigned long long)g_CapturedEntityVtable);

    for (int idx = 0; idx < g_TrackedGamespaceCount; idx++)
    {
        int64_t gs = g_TrackedGamespaces[idx];
        if (!IsSafePtr(reinterpret_cast<void*>(gs), 8192))
        {
            Log("GS[%d] @ 0x%llX not readable\n",
                idx, (unsigned long long)gs);
            continue;
        }

        int64_t  c1arr = *reinterpret_cast<int64_t*>(gs + 8016);
        uint64_t c1cnt = *reinterpret_cast<uint64_t*>(gs + 8064);
        int64_t  c2arr = *reinterpret_cast<int64_t*>(gs + 8080);
        uint64_t c2cnt = *reinterpret_cast<uint64_t*>(gs + 8128);
        int64_t  lnarr = *reinterpret_cast<int64_t*>(gs + 7904);
        uint64_t lncnt = *reinterpret_cast<uint64_t*>(gs + 7952);

        Log("GS[%d] @ 0x%llX  cache1=%llu  cache2=%llu  linear=%llu\n",
            idx, (unsigned long long)gs, c1cnt, c2cnt, lncnt);

        if (c1cnt > 0 && c1cnt < 50000 && c1arr)
        {
            uint64_t lim = c1cnt < 300 ? c1cnt : 300;
            for (uint64_t i = 0; i < lim; i++)
            {
                uint64_t hash = *reinterpret_cast<uint64_t*>(
                    c1arr + 16 * (int64_t)i);
                int64_t  ptr  = *reinterpret_cast<int64_t*>(
                    c1arr + 16 * (int64_t)i + 8);
                Log(" c1[%3llu] 0x%016llX -> 0x%llX\n",
                    i, (unsigned long long)hash,
                    (unsigned long long)ptr);
            }
            if (c1cnt > 300)
                Log(" ... (%llu more)\n", c1cnt - 300);
        }

        if (c2cnt > 0 && c2cnt < 50000 &&
            c2arr && c2arr != c1arr)
        {
            uint64_t lim = c2cnt < 300 ? c2cnt : 300;
            for (uint64_t i = 0; i < lim; i++)
            {
                uint64_t hash = *reinterpret_cast<uint64_t*>(
                    c2arr + 16 * (int64_t)i);
                int64_t  ptr  = *reinterpret_cast<int64_t*>(
                    c2arr + 16 * (int64_t)i + 8);
                Log(" c2[%3llu] 0x%016llX -> 0x%llX\n",
                    i, (unsigned long long)hash,
                    (unsigned long long)ptr);
            }
        }
    }

    Log("=== END DUMP ===\n");
}

// ---------------------------------------------------------------
// Hook 4: Body swap script DLL tick — game thread dispatch
// ---------------------------------------------------------------
static int __fastcall Hook_ScriptTick(int64_t scriptCtx)
{
    static uint32_t tickCount = 0;
    ++tickCount;
    if (tickCount == 1)
        Log("First body swap tick (ctx=0x%llX)\n",
            (unsigned long long)scriptCtx);

    int pending = g_PendingToggle.exchange(0, std::memory_order_acq_rel);
    if (pending != 0 && g_ScriptHandler)
    {
        SComponentEvent evt = {};

        if (pending == 1)
        {
            evt.name = EVT_ENTERED_COMBAT_LAND;
            g_ScriptHandler(scriptCtx, &evt);

            memset(&evt, 0, sizeof(evt));
            evt.name = EVT_TOGGLE_PLAYER_ACTOR;
            g_ScriptHandler(scriptCtx, &evt);
        }
        else
        {
            evt.name = EVT_ENTERED_ARENA_LAND;
            g_ScriptHandler(scriptCtx, &evt);

            memset(&evt, 0, sizeof(evt));
            evt.name = EVT_TOGGLE_PLAYER_ACTOR;
            g_ScriptHandler(scriptCtx, &evt);
        }

        Log("Events dispatched on game thread (pending=%d, tick=%u, ctx=0x%llX)\n",
            pending, tickCount, (unsigned long long)scriptCtx);
    }

    // Pending loadout reapply (delayed to let chassis swap settle)
    int reapply = g_PendingLoadoutReapply.load(std::memory_order_relaxed);
    if (reapply > 0)
    {
        reapply--;
        if (reapply == 0 && g_ApplyDefaultLoadout)
        {
            uintptr_t playerPtr = g_LocalPlayerPtr.load(
                std::memory_order_relaxed);
            if (playerPtr)
            {
                int64_t netGame = *reinterpret_cast<int64_t*>(
                    playerPtr + kPlayerNetGameOffset);
                if (netGame)
                {
                    int64_t flagsPtr = *reinterpret_cast<int64_t*>(
                        netGame + kNetGameServerFlagOffset);
                    if (flagsPtr &&
                        (*reinterpret_cast<int32_t*>(flagsPtr) & 4) == 0)
                    {
                        uint16_t userCount =
                            *reinterpret_cast<uint16_t*>(
                                netGame + kNetGameUserCountOffset);
                        int applied = 0;
                        for (uint16_t i = 0;
                             i < userCount && i < 16; i++)
                        {
                            int64_t userEntry =
                                *reinterpret_cast<int64_t*>(
                                    netGame + kNetGameUserArrayBase +
                                    kNetGameUserEntryStride * i);
                            if (userEntry)
                            {
                                ClearEquipmentSlots(netGame, i);
                                g_ApplyDefaultLoadout(netGame, i);
                                ++applied;
                            }
                        }
                        Log("Loadout reapplied for %d user(s)\n", applied);
                    }
                    else
                    {
                        Log("Skipping loadout reapply (server mode or null flags)\n");
                    }
                }
            }
        }
        g_PendingLoadoutReapply.store(reapply,
            std::memory_order_release);
    }

    // Pending equipment reinit
    int reinit = g_PendingEquipReinit.load(std::memory_order_relaxed);
    if (reinit > 0)
    {
        reinit--;
        if (reinit == 0)
        {
            int64_t equipComp = g_EquipmentComponentPtr.load(
                std::memory_order_relaxed);
            if (equipComp && Original_EquipmentInit)
            {
                int result = Original_EquipmentInit(equipComp);
                Log("Equipment re-initialized (comp=0x%llX, result=%d)\n",
                    (unsigned long long)equipComp, result);

                {
                    int64_t curWeapon = *reinterpret_cast<int64_t*>(
                        equipComp + kEquipWeaponHashOff);
                    int64_t curAbility = *reinterpret_cast<int64_t*>(
                        equipComp + kEquipAbilityHashOff);
                    int64_t curGrenade = *reinterpret_cast<int64_t*>(
                        equipComp + kEquipGrenadeHashOff);
                    Log("Weapon slot hashes after reinit: weapon=0x%llX ability=0x%llX grenade=0x%llX\n",
                        (unsigned long long)curWeapon,
                        (unsigned long long)curAbility,
                        (unsigned long long)curGrenade);

                    bool wEmpty = (curWeapon  == 0 ||
                                   curWeapon  == int64_t(-1));
                    bool aEmpty = (curAbility == int64_t(-1));
                    bool gEmpty = (curGrenade == int64_t(-1));
                    if (wEmpty || aEmpty || gEmpty)
                    {
                        if (wEmpty)
                            *reinterpret_cast<int64_t*>(
                                equipComp + kEquipWeaponHashOff) =
                                int64_t(kWeaponEntityHash1);
                        if (aEmpty)
                            *reinterpret_cast<int64_t*>(
                                equipComp + kEquipAbilityHashOff) =
                                int64_t(kWeaponEntityHash2);
                        if (gEmpty)
                            *reinterpret_cast<int64_t*>(
                                equipComp + kEquipGrenadeHashOff) =
                                int64_t(kWeaponEntityHash3);
                        Log("Injected lobby weapon entity hashes into equip slots\n");
                    }
                }
            }
            else
            {
                Log("Cannot reinit equipment (comp=0x%llX, func=%p)\n",
                    (unsigned long long)equipComp,
                    Original_EquipmentInit);
            }
        }
        g_PendingEquipReinit.store(reinit,
            std::memory_order_release);
    }

    // F10 entity dump (game thread)
    if (g_PendingEntityDump.exchange(false,
            std::memory_order_acq_rel))
        DumpGamespaceEntities();

    int result = Original_ScriptTick(scriptCtx);

    // Always return >= 1 so the engine keeps calling update
    return result > 0 ? result : 1;
}

// ---------------------------------------------------------------
// Hook 5: Combat weapons script DLL tick
// ---------------------------------------------------------------
static int __fastcall Hook_CombatWeaponsTick(int64_t scriptCtx)
{
    static uint32_t tickCount = 0;
    ++tickCount;
    if (tickCount == 1)
        Log("Combat weapons first tick (ctx=0x%llX)\n",
            (unsigned long long)scriptCtx);

    int pending = g_PendingWeaponsToggle.exchange(0, std::memory_order_acq_rel);
    if (pending == 1 && g_CombatWeaponsHandler)
    {
        SComponentEvent evt = {};

        evt.name = EVT_DISABLE_CEASE_FIRE;
        g_CombatWeaponsHandler(scriptCtx, &evt);

        memset(&evt, 0, sizeof(evt));
        evt.name = EVT_ENABLE_GUN;
        g_CombatWeaponsHandler(scriptCtx, &evt);

        memset(&evt, 0, sizeof(evt));
        evt.name = EVT_ENABLE_TAC;
        g_CombatWeaponsHandler(scriptCtx, &evt);

        memset(&evt, 0, sizeof(evt));
        evt.name = EVT_ENABLE_GRENADE;
        g_CombatWeaponsHandler(scriptCtx, &evt);

        memset(&evt, 0, sizeof(evt));
        evt.name = EVT_ENABLE_GEAR;
        g_CombatWeaponsHandler(scriptCtx, &evt);

        Log("Weapons enabled + cease-fire disabled (tick=%u, ctx=0x%llX)\n",
            tickCount, (unsigned long long)scriptCtx);
    }

    int result = Original_CombatWeaponsTick(scriptCtx);
    return result > 0 ? result : 1;
}

// ---------------------------------------------------------------
// Scan a module's code section for a 64-bit constant
// ---------------------------------------------------------------
static bool ModuleContainsConstant(uintptr_t base, uint64_t target)
{
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++)
    {
        uintptr_t start = base + sec->VirtualAddress;
        uintptr_t end   = start + sec->Misc.VirtualSize;
        if (end - start < 8) continue;

        for (uintptr_t addr = start; addr <= end - 8; addr++)
        {
            if (*reinterpret_cast<uint64_t*>(addr) == target)
                return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------
// Find and hook the body swap script DLL
// ---------------------------------------------------------------
static bool HookScriptDLL()
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (hSnap == INVALID_HANDLE_VALUE)
    {
        Log("CreateToolhelp32Snapshot failed (err=%u)\n",
            GetLastError());
        return false;
    }

    wchar_t decimalName[32];
    swprintf_s(decimalName, L"%llu", kBodySwapScriptHash);

    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    uintptr_t dllBase = 0;
    int moduleCount = 0;

    Log("Searching for body swap DLL...\n");

    if (Module32FirstW(hSnap, &me))
    {
        do
        {
            moduleCount++;

            if (wcsstr(me.szModule, L"92481bf498600a78") != nullptr)
            {
                Log("FOUND by hex name: %ls @ %p\n",
                    me.szModule, me.modBaseAddr);
                dllBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }

            if (wcsstr(me.szModule, decimalName) != nullptr)
            {
                Log("FOUND by decimal name: %ls @ %p\n",
                    me.szModule, me.modBaseAddr);
                dllBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }

            HMODULE hMod = reinterpret_cast<HMODULE>(me.modBaseAddr);
            FARPROC proc = GetProcAddress(hMod, "setup_bindings");
            if (proc)
            {
                uintptr_t modBase = reinterpret_cast<uintptr_t>(
                    me.modBaseAddr);
                Log("Script DLL candidate: %ls @ %p (has setup_bindings)\n",
                    me.szModule, me.modBaseAddr);

                if (ModuleContainsConstant(modBase, EVT_TOGGLE_PLAYER_ACTOR))
                {
                    Log("CONFIRMED body swap DLL by event hash match!\n");
                    dllBase = modBase;
                    break;
                }
                else
                {
                    Log("  -> Not body swap (no event hash match)\n");
                }
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);

    Log("Scanned %d modules\n", moduleCount);

    if (!dllBase)
    {
        Log("Body swap DLL not found yet\n");
        return false;
    }

    Log("Body swap DLL base: 0x%llX\n", (unsigned long long)dllBase);

    g_ScriptHandler = reinterpret_cast<ScriptHandler_t>(
        dllBase + kScriptHandlerRVA);
    Log("sendevent handler @ 0x%llX (RVA 0x%X)\n",
        (unsigned long long)(dllBase + kScriptHandlerRVA),
        (unsigned)kScriptHandlerRVA);

    Original_ScriptTick = reinterpret_cast<ScriptTick_t>(
        dllBase + kScriptTickRVA);
    if (!PatchDetour(
            reinterpret_cast<PVOID*>(&Original_ScriptTick),
            reinterpret_cast<PVOID>(Hook_ScriptTick)))
    {
        Log("FAILED to hook script tick!\n");
        return false;
    }

    Log("Script tick hooked @ RVA 0x%X\n", (unsigned)kScriptTickRVA);

    g_ScriptHooked.store(true, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------
// Find and hook the combat weapons script DLL
// ---------------------------------------------------------------
static bool HookCombatWeaponsDLL()
{
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return false;

    wchar_t decimalName[32];
    swprintf_s(decimalName, L"%llu", kCombatWeaponsScriptHash);

    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    uintptr_t dllBase = 0;

    Log("Searching for combat weapons DLL...\n");

    if (Module32FirstW(hSnap, &me))
    {
        do
        {
            if (wcsstr(me.szModule, L"d44f62ba114f0cde") != nullptr)
            {
                Log("Weapons DLL FOUND by hex: %ls @ %p\n",
                    me.szModule, me.modBaseAddr);
                dllBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }

            if (wcsstr(me.szModule, decimalName) != nullptr)
            {
                Log("Weapons DLL FOUND by dec: %ls @ %p\n",
                    me.szModule, me.modBaseAddr);
                dllBase = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                break;
            }

            HMODULE hMod = reinterpret_cast<HMODULE>(me.modBaseAddr);
            FARPROC proc = GetProcAddress(hMod, "setup_bindings");
            if (proc)
            {
                uintptr_t modBase = reinterpret_cast<uintptr_t>(
                    me.modBaseAddr);
                if (ModuleContainsConstant(modBase, EVT_ENABLE_GUN))
                {
                    Log("Weapons DLL CONFIRMED by event hash: %ls @ %p\n",
                        me.szModule, me.modBaseAddr);
                    dllBase = modBase;
                    break;
                }
            }
        } while (Module32NextW(hSnap, &me));
    }
    CloseHandle(hSnap);

    if (!dllBase)
    {
        Log("Combat weapons DLL not found\n");
        return false;
    }

    g_CombatWeaponsHandler = reinterpret_cast<ScriptHandler_t>(
        dllBase + kCombatWeaponsHandlerRVA);
    Log("Weapons handler @ 0x%llX (RVA 0x%X)\n",
        (unsigned long long)(dllBase + kCombatWeaponsHandlerRVA),
        (unsigned)kCombatWeaponsHandlerRVA);

    Original_CombatWeaponsTick = reinterpret_cast<ScriptTick_t>(
        dllBase + kCombatWeaponsTickRVA);
    if (!PatchDetour(
            reinterpret_cast<PVOID*>(&Original_CombatWeaponsTick),
            reinterpret_cast<PVOID>(Hook_CombatWeaponsTick)))
    {
        Log("FAILED to hook weapons tick!\n");
        return false;
    }

    Log("Combat weapons DLL hooked (tick @ RVA 0x%X)\n",
        (unsigned)kCombatWeaponsTickRVA);
    g_CombatWeaponsHooked.store(true, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------
// Install echovr.exe hooks
// ---------------------------------------------------------------
static bool InstallHooks()
{
    Log("Game base: 0x%llX\n", (unsigned long long)g_GameBase);

    Original_IsCombatGameType = reinterpret_cast<GameTypeCheck_t>(
        g_GameBase + kIsCombatGameTypeRVA);
    PatchDetour(
        reinterpret_cast<PVOID*>(&Original_IsCombatGameType),
        reinterpret_cast<PVOID>(Hook_IsCombatGameType));

    Original_IsArenaGameType = reinterpret_cast<GameTypeCheck_t>(
        g_GameBase + kIsArenaGameTypeRVA);
    PatchDetour(
        reinterpret_cast<PVOID*>(&Original_IsArenaGameType),
        reinterpret_cast<PVOID>(Hook_IsArenaGameType));

    Original_PlayerInit = reinterpret_cast<PlayerInit_t>(
        g_GameBase + kPlayerInitRVA);
    PatchDetour(
        reinterpret_cast<PVOID*>(&Original_PlayerInit),
        reinterpret_cast<PVOID>(Hook_PlayerInit));

    g_ApplyDefaultLoadout = reinterpret_cast<ApplyDefaultLoadout_t>(
        g_GameBase + kApplyDefaultLoadoutRVA);
    Log("ApplyDefaultLoadout @ 0x%llX\n",
        (unsigned long long)(g_GameBase + kApplyDefaultLoadoutRVA));

    Original_EquipmentInit = reinterpret_cast<EquipmentInit_t>(
        g_GameBase + kEquipmentInitRVA);
    PatchDetour(
        reinterpret_cast<PVOID*>(&Original_EquipmentInit),
        reinterpret_cast<PVOID>(Hook_EquipmentInit));
    Log("EquipmentInit hooked @ 0x%llX\n",
        (unsigned long long)(g_GameBase + kEquipmentInitRVA));

    Original_WeaponActorExpr = reinterpret_cast<WeaponActorExpr_t>(
        g_GameBase + kWeaponActorExprRVA);
    PatchDetour(
        reinterpret_cast<PVOID*>(&Original_WeaponActorExpr),
        reinterpret_cast<PVOID>(Hook_WeaponActorExpr));
    Log("WeaponActorExpr hooked @ 0x%llX\n",
        (unsigned long long)(g_GameBase + kWeaponActorExprRVA));

    Original_EntityLookup = reinterpret_cast<EntityLookup_t>(
        g_GameBase + kEntityLookupRVA);
    PatchDetour(
        reinterpret_cast<PVOID*>(&Original_EntityLookup),
        reinterpret_cast<PVOID>(Hook_EntityLookup));
    Log("EntityLookup hooked @ 0x%llX\n",
        (unsigned long long)(g_GameBase + kEntityLookupRVA));

    g_HashFunc = reinterpret_cast<HashFunc_t>(
        g_GameBase + kHashFuncRVA);
    g_SymTableInsert = reinterpret_cast<SymTableInsert_t>(
        g_GameBase + kSymTableInsertRVA);
    Log("Hash=0x%llX SymInsert=0x%llX\n",
        (unsigned long long)(g_GameBase + kHashFuncRVA),
        (unsigned long long)(g_GameBase + kSymTableInsertRVA));

    Log("echovr.exe hooks installed\n");
    return true;
}

// ---------------------------------------------------------------
// Toggle combat mode
// ---------------------------------------------------------------
static void ToggleCombat()
{
    if (!g_ScriptHooked.load(std::memory_order_acquire))
    {
        Log("Script DLL not hooked yet, ignoring toggle\n");
        return;
    }

    bool newState = !g_ForceCombat.load(std::memory_order_relaxed);
    g_ForceCombat.store(newState, std::memory_order_relaxed);

    uintptr_t player = g_LocalPlayerPtr.load(std::memory_order_relaxed);
    if (player != 0)
    {
        *reinterpret_cast<int32_t*>(player + kIsCombatOffset) =
            newState ? 1 : 0;
    }

    g_PendingToggle.store(newState ? 1 : 2, std::memory_order_release);

    PatchEquipBypass(newState);
    PatchWeaponGuard(newState);

    if (g_CombatWeaponsHooked.load(std::memory_order_acquire) && newState)
        g_PendingWeaponsToggle.store(1, std::memory_order_release);

    Log("Toggle -> %s (queued for game thread)\n",
        newState ? "COMBAT" : "ARENA");
}

// ===============================================================
// Plugin lifecycle
// ===============================================================

int Initialize(uintptr_t base_addr, const char* config_path)
{
    g_GameBase = base_addr;

    // Load config
    if (config_path)
    {
        std::string json = nevr::LoadConfigFile(config_path);
        if (!json.empty())
            g_Config = ParseConfig(json);
    }

    if (!g_Config.enabled)
    {
        Log("Plugin disabled by config\n");
        return 0;
    }

    Log("Initializing (base=0x%llX, auto_toggle=%d)\n",
        (unsigned long long)base_addr, (int)g_Config.auto_toggle);

    // MH_Initialize() is already called by the host — do NOT call it again

    // VEH crash protection removed — longjmp across MinHook trampolines
    // causes STATUS_STACK_BUFFER_OVERRUN. Using IsBadReadPtr validation instead.

    // Install echovr.exe hooks immediately
    InstallHooks();

    // If auto_toggle, enable combat on load
    if (g_Config.auto_toggle)
    {
        g_ForceCombat.store(true, std::memory_order_relaxed);
        PatchEquipBypass(true);
        PatchWeaponGuard(true);
        Log("Auto-toggle: combat enabled on load\n");
    }

    Log("Initialization complete. F9=toggle combat, F10=dump entities.\n");
    return 0;
}

void OnFrame()
{
    if (!g_Config.enabled) return;

    // --- Script DLL hook retry (one-time, with interval) ---
    if (!g_ScriptDLLSearchDone)
    {
        DWORD now = GetTickCount();
        if (now - g_ScriptDLLLastRetryTick >= kScriptDLLRetryIntervalMs)
        {
            g_ScriptDLLLastRetryTick = now;

            bool bodyswapDone = g_ScriptHooked.load(std::memory_order_acquire);
            bool weaponsDone  = g_CombatWeaponsHooked.load(std::memory_order_acquire);

            if (!bodyswapDone)
                bodyswapDone = HookScriptDLL();
            if (!weaponsDone)
                weaponsDone = HookCombatWeaponsDLL();

            if (bodyswapDone && weaponsDone)
            {
                g_ScriptDLLSearchDone = true;

                // If auto_toggle and script is now hooked, queue the toggle events
                if (g_Config.auto_toggle && g_ForceCombat.load(std::memory_order_relaxed))
                {
                    g_PendingToggle.store(1, std::memory_order_release);
                    if (g_CombatWeaponsHooked.load(std::memory_order_acquire))
                        g_PendingWeaponsToggle.store(1, std::memory_order_release);
                }
            }
            else
            {
                g_ScriptDLLRetryCount++;
                if (g_ScriptDLLRetryCount >= kScriptDLLMaxRetries)
                {
                    g_ScriptDLLSearchDone = true;
                    if (!bodyswapDone)
                        Log("GAVE UP searching for body swap DLL\n");
                    if (!weaponsDone)
                        Log("WARNING: Combat weapons DLL not found - cease-fire/gun events unavailable\n");
                }
                else if (g_ScriptDLLRetryCount % 10 == 0)
                {
                    Log("Script DLL search attempt %d/%d (bodyswap=%d, weapons=%d)\n",
                        g_ScriptDLLRetryCount, kScriptDLLMaxRetries,
                        (int)bodyswapDone, (int)weaponsDone);
                }
            }
        }
    }

    // --- F9/F10 hotkey polling ---
    bool key9  = (GetAsyncKeyState(VK_F9)  & 0x8000) != 0;
    bool key10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;

    if (key9 && !g_KeyWasDown9)
        ToggleCombat();

    if (key10 && !g_KeyWasDown10)
    {
        Log("F10: entity dump queued (WeaponExpr calls so far: %u)\n",
            g_WeaponExprCallCount.load());
        g_PendingEntityDump.store(true, std::memory_order_release);
    }

    g_KeyWasDown9  = key9;
    g_KeyWasDown10 = key10;
}

void Shutdown()
{
    Log("Shutting down\n");

    // Restore patches if active
    if (g_ForceCombat.load(std::memory_order_relaxed))
    {
        PatchEquipBypass(false);
        PatchWeaponGuard(false);
    }

    // MH_DisableHook/MH_Uninitialize handled by the host

    // Remove VEH
    // VEH removed — no cleanup needed

    // Free weapon entity stubs
    for (int i = 0; i < 3; i++)
    {
        if (g_WeaponStubs[i].compsysmapCopy)
            VirtualFree(g_WeaponStubs[i].compsysmapCopy, 0, MEM_RELEASE);
        if (g_WeaponStubs[i].stubData)
            VirtualFree(g_WeaponStubs[i].stubData, 0, MEM_RELEASE);
        g_WeaponStubs[i] = {};
    }

    Log("Shutdown complete\n");
}

} // namespace nevr::combat_patch

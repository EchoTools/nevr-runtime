/* SYNTHESIS -- custom tool code, not from binary */

#include "level_detect.h"

#include <MinHook.h>
#include <atomic>
#include <cstdint>

#include "address_registry.h"
#include "nevr_common.h"
#include "combat_log.h"

namespace combat_mod {

static constexpr uint64_t HASH_MPL_ARENACOMBAT = 0xDB696852ED977BC0ULL;

static std::atomic<bool> g_is_arena_combat{false};

typedef __int64 (__fastcall *LevelLoad_t)(void* a1, uint64_t levelHash, void* a3);
static LevelLoad_t orig_LevelLoad = nullptr;

static __int64 __fastcall Hook_LevelLoad(void* a1, uint64_t levelHash, void* a3) {
    if (levelHash == HASH_MPL_ARENACOMBAT)
        g_is_arena_combat.store(true, std::memory_order_release);

    combat_mod::PluginLog( "[level_detect] Loading level 0x%llX%s",
        static_cast<unsigned long long>(levelHash),
        (levelHash == HASH_MPL_ARENACOMBAT) ? " *** mpl_arenacombat ***" : "");

    return orig_LevelLoad(a1, levelHash, a3);
}

bool IsArenaCombat() {
    return g_is_arena_combat.load(std::memory_order_acquire);
}

void InstallLevelDetect(uintptr_t base, nevr::HookManager& hooks) {
    void* target = nevr::ResolveVA(base, nevr::addresses::VA_LEVEL_LOAD);

    MH_STATUS status = MH_CreateHook(target,
                                      reinterpret_cast<void*>(&Hook_LevelLoad),
                                      reinterpret_cast<void**>(&orig_LevelLoad));
    if (status != MH_OK) {
        combat_mod::PluginLog(
            "[level_detect] MH_CreateHook failed for LevelLoad: %d", status);
        return;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK) {
        combat_mod::PluginLog(
            "[level_detect] MH_EnableHook failed: %d", status);
        return;
    }

    hooks.Track(target);
    combat_mod::PluginLog(
        "[level_detect] Hooked LevelLoad @ VA 0x%llX",
        static_cast<unsigned long long>(nevr::addresses::VA_LEVEL_LOAD));
}

} // namespace combat_mod

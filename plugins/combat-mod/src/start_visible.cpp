/*
 * start_visible.cpp — Respect the StartVisible flag on cloned models.
 *
 * The engine's InitModelCI unconditionally calls SetVisible(0) on every
 * model at creation. The StartVisible flag at CModelCR model_entry+24
 * bit 4 (0x10) exists in the data but is ignored at runtime.
 *
 * Hooks InitModelCI and CNode3D::SetVisible. When a model is being
 * initialized and SetVisible(0) is called, checks the StartVisible flag.
 * If set, overrides to SetVisible(1).
 */

#include "start_visible.h"

#include <windows.h>
#include <MinHook.h>
#include <cstdint>

#include "safe_memory.h"
#include "plugin_log.h"
#include "nevr_common.h"
#include "address_registry.h"

namespace {

/* Clone actor hashes that need StartVisible enforcement */
constexpr uint64_t CLONE_ACTOR_HASH = 0x8B0FAB9AD54F8DAFULL;
constexpr uint64_t CLONE_CHILD_HASH = 0xD898546D8EC2AA38ULL;

/* Function types */
using FnInitModelCI = int64_t(__fastcall*)(int64_t, int64_t, void*, void*, int, int, int64_t);
using FnCNodeSetVisible = void(__fastcall*)(int64_t, uint32_t);

static FnInitModelCI o_InitModelCI = nullptr;
static FnCNodeSetVisible o_CNodeSetVisible = nullptr;

/* Per-thread context: are we inside InitModelCI? */
static thread_local bool g_InInit = false;
static thread_local bool g_ForceVisible = false;

static void __fastcall Hook_CNodeSetVisible(int64_t node, uint32_t visible) {
    if (g_InInit && visible == 0 && g_ForceVisible) {
        visible = 1;
    }
    o_CNodeSetVisible(node, visible);
}

static int64_t __fastcall Hook_InitModelCI(int64_t a1, int64_t a2, void* a3,
                                            void* a4, int a5, int a6, int64_t a7) {
    bool force = false;

    /* Read actor_hash from CModelCR base entry array.
     * Uses SafeReadU16/SafeReadU64 instead of __try/__except. */
    uint16_t next_handle = 0;
    if (nevr::SafeReadU16(static_cast<uintptr_t>(a1 + 0xC8 + 50), &next_handle)) {
        uint16_t cur_handle = next_handle - 1;

        uint64_t data_ptr = 0;
        if (nevr::SafeReadU64(static_cast<uintptr_t>(a1 + 184), &data_ptr) && data_ptr) {
            uint64_t base_array = 0;
            if (nevr::SafeReadU64(static_cast<uintptr_t>(data_ptr), &base_array) && base_array) {
                uint64_t actor_hash = 0;
                if (nevr::SafeReadU64(
                        static_cast<uintptr_t>(base_array + 24ULL * cur_handle + 8),
                        &actor_hash)) {
                    if (actor_hash == CLONE_ACTOR_HASH || actor_hash == CLONE_CHILD_HASH) {
                        force = true;
                    }
                }
            }
        }
    }

    g_InInit = true;
    g_ForceVisible = force;

    int64_t result = o_InitModelCI(a1, a2, a3, a4, a5, a6, a7);

    g_InInit = false;
    g_ForceVisible = false;

    return result;
}

} // anonymous namespace

namespace combat_mod {

void InstallStartVisible(uintptr_t base, std::vector<void*>& hooks) {
    void* t1 = nevr::ResolveVA(base, nevr::addresses::VA_INIT_MODEL_CI);
    if (MH_CreateHook(t1, reinterpret_cast<void*>(Hook_InitModelCI),
                       reinterpret_cast<void**>(&o_InitModelCI)) == MH_OK) {
        MH_EnableHook(t1);
        hooks.push_back(t1);
        combat_mod::PluginLog( "InitModelCI hooked (StartVisible)");
    }

    void* t2 = nevr::ResolveVA(base, nevr::addresses::VA_CNODE3D_SET_VISIBLE);
    if (MH_CreateHook(t2, reinterpret_cast<void*>(Hook_CNodeSetVisible),
                       reinterpret_cast<void**>(&o_CNodeSetVisible)) == MH_OK) {
        MH_EnableHook(t2);
        hooks.push_back(t2);
        combat_mod::PluginLog( "CNode3D::SetVisible hooked (StartVisible)");
    }
}

} // namespace combat_mod

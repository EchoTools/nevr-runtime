/* SYNTHESIS -- custom tool code, not from binary */

#include "nevr_plugin_interface.h"
#include "nevr_common.h"
#include "address_registry.h"
#include "hook_manager.h"
#include "safe_memory.h"

#include <MinHook.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>

static nevr::HookManager g_hooks;
static uintptr_t g_base_addr = 0;

static void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[anim_debugger] ");
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

// =========================================================================
// Function pointers (originals + resolved helpers)
// =========================================================================

// CCharacterAnimationCS::InitRootEvaluator @ 0x140340280
using InitRootEvaluator_fn = int(__fastcall*)(void* thisptr, uint64_t handle);
static InitRootEvaluator_fn orig_InitRootEvaluator = nullptr;

// CAnimTree::Create @ 0x140fb4640 (Quest: 0x26506b8)
// Loads AnimSet resource, binary-searches for tree name, unpacks evaluator.
// Signature: uint64_t __fastcall (void* spec, int64_t rig_data,
//     void** bindings, void** env, void** out_tree, uint32_t flags)
using AnimTreeCreate_fn = uint64_t(__fastcall*)(
    void* spec, int64_t rig_data, void** bindings, void** env,
    void** out_tree, uint32_t flags);
static AnimTreeCreate_fn orig_AnimTreeCreate = nullptr;

// CAnimationCRI init @ 0x14027b6f0
// Creates CAnimationCRI, calls LoadAndInit, binds skeleton.
// Signature: int __fastcall (void* factory, void* mem_block,
//     uint64_t resource_hash, int64_t component_data)
using AnimCRIInit_fn = int(__fastcall*)(
    void* factory, void* mem_block, uint64_t resource_hash, int64_t component_data);
static AnimCRIInit_fn orig_AnimCRIInit = nullptr;

// CResourceID::GetName -- resolves CSymbol64 hash to const char*
using CResourceID_GetName_fn = const char*(__fastcall*)(int64_t* symbol);
static CResourceID_GetName_fn fn_GetName = nullptr;

// VAs for the new hooks
static constexpr uint64_t VA_ANIM_TREE_CREATE = 0x140fb4640;
static constexpr uint64_t VA_ANIM_CRI_INIT    = 0x14027b6f0;

// =========================================================================
// Safe read helpers
// =========================================================================

static bool ReadI64(const void* addr, int64_t* out) {
    return nevr::SafeReadU64(reinterpret_cast<uintptr_t>(addr),
                             reinterpret_cast<uint64_t*>(out));
}

static bool ReadU16(const void* addr, uint16_t* out) {
    return nevr::SafeReadU16(reinterpret_cast<uintptr_t>(addr), out);
}

static bool ReadU32(const void* addr, uint32_t* out) {
    return nevr::SafeMemcpy(out, addr, sizeof(uint32_t));
}

static bool ReadBytes(const void* addr, void* buf, size_t len) {
    return nevr::SafeMemcpy(buf, addr, len);
}

// Resolve a CSymbol64 to a string, returning "<unknown>" on failure.
static const char* SymName(int64_t sym) {
    if (sym == -1 || sym == 0 || fn_GetName == nullptr) return "<unknown>";
    return fn_GetName(&sym);
}

// =========================================================================
// Hex dump helper
// =========================================================================

static void HexDump(const char* label, const void* addr, size_t len) {
    uint8_t buf[0x200];
    if (len > sizeof(buf)) len = sizeof(buf);
    if (!ReadBytes(addr, buf, len)) {
        Log("  %s: <read failed at 0x%llx>", label, (unsigned long long)(uintptr_t)addr);
        return;
    }
    Log("  %s (%zu bytes):", label, len);
    for (size_t row = 0; row < len; row += 16) {
        char hex[128];
        int pos = 0;
        pos += snprintf(hex + pos, sizeof(hex) - pos, "    +0x%03x: ", (int)row);
        size_t cols = (len - row < 16) ? len - row : 16;
        for (size_t col = 0; col < cols; col++) {
            pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", buf[row + col]);
        }
        while (pos < 68) hex[pos++] = ' ';
        for (size_t col = 0; col < cols; col++) {
            uint8_t c = buf[row + col];
            hex[pos++] = (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : '.';
        }
        hex[pos] = '\0';
        Log("%s", hex);
    }
}

// =========================================================================
// Actor name resolution via presence registry (no vtable calls)
//
// From vfunction14 @ 0x140340280 error path:
//   presence_base  = *(int64_t*)(this + 0x78)
//   presence_reg   = *(int64_t*)(presence_base + 0x370)
//   primary_table  = *(int64_t*)(presence_reg + 0xb8)
//   fallback_table = *(int64_t*)(presence_reg + 0x80)
//   symbol = primary_table[owner * 8] or fallback_table[owner * 8]
// =========================================================================

static const char* ResolveActorName(uint8_t* self, uint16_t owner_index) {
    int64_t presence_base = 0;
    if (!ReadI64(self + 0x78, &presence_base) || presence_base == 0) return nullptr;

    int64_t presence_reg = 0;
    if (!ReadI64(reinterpret_cast<uint8_t*>(presence_base) + 0x370, &presence_reg) ||
        presence_reg == 0)
        return nullptr;

    int64_t primary_table = 0, fallback_table = 0;
    ReadI64(reinterpret_cast<uint8_t*>(presence_reg) + 0xb8, &primary_table);
    ReadI64(reinterpret_cast<uint8_t*>(presence_reg) + 0x80, &fallback_table);

    int64_t symbol = -1;
    if (primary_table != 0)
        ReadI64(reinterpret_cast<uint8_t*>(primary_table) + (uint64_t)owner_index * 8, &symbol);
    if (symbol == -1 && fallback_table != 0)
        ReadI64(reinterpret_cast<uint8_t*>(fallback_table) + (uint64_t)owner_index * 8, &symbol);

    if (symbol == -1 || fn_GetName == nullptr) return nullptr;
    return fn_GetName(&symbol);
}

// =========================================================================
// HOOK 1: CCharacterAnimationCS::InitRootEvaluator @ 0x140340280
//
// Post-hook: call original, dump diagnostics on failure.
// This is where the "no root animation evaluator" error fires.
// =========================================================================

static void DumpInitRootDiagnostics(void* thisptr, uint64_t handle) {
    auto* self = reinterpret_cast<uint8_t*>(thisptr);

    // --- CI entry (SCharacterAnimationCI, stride 0x1e0) ---
    int64_t ci_table_ptr = 0, ci_index_table = 0, ci_base = 0;
    uint16_t ci_idx = 0;
    uint8_t* ci_entry = nullptr;
    bool have_ci = false;
    uint16_t owner_index = 0;

    if (ReadI64(self + 0x1e0, &ci_table_ptr) && ci_table_ptr != 0 &&
        ReadI64(reinterpret_cast<uint8_t*>(ci_table_ptr) + 8, &ci_index_table) &&
        ci_index_table != 0 &&
        ReadU16(reinterpret_cast<uint8_t*>(ci_index_table) + (handle & 0xFFFF) * 4, &ci_idx) &&
        ReadI64(self + 0x1f0, &ci_base) && ci_base != 0) {
        ci_entry = reinterpret_cast<uint8_t*>(ci_base + (uint64_t)ci_idx * 0x1e0);
        have_ci = true;
        Log("  CI: idx=%u addr=0x%llx stride=0x1e0",
            ci_idx, (unsigned long long)(uintptr_t)ci_entry);
    } else {
        Log("  CI: could not resolve (ci_table=0x%llx ci_base=0x%llx)",
            (unsigned long long)ci_table_ptr, (unsigned long long)ci_base);
    }

    // --- Actor name from CI[0] ---
    if (have_ci && ReadU16(ci_entry, &owner_index)) {
        Log("  owner/presence index = %u (0x%04x)", owner_index, owner_index);
        const char* name = ResolveActorName(self, owner_index);
        if (name)
            Log("  >>> ACTOR: %s", name);
        else
            Log("  >>> ACTOR: <could not resolve name>");
    }

    // --- CD entry (component data, stride 0x40) ---
    if (have_ci) {
        int64_t cd_ptr = 0, cd_table = 0;
        if (ReadI64(self + 0x1c0, &cd_ptr) && cd_ptr != 0 &&
            ReadI64(reinterpret_cast<uint8_t*>(cd_ptr) + 0x158, &cd_table) && cd_table != 0) {
            uint8_t* cd_entry = reinterpret_cast<uint8_t*>(
                cd_table + (uint64_t)owner_index * 0x40);
            Log("  CD: addr=0x%llx stride=0x40",
                (unsigned long long)(uintptr_t)cd_entry);

            int64_t anim_scheme = 0;
            if (ReadI64(cd_entry + 0x30, &anim_scheme)) {
                Log("  CD+0x30 (anim scheme ptr) = 0x%llx%s",
                    (unsigned long long)(uint64_t)anim_scheme,
                    anim_scheme == 0 ? "  *** NULL -- no animation scheme loaded ***" : "");
            }
            HexDump("CD entry", cd_entry, 0x40);
        } else {
            Log("  CD: could not resolve (cd_ptr=0x%llx)", (unsigned long long)cd_ptr);
        }
    }

    // --- Animation data entry (CAnimationCS, stride 0x128) ---
    int64_t anim_idx_table = 0, anim_base = 0;
    uint16_t anim_idx = 0;

    if (ReadI64(self + 0xD0, &anim_idx_table) && anim_idx_table != 0 &&
        ReadU16(reinterpret_cast<uint8_t*>(anim_idx_table) + (handle & 0xFFFF) * 4, &anim_idx) &&
        ReadI64(self + 0x100, &anim_base) && anim_base != 0) {
        uint8_t* entry = reinterpret_cast<uint8_t*>(
            anim_base + (uint64_t)anim_idx * 0x128);
        Log("  Anim data: idx=%u addr=0x%llx stride=0x128",
            anim_idx, (unsigned long long)(uintptr_t)entry);

        int64_t entity_ptr = 0;
        uint32_t id1 = 0, id2 = 0;
        int64_t body_ptr = 0;

        if (ReadI64(entry + 0x08, &entity_ptr))
            Log("  anim+0x08 entity     = 0x%llx",
                (unsigned long long)(uint64_t)entity_ptr);
        if (ReadU32(entry + 0x10, &id1))
            Log("  anim+0x10 body_id1   = 0x%08x%s",
                id1, id1 == 0xFFFFFFFF ? "  [NONE -- no direct physics body ref]" : "");
        if (ReadU32(entry + 0x14, &id2))
            Log("  anim+0x14 body_id2   = 0x%08x", id2);
        if (ReadI64(entry + 0x18, &body_ptr))
            Log("  anim+0x18 body_ptr   = 0x%llx%s",
                (unsigned long long)(uint64_t)body_ptr,
                body_ptr == 0 ? "  [NULL]" : "");

        if (entity_ptr != 0) {
            int64_t tree = 0;
            if (ReadI64(reinterpret_cast<uint8_t*>(entity_ptr) + 0x418, &tree))
                Log("  entity+0x418 tree    = 0x%llx%s",
                    (unsigned long long)(uint64_t)tree,
                    tree == 0 ? "  *** NULL -- no fallback anim tree ***" : "");
        }

        if (id1 == 0xFFFFFFFF) {
            int64_t tree = 0;
            bool tree_null = (entity_ptr == 0) ||
                !ReadI64(reinterpret_cast<uint8_t*>(entity_ptr) + 0x418, &tree) ||
                tree == 0;
            if (tree_null) {
                Log("  DIAGNOSIS: body_id1=-1 AND attachment tree is NULL.");
                Log("             SetupAnimId missing from data, or .anim resource failed to load.");
            }
        }

        HexDump("Anim data entry (first 0x80)", entry, 0x80);
    } else {
        Log("  Anim data: could not resolve (idx_table=0x%llx base=0x%llx)",
            (unsigned long long)anim_idx_table, (unsigned long long)anim_base);
    }

    if (have_ci) {
        HexDump("CI entry (first 0x40)", ci_entry, 0x40);
    }
}

static int __fastcall hook_InitRootEvaluator(void* thisptr, uint64_t handle) {
    int result = orig_InitRootEvaluator(thisptr, handle);
    if (result != 0) {
        Log("================================================================");
        Log("HOOK 1: InitRootEvaluator FAILED -- missing setupanim");
        Log("================================================================");
        Log("  this=0x%llx handle=0x%llx result=%d",
            (unsigned long long)(uintptr_t)thisptr,
            (unsigned long long)handle, result);
        DumpInitRootDiagnostics(thisptr, handle);
        Log("================================================================");
    }
    return result;
}

// =========================================================================
// HOOK 2: CAnimTree::Create @ 0x140fb4640
//
// Post-hook: call original, log on failure. This fires when:
//   - The AnimSet resource fails to load (CResourceInstance::LoadAndInit != 0)
//   - The tree name doesn't exist in the AnimSet ("Unable to find animation tree")
//   - The tree type doesn't match (anim vs ik chain)
//
// CAnimTree::Create is called by PlayAnimation (0x140fbc810) and
// fcn.140631e60 (initial scene setup). Only 3 call sites total.
//
// On the spec object (this):
//   +0x00: CSymbol64 tree_name (the name being looked up in the AnimSet)
//   +0x08: CSymbol64 animset_name (the AnimSet resource being loaded)
// =========================================================================

static uint64_t __fastcall hook_AnimTreeCreate(
    void* spec, int64_t rig_data, void** bindings, void** env,
    void** out_tree, uint32_t flags)
{
    uint64_t result = orig_AnimTreeCreate(spec, rig_data, bindings, env, out_tree, flags);

    if (result != 0) {
        Log("----------------------------------------------------------------");
        Log("HOOK 2: CAnimTree::Create FAILED (result=0x%llx)",
            (unsigned long long)result);

        // Read tree name and animset name from spec
        int64_t tree_sym = 0, animset_sym = 0;
        if (ReadI64(spec, &tree_sym))
            Log("  tree name   : 0x%016llx -> %s",
                (unsigned long long)(uint64_t)tree_sym, SymName(tree_sym));
        if (ReadI64(reinterpret_cast<uint8_t*>(spec) + 8, &animset_sym))
            Log("  animset name: 0x%016llx -> %s",
                (unsigned long long)(uint64_t)animset_sym, SymName(animset_sym));

        // rig name is at *(*(rig_data + 0x70))
        if (rig_data != 0) {
            int64_t rig_ptr = 0;
            if (ReadI64(reinterpret_cast<uint8_t*>(rig_data) + 0x70, &rig_ptr) &&
                rig_ptr != 0) {
                int64_t rig_sym = 0;
                if (ReadI64(reinterpret_cast<void*>(rig_ptr), &rig_sym))
                    Log("  rig name    : 0x%016llx -> %s",
                        (unsigned long long)(uint64_t)rig_sym, SymName(rig_sym));
            }
        }

        Log("  This means the AnimSet resource was loaded but the tree name");
        Log("  was not found inside it, or there was a type mismatch.");
        Log("----------------------------------------------------------------");
    }

    return result;
}

// =========================================================================
// HOOK 3: CAnimationCRI init @ 0x14027b6f0
//
// Post-hook: call original, log on failure. This fires when:
//   - CResourceInstance::LoadAndInit fails (resource not found in archive)
//   - Skeleton binding fails (FUN_1402c6900 returns non-zero)
//
// param_3 = resource_hash (CSymbol64 of the .anim resource)
// param_4 = component data pointer (CD)
// =========================================================================

static int __fastcall hook_AnimCRIInit(
    void* factory, void* mem_block, uint64_t resource_hash, int64_t component_data)
{
    int result = orig_AnimCRIInit(factory, mem_block, resource_hash, component_data);

    if (result != 0) {
        Log("----------------------------------------------------------------");
        Log("HOOK 3: CAnimationCRI init FAILED (result=%d)", result);
        Log("  resource hash: 0x%016llx -> %s",
            (unsigned long long)resource_hash, SymName(static_cast<int64_t>(resource_hash)));
        Log("  This means the .anim resource could not be loaded from the");
        Log("  archive, or the skeleton binding failed after loading.");

        // If component_data is valid, dump a few key offsets
        if (component_data != 0) {
            // component_data + 0x80 -> +0x370 = presence registry (for context)
            int64_t presence_ptr = 0;
            if (ReadI64(reinterpret_cast<uint8_t*>(component_data) + 0x80, &presence_ptr) &&
                presence_ptr != 0) {
                int64_t presence_reg = 0;
                ReadI64(reinterpret_cast<uint8_t*>(presence_ptr) + 0x370, &presence_reg);
                Log("  component_data+0x80 (presence) = 0x%llx",
                    (unsigned long long)(uint64_t)presence_ptr);
            }
            // component_data + 0xb8/0xc0 = skeleton pointers (will be null on failure)
            int64_t skel_a = 0, skel_b = 0;
            ReadI64(reinterpret_cast<uint8_t*>(component_data) + 0xb8, &skel_a);
            ReadI64(reinterpret_cast<uint8_t*>(component_data) + 0xc0, &skel_b);
            Log("  skeleton_a (CD+0xb8) = 0x%llx%s",
                (unsigned long long)(uint64_t)skel_a,
                skel_a == 0 ? "  [NULL -- not bound]" : "");
            Log("  skeleton_b (CD+0xc0) = 0x%llx%s",
                (unsigned long long)(uint64_t)skel_b,
                skel_b == 0 ? "  [NULL -- not bound]" : "");
        }
        Log("----------------------------------------------------------------");
    }

    return result;
}

// =========================================================================
// Plugin interface
// =========================================================================

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void) {
    NvrPluginInfo info = {};
    info.name = "anim_debugger";
    info.description =
        "Verbose diagnostics for animation loading and InitRootEvaluator failures";
    info.version_major = 2;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void) {
    return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx) {
    g_base_addr = ctx->base_addr;
    Log("initializing (base=0x%llx)", (unsigned long long)g_base_addr);

    MH_Initialize();

    // Resolve CResourceID::GetName for symbol-to-string resolution
    fn_GetName = reinterpret_cast<CResourceID_GetName_fn>(
        nevr::ResolveVA(g_base_addr, nevr::addresses::VA_CRESOURCEID_GET_NAME));

    int hooks_installed = 0;

    // Hook 1: InitRootEvaluator -- the "no root animation evaluator" error
    {
        auto* target = nevr::ResolveVA(
            g_base_addr, nevr::addresses::VA_INIT_ROOT_EVALUATOR);
        MH_STATUS s = g_hooks.CreateAndEnable(
            target,
            reinterpret_cast<void*>(&hook_InitRootEvaluator),
            reinterpret_cast<void**>(&orig_InitRootEvaluator));
        if (s == MH_OK) {
            Log("  hook 1: InitRootEvaluator @ VA 0x%llx",
                (unsigned long long)nevr::addresses::VA_INIT_ROOT_EVALUATOR);
            hooks_installed++;
        } else {
            Log("  hook 1: InitRootEvaluator FAILED (MH_STATUS=%d)", s);
        }
    }

    // Hook 2: CAnimTree::Create -- "Unable to find animation tree" errors
    {
        auto* target = nevr::ResolveVA(g_base_addr, VA_ANIM_TREE_CREATE);
        MH_STATUS s = g_hooks.CreateAndEnable(
            target,
            reinterpret_cast<void*>(&hook_AnimTreeCreate),
            reinterpret_cast<void**>(&orig_AnimTreeCreate));
        if (s == MH_OK) {
            Log("  hook 2: CAnimTree::Create @ VA 0x%llx",
                (unsigned long long)VA_ANIM_TREE_CREATE);
            hooks_installed++;
        } else {
            Log("  hook 2: CAnimTree::Create FAILED (MH_STATUS=%d)", s);
        }
    }

    // Hook 3: CAnimationCRI init -- resource load + skeleton bind failures
    {
        auto* target = nevr::ResolveVA(g_base_addr, VA_ANIM_CRI_INIT);
        MH_STATUS s = g_hooks.CreateAndEnable(
            target,
            reinterpret_cast<void*>(&hook_AnimCRIInit),
            reinterpret_cast<void**>(&orig_AnimCRIInit));
        if (s == MH_OK) {
            Log("  hook 3: CAnimationCRI init @ VA 0x%llx",
                (unsigned long long)VA_ANIM_CRI_INIT);
            hooks_installed++;
        } else {
            Log("  hook 3: CAnimationCRI init FAILED (MH_STATUS=%d)", s);
        }
    }

    Log("initialization complete (%d/3 hooks installed)", hooks_installed);
    return (hooks_installed > 0) ? 0 : -1;
}

NEVR_PLUGIN_API void NvrPluginShutdown(void) {
    Log("shutting down (%zu hooks)", g_hooks.count());
    g_hooks.RemoveAll();
}

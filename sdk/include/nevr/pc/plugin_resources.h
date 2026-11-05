/*
 * nevr/pc/plugin_resources.h - carry a resource set in the plugin's own file and register it (plugin API v9, #2161;
 * docs/reference/plugin-resources.md).
 *
 * A resource set (nevr/resource_set.h) is embedded in a read-only section of the plugin DLL, so the plugin stays one
 * file and the runtime can serve the set's chunks straight from it. Two ways to embed one:
 *
 *   NEVR_EMBED_RESOURCE_SET_FILE(levels, "build/levels.nevrres");   // GCC or Clang (MinGW): .incbin of a file
 *   NEVR_RESOURCE_SET_SECTION const unsigned char levels_data[] = {...};  // any compiler that has the attribute
 *
 * The first defines `levels_data` and `levels_size`; the second is for a set written out as an array. Either puts
 * the bytes in the section `.nevrres`, which the linker keeps as initialised, read-only data with no relocation in
 * it: the runtime refuses a set whose bytes a relocation touches (NEVR_RESOURCES_NOT_IN_FILE).
 *
 * Then, from NevrPluginInit / NevrPluginInitEx:
 *
 *   NevrRegisterEmbeddedResources(ctx, levels_data, levels_size, "my levels");
 *
 * It registers the set with ctx->register_resources, then each of the set's overrides (replacement bytes for
 * resources the game already has) with NEVR_RegisterResourceOverride (nevr/pc/resource_registry.h). It returns the
 * set's NevrResourcesStatus; an override the host cannot take is counted in `overrides_failed`.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "nevr/plugin_interface.h"
#include "nevr/pc/resource_registry.h"
#include "nevr/resource_set.h"

#if defined(__GNUC__) || defined(__clang__)
#define NEVR_RESOURCE_SET_SECTION __attribute__((section(".nevrres"), used))
// `name`_data is the first byte and `name`_size the byte count, as extern "C" symbols in this DLL. The section flag
// "dr" is PE's initialised, read-only data.
#define NEVR_EMBED_RESOURCE_SET_FILE(name, path)                                                      \
    extern "C" const unsigned char name##_data[];                                                     \
    extern "C" const unsigned char name##_end[];                                                      \
    __asm__(".pushsection .nevrres,\"dr\"\n"                                                          \
            ".balign 16\n"                                                                            \
            ".globl " NEVR_RESOURCE_SET_ASM_NAME(name##_data) "\n" NEVR_RESOURCE_SET_ASM_NAME(name##_data) ":\n" \
            ".incbin \"" path "\"\n"                                                                  \
            ".globl " NEVR_RESOURCE_SET_ASM_NAME(name##_end) "\n" NEVR_RESOURCE_SET_ASM_NAME(name##_end) ":\n"   \
            ".popsection\n");                                                                         \
    static const std::uint64_t name##_size =                                                          \
        static_cast<std::uint64_t>(name##_end - name##_data)
// x86-64 PE and ELF symbols carry no leading underscore.
#define NEVR_RESOURCE_SET_ASM_NAME(sym) #sym
#endif

struct NevrEmbeddedResourcesResult {
    int32_t status = NEVR_RESOURCES_BAD_ARGUMENT;  // the set's NevrResourcesStatus, or -1 when the host has no v9 entry
    uint32_t overrides_registered = 0;
    uint32_t overrides_failed = 0;
};

// Registers the set at `data` (`size` bytes, inside this plugin's image) and then its overrides. Call from init.
inline NevrEmbeddedResourcesResult NevrRegisterEmbeddedResources(const NevrGameContext* ctx, const void* data,
                                                                  uint64_t size, const char* label) {
    NevrEmbeddedResourcesResult result;
    const uint64_t needed = offsetof(NevrGameContext, register_resources) + sizeof(ctx->register_resources);
    if (ctx == nullptr || ctx->ctx_size < needed || ctx->register_resources == nullptr) {
        result.status = -1;
        return result;
    }
    NevrResourcesDesc desc = {};
    desc.struct_size = sizeof(desc);
    desc.data = data;
    desc.size = size;
    desc.label = label;
    result.status = ctx->register_resources(ctx->plugin_id, &desc);
    if (result.status != NEVR_RESOURCES_OK) return result;
    nevr_resource_set::View view;
    if (!nevr_resource_set::Parse(data, size, &view).ok()) return result;  // the host accepted it, so it parses
    for (uint32_t i = 0; i < view.override_count(); ++i) {
        const nevr_resource_set::Override o = view.OverrideAt(i);
        const unsigned char* bytes = static_cast<const unsigned char*>(data) + o.offset;
        if (nevr::RegisterResourceOverride(o.type, o.name, bytes, o.size, label)) {
            ++result.overrides_registered;
        } else {
            ++result.overrides_failed;
        }
    }
    return result;
}

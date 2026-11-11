/*
 * nevr/pc/plugin_resources.h - carry a resource set in the plugin's own file and register it (plugin API v9, #2161;
 * docs/reference/plugin-resources.md).
 *
 * A resource set (nevr/resource_set.h) is embedded in a read-only section of the plugin DLL, so the plugin stays one
 * file and the runtime can serve the set's chunks straight from it. Embed it with #embed (C23, C++26; GCC 15 and
 * Clang 19 accept it in C++ as an extension) into the section NEVR_RESOURCE_SET_SECTION names:
 *
 *   NEVR_RESOURCE_SET_SECTION const unsigned char levels_data[] = {
 *   #embed "levels.nevrres"
 *   };
 *
 * The section `.nevrres` is initialised, read-only data with no relocation in it: the runtime refuses a set in a
 * writable section or one whose bytes a relocation touches (NEVR_RESOURCES_NOT_IN_FILE). The MinGW assembler cannot
 * switch sections and back from a top-level asm statement on PE (it has no .pushsection or .previous), so #embed is the
 * one way; a 1 GiB set builds in about two minutes.
 *
 * Then, from NevrPluginInit / NevrPluginInitEx:
 *
 *   NevrRegisterEmbeddedResources(ctx, levels_data, sizeof(levels_data), "my levels");
 *
 * It registers the set with ctx->register_resources and returns its NevrResourcesStatus. The label is 1 to 64 bytes
 * of printable ASCII with no double quote or backslash. The set's overrides (replacement bytes for resources the game
 * already has) are registered by the runtime when it mounts the set, and only then: a set that is refused, or whose
 * plugin's init fails, replaces nothing.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "nevr/plugin_interface.h"

#if defined(__GNUC__) || defined(__clang__)
#define NEVR_RESOURCE_SET_SECTION __attribute__((section(".nevrres"), used))
#endif

struct NevrEmbeddedResourcesResult {
    int32_t status = NEVR_RESOURCES_BAD_ARGUMENT;  // the set's NevrResourcesStatus, or -1 when the host has no v9 entry
};

// Registers the set at `data` (`size` bytes, inside this plugin's image). Call from init.
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
    return result;
}

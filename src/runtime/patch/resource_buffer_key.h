/* #1421: which buffer of a resource an override entry names.
 *
 * The game reads a resource as up to two buffers under one (type, name), each by its own on-disk type:
 * CSymbol64::Make(<logical type>, "Win10") for buffer 0 and Make(<logical type>, "Win10GPU") for buffer 1
 * (CResourceID_BuildPath 0x140fa19a0, docs/engine/asset-system.md). For a CGTextureResource buffer 0 is the 256-byte
 * descriptor and buffer 1 the pixel payload. Make continues the CSymbol64 hash of the class name over the suffix, so
 * the key follows from the logical type the callback reads at CResource + 0x28 alone, with no call into the game.
 *
 * An entry's type names its buffer:
 *   - 0, the logical type, or Make(<logical>, "Win10"): buffer 0;
 *   - Make(<logical>, "Win10GPU"): buffer 1.
 * Before #1421 the callback matched an entry by 0 or the logical type, on buffer 0 only: those entries still name buffer
 * 0. An entry keyed by either on-disk type matched nothing then; it now names the buffer that type is read by. A runtime
 * from before #1421 never matches a buffer-1 entry, so it cannot serve a payload as a descriptor.
 *
 * A texture's descriptor and payload are two entries, each served on its own, and the game finishes the two buffers in
 * either order (#1421 step 1). The descriptor is the header the payload is read with, so a payload of another size
 * needs its descriptor entry too, and a payload that is not served leaves a served descriptor entry without it:
 * resource_override.cpp says either in a Warning that names the texture.
 *
 * Pure: src/runtime/tests/test_resource_override.cpp drives it through the resource callback.
 */
#pragma once

#include <cstdint>

#include "nevr/symbol_hash.h"

namespace nevr_resource_key {

inline constexpr char kBufferZeroSuffix[] = "Win10";
inline constexpr char kBufferOneSuffix[] = "Win10GPU";

/// The on-disk type the game reads buffer `index` (0 or 1) of a resource of logical type `logical` by.
constexpr std::uint64_t OnDiskType(std::uint64_t logical, std::uint32_t index) {
    return EchoVR::CSymbol64Hash(index == 0 ? kBufferZeroSuffix : kBufferOneSuffix, logical);
}

/// Whether an entry keyed by `entry_type` names buffer `index` of a resource of logical type `logical`.
constexpr bool NamesBuffer(std::uint64_t entry_type, std::uint64_t logical, std::uint32_t index) {
    if (index == 0) return entry_type == 0 || entry_type == logical || entry_type == OnDiskType(logical, 0);
    if (index == 1) return entry_type == OnDiskType(logical, 1);
    return false;
}

/// CGTextureResource, as #1421's step 1 saw the game read it: buffer 0 by 0x4a4c32c49300b8a0, buffer 1 by
/// 0xbeac1969cb7b8861.
inline constexpr std::uint64_t kTextureResourceType = 0xb3ebe9e0bf0eb81fULL;
static_assert(OnDiskType(kTextureResourceType, 0) == 0x4a4c32c49300b8a0ULL, "Make(CGTextureResource, \"Win10\")");
static_assert(OnDiskType(kTextureResourceType, 1) == 0xbeac1969cb7b8861ULL, "Make(CGTextureResource, \"Win10GPU\")");

}  // namespace nevr_resource_key

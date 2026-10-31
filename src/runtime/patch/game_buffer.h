/* #1146: a buffer written into a streaming callback block belongs to the game from then on.
 *
 * The game's resource release (echovr.exe fcn.140fa2210, called from CResourceManager::Unload when the last reference
 * goes) frees buffer 0 of a resource through the CElephantAllocator at memory-context + 0x10, virtual slot +0x38
 * (FUN_1400cf1e0). That slot tests the allocator, never the pointer, and the pool free behind it reads a 0x20-byte header
 * in front of the pointer. So a buffer from VirtualAlloc, HeapAlloc or the DLL's own heap must never reach a block: the
 * game would free it into its Resource heap.
 *
 * This header holds the two ways a replacement reaches the game. CopyIntoGameHeap allocates through the same allocator,
 * slot +0x28 (FUN_1400cdee0(allocator, alignment, size), CElephantAllocator::vslot[5]; the game's own reader allocates
 * the buffer it reads through that slot, SetupReadBuffer 0x141524952), and copies the bytes in. The caller keeps its own
 * bytes and writes the block only when the copy exists in full. The game's original buffer is left alone (the reader may
 * own it), so one buffer per replaced load stays allocated. CopyIntoGameHeapFor is the same copy for a buffer the runtime
 * serves with no callback block (a plugin asset, plugin_assets.cpp): the release allocator alone decides.
 *
 * WriteInPlace (#1421) writes over the buffer the game's reader already allocated, when the bytes fit it: no allocation,
 * and the buffer the game frees is still its own. It serves a texture's payload (buffer 1), which is up to 78 MB, where a
 * game-heap copy would keep a second payload-sized buffer per load.
 *
 * Pure: raw loads and calls through a table the caller names, so a host test drives it with a fake allocator
 * (src/runtime/tests/test_resource_override.cpp). The Quest twin is src/quest/sentinel/menu_cards_swap.h, which
 * additionally requires the block's own allocator to be the one the release uses.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nevr_game_buffer {

/// CElephantAllocator virtual slots (byte offsets into its table) and where the release finds its allocator.
inline constexpr size_t kAllocSlot = 0x28;
inline constexpr size_t kFreeSlot = 0x38;
inline constexpr size_t kContextAllocatorOffset = 0x10;
/// The game's memory-context pointer is a global: 0x142098f38 in the pinned echovr.exe, image base 0x140000000.
inline constexpr uintptr_t kMemoryContextRva = 0x2098f38;
/// The callback block (slot + 0x478 of the streaming request, echovr.exe ServeRequest 0x141524bfe) names the allocator the
/// read buffer came from and the alignment it was asked with (SetupReadBuffer 0x141524910 writes both before it allocates).
inline constexpr size_t kBlockAllocator = 0x28;
inline constexpr size_t kBlockAlignment = 0x30;
/// The buffer the game's reader filled and its size (AsyncResourceIOCallback 0x140fa16d0 stores both in the resource).
inline constexpr size_t kBlockBuffer = 0x18;
inline constexpr size_t kBlockSize = 0x20;
/// The allocator rounds anything under this up (0x1400cdf3c).
inline constexpr uint64_t kMinAlignment = 16;

enum class Copy {
    Copied,
    NoBytes,            // nothing to copy
    NoContext,          // the game has not published its memory context (or no slot was given)
    NoAllocator,        // the context or the block holds no allocator
    AllocatorMismatch,  // the block's allocator is not the one the release uses
    AllocFailed,        // the allocator returned null
};

inline const char* CopyName(Copy c) {
    switch (c) {
        case Copy::Copied: return "copied";
        case Copy::NoBytes: return "no_bytes";
        case Copy::NoContext: return "no_memory_context";
        case Copy::NoAllocator: return "no_allocator";
        case Copy::AllocatorMismatch: return "allocator_mismatch";
        case Copy::AllocFailed: return "alloc_failed";
    }
    return "unknown";
}

using AllocFn = void* (*)(void* allocator, uint64_t alignment, uint64_t size);

/// The allocator the game releases buffer 0 through: the pointer at context + 0x10, null when there is none.
/// `contextSlot` is the address of the game's memory-context pointer.
inline void* ReleaseAllocator(void* const* contextSlot) {
    if (contextSlot == nullptr) return nullptr;
    const unsigned char* const context = static_cast<const unsigned char*>(*contextSlot);
    if (context == nullptr) return nullptr;
    void* allocator = nullptr;
    std::memcpy(&allocator, context + kContextAllocatorOffset, sizeof(allocator));
    return allocator;
}

/// Allocates `size` bytes through `release` (the game's release allocator) at `alignment`, raised to kMinAlignment, and
/// copies `bytes` in. On Copied, *out is the copy; on any other result *out is left alone.
inline Copy AllocateCopy(void* release, uint64_t alignment, const void* bytes, uint64_t size, void** out) {
    if (alignment < kMinAlignment) alignment = kMinAlignment;
    void* const* table = nullptr;
    std::memcpy(&table, release, sizeof(table));
    const AllocFn alloc = reinterpret_cast<AllocFn>(table[kAllocSlot / sizeof(void*)]);
    void* const copy = alloc(release, alignment, size);
    if (copy == nullptr) return Copy::AllocFailed;
    std::memcpy(copy, bytes, static_cast<size_t>(size));
    *out = copy;
    return Copy::Copied;
}

/// Allocates `size` bytes for the block `block` (at least kBlockAlignment + 8 bytes), through the allocator the game
/// releases buffer 0 with, and copies `bytes` in. Only when the block's own allocator is that one: a buffer the game's
/// reader did not allocate there is not ours to replace. On Copied, *out is the copy and *alignment the alignment it was
/// allocated with; on any other result both are left alone.
inline Copy CopyIntoGameHeap(void* const* contextSlot, const unsigned char* block, const void* bytes, uint64_t size,
                             void** out) {
    if (bytes == nullptr || size == 0) return Copy::NoBytes;
    if (contextSlot == nullptr || *contextSlot == nullptr) return Copy::NoContext;
    void* const release = ReleaseAllocator(contextSlot);
    void* blockAllocator = nullptr;
    std::memcpy(&blockAllocator, block + kBlockAllocator, sizeof(blockAllocator));
    if (release == nullptr || blockAllocator == nullptr) return Copy::NoAllocator;
    if (blockAllocator != release) return Copy::AllocatorMismatch;
    uint64_t alignment = 0;
    std::memcpy(&alignment, block + kBlockAlignment, sizeof(alignment));
    return AllocateCopy(release, alignment, bytes, size, out);
}

/// The same copy with no block to name the allocator: for a buffer the runtime hands the engine itself (a plugin
/// asset, #1146's sibling). The release allocator is the only one the game frees through, so it is the only one used.
/// The alignment is the allocator's minimum.
inline Copy CopyIntoGameHeapFor(void* const* contextSlot, const void* bytes, uint64_t size, void** out) {
    if (bytes == nullptr || size == 0) return Copy::NoBytes;
    if (contextSlot == nullptr || *contextSlot == nullptr) return Copy::NoContext;
    void* const release = ReleaseAllocator(contextSlot);
    if (release == nullptr) return Copy::NoAllocator;
    return AllocateCopy(release, kMinAlignment, bytes, size, out);
}

enum class InPlace {
    Written,
    NoBytes,       // nothing to write
    NoGameBuffer,  // the block holds no buffer
    TooLarge,      // the bytes are larger than the buffer the game's reader allocated
};

inline const char* InPlaceName(InPlace r) {
    switch (r) {
        case InPlace::Written: return "written";
        case InPlace::NoBytes: return "no_bytes";
        case InPlace::NoGameBuffer: return "no_game_buffer";
        case InPlace::TooLarge: return "larger_than_the_game_buffer";
    }
    return "unknown";
}

/// Writes `bytes` over the buffer the block `block` holds and sets the block's size to `size`, when `size` is at most
/// the size the block holds: the buffer stays the one the game's reader allocated. On any other result the block and
/// its buffer are left alone.
inline InPlace WriteInPlace(unsigned char* block, const void* bytes, uint64_t size) {
    if (bytes == nullptr || size == 0) return InPlace::NoBytes;
    void* buffer = nullptr;
    uint64_t held = 0;
    std::memcpy(&buffer, block + kBlockBuffer, sizeof(buffer));
    std::memcpy(&held, block + kBlockSize, sizeof(held));
    if (buffer == nullptr) return InPlace::NoGameBuffer;
    if (size > held) return InPlace::TooLarge;
    std::memcpy(buffer, bytes, static_cast<size_t>(size));
    std::memcpy(block + kBlockSize, &size, sizeof(size));
    return InPlace::Written;
}

}  // namespace nevr_game_buffer

/* #966 / ADR 0017 D4: the Quest twin of the PC main-menu cards (src/runtime/patch/menu_cards.h), the pure half.
 *
 * What the game does (pinned libr15, the disassembly names; every offset below is read from the library's own code, see
 * pinned_targets.h): when the streaming reader has the bytes of a resource it calls
 * CResourceManager::AsyncResourceIOCallback(data*). `data` is the request context's callback block:
 *
 *   +0x00 status        4 = the read finished (the callback stores 5 in the resource for any other status)
 *   +0x08 CResource*    the resource being loaded
 *   +0x10 buffer index  0 = the primary buffer
 *   +0x18 buffer        the bytes: the callback copies it into the resource (CResource + 0x40 + index * 8)
 *   +0x20 size          their length (CResource + 0x50 + index * 8)
 *   +0x28 allocator     the CAllocator the buffer came from
 *   +0x30 alignment     the alignment it was allocated with
 *
 * and the resource's +0x28 holds its class symbol and +0x38 its name symbol. A resource is freed by
 * CResourceManager::FreeResource (0x2544008): it releases buffer 0 through the allocator at CMemoryContext::context +
 * 0x10, virtual slot +0x40 (`free(allocator, pointer)`). So bytes the game will free later must come from that
 * allocator, slot +0x30 (`alloc(allocator, alignment, size)`, as the streaming worker allocates them).
 *
 * The decision made here, before the game's callback runs: when the resource is the main-menu page and its bytes are the
 * pinned stock page, serve a copy with the two cards added instead. The copy is allocated through the allocator the
 * block names, and only when that allocator is the one FreeResource will use; any doubt leaves the game's own bytes in
 * place. The game's original buffer is left alone: the caller of the streaming reader may own it, so it is not freed here
 * (one page is lost per load of the menu).
 *
 * This header is pure: raw loads and stores on a block the caller passes and calls through the allocator's virtual table,
 * so a host test drives it with a block and an allocator it builds (src/quest/tests/menu_cards_hook_test.cpp). Free of
 * exceptions and of Android.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nevr_quest::menu_cards {

namespace block {
inline constexpr std::size_t kStatus = 0x00;
inline constexpr std::size_t kResource = 0x08;
inline constexpr std::size_t kIndex = 0x10;
inline constexpr std::size_t kBuffer = 0x18;
inline constexpr std::size_t kSize = 0x20;
inline constexpr std::size_t kAllocator = 0x28;
inline constexpr std::size_t kAlignment = 0x30;
inline constexpr std::uint64_t kStatusDone = 4;
}  // namespace block

namespace resource {
inline constexpr std::size_t kClass = 0x28;
inline constexpr std::size_t kName = 0x38;
}  // namespace resource

/// CAllocator virtual slots (byte offsets into its table) and where FreeResource finds its allocator.
inline constexpr std::size_t kAllocSlot = 0x30;
inline constexpr std::size_t kFreeSlot = 0x40;
inline constexpr std::size_t kContextAllocatorOffset = 0x10;

template <typename T>
inline T Load(const void* base, std::size_t at) {
  T v;
  std::memcpy(&v, static_cast<const unsigned char*>(base) + at, sizeof(v));
  return v;
}
template <typename T>
inline void Store(void* base, std::size_t at, T v) {
  std::memcpy(static_cast<unsigned char*>(base) + at, &v, sizeof(v));
}

/// What a callback block says about the resource it delivers.
struct Delivery {
  std::uint64_t status = 0;
  std::uint64_t index = 0;
  std::uint64_t classSymbol = 0;
  std::uint64_t nameSymbol = 0;
  unsigned char* buffer = nullptr;
  std::uint64_t size = 0;
  void* allocator = nullptr;
  std::uint64_t alignment = 0;
};

/// Reads the block and the resource it points at. A block with no resource reads as an empty delivery.
inline Delivery Inspect(const unsigned char* data) {
  Delivery d;
  if (data == nullptr) return d;
  d.status = Load<std::uint64_t>(data, block::kStatus);
  d.index = Load<std::uint32_t>(data, block::kIndex);
  d.buffer = Load<unsigned char*>(data, block::kBuffer);
  d.size = Load<std::uint64_t>(data, block::kSize);
  d.allocator = Load<void*>(data, block::kAllocator);
  d.alignment = Load<std::uint64_t>(data, block::kAlignment);
  const unsigned char* const res = Load<const unsigned char*>(data, block::kResource);
  if (res != nullptr) {
    d.classSymbol = Load<std::uint64_t>(res, resource::kClass);
    d.nameSymbol = Load<std::uint64_t>(res, resource::kName);
  }
  return d;
}

/// A finished read of the primary buffer, with bytes.
inline bool IsFinishedPrimary(const Delivery& d) {
  return d.status == block::kStatusDone && d.index == 0 && d.buffer != nullptr && d.size != 0;
}

enum class Swap {
  Swapped,
  NoServedPage,        // nothing to serve
  NoAllocator,         // the block names no allocator, or the game has none
  AllocatorMismatch,   // the block's allocator is not the one FreeResource releases buffer 0 through
  AllocFailed,         // the allocator returned null
};

inline const char* SwapName(Swap s) {
  switch (s) {
    case Swap::Swapped: return "swapped";
    case Swap::NoServedPage: return "no_served_page";
    case Swap::NoAllocator: return "no_allocator";
    case Swap::AllocatorMismatch: return "allocator_mismatch";
    case Swap::AllocFailed: return "alloc_failed";
  }
  return "unknown";
}

using AllocFn = void* (*)(void* allocator, std::uint64_t alignment, std::uint64_t size);

/// Calls the allocator's allocation slot.
inline void* AllocateThrough(void* allocator, std::uint64_t alignment, std::uint64_t size) {
  void* const* const table = Load<void* const*>(allocator, 0);
  return reinterpret_cast<AllocFn>(table[kAllocSlot / sizeof(void*)])(allocator, alignment, size);
}

/// Points the block at a copy of `served` allocated through the block's own allocator. `releaseAllocator` is what
/// FreeResource will release buffer 0 through (the context's allocator, 0 when there is none). Writes the block only
/// when the copy exists in full.
inline Swap SwapIn(unsigned char* data, const unsigned char* served, std::size_t servedSize, void* releaseAllocator) {
  if (served == nullptr || servedSize == 0) return Swap::NoServedPage;
  void* const allocator = Load<void*>(data, block::kAllocator);
  if (allocator == nullptr || releaseAllocator == nullptr) return Swap::NoAllocator;
  if (allocator != releaseAllocator) return Swap::AllocatorMismatch;
  void* const copy = AllocateThrough(allocator, Load<std::uint64_t>(data, block::kAlignment), servedSize);
  if (copy == nullptr) return Swap::AllocFailed;
  std::memcpy(copy, served, servedSize);
  Store<void*>(data, block::kBuffer, copy);
  Store<std::uint64_t>(data, block::kSize, servedSize);
  return Swap::Swapped;
}

}  // namespace nevr_quest::menu_cards

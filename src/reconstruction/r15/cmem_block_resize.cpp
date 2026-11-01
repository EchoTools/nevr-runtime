// NRadEngine::CMemBlock::Resize
// Quest libr15.so VA 0x12dc30c; ReVault binary 576.
// ReVault raw parent 100220323, reconstruction node 100600497; grade H: every branch (size unchanged, alignment zero or not, new size zero, old size zero, keep flag) traced through the 54 instructions; Ghidra's prototype omits the third argument w2, and the stored size 216 ends at the ret.
// @0x12dc30c — NRadEngine::CMemBlock::Resize
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CAllocator;

// CMemory::Copy(void*, void const*, unsigned long long), Quest libr15.so VA 0x12cb908 (thunk 0x100cc90).
namespace CMemory {
void Copy(void* destination, const void* source, std::uint64_t count);
}  // namespace CMemory

// Raw layout, none of it recovered as named fields: +0x00 block pointer, +0x08 size (u64), +0x10 CAllocator*,
// +0x18 alignment (int32; zero selects the vtable +0x18 path).
struct CMemBlock {
  // NRadEngine::CMemBlock::Resize(unsigned long long, unsigned int), Quest libr15.so VA 0x12dc30c (thunk 0x103e400).
  // Three arguments at the call sites: this, the new size (x1, 64-bit) and keep_contents (w2, tested as 32-bit).
  void Resize(std::uint64_t new_size, std::uint32_t keep_contents);
};

namespace {

void* LoadPtr(const std::byte* base, std::size_t offset) {
  void* value = nullptr;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

std::uint32_t LoadU32(const std::byte* base, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
  std::uint64_t value = 0;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

void StorePtr(std::byte* base, std::size_t offset, const void* value) {
  std::memcpy(base + offset, &value, sizeof value);
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
  std::memcpy(base + offset, &value, sizeof value);
}

// Reads the function pointer at byte offset `slot` of the vtable of `object` (the vptr is its first word).
// ReVault has no names for the CAllocator slots; each call site casts the pointer to the signature its disassembly shows.
template <typename Fn>
Fn VirtualSlot(const void* object, std::size_t slot) {
  std::uintptr_t vtable = 0;
  std::memcpy(&vtable, object, sizeof vtable);
  Fn fn = nullptr;
  std::memcpy(&fn, reinterpret_cast<const std::byte*>(vtable) + slot, sizeof fn);
  return fn;
}

// Vtable slot +0x18 of a CAllocator: (old block or null, new size) returns the new block; no name recovered.
using ReallocFn = void* (*)(CAllocator*, void*, std::uint64_t);
// Vtable slot +0x30 of a CAllocator: (alignment, size) returns a new block; no name recovered.
using AllocAlignedFn = void* (*)(CAllocator*, std::uint64_t, std::uint64_t);
// Vtable slot +0x40 of a CAllocator: releases a block; its result is not used.
using FreeFn = void (*)(CAllocator*, void*);

}  // namespace

void CMemBlock::Resize(std::uint64_t new_size, std::uint32_t keep_contents) {
  auto* const self = reinterpret_cast<std::byte*>(this);
  // 0x12dc318-0x12dc320: an unchanged size returns without touching the block.
  const std::uint64_t old_size = LoadU64(self, 0x08);
  if (old_size == new_size) {
    return;
  }
  CAllocator* allocator = nullptr;
  std::memcpy(&allocator, self + 0x10, sizeof allocator);
  const std::uint32_t alignment = LoadU32(self, 0x18);

  if (alignment == 0) {
    // 0x12dc394-0x12dc3cc: vtable +0x18 (old block, or null when the old size is zero, and the new size).
    // A zero new size stores all ones as the block pointer.
    void* const old_block = old_size != 0 ? LoadPtr(self, 0x00) : nullptr;
    void* const resized = VirtualSlot<ReallocFn>(allocator, 0x18)(allocator, old_block, new_size);
    StorePtr(self, 0x00, new_size != 0 ? resized : reinterpret_cast<void*>(~std::uintptr_t{0}));
  } else {
    // 0x12dc334-0x12dc38c: a new block through vtable +0x30 when the new size is nonzero.
    void* fresh = reinterpret_cast<void*>(~std::uintptr_t{0});
    std::uint64_t current_size = old_size;
    if (new_size != 0) {
      fresh = VirtualSlot<AllocAlignedFn>(allocator, 0x30)(allocator, alignment, new_size);
      // 0x12dc350: the size is read again after the call.
      current_size = LoadU64(self, 0x08);
    }
    if (current_size != 0) {
      // 0x12dc35c-0x12dc370: copy min(old, new) bytes only when both sizes are nonzero and keep_contents is set.
      if (new_size != 0 && keep_contents != 0) {
        const std::uint64_t count = current_size < new_size ? current_size : new_size;
        CMemory::Copy(fresh, LoadPtr(self, 0x00), count);
      }
      // 0x12dc378-0x12dc388: vtable +0x40 releases the old block.
      VirtualSlot<FreeFn>(allocator, 0x40)(allocator, LoadPtr(self, 0x00));
    }
    StorePtr(self, 0x00, fresh);
  }
  // 0x12dc3d0: the size becomes the new size.
  StoreU64(self, 0x08, new_size);
}

}  // namespace NRadEngine

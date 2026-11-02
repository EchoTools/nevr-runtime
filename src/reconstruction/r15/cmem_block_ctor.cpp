// NRadEngine::CMemBlock::CMemBlock
// Quest libr15.so VA 0x12dbcc8; ReVault binary 576.
// ReVault raw parent 100220302, reconstruction node 100600496; grade H: all three allocation branches and the block pointer, alignment and flag stores checked against the 30 instructions; the stored size 120 ends at the ret.
// @0x12dbcc8 — NRadEngine::CMemBlock::CMemBlock
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CAllocator;

// Raw layout, none of it recovered as named fields: +0x00 block pointer, +0x08 size (u64), +0x10 CAllocator*,
// +0x18 alignment (int32; zero selects the unaligned allocation), +0x1c flags (u32; bit 0 set by the constructor).
struct CMemBlock {
  // NRadEngine::CMemBlock::CMemBlock(unsigned long long, unsigned long long, CAllocator*), Quest libr15.so VA 0x12dbcc8
  // (thunk 0xfedec0).
  CMemBlock(std::uint64_t size, std::uint64_t alignment, CAllocator* allocator);
};

namespace {

void StorePtr(std::byte* base, std::size_t offset, const void* value) {
  std::memcpy(base + offset, &value, sizeof value);
}

void StoreU32(std::byte* base, std::size_t offset, std::uint32_t value) {
  std::memcpy(base + offset, &value, sizeof value);
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
  std::memcpy(base + offset, &value, sizeof value);
}

std::uint32_t LoadU32(const std::byte* base, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
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

// Vtable slot +0x10 of a CAllocator: allocates `size` bytes (x1 = size).
using AllocFn = void* (*)(CAllocator*, std::uint64_t);
// Vtable slot +0x30 of a CAllocator: allocates `size` bytes with `alignment` (x1 = alignment, x2 = size).
using AllocAlignedFn = void* (*)(CAllocator*, std::uint64_t, std::uint64_t);

}  // namespace

CMemBlock::CMemBlock(std::uint64_t size, std::uint64_t alignment, CAllocator* allocator) {
  auto* const self = reinterpret_cast<std::byte*>(this);
  // 0x12dbcd8-0x12dbce0: size at +0x08, allocator at +0x10, flags 0 at +0x1c, block pointer 0 at +0x00.
  StoreU64(self, 0x08, size);
  StorePtr(self, 0x10, allocator);
  StoreU32(self, 0x1c, 0);
  StorePtr(self, 0x00, nullptr);

  void* block = nullptr;
  if (size == 0) {
    // 0x12dbd08: no allocation; the block pointer becomes all ones.
    block = reinterpret_cast<void*>(~std::uintptr_t{0});
  } else if (alignment == 0) {
    // 0x12dbd10-0x12dbd20: allocator->vtable[+0x10](allocator, size).
    block = VirtualSlot<AllocFn>(allocator, 0x10)(allocator, size);
  } else {
    // 0x12dbcf0-0x12dbd00: allocator->vtable[+0x30](allocator, alignment, size); x2 still holds size from 0x12dbce8.
    block = VirtualSlot<AllocAlignedFn>(allocator, 0x30)(allocator, alignment, size);
  }

  // 0x12dbd24-0x12dbd30: the block pointer is stored, then the flags word is read after the call, alignment goes to
  // +0x18 and bit 0 is set in the flags.
  StorePtr(self, 0x00, block);
  const std::uint32_t flags = LoadU32(self, 0x1c);
  StoreU32(self, 0x18, static_cast<std::uint32_t>(alignment));
  StoreU32(self, 0x1c, flags | 1u);
}

}  // namespace NRadEngine

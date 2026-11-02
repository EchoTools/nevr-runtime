// NRadEngine::CMemBlock::Constructor_
// Quest libr15.so VA 0x12dbb58; ReVault binary 576.
// ReVault raw parent 100220295, reconstruction node 100600676; grade M: all 43 instructions read and checked; the 3 indirect calls (blr) are ReVault-named CAllocator slots (Alloc +0x10, AllocAligned +0x30, Free +0x38 or FreeAligned +0x40), placeholders: none; Alloc and AllocAligned return long in ReVault and are declared void* here (ambiguous return width); syntax-checked with mingw.
// @0x12dbb58 — NRadEngine::CMemBlock::Constructor_
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// CAllocator vtable (0x372df40), slots in the binary's order. +0x00 and +0x08 are the two destructor entries
// (~CAllocator at 0x12c8654 and 0x12c8658; the destructor body is not reconstructed here). The names of the other
// slots are ReVault's, taken from the CLinearAllocator vtable (0x372e4d8), whose overrides sit at the same slot
// indices; CAllocator itself lists them as PURE_VIRTUAL.
class CAllocator {
 public:
  virtual ~CAllocator();
  // +0x10. ReVault: long Alloc(unsigned long long). The result is stored as the block pointer.
  virtual void* Alloc(std::uint64_t size) = 0;
  // +0x18. ReVault: long ReAlloc(void const*, unsigned long long). Parameter names not recovered.
  virtual void* ReAlloc(const void*, std::uint64_t) = 0;
  // +0x20. ReVault: undefined8 Calloc(unsigned long long, unsigned long long). Parameter names not recovered.
  virtual void* Calloc(std::uint64_t, std::uint64_t) = 0;
  // +0x28. ReVault: undefined8 ReCalloc(void const*, unsigned long long, unsigned long long). Parameter names not recovered.
  virtual void* ReCalloc(const void*, std::uint64_t, std::uint64_t) = 0;
  // +0x30. ReVault: long AllocAligned(unsigned long long, unsigned long long). x1 = alignment, x2 = size.
  virtual void* AllocAligned(std::uint64_t alignment, std::uint64_t size) = 0;
  // +0x38. ReVault: void Free(void const*). x1 = block.
  virtual void Free(const void* block) = 0;
  // +0x40. ReVault: void FreeAligned(void const*). x1 = block.
  virtual void FreeAligned(const void* block) = 0;
};

// Re-initialises a CMemBlock that already holds an allocator: releases the block it holds (through the allocator at
// +0x10), stores the new size, then allocates from that allocator (Alloc, vtable +0x10, when alignment is zero; else
// AllocAligned, vtable +0x30). Raw layout, none of it recovered as named fields: +0x00 block pointer, +0x08 size (u64),
// +0x10 CAllocator*, +0x18 alignment (int32), +0x1c flags (u32; bit 0 set here, other bits kept). This function accesses
// only +0x00 through +0x1f of the 0x38-byte CMemBlock (size from the merged session_api_execute.cpp and
// cmem_block_resize.cpp, not from a ReVault type record).
struct CMemBlock {
  alignas(8) std::byte raw_[0x38];

  // NRadEngine::CMemBlock::Constructor_(unsigned long long, unsigned long long): size and alignment; this in x0.
  void Constructor_(std::uint64_t size, std::uint64_t alignment);
};
static_assert(alignof(CMemBlock) == 8);
static_assert(offsetof(CMemBlock, raw_) == 0x00);
static_assert(sizeof(CMemBlock::raw_) == 0x38);

namespace {

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
  std::uint64_t value = 0;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

std::uint32_t LoadU32(const std::byte* base, std::size_t offset) {
  std::uint32_t value = 0;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

std::uint8_t LoadU8(const std::byte* base, std::size_t offset) {
  std::uint8_t value = 0;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

void* LoadPtr(const std::byte* base, std::size_t offset) {
  void* value = nullptr;
  std::memcpy(&value, base + offset, sizeof value);
  return value;
}

void StorePtr(std::byte* base, std::size_t offset, const void* value) {
  std::memcpy(base + offset, &value, sizeof value);
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
  std::memcpy(base + offset, &value, sizeof value);
}

void StoreU32(std::byte* base, std::size_t offset, std::uint32_t value) {
  std::memcpy(base + offset, &value, sizeof value);
}

}  // namespace

void CMemBlock::Constructor_(std::uint64_t size, std::uint64_t alignment) {
  std::byte* const self = raw_;

  // 0x12dbb60-0x12dbb78: the old block is released only when its size (+0x08) and its block pointer (+0x00) are set.
  if (LoadU64(self, 0x08) != 0 && LoadPtr(self, 0x00) != nullptr) {
    // 0x12dbb7c-0x12dbb80: bit 0 of the flags byte (+0x1c) marks a block the allocator owns.
    if ((LoadU8(self, 0x1c) & 1u) != 0) {
      // 0x12dbb84-0x12dbb9c: the stored alignment (+0x18) picks the release slot: zero takes +0x38, otherwise +0x40.
      auto* const allocator = static_cast<CAllocator*>(LoadPtr(self, 0x10));
      void* const old_block = LoadPtr(self, 0x00);
      if (LoadU32(self, 0x18) == 0) {
        // 0x12dbba0: blr with x8 = vtable +0x38 (x0 = allocator, x1 = block).
        allocator->Free(old_block);
      } else {
        // 0x12dbba0: blr with x8 = vtable +0x40 (x0 = allocator, x1 = block).
        allocator->FreeAligned(old_block);
      }
    }
    // 0x12dbba4: the block pointer and the size are cleared.
    StoreU64(self, 0x00, 0);
    StoreU64(self, 0x08, 0);
  }

  // 0x12dbba8: the new size is stored before the allocation.
  StoreU64(self, 0x08, size);

  void* block = nullptr;
  if (size == 0) {
    // 0x12dbbd0: a zero size gives block = -1 and no allocation.
    block = reinterpret_cast<void*>(~std::uintptr_t{0});
  } else {
    auto* const allocator = static_cast<CAllocator*>(LoadPtr(self, 0x10));
    if (alignment == 0) {
      // 0x12dbbd8-0x12dbbe4: blr with x8 = vtable +0x10 (x0 = allocator, x1 = size).
      block = allocator->Alloc(size);
    } else {
      // 0x12dbbb4-0x12dbbc8: blr with x8 = vtable +0x30 (x0 = allocator, x1 = alignment, x2 = size).
      block = allocator->AllocAligned(alignment, size);
    }
  }

  // 0x12dbbe8-0x12dbbf4: the flags word is read after the call, the block pointer is stored, alignment (low 32 bits)
  // goes to +0x18 and flags | 1 to +0x1c.
  const std::uint32_t flags = LoadU32(self, 0x1c);
  StorePtr(self, 0x00, block);
  StoreU32(self, 0x18, static_cast<std::uint32_t>(alignment));
  StoreU32(self, 0x1c, flags | 1u);
}

}  // namespace NRadEngine

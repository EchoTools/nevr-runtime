// NRadEngine::CMemBlock::~CMemBlock
// Quest libr15.so VA 0x12dbd40; ReVault binary 576.
// ReVault raw parent 100220303, reconstruction node 100600675; grade H: all 19 instructions checked; the one indirect call (blr) reaches CAllocator::Free (vtable +0x38) or CAllocator::FreeAligned (vtable +0x40), both named by ReVault; placeholders: none; syntax-checked with mingw.
// @0x12dbd40 — NRadEngine::CMemBlock::~CMemBlock
/* @confidence: H */

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

// Raw layout of CMemBlock, none of it recovered as named fields: +0x00 block pointer, +0x08 size (u64), +0x10
// CAllocator*, +0x18 alignment (int32), +0x1c flags (byte, bit 0 tested). This function accesses only +0x00 through
// +0x1f of the 0x38-byte CMemBlock (size from the merged session_api_execute.cpp and cmem_block_resize.cpp, not from a
// ReVault type record).
struct CMemBlock {
  alignas(8) std::byte raw_[0x38];

  // NRadEngine::CMemBlock::~CMemBlock(), thunk 0x0100e200; the body is at 0x12dbd40.
  ~CMemBlock();
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

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
  std::memcpy(base + offset, &value, sizeof value);
}

}  // namespace

CMemBlock::~CMemBlock() {
  std::byte* const self = raw_;
  // 0x12dbd44-0x12dbd54: the size word (+0x08) and the block pointer (+0x00) must both be nonzero.
  if (LoadU64(self, 0x08) != 0 && LoadPtr(self, 0x00) != nullptr) {
    // 0x12dbd58-0x12dbd5c: bit 0 of the flags byte (+0x1c) selects the release.
    if ((LoadU8(self, 0x1c) & 1u) != 0) {
      // 0x12dbd60-0x12dbd6c: the allocator is at +0x10; alignment (+0x18) zero selects slot +0x38, else slot +0x40.
      auto* const allocator = static_cast<CAllocator*>(LoadPtr(self, 0x10));
      const void* const block = LoadPtr(self, 0x00);
      if (LoadU32(self, 0x18) == 0) {
        // 0x12dbd7c: blr with x8 = vtable +0x38, x0 = allocator, x1 = the block pointer loaded at 0x12dbd4c.
        allocator->Free(block);
      } else {
        // 0x12dbd7c: blr with x8 = vtable +0x40, x0 = allocator, x1 = the block pointer loaded at 0x12dbd4c.
        allocator->FreeAligned(block);
      }
    }
    // 0x12dbd80: both words are zeroed; reached from the flag test as well as after the call.
    StoreU64(self, 0x00, 0);
    StoreU64(self, 0x08, 0);
  }
  // 0x12dbd84-0x12dbd88: epilogue and ret.
}

}  // namespace NRadEngine

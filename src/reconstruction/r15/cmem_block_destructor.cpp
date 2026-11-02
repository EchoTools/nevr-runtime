// NRadEngine::CMemBlock::Destructor_
// Quest libr15.so VA 0x12dbc04; ReVault binary 576.
// ReVault raw parent 100220298, reconstruction node 100600693; grade H: all 19 instructions (0x12dbc04-0x12dbc4c) checked, no placeholder; both allocator release slots are named (Free +0x38, FreeAligned +0x40).
// @0x12dbc04 — NRadEngine::CMemBlock::Destructor_
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// CAllocator vtable (0x372df40), slots in the binary's order. +0x00 and +0x08 are the two destructor entries
// (~CAllocator at 0x12c8654 and 0x12c8658). The names of the other slots are ReVault's, taken from the CLinearAllocator
// vtable (0x372e4d8), whose overrides sit at the same slot indices; CAllocator itself lists them as PURE_VIRTUAL.
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

// Raw layout, none of it recovered as named fields: +0x00 block pointer, +0x08 size (u64), +0x10 CAllocator*,
// +0x18 alignment (int32; zero selects Free, else FreeAligned), +0x1c flags (byte; bit 0 selects the release). This
// function accesses only +0x00 through +0x1f of the 0x38-byte CMemBlock (size as in the merged session_api_execute.cpp,
// not from a ReVault type record). Its body is byte-identical to ~CMemBlock at 0x12dbd40.
struct CMemBlock {
  alignas(8) std::byte raw_[0x38];

  // NRadEngine::CMemBlock::Destructor_(), Quest libr15.so VA 0x12dbc04: this in x0, no other argument.
  void Destructor_();
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

void CMemBlock::Destructor_() {
  std::byte* const self = raw_;
  // 0x12dbc08-0x12dbc18: the size word (+0x08) and the block pointer (+0x00) must both be nonzero.
  if (LoadU64(self, 0x08) != 0 && LoadPtr(self, 0x00) != nullptr) {
    // 0x12dbc1c-0x12dbc20: bit 0 of the flags byte (+0x1c) selects the release.
    if ((LoadU8(self, 0x1c) & 1u) != 0) {
      // 0x12dbc24-0x12dbc30: the allocator is at +0x10; alignment (+0x18) zero selects slot +0x38, else slot +0x40.
      auto* const allocator = static_cast<CAllocator*>(LoadPtr(self, 0x10));
      const void* const block = LoadPtr(self, 0x00);
      if (LoadU32(self, 0x18) == 0) {
        // 0x12dbc3c-0x12dbc40: blr with x8 = vtable +0x38 (x0 = allocator, x1 = block).
        allocator->Free(block);
      } else {
        // 0x12dbc34-0x12dbc40: blr with x8 = vtable +0x40 (x0 = allocator, x1 = block).
        allocator->FreeAligned(block);
      }
    }
    // 0x12dbc44: both words are zeroed; reached from the flag test as well as after the call.
    StoreU64(self, 0x00, 0);
    StoreU64(self, 0x08, 0);
  }
  // 0x12dbc48-0x12dbc4c: epilogue and ret.
}

}  // namespace NRadEngine

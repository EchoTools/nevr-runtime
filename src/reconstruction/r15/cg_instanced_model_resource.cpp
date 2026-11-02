// NRadEngine::CGInstancedModelResource::CGInstancedModelResource
// Quest libr15.so VA 0x1a078dc; ReVault binary 576.
// ReVault raw parent 100260884, reconstruction node 100600488; grade M: all 94 instructions traced to the stored size 376 (ret at 0x1a07a50); no branches; 17 callees named through thunks and checked against their mangled symbols; the two vptr values come from load-time relocation slots (file bytes 0), so they are not byte-checked.
// @0x1a078dc — NRadEngine::CGInstancedModelResource::CGInstancedModelResource
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

// Vtable addresses held in two data slots. Each slot is a load-time relocation (its file bytes are 0), so the
// address is not in the file. The stored vptr is the slot value plus 0x10.
extern const std::byte* const g_cpresence_vtable_0x381f860;                 // *(u64*)0x0381f860: CPresence vtable
extern const std::byte* const g_cginstancedmodelresource_vtable_0x38299e0;  // *(u64*)0x038299e0: CGInstancedModelResource vtable

namespace NRadEngine {

class CAllocator;
class CPresenceInit;

class CMemoryContext {
public:
    // NRadEngine::CMemoryContext::Current(void), thunk 0x01028430. One 64-bit word, stored at +0x30.
    static std::uint64_t Current();
    // NRadEngine::CMemoryContext::CurrentPtr(void), thunk 0x00ffbc50. The allocator passed to CMemBlock.
    static CAllocator* CurrentPtr();
};

// NRadEngine::CMemBlock::CMemBlock(unsigned long long, unsigned long long, CAllocator*), thunk 0x00fedec0.
class CMemBlock {
public:
    CMemBlock(std::uint64_t, std::uint64_t, CAllocator* allocator);
};

class CGInstancedModelResource {
public:
    // Raw layout used here: +0x00 vptr (CPresence, then CGInstancedModelResource), +0x08 zero word, +0x10 and +0x18
    // zero words, +0x20 and +0x28 words copied from the CPresenceInit argument (its +0x08 and +0x10), +0x30 the
    // CMemoryContext word, +0x38 all ones, +0x40..+0x5f zero, +0x60 a 16-byte header, +0x70 zero word, and eight
    // CMemBlock objects from +0x78 to +0x200, 0x38 apart. Each CMemBlock has its capacity word at +0x20 and zeros at
    // +0x28 and +0x30.
    CGInstancedModelResource(const CPresenceInit& init);
};

namespace {

// The 16 bytes at VA 0x02d35df0 (low word 7, high word 0), stored at +0x60.
constexpr std::uint64_t kModelHeaderWords[2] = {7, 0};
// The capacity word written at +0x20 of each CMemBlock (mov w20,#0x20 at 0x01a07958).
constexpr std::uint64_t kBlockCapacity = 0x20;

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StorePtr(std::byte* base, std::size_t offset, const std::byte* value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// Constructs a CMemBlock at +offset with the allocator from CMemoryContext::CurrentPtr(), then writes its
// capacity word at +0x20 and zeros at +0x28 and +0x30.
void InitBlock(std::byte* self, std::size_t offset) {
    CAllocator* const allocator = CMemoryContext::CurrentPtr();
    new (self + offset) CMemBlock(0, 0, allocator);
    StoreU64(self, offset + 0x20, kBlockCapacity);
    StoreU64(self, offset + 0x28, 0);
    StoreU64(self, offset + 0x30, 0);
}

}  // namespace

CGInstancedModelResource::CGInstancedModelResource(const CPresenceInit& init) {
    auto* const self = reinterpret_cast<std::byte*>(this);
    const auto* const source = reinterpret_cast<const std::byte*>(&init);

    StoreU64(self, 0x10, 0);
    StoreU64(self, 0x18, 0);
    StorePtr(self, 0x00, g_cpresence_vtable_0x381f860 + 0x10);
    StoreU64(self, 0x08, 0);
    StoreU64(self, 0x20, LoadU64(source, 0x08));
    StoreU64(self, 0x28, LoadU64(source, 0x10));
    StoreU64(self, 0x30, CMemoryContext::Current());
    StoreU64(self, 0x38, ~std::uint64_t{0});
    std::memset(self + 0x40, 0, 0x20);
    StorePtr(self, 0x00, g_cginstancedmodelresource_vtable_0x38299e0 + 0x10);
    std::memcpy(self + 0x60, kModelHeaderWords, sizeof kModelHeaderWords);
    StoreU64(self, 0x70, 0);

    InitBlock(self, 0x78);
    InitBlock(self, 0xb0);
    InitBlock(self, 0xe8);
    InitBlock(self, 0x120);
    InitBlock(self, 0x158);
    InitBlock(self, 0x190);
    InitBlock(self, 0x1c8);
    InitBlock(self, 0x200);
}

}  // namespace NRadEngine

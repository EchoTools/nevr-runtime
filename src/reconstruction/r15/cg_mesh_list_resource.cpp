// NRadEngine::CGMeshListResource::CGMeshListResource
// Quest libr15.so VA 0x19677cc; ReVault binary 576.
// ReVault raw parent 100257968, reconstruction node 100600478; grade M: all 129 instructions traced to the stored size 516 (the ret at 0x19679cc), every callee named from its thunk; the two vtable slots are load-time relocations (file bytes 0), so the vptr values are not byte-checked.
// @0x19677cc — NRadEngine::CGMeshListResource::CGMeshListResource
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

// Vtable addresses held in two data slots. Each slot is a load-time relocation (its file bytes are 0), so the
// address is not in the file. The stored vptr is the slot value plus 0x10.
extern const std::byte* const g_cpresence_vtable_0x381f860;           // *(u64*)0x0381f860: CPresence vtable
extern const std::byte* const g_cgmeshlistresource_vtable_0x380c7d8;  // *(u64*)0x0380c7d8: CGMeshListResource vtable

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

// NRadEngine::CMemBlock::CMemBlock(CMemBlock*, ulonglong, ulonglong, CAllocator*), thunk 0x00fedec0.
class CMemBlock {
public:
    CMemBlock(std::uint64_t, std::uint64_t, CAllocator* allocator);
};

// NRadEngine::CGMeshListData::CGMeshListData(void), 0x019679d0, reached through thunk 0x00ff9c70.
class CGMeshListData {
public:
    CGMeshListData();
};

class CGMeshListResource {
public:
    // Raw layout used here: +0x00 vptr, +0x08 zero word, +0x10 and +0x18 zero words, +0x20 and +0x28 words copied from
    // the CPresenceInit argument (at its +0x08 and +0x10), +0x30 the CMemoryContext word, +0x38 all ones,
    // +0x40..+0x5f zero, +0x60 a 16-byte constant, +0x70 CGMeshListData, eleven CMemBlock objects from +0x2b0
    // to +0x500 (0x38 apart). Each CMemBlock has its capacity word at +0x20 and zeros at +0x28 and +0x30.
    CGMeshListResource(const CPresenceInit& init);
};

namespace {

// The 16 bytes at VA 0x02d35df0 (low word 7, high word 0), stored at +0x60.
constexpr std::uint64_t kMeshListCounts[2] = {7, 0};
// The capacity word written at +0x20 of each CMemBlock (mov w20,#0x20).
constexpr std::uint64_t kBlockCapacity = 0x20;

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StorePtr(std::byte* base, std::size_t offset, const std::byte* value) {
    std::memcpy(base + offset, &value, sizeof value);
}

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

// Constructs a CMemBlock at +offset with the allocator from CMemoryContext::CurrentPtr(), then writes its
// capacity word at +0x20 and zeros at +0x30 and +0x28.
void InitBlock(std::byte* self, std::size_t offset) {
    CAllocator* const allocator = CMemoryContext::CurrentPtr();
    new (self + offset) CMemBlock(0, 0, allocator);
    StoreU64(self, offset + 0x20, kBlockCapacity);
    StoreU64(self, offset + 0x30, 0);
    StoreU64(self, offset + 0x28, 0);
}

}  // namespace

CGMeshListResource::CGMeshListResource(const CPresenceInit& init) {
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
    StorePtr(self, 0x00, g_cgmeshlistresource_vtable_0x380c7d8 + 0x10);
    std::memcpy(self + 0x60, kMeshListCounts, sizeof kMeshListCounts);
    new (self + 0x70) CGMeshListData();

    InitBlock(self, 0x2b0);
    InitBlock(self, 0x2e8);
    InitBlock(self, 0x320);
    InitBlock(self, 0x358);
    InitBlock(self, 0x390);
    InitBlock(self, 0x3c8);
    InitBlock(self, 0x400);
    InitBlock(self, 0x438);
    InitBlock(self, 0x470);
    InitBlock(self, 0x4a8);
    InitBlock(self, 0x4e0);
}

}  // namespace NRadEngine

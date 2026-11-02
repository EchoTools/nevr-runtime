// NRadEngine::CInputListener::CInputListener
// Quest libr15.so VA 0x26379f4; ReVault binary 576.
// ReVault raw parent 100319383, reconstruction node 100600783; grade M: all 57 instructions (0x26379f4-0x2637ad4, stored size 228) checked; all three callees carry real names (CMemoryContext::CurrentPtr, CMemBlock::CMemBlock, CTableXT<...SNamedInputDevice>::GrowBy), no placeholder; M because the class size is in neither ReVault nor the merged tree, so the file asserts only the 0x90 lower bound that the stores reach.
// @0x26379f4 — NRadEngine::CInputListener::CInputListener
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace NRadEngine {

class CAllocator;
class CInputDevice;

class CMemoryContext {
public:
    // NRadEngine::CMemoryContext::CurrentPtr() @0x012cae90 (thunk 0x00ffbc50). Static; takes no arguments.
    static CAllocator* CurrentPtr();
};

class CMemBlock {
public:
    // NRadEngine::CMemBlock::CMemBlock(unsigned long long, unsigned long long, CAllocator*) @0x012dbcc8 (thunk 0x00fedec0).
    // Built in place: block pointer +0x00, size +0x08, allocator +0x10, alignment +0x18, flags +0x1c. No size is
    // asserted here: the merged tree gives 0x38 in most files and 0x28 in one.
    CMemBlock(std::uint64_t size, std::uint64_t alignment, CAllocator* allocator);
};

class CInputListener {
public:
    // Element of the table that starts at +0x00 (see GrowBy below): 16 bytes, +0x00 the CInputDevice*, +0x08 the
    // device word at its +0x10. ReVault has no layout for it.
    struct SNamedInputDevice;

    // NRadEngine::CInputListener::CInputListener(NRadEngine::CInputDevice*) @0x026379f4; reached through thunk 0x01034dd0.
    CInputListener(CInputDevice* device);

private:
    // The bytes this constructor writes, +0x00 to +0x8f. The full size is in neither ReVault nor the merged tree, so
    // this is a lower bound, not the class size.
    alignas(8) std::byte raw_[0x90];
};

template <typename T>
class CTableXT;

template <>
class CTableXT<CInputListener::SNamedInputDevice> {
public:
    // NRadEngine::CTableXT<NRadEngine::CInputListener::SNamedInputDevice>::GrowBy(unsigned long long) @0x02639e5c
    // (thunk 0x00ff5bb0). Adds `count` rows and returns the count read on entry.
    std::uint64_t GrowBy(std::uint64_t count);
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

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    std::byte* value = nullptr;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

}  // namespace

CInputListener::CInputListener(CInputDevice* device) {
    auto* const self = reinterpret_cast<std::byte*>(this);

    // 0x26379f4-0x2637a18: CMemBlock at +0x00 with size 0x80, alignment 0 and CMemoryContext::CurrentPtr() as allocator.
    new (self) CMemBlock(0x80, 0, CMemoryContext::CurrentPtr());
    // 0x2637a2c-0x2637a30: +0x40 zero (u64), +0x48 zero (u32).
    StoreU64(self, 0x40, 0);
    StoreU32(self, 0x48, 0);
    // 0x2637a1c-0x2637a34: +0x28 = 8, +0x20 = 0, +0x38 = -1, +0x30 = 0 (the 16-byte constants at 0x2c9fed0 and 0x2c2fff0).
    StoreU64(self, 0x28, 8);
    StoreU64(self, 0x20, 0);
    StoreU64(self, 0x38, ~std::uint64_t{0});
    StoreU64(self, 0x30, 0);
    // 0x2637a38-0x2637a50: CMemBlock at +0x50 with size 0x24000, alignment 0 and CMemoryContext::CurrentPtr() as allocator.
    new (self + 0x50) CMemBlock(0x24000, 0, CMemoryContext::CurrentPtr());
    // 0x2637a54-0x2637a64: +0x78 = 0x100, +0x70 = 0, +0x88 = 1, +0x80 = 0 (the 16-byte constants at 0x2ca3d50 and 0x2c30580).
    StoreU64(self, 0x78, 0x100);
    StoreU64(self, 0x70, 0);
    StoreU64(self, 0x88, 1);
    StoreU64(self, 0x80, 0);

    // 0x2637a68: a null device skips the registration.
    if (device == nullptr) {
        return;
    }

    // 0x2637a6c-0x2637a7c: when the count at +0x30 is zero, GrowBy(1) adds one row and returns the count read on entry.
    if (LoadU64(self, 0x30) == 0) {
        auto* const table = reinterpret_cast<CTableXT<CInputListener::SNamedInputDevice>*>(this);
        const std::uint64_t old_count = table->GrowBy(1);
        // 0x2637a80-0x2637a94: limit = min(old_count + 1, count at +0x30), by csinc and b.ls.
        std::uint64_t limit = LoadU64(self, 0x30);
        if (old_count + 1 < limit) {
            limit = old_count + 1;
        }
        // 0x2637a98-0x2637ab8: each row in [old_count, limit) becomes {0, -1}; the block pointer is reloaded each time.
        for (std::uint64_t row = old_count; row < limit; ++row) {
            std::byte* const block = LoadPtr(self, 0x00);
            StoreU64(block, row * 0x10, 0);
            StoreU64(block, row * 0x10 + 0x08, ~std::uint64_t{0});
        }
    }

    // 0x2637abc-0x2637ac8: row 0 becomes {device, the word at device+0x10}.
    std::byte* const row0 = LoadPtr(self, 0x00);
    StorePtr(row0, 0x00, device);
    StoreU64(row0, 0x08, LoadU64(reinterpret_cast<const std::byte*>(device), 0x10));
}

}  // namespace NRadEngine

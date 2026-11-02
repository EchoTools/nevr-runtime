// NRadEngine::CTableXT<NRadEngine::NRadGame::SR15NetBanReasonExpressionData>::Reset
// Quest libr15.so VA 0x135f634; ReVault binary 576.
// ReVault raw parent 100223556, reconstruction node 100600501; grade H: all 77 instructions and every branch (tbnz, tst, cbz, csel, b.cs, the tail calls) checked against revault_disassemble; stored size 308 ends at the final ret.
// @0x135f634 — NRadEngine::CTableXT<NRadEngine::NRadGame::SR15NetBanReasonExpressionData>::Reset
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CAllocator;

namespace NRadGame {
struct SR15NetBanReasonExpressionData;
}  // namespace NRadGame

class CMemoryContext {
public:
    // NRadEngine::CMemoryContext::CurrentPtr() @0x012cae90 (thunk 0x00ffbc50). Static; takes no arguments.
    static CAllocator* CurrentPtr();
    // NRadEngine::CMemoryContext::SetCurrent(NRadEngine::CAllocator*) @0x012cafac (thunk 0x00fd3590). Static.
    static void SetCurrent(CAllocator* allocator);
};

class CMemBlock {
public:
    // NRadEngine::CMemBlock::Size() const @0x012dc1c4 (thunk 0x01017920).
    std::uint64_t Size() const;
    // NRadEngine::CMemBlock::Resize(unsigned long long, unsigned int) @0x012dc30c (thunk 0x0103e400).
    void Resize(std::uint64_t size, std::uint32_t mode);
};

template <typename T>
class CTable;

template <typename T>
class CTableXT;

template <>
class CTable<NRadGame::SR15NetBanReasonExpressionData> {
public:
    // NRadEngine::CTable<NRadEngine::NRadGame::SR15NetBanReasonExpressionData>::Reset(unsigned long long, unsigned long long)
    // @0x013de12c (thunk 0x01011bc0). Tail-called on the same object.
    void Reset(std::uint64_t count, std::uint64_t flag);
};

template <>
class CTableXT<NRadGame::SR15NetBanReasonExpressionData> {
public:
    // NRadEngine::CTableXT<...>::GrowBy(unsigned long long) @0x013de268 (thunk 0x0104abd0). Result unused.
    std::uint64_t GrowBy(std::uint64_t count);
    void Reset(std::uint64_t count, std::uint64_t flag);
};

namespace {

// Offsets in the table object, which is also a CMemBlock at offset 0. The rest of the layout is not recovered.
constexpr std::size_t kAllocatorOffset = 0x10;     // CAllocator* that the row array is reset under
constexpr std::size_t kFlagsOffset = 0x1c;         // u32 CMemBlock flags; bit 2 and bits 1-2 are tested
constexpr std::size_t kSavedOffset = 0x20;         // u64 set to 1 around GrowBy, then restored
constexpr std::size_t kResizedCountOffset = 0x28;  // u64 set from the row count after Resize
constexpr std::size_t kCountOffset = 0x30;         // u64 row count
// Bytes per row: the row array is count * 0xc8 bytes (the stride of SR15NetBanReasonExpressionData).
constexpr std::uint64_t kRowBytes = 0xc8;

std::uint8_t LoadU8(const std::byte* base, std::size_t offset) {
    std::uint8_t value = 0;
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

std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    std::byte* value = nullptr;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

}  // namespace

void CTableXT<NRadGame::SR15NetBanReasonExpressionData>::Reset(std::uint64_t count, std::uint64_t flag) {
    auto* const self = reinterpret_cast<std::byte*>(this);
    auto* const block = reinterpret_cast<CMemBlock*>(this);

    // 0x135f640-0x135f650: when bit 2 of the flags is clear, the row array is reset under the table's allocator.
    std::uint32_t flags = LoadU32(self, kFlagsOffset);
    if ((flags & 0x4u) == 0) {
        // 0x135f654-0x135f674: read the allocator, save the current context, make the allocator current, clear the
        // count, make the saved context current again, then read the flags again.
        CAllocator* const allocator = reinterpret_cast<CAllocator*>(LoadPtr(self, kAllocatorOffset));
        CAllocator* const previous = CMemoryContext::CurrentPtr();
        CMemoryContext::SetCurrent(allocator);
        StoreU64(self, kCountOffset, 0);
        CMemoryContext::SetCurrent(previous);
        flags = LoadU32(self, kFlagsOffset);
    }

    // 0x135f678-0x135f698: bit 1 or bit 2 set -> the base class Reset runs on the same object (tail call).
    if ((flags & 0x6u) != 0) {
        reinterpret_cast<CTable<NRadGame::SR15NetBanReasonExpressionData>*>(this)->Reset(count, flag);
        return;
    }

    // 0x135f69c-0x135f6a0: rows = max(count, flag) (csel on cc).
    const std::uint64_t rows = count < flag ? flag : count;
    if (rows == 0) {  // cbz x21, 0x135f710
        // 0x135f710-0x135f754: bit 2 set -> return; otherwise shrink the row array when it is larger than the rows.
        if ((flags & 0x4u) != 0) {
            return;
        }
        const std::uint64_t size = block->Size();
        const std::uint64_t bytes = LoadU64(self, kCountOffset) * kRowBytes;
        if (bytes < size) {
            block->Resize(bytes, 1);
            StoreU64(self, kResizedCountOffset, LoadU64(self, kCountOffset));
            block->Size();
        }
        return;
    }

    // 0x135f6a8-0x135f6c8: GrowBy(rows) runs with the u64 at +0x20 set to 1; the flags are read after it.
    const std::uint64_t saved = LoadU64(self, kSavedOffset);
    StoreU64(self, kSavedOffset, 1);
    GrowBy(rows);
    const std::uint8_t flags_after = LoadU8(self, kFlagsOffset);
    StoreU64(self, kSavedOffset, saved);
    StoreU64(self, kCountOffset, rows);

    // 0x135f6cc-0x135f704: bit 2 of the flags clear -> the row array is resized when it is smaller than the rows.
    if ((flags_after & 0x4u) == 0) {
        const std::uint64_t size = block->Size();
        const std::uint64_t bytes = LoadU64(self, kCountOffset) * kRowBytes;
        if (bytes < size) {
            block->Resize(bytes, 1);
            StoreU64(self, kResizedCountOffset, LoadU64(self, kCountOffset));
            block->Size();
        }
    }

    // 0x135f708: the count is set to the count argument, not to the rows.
    StoreU64(self, kCountOffset, count);
}

}  // namespace NRadEngine

// NRadEngine::CStackAllocator::DirectFree
// Quest libr15.so VA 0x12df3fc; ReVault binary 576.
// ReVault raw parent 100220423, reconstruction node 100600695; grade H: all 18 instructions (72 bytes, ret at 0x12df440) checked against the listing; no callees or globals, so no placeholders; the record block at this+0x48 has no ReVault layout and is written at raw offsets; syntax-checked with mingw
// @0x12df3fc — NRadEngine::CStackAllocator::DirectFree
/* @confidence: H */

#include <cstddef>
#include <cstdint>

namespace NRadEngine {
struct CStackAllocator {
    // 0x50 is the extent DirectFree touches (the pointer at this+0x48); the true size is not recovered.
    alignas(8) std::byte raw[0x50];
    void DirectFree();
};
static_assert(sizeof(CStackAllocator) == 0x50);
} // namespace NRadEngine

namespace {
// Pointer stored at byte offset `offset` of the object at `base`.
std::byte* FieldPointer(void* base, std::size_t offset) {
    return *reinterpret_cast<std::byte**>(static_cast<std::byte*>(base) + offset);
}

// 64-bit word stored at byte offset `offset` of the object at `base`.
std::uint64_t& FieldWord(void* base, std::size_t offset) {
    return *reinterpret_cast<std::uint64_t*>(static_cast<std::byte*>(base) + offset);
}
} // namespace

void NRadEngine::CStackAllocator::DirectFree() {
    // this+0x48: pointer to the record block (Alloc at 0x12df5c4 reads it the same way).
    // block+0x08: record array of 16-byte records; block+0x10: record count.
    // Word +0x8 of each record is the aligned size that Alloc stores there.
    std::byte* const block = FieldPointer(this, 0x48);
    if (FieldWord(block, 0x10) != 0) {
        // Pop the top record, then keep popping while the record below the new top has a zero size word.
        std::uint64_t index = FieldWord(block, 0x10) - 1;
        FieldWord(block, 0x10) = index;
        if (1 < index) {
            // Word +0x8 of record (index - 1): block+0x08 + index*0x10 - 0x8.
            std::byte* word = FieldPointer(block, 0x08) + index * 0x10 - 0x8;
            do {
                if (*reinterpret_cast<std::uint64_t*>(word) != 0) {
                    return;
                }
                index = index - 1;
                word = word - 0x10;
                FieldWord(block, 0x10) = index;
            } while (1 < index);
        }
    }
}

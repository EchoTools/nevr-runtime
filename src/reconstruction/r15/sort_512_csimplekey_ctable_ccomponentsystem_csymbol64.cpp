// NRadEngine::Sort<512ull, CSimpleKey<CTable<CComponentSystem*>, CSymbol64>*, CTable<CSimpleKey<CTable<CComponentSystem*>, CSymbol64>>::Sort<CSimpleKey<CTable<CComponentSystem*>, CSymbol64>>()::{lambda#1}, Sort<512ull, ...>()::{lambda(CSimpleKey<CTable<CComponentSystem*>, CSymbol64>*, unsigned long long, unsigned long long)#1}>
// Quest libr15.so VA 0x130ec20; ReVault binary 576.
// ReVault raw parent 100427640, reconstruction node 100600701; grade M: all 112 instructions (0x130ec20-0x130eddc) read and each branch checked against the flags that feed it; 0 placeholders (memset is the libc import, swap is NRadEngine::swap); the element is the merged SystemEntry, not ReVault's CSimpleKey<CTable<CComponentSystem*>,CSymbol64>, so not every name is ReVault's; syntax-checked.
// @0x130ec20 — NRadEngine::Sort<512ull, CSimpleKey<CTable<CComponentSystem*>, CSymbol64>*, CTable<CSimpleKey<CTable<CComponentSystem*>, CSymbol64>>::Sort<CSimpleKey<CTable<CComponentSystem*>, CSymbol64>>()::{lambda#1}, Sort<512ull, ...>()::{lambda(CSimpleKey<CTable<CComponentSystem*>, CSymbol64>*, unsigned long long, unsigned long long)#1}>
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// One 0x40-byte entry of CTable<CSimpleKey<CTable<CComponentSystem*>, CSymbol64>>: the key at +0x00 is the only
// field this function reads. Reached through raw offsets.
struct SystemEntry {
    alignas(8) std::byte raw[0x40];
};
static_assert(sizeof(SystemEntry) == 0x40);

// NRadEngine::swap<CSimpleKey<CTable<CComponentSystem*>, CSymbol64>>(CSimpleKey&, CSimpleKey&), VA 0x130ede0
// (thunk 0x1052120). Out of line: it moves the whole 0x40-byte entry, so the calls below go to it.
template <class T>
void swap(T& a, T& b);

}  // namespace NRadEngine

namespace {

// The 8-byte key at +0x00 as the signed 64-bit value the compares use (cmp, b.ge, b.gt).
inline std::int64_t KeyOf(const NRadEngine::SystemEntry& entry) {
    std::int64_t key;
    std::memcpy(&key, entry.raw, sizeof(key));
    return key;
}

}  // namespace

namespace NRadEngine {

// Quicksort over [data, data + count), entries ordered by the signed 64-bit key at +0x00. An explicit stack of
// Block {lo, hi} pairs (Block * 16 = 0x2000 bytes at sp+0x10) replaces recursion. The middle entry goes to data[hi]
// as the pivot, then a three-way partition runs over [lo, hi): [lo, less_end) below the pivot, [less_end, equal_end)
// equal to it. The pivot goes to data[equal_end]; the left part [lo, less_end - 1] and the right part
// [equal_end + 1, hi] are pushed when each holds two or more entries, the right one on top so it runs first.
// x2 (Less const&) and x3 (Swap const&) are never read: the compare is inline, the swap is a call to swap at 0x130ede0.
template <unsigned long long Block, class Iterator, class Less, class Swap>
void Sort(Iterator data, unsigned long long count, [[maybe_unused]] const Less& less,
          [[maybe_unused]] const Swap& swap) {
    // 0x130ec20 cmp x1, #2 ; b.cc 0x130eddc: the ret, before any frame is built
    if (count < 2) {
        return;
    }

    unsigned long long ranges[Block * 2];    // sp+0x10 (x22), 0x2000 bytes
    std::memset(ranges, 0, sizeof(ranges));  // 0x130ec60 bl memset(sp+0x10, 0, 0x2000) (thunk 0xfe71f0)
    ranges[1] = count - 1;                   // 0x130ec6c str x20, [sp, #0x18]: first range {0, count - 1}

    long long top = 0;          // x23: the {lo, hi} pair being worked on, at sp+0x10 + top * 16
    unsigned long long lo = 0;  // x25
    for (;;) {
        const unsigned long long hi = ranges[top * 2 + 1];  // 0x130ec70 ldr x24, [x23, #8]

        // 0x130ec74-0x130ec7c: byte offset ((lo + hi) << 5) & ~0x3f, which is middle * 0x40 for the 0x40-byte entry.
        const unsigned long long middle = (((lo + hi) << 5) & ~0x3fULL) >> 6;
        NRadEngine::swap(data[middle], data[hi]);  // 0x130ec80-0x130ec8c: x20 = &data[hi]; bl swap(&data[middle], x20)

        unsigned long long less_end = lo;   // x27
        unsigned long long equal_end = lo;  // x26
        if (lo < hi) {                      // 0x130ec90-0x130ec9c: b.cs 0x130ed3c skips the scan when lo >= hi
            unsigned long long scan = lo;   // x28, with x21 = &data[scan]
            do {
                const std::int64_t scanned = KeyOf(data[scan]);  // 0x130ecb0 ldr x8, [x21]
                const std::int64_t pivot = KeyOf(data[hi]);      // 0x130ecb4 ldr x9, [x20]
                if (scanned < pivot) {                           // 0x130ecb8-0x130ecc0: b.ge 0x130ed20 not taken
                    if (scan > less_end) {                       // 0x130ecec cmp x28, x27 ; b.ls 0x130ecc4
                        if (equal_end != less_end) {             // 0x130ecf4 cmp x26, x27 ; b.eq 0x130ed08
                            NRadEngine::swap(data[equal_end], data[less_end]);  // 0x130ecfc-0x130ed04
                        }
                        if (scan != equal_end) {                 // 0x130ed08 cmp x28, x26 ; b.eq 0x130ecc4
                            NRadEngine::swap(data[scan], data[less_end]);       // 0x130ed10-0x130ed18
                        }
                    }
                    ++less_end;   // 0x130ecc4 add x27, x27, #1
                    ++equal_end;  // 0x130ecc8 add x26, x26, #1
                } else if (scanned <= pivot) {  // 0x130ed20 b.gt 0x130eccc not taken: the key equals the pivot's
                    if (scan != equal_end) {    // 0x130ed24 cmp x28, x26 ; b.eq 0x130ecc8
                        NRadEngine::swap(data[scan], data[equal_end]);  // 0x130ed2c-0x130ed34
                    }
                    ++equal_end;  // 0x130ecc8 add x26, x26, #1 (b 0x130ed38 path)
                }
                ++scan;  // 0x130eccc add x28, x28, #1 ; 0x130ecd0-0x130ecd8: b.eq 0x130ed3c when scan == hi
            } while (scan != hi);
        }

        // 0x130ed3c-0x130ed44: the pivot goes to data[equal_end] unless it is already there.
        if (hi != equal_end) {
            NRadEngine::swap(data[hi], data[equal_end]);  // 0x130ed70-0x130ed78: bl swap(&data[hi], &data[equal_end])
        }

        // 0x130ed48-0x130ed50 and 0x130ed7c-0x130ed84: the left part [lo, less_end - 1] holds two or more entries.
        if (less_end > lo + 1) {
            ranges[top * 2] = lo;                // 0x130ed88-0x130ed90 stp x25, x8, [x23]: {lo, less_end - 1}
            ranges[top * 2 + 1] = less_end - 1;
            if (equal_end + 1 < hi) {            // 0x130ed94-0x130ed9c: b.cs 0x130ed60 when equal_end + 1 >= hi
                ++top;                           // 0x130eda0 str x8, [x21, #0x10]!: the right part goes on top
                ranges[top * 2] = equal_end + 1;
                ranges[top * 2 + 1] = hi;        // 0x130eda4 str x24, [x21, #8]
            }
        } else if (equal_end + 1 < hi) {         // 0x130ed54-0x130ed5c: b.cc 0x130eda0, the right part of two or more
            ranges[top * 2] = equal_end + 1;     // 0x130eda0 with x21 = top: the pair on top is replaced
            ranges[top * 2 + 1] = hi;
        } else {
            --top;                               // 0x130ed60 mov x23, x21 with x21 = top - 1: pop
        }

        // 0x130ed64-0x130ed6c, 0x130edac-0x130edb0: the stack is empty once the pair pointer falls below sp+0x10.
        if (top < 0) {
            return;
        }
        lo = ranges[top * 2];  // 0x130edb4 ldr x25, [x23] ; 0x130edb8 b 0x130ec70
    }
}

}  // namespace NRadEngine

// NRadEngine::Sort<512ull, CSimpleKey<CComponentSystem*, CSymbol64>*, CTable<CSimpleKey<CComponentSystem*, CSymbol64>>::Sort<CSimpleKey<CComponentSystem*, CSymbol64>>()::{lambda#1}, Sort<512ull, ...>()::{lambda(CSimpleKey<CComponentSystem*, CSymbol64>*, unsigned long long, unsigned long long)#1}>
// Quest libr15.so VA 0x130d850; ReVault binary 576.
// ReVault raw parent 100427639, reconstruction node 100600678; grade H: all 120 instructions read through the ret at 0x130da2c and each branch checked against the flags that feed it, the only callee is the memset import (thunk 0xfe71f0), the Less and Swap arguments (x2, x3) are never read because their bodies are inlined, 0 placeholders; syntax-checked.
// @0x130d850 — NRadEngine::Sort<512ull, CSimpleKey<CComponentSystem*, CSymbol64>*, CTable<CSimpleKey<CComponentSystem*, CSymbol64>>::Sort<CSimpleKey<CComponentSystem*, CSymbol64>>()::{lambda#1}, Sort<512ull, ...>()::{lambda(CSimpleKey<CComponentSystem*, CSymbol64>*, unsigned long long, unsigned long long)#1}>
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace NRadEngine {

class CComponentSystem;

struct CSymbol64 {
    std::uint64_t bits;  // +0x00
};
static_assert(sizeof(CSymbol64) == 0x08);

// One 16-byte table entry: the key at +0x00, the value (here a CComponentSystem*) at +0x08. Sort orders on the
// 8 bytes at +0x00 as a signed 64-bit integer (cmp, then b.ge and b.gt).
template <class Value, class Key>
struct CSimpleKey {
    Key key;      // +0x00
    Value value;  // +0x08
};
static_assert(sizeof(CSimpleKey<CComponentSystem*, CSymbol64>) == 0x10);
static_assert(alignof(CSimpleKey<CComponentSystem*, CSymbol64>) == 0x08);

// Quicksort over [data, data + count) with an explicit stack of Block {lo, hi} pairs (Block * 16 = 0x2000 bytes at
// sp, with a 16-byte swap temp at sp + 0x2000). The middle element goes to data[hi] as the pivot, then a
// three-way partition runs over [lo, hi): [lo, less_end) below the pivot, [less_end, equal_end) equal to it.
// The pivot is swapped to data[equal_end]; the left part [lo, less_end - 1] and the right part [equal_end + 1, hi]
// are pushed when each holds two or more entries, the right one on top so it runs first.
// x2 (Less const&) and x3 (Swap const&) arrive as addresses of empty closures and are not read: the compare and
// the swap are inlined, each swap going through the 16-byte temp at sp + 0x2000.
template <unsigned long long Block, class Iterator, class Less, class Swap>
void Sort(Iterator data, unsigned long long count, [[maybe_unused]] const Less& less,
          [[maybe_unused]] const Swap& swap) {
    // @0x130d850 cmp x1, #2 ; b.cc 0x130da2c: the ret, before any frame is built
    if (count < 2) {
        return;
    }

    unsigned long long ranges[Block * 2];  // x20 = sp, 0x2000 bytes
    std::memset(ranges, 0, sizeof(ranges));  // 0x130d884 bl memset(sp, 0, 0x2000)
    ranges[1] = count - 1;                   // 0x130d890 str x21, [sp, #8]: first range {0, count - 1}

    long long top = 0;           // x8 = sp + top * 16: the {lo, hi} pair being worked on
    unsigned long long lo = 0;   // x10
    for (;;) {
        const unsigned long long hi = ranges[top * 2 + 1];  // x9: ldr x9, [x8, #8]

        // 0x130d89c-0x130d8a4: byte offset ((lo + hi) << 3) & ~0xf, i.e. the element index below.
        const unsigned long long middle = ((lo + hi) & 0x1ffffffffffffffeULL) >> 1;
        std::swap(data[middle], data[hi]);  // 0x130d8a8-0x130d8c8: pivot to data[hi]

        unsigned long long less_end = lo;   // x13
        unsigned long long equal_end = lo;  // x11
        if (lo < hi) {                      // 0x130d8cc b.cs 0x130d988 skips the scan when lo >= hi
            unsigned long long scan = lo;   // x15, with x14 = &data[scan]
            do {
                const auto scanned = static_cast<long long>(data[scan].key.bits);  // 0x130d8fc ldr x16, [x14]
                const auto pivot = static_cast<long long>(data[hi].key.bits);      // 0x130d900 ldr x17, [x12]
                if (scanned < pivot) {                                             // b.ge 0x130d960 not taken
                    if (scan > less_end) {                                         // cmp x15, x13 ; b.ls 0x130d8e4
                        if (equal_end != less_end) {                               // cmp x11, x13 ; b.eq 0x130d93c
                            std::swap(data[equal_end], data[less_end]);            // 0x130d924-0x130d938
                        }
                        if (scan != equal_end) {                                   // cmp x15, x11 ; b.eq 0x130d8e4
                            std::swap(data[scan], data[less_end]);                 // 0x130d944-0x130d958
                        }
                    }
                    ++less_end;   // 0x130d8e4 add x13, x13, #1
                    ++equal_end;  // 0x130d8e8 add x11, x11, #1
                } else if (scanned <= pivot) {  // 0x130d960 b.gt 0x130d8ec not taken: the key equals the pivot
                    if (scan != equal_end) {    // cmp x15, x11 ; b.eq 0x130d8e8
                        std::swap(data[scan], data[equal_end]);  // 0x130d96c-0x130d980
                    }
                    ++equal_end;  // 0x130d8e8
                }
                ++scan;  // 0x130d8ec add x15, x15, #1 ; cmp x9, x15 ; b.eq 0x130d988
            } while (scan != hi);
        }

        // 0x130d988 cmp x9, x11 ; b.ne 0x130d9bc: unless the scan already ended on the pivot, it goes into place.
        if (hi != equal_end) {
            std::swap(data[hi], data[equal_end]);  // 0x130d9bc-0x130d9d4
        }

        // 0x130d994 / 0x130d9d8: cmp x13, x10 + 1 ; b.hi 0x130d9e4 -- a left part of two or more entries.
        if (less_end > lo + 1) {
            ranges[top * 2] = lo;                 // 0x130d9ec stp x10, x12, [x8]
            ranges[top * 2 + 1] = less_end - 1;   // x12 = x13 - 1
            if (equal_end + 1 < hi) {             // 0x130d9f0-0x130d9f8: b.cs 0x130d9ac when equal_end + 1 >= hi
                ++top;                            // 0x130d9fc str x8, [x14, #0x10]!
                ranges[top * 2] = equal_end + 1;
                ranges[top * 2 + 1] = hi;         // 0x130da00 str x9, [x14, #8]
            }
        } else if (equal_end + 1 < hi) {          // 0x130d9a0-0x130d9a8: b.cc 0x130d9fc, a right part of two or more
            ranges[top * 2] = equal_end + 1;      // x14 = x8 - 0x10, then [x14, #0x10]!: the pair on top is replaced
            ranges[top * 2 + 1] = hi;
        } else {
            --top;  // 0x130d9ac mov x8, x14: nothing to push, pop to the pair below
        }

        // 0x130d9b0 / 0x130da08 / 0x130da0c: cmp x14, x20 ; b.cc 0x130da18, the epilogue and ret when the stack is empty.
        if (top < 0) {
            return;
        }
        lo = ranges[top * 2];  // 0x130da10 ldr x10, [x8] ; b 0x130d894
    }
}

}  // namespace NRadEngine

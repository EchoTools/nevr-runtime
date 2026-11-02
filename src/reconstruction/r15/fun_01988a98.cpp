// FUN_01988a98
// Quest libr15.so VA 0x1988a98; ReVault binary 576.
// ReVault raw parent 100258408, reconstruction node 100600770; grade M: all 88 instructions (stored size 352, last one the ret at 0x1988bf4) traced against revault_disassemble, but the function itself keeps the unrecovered placeholder label FUN_01988a98 (ReVault row label FUN_01888a98; the Ghidra wrappers NRadEngine::CGRawInterfaceVK::DeferredDestroy(VkBuffer_T*) at 0x1988a8c and (VkImage_T*) at 0x1988bf8 forward (9, handle) and (10, handle) to it), 6 more names stay placeholders (globals DAT_0389b478, DAT_0389b2d0, DAT_0389b780, DAT_0389b400; callees FUN_019ebb48, FUN_01984fa4; VAs 0x389b478, 0x389b2d0, 0x389b780, 0x389b400, 0x19ebb48, 0x1984fa4) and the queue object is raw offsets.
// @0x1988a98 — FUN_01988a98
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

// Placeholders: no name for these exists in ReVault or in Ghidra's output (the demangler gives none).
// All are Ghidra's own labels, at global scope; line 3 lists their VAs.

// 0x19ebb48: called as FUN_019ebb48(0x20) while DAT_0389b400 is nonzero. ReVault stores it as FUN_018ebb48
// (the base-0 label of the same function). Only its first 30 instructions were read; the argument is w0.
/* UNRESOLVED: FUN_019ebb48 */
void FUN_019ebb48(std::uint32_t value);

// 0x1984fa4: tail-called with (kind, handle). ReVault stores it as FUN_01884fa4. Its first instructions
// compute kind - 5, branch away when that exceeds 0x14, and jump through a table; the result is not used.
/* UNRESOLVED: FUN_01984fa4 */
void FUN_01984fa4(std::uint32_t kind, std::uint64_t handle);

// 0x389b478: a 64-bit pointer variable (loaded with ldr x22,[0x389b478]) to the int spinlock that guards the
// deferred queue. Not the lock itself: the lock is the int it points to.
/* UNRESOLVED: DAT_0389b478 */
extern std::int32_t* DAT_0389b478;

// 0x389b2d0: a 64-bit pointer variable (loaded with ldr x21,[0x389b2d0]) to the deferred-destroy queue object.
// Raw layout, touched extent 0x38 (the true size is not known): +0x00 CMemBlock (block pointer at +0x00),
// +0x20 u64 minimum growth, +0x28 u64 capacity in entries, +0x30 u64 count of entries.
/* UNRESOLVED: DAT_0389b2d0 */
extern std::uint8_t* DAT_0389b2d0;

// 0x389b780: the int spinlock taken (address built with adrp/add) around the wait on DAT_0389b400.
/* UNRESOLVED: DAT_0389b780 */
extern std::int32_t DAT_0389b780;

// 0x389b400: 64-bit word (ldr x8,[0x389b400]); the immediate path waits until it is zero.
/* UNRESOLVED: DAT_0389b400 */
extern std::uint64_t DAT_0389b400;

namespace NRadEngine {

namespace CGS {
// NRadEngine::CGS::currframe, reached through GOT slot 0x3829558; a 64-bit word.
extern std::uint64_t currframe;
// NRadEngine::CGS::CurrentGPUFrame() @0x1988a18 (4 instructions: GOT slot 0x380f360, then ldr x0,[x8,#0xb70], ret).
// The listing of this function inlines that same load, once before the comparison and once under the lock.
std::uint64_t CurrentGPUFrame();
}  // namespace CGS

struct CMemBlock {
    // NRadEngine::CMemBlock::Resize(unsigned long long, unsigned int) @0x012dc30c (thunk 0x0103e400).
    // This function passes w2 = 1 as the keep-contents flag; the queue object is a CMemBlock at offset 0.
    void Resize(std::uint64_t size, std::uint32_t keep_contents);
};

}  // namespace NRadEngine

namespace {

// Entry of the deferred queue, 0x18 bytes: +0x00 u64 frame at which the destroy may run, +0x08 u32 kind,
// +0x0c u32 zero, +0x10 u64 handle.
constexpr std::uint64_t kEntryBytes = 0x18;
constexpr std::size_t kEntryFrame = 0x00;
constexpr std::size_t kEntryKind = 0x08;
constexpr std::size_t kEntryZero = 0x0c;
constexpr std::size_t kEntryHandle = 0x10;

// Offsets in the queue object (see DAT_0389b2d0).
constexpr std::size_t kQueueBlock = 0x00;
constexpr std::size_t kQueueMinimumGrowth = 0x20;
constexpr std::size_t kQueueCapacity = 0x28;
constexpr std::size_t kQueueCount = 0x30;

template <typename T>
T Load(const std::uint8_t* base, std::size_t offset) {
    T value;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

template <typename T>
void Store(std::uint8_t* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// ldar spin until the word is zero, then ldaxr/stlxr 0 -> 1; a lost exclusive (clrex) goes back to the spin.
void Acquire(std::int32_t* lock) {
    for (;;) {
        while (__atomic_load_n(lock, __ATOMIC_ACQUIRE) != 0) {
        }
        std::int32_t expected = 0;
        if (__atomic_compare_exchange_n(lock, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return;
        }
    }
}

// stlr wzr.
void Release(std::int32_t* lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

}  // namespace

// FUN_01988a98 is the decompiler/tree label; ReVault stores the same function as FUN_01888a98 (base-0 label).
// No recovered name exists: a ReVault search for "DeferredDestroyObject" finds nothing. The Ghidra wrappers
// NRadEngine::CGRawInterfaceVK::DeferredDestroy(VkBuffer_T*) @0x1988a8c and (VkImage_T*) @0x1988bf8 forward
// (9, handle) and (10, handle) to it; the batch-12 reconstruction of CGEngineResources::Shutdown passes 0xe.
void FUN_01988a98(std::uint32_t kind, std::uint64_t handle) {
    // 0x1988aa8: cbz x1 -> 0x1988be4 (restore registers, ret): a null handle is ignored.
    if (handle == 0) {
        return;
    }

    // 0x1988ac4-0x1988ad0: currframe against the GPU frame; equal falls through to the immediate path.
    if (NRadEngine::CGS::currframe == NRadEngine::CGS::CurrentGPUFrame()) {
        // 0x1988ad4-0x1988b00: take DAT_0389b780.
        Acquire(&DAT_0389b780);
        // 0x1988b34-0x1988b4c: wait for DAT_0389b400 to be zero, calling FUN_019ebb48(0x20) per pass.
        while (DAT_0389b400 != 0) {
            FUN_019ebb48(0x20);
        }
        // 0x1988b50: stlr wzr releases DAT_0389b780.
        Release(&DAT_0389b780);
        // 0x1988b54-0x1988b6c: restore registers, then `b 0x1984fa4` with (kind, handle).
        FUN_01984fa4(kind, handle);
        return;
    }

    // 0x1988b04-0x1988b30: take the lock that DAT_0389b478 points to.
    std::int32_t* const lock = DAT_0389b478;
    Acquire(lock);

    // 0x1988b70-0x1988b80: the queue pointer is read under the lock, and the GPU frame is read again; the
    // entry becomes due two frames later.
    std::uint8_t* const queue = DAT_0389b2d0;
    const std::uint64_t due_frame = NRadEngine::CGS::CurrentGPUFrame() + 2;
    std::uint64_t capacity = Load<std::uint64_t>(queue, kQueueCapacity);
    std::uint64_t count = Load<std::uint64_t>(queue, kQueueCount);

    // 0x1988b84-0x1988bb8: when count >= capacity (b.cc skips when count < capacity) grow the block by
    // max(capacity, minimum growth) entries, 0x18 bytes each, and record the new capacity.
    if (count >= capacity) {
        const std::uint64_t minimum_growth = Load<std::uint64_t>(queue, kQueueMinimumGrowth);
        const std::uint64_t growth = minimum_growth < capacity ? capacity : minimum_growth;
        reinterpret_cast<NRadEngine::CMemBlock*>(queue)->Resize((growth + capacity) * kEntryBytes, 1);
        // 0x1988bb0: capacity and count are read again after Resize.
        capacity = Load<std::uint64_t>(queue, kQueueCapacity);
        count = Load<std::uint64_t>(queue, kQueueCount);
        Store<std::uint64_t>(queue, kQueueCapacity, capacity + growth);
    }

    // 0x1988bbc-0x1988bdc: the block pointer is read, the count is incremented and stored, then the entry
    // at index (new count - 1) is written.
    std::uint8_t* const entries = Load<std::uint8_t*>(queue, kQueueBlock);
    count += 1;
    Store<std::uint64_t>(queue, kQueueCount, count);
    std::uint8_t* const entry = entries + (count - 1) * kEntryBytes;
    Store<std::uint64_t>(entry, kEntryFrame, due_frame);
    Store<std::uint32_t>(entry, kEntryKind, kind);
    Store<std::uint32_t>(entry, kEntryZero, 0);
    Store<std::uint64_t>(entry, kEntryHandle, handle);

    // 0x1988be0: stlr wzr releases the queue lock; 0x1988be4-0x1988bf4: restore registers, ret.
    Release(lock);
}

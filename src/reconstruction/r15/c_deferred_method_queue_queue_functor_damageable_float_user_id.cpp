// NRadEngine::CDeferredMethodQueue::QueueFunctor<NRadEngine::CDeferredMethodQueue::SFunctor3<void, NRadEngine::NRadGame::CR15NetDamageableCS, NRadEngine::SCompactPoolHandle, float, NRadEngine::NRadGame::SR15NetUserId>>
// Quest libr15.so VA 0x235d918; ReVault binary 576.
// ReVault raw parent 100310177, reconstruction node 100600769; grade H: all 141 instructions of 0x235d918-0x235db48 mapped (prologue and epilogue compiler-generated), every callee named (NRadEngine::CMemBlock::Size, NRadEngine::CMemBlock::GetString, NRadEngine::NWriteLog::WriteLog), 0 placeholders; both brk #1 traps are the tree's SoftwareBreakpoint call; queue fields are raw offsets; the thunk-to-callee links are Ghidra's (their GOT slots hold 0x00ecfb40 on disk).
// @0x235d918 — NRadEngine::CDeferredMethodQueue::QueueFunctor<NRadEngine::CDeferredMethodQueue::SFunctor3<void, NRadEngine::NRadGame::CR15NetDamageableCS, NRadEngine::SCompactPoolHandle, float, NRadEngine::NRadGame::SR15NetUserId>>
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// NRadEngine::CMemBlock::Size() const @0x012dc1c4 (thunk 0x01017920): the word at +0x08 of the block; the caller
// compares it as a signed 64-bit value. NRadEngine::CMemBlock::GetString() const @0x012dc1d4 (thunk 0x01003b60): the
// word at +0x00 of the block, used here as the base address of the block's bytes (undefined8 in ReVault's decompile).
// The thunks jump through GOT slots that hold 0x00ecfb40 on disk (0x037e3378, 0x037d9498), so their targets are
// Ghidra's relocation resolution, not read from the file.
class CMemBlock {
public:
    std::int64_t Size() const;
    std::uint64_t GetString() const;
};

namespace NWriteLog {
enum ELogLevel : int;
// NRadEngine::NWriteLog::WriteLog @0x01088c20 (thunk 0x0104b960): level, channel, format string, then the values.
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

// brk #1 is written as the trap call the tree uses: SoftwareBreakpoint(1, <brk address + 4>)(); the trap continues there.
using BreakpointHandler = void (*)();
BreakpointHandler SoftwareBreakpoint(int kind, std::uintptr_t address);

namespace NRadGame {
class CR15NetDamageableCS;
struct SR15NetUserId;
}  // namespace NRadGame

struct SCompactPoolHandle;

// The queue has a vtable at 0x036fad30 in ReVault (slots 0 and 1: ~CDeferredMethodQueue); this body never touches it,
// and no field layout is recovered, so every field below is a raw byte offset from `this`:
//   +0x010 pointer to a control block P: P+0x40 tail word, P+0x80 free-node head word, P+0xc0 u64 call count.
//   +0x158 pointer to the node table, 0x40 bytes per node: +0x00 link word (low 32 bits the next node index, -1 ends
//          a list), +0x04 u32 tag, +0x08 record pointer.
//   +0x1c8 an embedded CMemBlock that holds the records.
//   +0x1e8 u64 byte cursor into that block.
// A list word packs a node index in its low 32 bits and a tag in its high 32 bits. The roles of P+0x40 and P+0x80
// are read off the compare-and-swap pattern; the offsets are the operands of the code.
class CDeferredMethodQueue {
public:
    // The record a functor is queued in: an 8-byte invoker word, then the functor; QueueFunctor returns the address
    // after the invoker word. The functor layout is not recovered, so the template is only declared.
    template <typename R, typename A, typename B, typename C, typename D>
    struct SFunctor3;

    template <typename Functor>
    Functor* QueueFunctor(void (*invoke)(CDeferredMethodQueue&, char*));
};

}  // namespace NRadEngine

namespace {

// The two WriteLog format strings, byte for byte from the binary (0x02ca217b and 0x02ca229e).
constexpr char kOutOfSpaceFormat[] =
    "CDeferredMethodQueue buffer is out of space (size = %lld). This is probably because of too many deferred "
    "calls from scriptnodes in Autorun threads that should be in Initialize instead. Please verify recent changes "
    "and if the issue continues, shelve your changes and alert the gameplay team.";
constexpr char kMaxCallsFormat[] =
    "CDeferredMethodQueue buffer has exceeded max number of calls (%llu). This is probably because of too many "
    "deferred calls from scriptnodes in Autorun threads that should be in Initialize instead. Please verify recent "
    "changes and if the issue continues, shelve your changes and alert the gameplay team.";

std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    std::uintptr_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return reinterpret_cast<std::byte*>(value);
}

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

void StoreU32(std::byte* base, std::size_t offset, std::uint32_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// An ldaxr / add / stlxr loop on the 64-bit word at base+offset; returns the value read.
std::uint64_t FetchAddWord(std::byte* base, std::size_t offset, std::uint64_t value) {
    return __atomic_fetch_add(reinterpret_cast<std::uint64_t*>(base + offset), value, __ATOMIC_ACQ_REL);
}

// The ldaxr / cmp / stlxr loop of the binary: store `desired` when the 64-bit word still equals `expected` (clrex
// and false when it does not; a failed store alone is retried).
bool CasWord(std::byte* base, std::size_t offset, std::uint64_t expected, std::uint64_t desired) {
    auto* const word = reinterpret_cast<std::uint64_t*>(base + offset);
    for (;;) {
        std::uint64_t observed = expected;
        if (__atomic_compare_exchange_n(word, &observed, desired, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return true;
        }
        if (observed != expected) {
            return false;
        }
    }
}

}  // namespace

namespace NRadEngine {

using DamageableFloatUserIdFunctor =
    CDeferredMethodQueue::SFunctor3<void, NRadGame::CR15NetDamageableCS, SCompactPoolHandle, float,
                                    NRadGame::SR15NetUserId>;

// ReVault's call graph lists a FUN_0225daa4 at 0x235daa4 as a tail callee of this function (sites 0x235da88) and as
// its caller (sites 0x235daa4, 0x235daa8, 0x235dadc). It is a mis-split fragment of this body: the disassembly shows
// those sites as branches inside 0x235d918-0x235db48, and the only calls the body makes are the three named below.
template <>
DamageableFloatUserIdFunctor* CDeferredMethodQueue::QueueFunctor<DamageableFloatUserIdFunctor>(
    void (*invoke)(CDeferredMethodQueue&, char*)) {
    // 0x235d918-0x235d938: frame setup, 64-byte stack realignment and callee-saved saves (compiler-generated);
    // x0 is kept in x20 (the queue) and x1 in x19 (the invoker).
    auto* const queue = reinterpret_cast<std::byte*>(this);
    auto* const block = reinterpret_cast<CMemBlock*>(queue + 0x1c8);

    // 0x235d93c-0x235d94c: ldaxr / add 0x30 / stlxr on the cursor at queue+0x1e8; x22 keeps the old value.
    const std::uint64_t reserved = FetchAddWord(queue, 0x1e8, 0x30);

    // 0x235d950-0x235d984: x21 = the block; the reservation must not pass its Size() (signed compare, b.le skips the
    // log). Size() is called again for the log argument; brk #1 at 0x235d984 continues at 0x235d988.
    if (static_cast<std::int64_t>(reserved + 0x30) > block->Size()) {
        NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0ULL, kOutOfSpaceFormat,
                            static_cast<long long>(block->Size()));
        SoftwareBreakpoint(1, 0x235d988)();
    }

    // 0x235d988-0x235d990: record = GetString() + reserved (x21); the invoker goes in its first 8 bytes at the end.
    std::byte* const record = reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(block->GetString() + reserved));

    // 0x235d994-0x235d9e8: pop the free-node head P+0x80. The head is read through a fresh queue+0x10 each pass; index
    // -1 is an empty list. The word is saved, the node table entry at that index gives the next head word, and the
    // swap from the saved word to the next one restarts the pass when the head moved.
    std::byte* ctl = nullptr;
    std::byte* nodes = nullptr;
    std::uint64_t head = 0;
    for (;;) {
        ctl = LoadPtr(queue, 0x10);
        head = LoadU64(ctl, 0x80);
        if (static_cast<std::uint32_t>(head) == 0xffffffffu) {
            // 0x235db2c-0x235db44: WriteLog(8, 0, fmt, P+0xc0); brk #1 at 0x235db44 continues at 0x235db48, which is
            // `b 0x235db0c`: the invoker is stored and the record returned.
            NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), 0ULL, kMaxCallsFormat,
                                static_cast<unsigned long long>(LoadU64(ctl, 0xc0)));
            SoftwareBreakpoint(1, 0x235db48)();
            StoreU64(record, 0x00, reinterpret_cast<std::uintptr_t>(invoke));
            return reinterpret_cast<DamageableFloatUserIdFunctor*>(record + 8);
        }
        nodes = LoadPtr(queue, 0x158);
        const std::uint64_t head_next =
            LoadU64(nodes, static_cast<std::size_t>(static_cast<std::uint32_t>(head)) * 0x40u);
        if (CasWord(ctl, 0x80, head, head_next)) {
            break;
        }
    }

    // 0x235d9ec-0x235da00: P+0xc0 += 1 (atomic), P read again from queue+0x10.
    FetchAddWord(LoadPtr(queue, 0x10), 0xc0, 1);

    // 0x235da04-0x235da24: the popped node (index = low half of the saved head word) takes the record pointer at +0x08
    // and the link word -1 at +0x00 (the end of a list).
    const std::size_t popped_offset = static_cast<std::size_t>(static_cast<std::uint32_t>(head)) * 0x40u;
    nodes = LoadPtr(queue, 0x158);
    StoreU64(nodes + popped_offset, 0x08, reinterpret_cast<std::uintptr_t>(record));
    StoreU32(nodes + popped_offset, 0x00, 0xffffffffu);

    // 0x235da2c-0x235dad8: link the popped node after the tail.
    //   0x235da2c-0x235da6c: tail = P+0x40; tail_next = the link word of the node at the tail's index; the saved head
    //   word's high half becomes tail tag + 1 and the popped node's tag (+0x04) becomes tail tag + 2.
    //   0x235da70-0x235da7c: P+0x40 read again; a moved tail restarts the pass.
    //   0x235da80-0x235daa8: a tail_next whose index is not -1 advances P+0x40 from tail to tail_next (a lost swap is
    //   ignored), then the pass restarts.
    //   0x235daac-0x235dad8: otherwise the tail node's link word is swapped from tail_next to the saved head word; a
    //   lost swap restarts the pass.
    std::uint64_t tail = 0;
    for (;;) {
        ctl = LoadPtr(queue, 0x10);
        tail = LoadU64(ctl, 0x40);
        nodes = LoadPtr(queue, 0x158);
        const std::uint64_t tail_next =
            LoadU64(nodes, static_cast<std::size_t>(static_cast<std::uint32_t>(tail)) * 0x40u);
        const std::uint32_t tail_tag = static_cast<std::uint32_t>(tail >> 32);
        head = (static_cast<std::uint64_t>(tail_tag + 1u) << 32) | static_cast<std::uint32_t>(head);
        StoreU32(nodes + popped_offset, 0x04, tail_tag + 2u);
        ctl = LoadPtr(queue, 0x10);
        if (LoadU64(ctl, 0x40) != tail) {
            continue;
        }
        if (static_cast<std::uint32_t>(tail_next) != 0xffffffffu) {
            (void)CasWord(ctl, 0x40, tail, tail_next);
            continue;
        }
        nodes = LoadPtr(queue, 0x158);
        if (CasWord(nodes + static_cast<std::size_t>(static_cast<std::uint32_t>(tail)) * 0x40u, 0x00, tail_next,
                    head)) {
            break;
        }
    }

    // 0x235dae0-0x235db08: P+0x40 moves from the old tail to the linked word (P read again from queue+0x10); a lost
    // swap is left to the next caller.
    (void)CasWord(LoadPtr(queue, 0x10), 0x40, tail, head);

    // 0x235db0c-0x235db28: the invoker word goes at the record's start and the address after it is returned; the
    // epilogue restores the callee-saved registers (compiler-generated).
    StoreU64(record, 0x00, reinterpret_cast<std::uintptr_t>(invoke));
    return reinterpret_cast<DamageableFloatUserIdFunctor*>(record + 8);
}

}  // namespace NRadEngine

// NRadEngine::CDeferredMethodQueue::QueueFunctor<NRadEngine::CDeferredMethodQueue::SFunctor1<void,NRadEngine::CncaGameSpace,NRadEngine::SActor>>
// Quest libr15.so VA 0x1877890; ReVault binary 576.
// ReVault raw parent 100253378, reconstruction node 100600682; grade H: all 141 instructions of 0x1877890-0x1877ac0 mapped (prologue and epilogue compiler-generated), no placeholders; both brk #1 traps are SoftwareBreakpoint continuations; queue fields are raw offsets.
// @0x1877890 — NRadEngine::CDeferredMethodQueue::QueueFunctor<NRadEngine::CDeferredMethodQueue::SFunctor1<void,NRadEngine::CncaGameSpace,NRadEngine::SActor>>
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// What the body calls. CMemBlock is the object embedded at queue+0x1c8: Size() and GetString() are
// NRadEngine::CMemBlock::Size and NRadEngine::CMemBlock::GetString (thunks 0x1017920 and 0x1003b60). The call does
// not show whether GetString is const, so it is not.
class CMemBlock {
public:
    std::uint64_t Size() const;
    std::uint64_t GetString();
};

namespace NWriteLog {
// NRadEngine::NWriteLog::WriteLog @ 0x1088c20 (thunk 0x104b960). Variadic; enumerators are not established.
enum ELogLevel : std::int32_t {};
void WriteLog(ELogLevel level, unsigned long long channel, const char* format, ...);
}  // namespace NWriteLog

// brk #1 is written as the trap call the tree uses: SoftwareBreakpoint(1, <brk address + 4>)(); the trap continues there.
using BreakpointHandler = void (*)();
BreakpointHandler SoftwareBreakpoint(int kind, std::uintptr_t address);

struct CDeferredMethodQueue {
    // The 0x20-byte functor record that QueueFunctor returns; the invoker slot is the 8 bytes before it. The template
    // arguments of SFunctor1<void, CncaGameSpace, SActor> are carried by the name, as connect.cpp does.
    struct SFunctor1 {
        alignas(8) std::byte storage[0x20];
    };
    static_assert(sizeof(SFunctor1) == 0x20);

    using InvokeMethodFn = void (*)(CDeferredMethodQueue*, void*);
    // The queue is the first argument (x0, the binary's this); the invoker is the second (x1).
    static SFunctor1* QueueFunctor(void* queue, InvokeMethodFn invoke_method);
};

}  // namespace NRadEngine

namespace {

// Format strings of the two WriteLog calls, byte for byte from the binary (0x2ca217b and 0x2ca229e).
constexpr char kOutOfSpaceFormat[] =
    "CDeferredMethodQueue buffer is out of space (size = %lld). This is probably because of too many deferred "
    "calls from scriptnodes in Autorun threads that should be in Initialize instead. Please verify recent changes "
    "and if the issue continues, shelve your changes and alert the gameplay team.";
constexpr char kMaxCallsFormat[] =
    "CDeferredMethodQueue buffer has exceeded max number of calls (%llu). This is probably because of too many "
    "deferred calls from scriptnodes in Autorun threads that should be in Initialize instead. Please verify recent "
    "changes and if the issue continues, shelve your changes and alert the gameplay team.";

// Raw access at byte offsets; no layout is recovered for the queue, its state block or its entries.
std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value{};
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    std::uintptr_t value{};
    std::memcpy(&value, base + offset, sizeof value);
    return reinterpret_cast<std::byte*>(value);
}

void StoreU32(std::byte* base, std::size_t offset, std::uint32_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// The ldaxr/stlxr pair of the binary: store desired when the 64-bit word still equals expected. A changed word
// returns false (the caller restarts); a failed store alone is retried.
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

CDeferredMethodQueue::SFunctor1* CDeferredMethodQueue::QueueFunctor(void* queue, InvokeMethodFn invoke_method) {
    // Raw queue layout: queue+0x10 the state block P; queue+0x158 the entry table E, 0x40 bytes per entry (+0x00 u32
    // next index, +0x04 u32 tag, +0x08 the slot pointer); queue+0x1c8 the CMemBlock; queue+0x1e8 the reserved-byte
    // counter. P+0x40 is the tail word, P+0x80 the free-list head word, P+0xc0 the call count. A list word holds an
    // index in its low 32 bits and a tag in its high 32 bits; index -1 ends a list.
    auto* const base = static_cast<std::byte*>(queue);
    // 0x1877890-0x18778b0: frame setup and callee-saved saves (compiler-generated); x0 is kept in x20 (the queue) and
    // x1 in x19 (the invoker).

    // 0x18778b4-0x18778c4: ldaxr / add 0x28 / stlxr loop on queue+0x1e8; x22 keeps the old value.
    const std::uint64_t reserved =
        __atomic_fetch_add(reinterpret_cast<std::uint64_t*>(base + 0x1e8), std::uint64_t{0x28}, __ATOMIC_ACQ_REL);
    const std::uint64_t reserved_end = reserved + 0x28;

    // 0x18778c8-0x18778d8: x21 = &CMemBlock; the reservation must not pass its Size() (signed compare, b.le).
    auto* const block = reinterpret_cast<CMemBlock*>(base + 0x1c8);
    if (static_cast<std::int64_t>(reserved_end) > static_cast<std::int64_t>(block->Size())) {
        // 0x18778dc-0x18778f8: WriteLog(8, 0, fmt, Size()); 0x18778fc brk #1, whose trap continues at 0x1877900.
        const std::uint64_t size = block->Size();
        NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), std::uint64_t{0}, kOutOfSpaceFormat,
                            static_cast<long long>(size));
        SoftwareBreakpoint(1, 0x1877900)();
    }

    // 0x1877900-0x1877908: slot = CMemBlock::GetString() + reserved (x21); the invoker goes in its first 8 bytes.
    std::byte* const slot = reinterpret_cast<std::byte*>(static_cast<std::uintptr_t>(block->GetString() + reserved));

    // 0x1877a84-0x1877aa0: store the invoker in the slot and return the record 8 bytes on.
    const auto store_and_return = [&]() -> CDeferredMethodQueue::SFunctor1* {
        StoreU64(slot, 0x00, reinterpret_cast<std::uintptr_t>(invoke_method));
        return reinterpret_cast<CDeferredMethodQueue::SFunctor1*>(slot + 8);
    };

    std::byte* const state = LoadPtr(base, 0x10);
    std::byte* const entries = LoadPtr(base, 0x158);

    // 0x187790c-0x1877960: pop the free-list head P+0x80, retried while it moved; index -1 is an empty list.
    std::uint64_t claimed = 0;
    for (;;) {
        claimed = LoadU64(state, 0x80);
        if (static_cast<std::uint32_t>(claimed) == 0xffffffffu) {
            // 0x1877aa4-0x1877abc: WriteLog(8, 0, fmt, P+0xc0); brk #1 at 0x1877abc, whose trap continues at 0x1877ac0,
            // which is `b 0x1877a84`: the invoker is stored and the record returned.
            NWriteLog::WriteLog(static_cast<NWriteLog::ELogLevel>(8), std::uint64_t{0}, kMaxCallsFormat,
                                static_cast<unsigned long long>(LoadU64(state, 0xc0)));
            SoftwareBreakpoint(1, 0x1877ac0)();
            return store_and_return();
        }
        const std::uint64_t next_head =
            LoadU64(entries, static_cast<std::size_t>(static_cast<std::uint32_t>(claimed)) * 0x40u);
        if (CasWord(state, 0x80, claimed, next_head)) {
            break;
        }
    }
    const auto claimed_index = static_cast<std::uint32_t>(claimed);
    std::byte* const claimed_entry = entries + static_cast<std::size_t>(claimed_index) * 0x40u;

    // 0x1877964-0x187796c: P+0xc0 += 1 (atomic). 0x187797c-0x1877998: the claimed entry takes the slot at +0x08 and
    // the next index -1 at +0x00.
    __atomic_fetch_add(reinterpret_cast<std::uint64_t*>(state + 0xc0), std::uint64_t{1}, __ATOMIC_ACQ_REL);
    StoreU64(claimed_entry, 0x08, reinterpret_cast<std::uintptr_t>(slot));
    StoreU32(claimed_entry, 0x00, 0xffffffffu);

    // 0x18779a4-0x18779e4: tail = P+0x40; tail_next = entry[tail] word; the claimed identity becomes (its index,
    // tail tag + 1) and its entry's tag becomes tail tag + 2. 0x18779e4-0x18779f4: a moved tail restarts the loop.
    // 0x18779f8-0x1877a20: a tail whose next is not -1 is advanced to that next, then the loop restarts.
    // 0x1877a24-0x1877a54: otherwise the tail entry's first word is CASed from tail_next to the claimed identity;
    // a lost race restarts the loop.
    std::uint64_t tail = 0;
    for (;;) {
        tail = LoadU64(state, 0x40);
        const std::uint64_t tail_next =
            LoadU64(entries, static_cast<std::size_t>(static_cast<std::uint32_t>(tail)) * 0x40u);
        const auto tail_tag = static_cast<std::uint32_t>(tail >> 32);
        claimed = (static_cast<std::uint64_t>(tail_tag + 1u) << 32) | claimed_index;
        StoreU32(claimed_entry, 0x04, tail_tag + 2u);
        if (LoadU64(state, 0x40) != tail) {
            continue;
        }
        if (static_cast<std::uint32_t>(tail_next) != 0xffffffffu) {
            CasWord(state, 0x40, tail, tail_next);
            continue;
        }
        std::byte* const tail_entry = entries + static_cast<std::size_t>(static_cast<std::uint32_t>(tail)) * 0x40u;
        if (CasWord(tail_entry, 0x00, tail_next, claimed)) {
            break;
        }
    }

    // 0x1877a58-0x1877a7c: P+0x40 moves from the old tail to the claimed identity; a lost race is left to the helper.
    CasWord(state, 0x40, tail, claimed);
    // 0x1877a84-0x1877aa0: store the invoker in the slot and return the record.
    return store_and_return();
}

}  // namespace NRadEngine

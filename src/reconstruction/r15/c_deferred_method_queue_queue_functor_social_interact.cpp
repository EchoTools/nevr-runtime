// NRadEngine::CDeferredMethodQueue::QueueFunctor<NRadEngine::CDeferredMethodQueue::SFunctor3<void, NRadEngine::NRadGame::CR15NetSocialInteractCS, NRadEngine::SCompactPoolHandle, unsigned int, unsigned long long>>
// Quest libr15.so VA 0x252c6f8; ReVault binary 576.
// ReVault raw parent 100316045, reconstruction node 100600481; grade M: the node text carries @confidence M with no further prose; its comments name the record layout and the CAS pattern, and its enclosing-function comment flags a mis-split fragment.
// @0x252c6f8 — NRadEngine::CDeferredMethodQueue::QueueFunctor<NRadEngine::CDeferredMethodQueue::SFunctor3<void, NRadEngine::NRadGame::CR15NetSocialInteractCS, NRadEngine::SCompactPoolHandle, unsigned int, unsigned long long>>
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

// Callees, each under the ReVault name of the function its thunk reaches; the thunk VA is in each comment.
// CMemBlock::Size @0x01017920 (thunk 0x01017920): capacity of the block, a signed 64-bit value.
// CMemBlock::GetString @0x01003b60 (thunk 0x01003b60): base address of the block's bytes.
class CMemBlock {
public:
    static std::int64_t Size(std::byte* block);
    static std::uint64_t GetString(std::byte* block);
};

// NWriteLog::WriteLog @0x0104b960 (thunk 0x0104b960): log level, channel, format string and one value.
namespace NWriteLog {
void WriteLog(std::int32_t level, std::int32_t channel, const char* format, std::uint64_t value);
}  // namespace NWriteLog

namespace {

// The two overflow messages, read from VA 0x02ca217b and 0x02ca229e.
constexpr char kOutOfSpace[] =
    "CDeferredMethodQueue buffer is out of space (size = %lld). This is probably because of too many deferred "
    "calls from scriptnodes in Autorun threads that should be in Initialize instead. Please verify recent changes "
    "and if the issue continues, shelve your changes and alert the gameplay team.";
constexpr char kMaxCalls[] =
    "CDeferredMethodQueue buffer has exceeded max number of calls (%llu). This is probably because of too many deferred "
    "calls from scriptnodes in Autorun threads that should be in Initialize instead. Please verify recent changes "
    "and if the issue continues, shelve your changes and alert the gameplay team.";

std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    std::byte* value = nullptr;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
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

void StorePtr(std::byte* base, std::size_t offset, const std::byte* value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// The word at `offset` as an atomic operand: each ldaxr/stlxr loop of the code is one __atomic operation on it.
std::uint64_t* AtomicWord(std::byte* base, std::size_t offset) {
    return reinterpret_cast<std::uint64_t*>(base + offset);
}

}  // namespace

namespace NRadEngine {

namespace NRadGame {
class CR15NetSocialInteractCS;
}  // namespace NRadGame

struct SCompactPoolHandle;

// Raw layout used here; none of it is recovered as fields. The offsets are the operands of the code.
// +0x10: control block pointer. In the control block: +0x40 tail word, +0x80 free-node word, +0xc0 u64 record count.
// A word packs a node index in its low half and a tag in its high half. Roles are taken from the CAS pattern.
// +0x158: node table, 0x40-byte nodes: +0x00 u64 link word (low half the next node index, -1 at the end),
// +0x04 u32 tag, +0x08 record pointer. +0x1c8: CMemBlock holding the records. +0x1e8: u64 block cursor.
class CDeferredMethodQueue {
public:
    // The record of a functor: its invoker word, then the arguments. The layout past the invoker is not recovered.
    template <typename R, typename A, typename B, typename C, typename D>
    struct SFunctor3;

    template <typename Functor>
    Functor* QueueFunctor(void (*invoke)(CDeferredMethodQueue&, char*));
};

using SocialInteractFunctor = CDeferredMethodQueue::SFunctor3<void, NRadGame::CR15NetSocialInteractCS,
                                                              SCompactPoolHandle, unsigned int, unsigned long long>;

template <>
SocialInteractFunctor* CDeferredMethodQueue::QueueFunctor<SocialInteractFunctor>(
    void (*invoke)(CDeferredMethodQueue&, char*)) {
    auto* const queue = reinterpret_cast<std::byte*>(this);
    std::byte* const buffer = queue + 0x1c8;

    std::byte* ctl = nullptr;
    std::byte* pool = nullptr;
    std::byte* node = nullptr;
    std::byte* record = nullptr;
    std::uint64_t old_cursor = 0;
    std::uint64_t popped = 0;
    std::uint64_t next_word = 0;
    std::uint64_t tail_word = 0;
    std::uint64_t tail_next = 0;
    std::uint64_t expected = 0;
    std::uint32_t idx = 0;
    std::uint32_t tail_high = 0;

    // Reserve 0x30 bytes at the block cursor (queue+0x1e8, an ldaxr/stlxr fetch-and-add).
    old_cursor = __atomic_fetch_add(AtomicWord(queue, 0x1e8), 0x30, __ATOMIC_ACQ_REL);
    if (static_cast<std::int64_t>(old_cursor + 0x30) > CMemBlock::Size(buffer)) {
        NWriteLog::WriteLog(8, 0, kOutOfSpace, static_cast<std::uint64_t>(CMemBlock::Size(buffer)));
        __builtin_trap();
    }
    record = reinterpret_cast<std::byte*>(CMemBlock::GetString(buffer) + old_cursor);

L_alloc:
    ctl = LoadPtr(queue, 0x10);
    popped = LoadU64(ctl, 0x80);
    if (static_cast<std::uint32_t>(popped) == 0xffffffffu) {
        NWriteLog::WriteLog(8, 0, kMaxCalls, LoadU64(ctl, 0xc0));
        __builtin_trap();
    }
    pool = LoadPtr(queue, 0x158);
    idx = static_cast<std::uint32_t>(popped);
    next_word = LoadU64(pool, static_cast<std::size_t>(idx) * 0x40);

    // Pop the free node: the free-node word moves from `popped` to the node's link word.
    for (;;) {
        if (__atomic_load_n(AtomicWord(ctl, 0x80), __ATOMIC_ACQUIRE) != popped) {
            goto L_alloc;
        }
        expected = popped;
        if (__atomic_compare_exchange_n(AtomicWord(ctl, 0x80), &expected, next_word, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    // Count the record: ctl+0xc0 is incremented with an ldaxr/stlxr loop.
    __atomic_fetch_add(AtomicWord(ctl, 0xc0), 1, __ATOMIC_ACQ_REL);

    // Fill the popped node: its record pointer, then its link word at +0x00 is -1 (the end of the list).
    pool = LoadPtr(queue, 0x158);
    node = pool + static_cast<std::size_t>(idx) * 0x40;
    StorePtr(node, 0x08, record);
    StoreU32(node, 0x00, 0xffffffffu);
    goto L_tail;

L_tail:
    ctl = LoadPtr(queue, 0x10);
    tail_word = LoadU64(ctl, 0x40);
    pool = LoadPtr(queue, 0x158);
    tail_next = LoadU64(pool, static_cast<std::size_t>(static_cast<std::uint32_t>(tail_word)) * 0x40);

    // The popped word's high half becomes the tail's tag + 1; the popped node's tag becomes the tail's tag + 2.
    tail_high = static_cast<std::uint32_t>(tail_word >> 32);
    popped = (popped & 0xffffffffu) | (static_cast<std::uint64_t>(tail_high + 1u) << 32);
    node = pool + static_cast<std::size_t>(idx) * 0x40;
    StoreU32(node, 0x04, tail_high + 2u);

    // The tail must still hold the word read above; if it does not, read it again.
    ctl = LoadPtr(queue, 0x10);
    if (LoadU64(ctl, 0x40) != tail_word) {
        goto L_tail;
    }
    if (static_cast<std::uint32_t>(tail_next) != 0xffffffffu) {
        // The tail's link is a real node: move the tail past it (ldaxr/stlxr on ctl+0x40), then read again.
        expected = tail_word;
        (void)__atomic_compare_exchange_n(AtomicWord(ctl, 0x40), &expected, tail_next, false, __ATOMIC_ACQ_REL,
                                          __ATOMIC_ACQUIRE);
        goto L_tail;
    }

    // The tail's link is the end: link the popped node after the tail (CAS on the tail node's link word).
    node = pool + static_cast<std::size_t>(static_cast<std::uint32_t>(tail_word)) * 0x40;
    for (;;) {
        if (__atomic_load_n(AtomicWord(node, 0x00), __ATOMIC_ACQUIRE) != tail_next) {
            goto L_tail;
        }
        expected = tail_next;
        if (__atomic_compare_exchange_n(AtomicWord(node, 0x00), &expected, popped, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    // Move the tail to the linked node; a lost race is left to the next caller.
    ctl = LoadPtr(queue, 0x10);
    expected = tail_word;
    (void)__atomic_compare_exchange_n(AtomicWord(ctl, 0x40), &expected, popped, false, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE);

    // The invoker word goes at the record's start; the result is the address after it.
    StoreU64(record, 0x00, reinterpret_cast<std::uintptr_t>(invoke));
    return reinterpret_cast<SocialInteractFunctor*>(record + 8);
}

}  // namespace NRadEngine

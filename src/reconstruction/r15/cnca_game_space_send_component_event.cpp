// NRadEngine::CncaGameSpace::SendComponentEvent
// Quest libr15.so VA 0x1907264; ReVault binary 576.
// ReVault raw parent 100256041, reconstruction node 100600677; grade M: 62 instructions checked to the ret at 0x1907358; 1 placeholder stays, DAT_038611b8 (VA 0x38611b8, the TLS index word); every callee and type name is real; syntax-checked.
// @0x1907264 — NRadEngine::CncaGameSpace::SendComponentEvent
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// Raw access at a byte offset. ReVault has no layout for the objects these reach.
template <typename T>
T LoadAt(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

template <typename T>
void StoreAt(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof value);
}

}  // namespace

// VA 0x38611b8: the 32-bit TLS index word read before GetTLS. ReVault has no
// name for it (placeholder).
extern std::uint32_t DAT_038611b8;

namespace NRadEngine {

// NRadEngine::CSymbol64: one 8-byte word, passed by value in one register.
struct CSymbol64 {
    std::uint64_t value;
};
static_assert(sizeof(CSymbol64) == 0x8);

// NRadEngine::SActor: one 8-byte word.
struct SActor {
    std::uint64_t value;
};
static_assert(sizeof(SActor) == 0x8);

// The 0x28-byte record the callee reads. ReVault has no layout for it, so each
// 8-byte word is named by its offset.
struct SComponentEvent {
    std::uint64_t at_00;  // +0x00: CSymbol64 event id, the FireEvent key
    std::uint64_t at_08;  // +0x08: the alias game space's +0x58, set per alias
    std::uint64_t at_10;  // +0x10: SActor; its low signed halfword -1 means no actor
    std::uint64_t at_18;  // +0x18: copied unchanged
    std::uint64_t at_20;  // +0x20: copied unchanged
};
static_assert(sizeof(SComponentEvent) == 0x28);
static_assert(offsetof(SComponentEvent, at_20) == 0x20);

struct CStackAllocator {
    // NRadEngine::CStackAllocator::DirectAlloc @ 0x12df278 (thunk 0x10412a0): a member.
    // The allocator is this (x0); bytes and alignment are x1 and x2.
    void* DirectAlloc(std::size_t bytes, std::size_t alignment);
    // NRadEngine::CStackAllocator::DirectFree @ 0x12df3fc (thunk 0x1003d40).
    void DirectFree();
};

struct CSysThread {
    static void* GetTLS(int index);
};

class CncaGameSpace;

// NRadEngine::CEventDB::FireEvent<NRadEngine::SComponentEvent> @ 0x1921278 (thunk 0x101bac0),
// called on the object at this+0xbd8 (that address is x0 on entry to the call).
struct CEventDB {
    template <typename Event>
    void FireEvent(CSymbol64 symbol, const Event& event, std::uint32_t flags);
};

// NRadEngine::SActorAlias: the element type of the alias table. ReVault has no
// layout for it; the 16-byte entry the loop reads is SAliasEntry.
struct SActorAlias;

// NRadEngine::CTempTable<NRadEngine::SActorAlias, 4294967295u>: the table
// CActorAliasCS::Aliases fills. ReVault has no member layout for it, so this is
// raw storage, and the words the caller reads and writes are named by offset.
template <typename T, unsigned int Max>
class CTempTable {
public:
    alignas(8) std::byte storage[0x20];  // +0x00 entries, +0x08 NumAliases count,
                                         // +0x10 TLS allocator, +0x18 entries written
};
static_assert(sizeof(CTempTable<SActorAlias, 4294967295u>) == 0x20);

// One 16-byte entry of the alias array. ReVault has no layout for it.
struct SAliasEntry {
    CncaGameSpace* space;   // +0x00: passed as this to SendComponentEvent
    std::uint64_t word_08;  // +0x08: copied into the record's +0x10 word
};
static_assert(sizeof(SAliasEntry) == 0x10);
static_assert(offsetof(SAliasEntry, word_08) == 0x08);

struct CActorAliasCS {
    // NRadEngine::CActorAliasCS::NumAliases @ 0x14611bc (thunk 0xfed760): x0 game space, x1 SActor, count in x0.
    static std::int64_t NumAliases(CncaGameSpace* game_space, SActor actor);
    // NRadEngine::CActorAliasCS::Aliases @ 0x1461318 (thunk 0x1021010): x0 table, x1 game space, x2 SActor.
    static void Aliases(CTempTable<SActorAlias, 4294967295u>& table,
                        CncaGameSpace* game_space, SActor actor);
};

class CncaGameSpace {
public:
    void SendComponentEvent(const SComponentEvent& event, std::uint32_t flags);

    alignas(8) std::byte before_58_[0x58];  // +0x00..+0x57: not recovered
    std::uint64_t word_58_;                 // +0x58: no name in ReVault
};
static_assert(offsetof(CncaGameSpace, word_58_) == 0x58);

// Fires the event on the object at this+0xbd8, then sends a copy of the event
// to each game space aliased to its actor. In each copy, the +0x08 word is that
// space's +0x58 word and the +0x10 word is the alias entry's second word.
void CncaGameSpace::SendComponentEvent(const SComponentEvent& event, std::uint32_t flags) {
    auto* const event_db =
        reinterpret_cast<CEventDB*>(reinterpret_cast<std::byte*>(this) + 0xbd8);
    event_db->FireEvent<SComponentEvent>(CSymbol64{event.at_00}, event, flags);

    // A low halfword of -1 in the actor word means there is no actor to alias.
    if (static_cast<std::int16_t>(event.at_10) == -1) {
        return;
    }
    const SActor actor{event.at_10};
    const std::int64_t alias_count = CActorAliasCS::NumAliases(this, actor);
    if (alias_count == 0) {
        return;
    }

    void* const tls_allocator = CSysThread::GetTLS(static_cast<int>(DAT_038611b8));
    // Filled by Aliases: the entries, then the count of entries written.
    CTempTable<SActorAlias, 4294967295u> table{};
    StoreAt<std::uint64_t>(table.storage, 0x08, static_cast<std::uint64_t>(alias_count));
    StoreAt<void*>(table.storage, 0x10, tls_allocator);
    void* const elements = static_cast<CStackAllocator*>(tls_allocator)->DirectAlloc(
        static_cast<std::size_t>(alias_count) << 4, 8);
    StoreAt<void*>(table.storage, 0x00, elements);
    StoreAt<std::uint64_t>(table.storage, 0x18, 0);
    CActorAliasCS::Aliases(table, this, actor);

    // The copy starts as the event; the loop rewrites +0x08 and +0x10 per entry.
    SComponentEvent record = event;
    // Only the low 60 bits of the length are the count (the disassembly shifts
    // the length left by four, which drops the top four bits).
    constexpr std::uint64_t kCountMask = 0x0fffffffffffffffull;
    const std::uint64_t entries = LoadAt<std::uint64_t>(table.storage, 0x18) & kCountMask;
    if (entries != 0) {
        auto* entry = static_cast<SAliasEntry*>(LoadAt<void*>(table.storage, 0x00));
        SAliasEntry* const end = entry + entries;
        do {
            record.at_08 = entry->space->word_58_;
            record.at_10 = entry->word_08;
            entry->space->SendComponentEvent(record, flags);
            ++entry;
        } while (entry != end);
    }

    if (tls_allocator != nullptr) {
        static_cast<CStackAllocator*>(tls_allocator)->DirectFree();
    }
}

}  // namespace NRadEngine

// NRadEngine::CncaGameSpace::SendComponentEventGlobal
// Quest libr15.so VA 0x190281c; ReVault binary 576.
// ReVault raw parent 100255942, reconstruction node 100600679; grade M: 1 placeholder (DAT_038611b8, the global at VA 0x38611b8, no name in ReVault); all 83 instructions checked to the ret at 0x1902964; every callee named; syntax-checked under -Wall -Wextra -Werror.
// @0x190281c — NRadEngine::CncaGameSpace::SendComponentEventGlobal
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

// The global at VA 0x38611b8, read as a 32-bit word (adrp 0x3861000, ldr w0 #0x1b8).
// ReVault has no name for it; it is the key passed to CSysThread::GetTLS.
extern std::uint32_t DAT_038611b8;

namespace NRadEngine {

// The 0x28-byte record this function copies. ReVault has no layout for it, so each
// 8-byte word is named by its offset.
struct SComponentEvent {
    std::uint64_t at_00;  // +0x00: CSymbol64 event id, the FireEvent key
    std::uint64_t at_08;  // +0x08: the alias game space's +0x58, set per alias
    std::uint64_t at_10;  // +0x10: SActor; its low signed halfword -1 means skip
    std::uint64_t at_18;  // +0x18: copied unchanged
    std::uint64_t at_20;  // +0x20: copied unchanged
};
static_assert(sizeof(SComponentEvent) == 0x28);
static_assert(offsetof(SComponentEvent, at_20) == 0x20);

// NRadEngine::CSymbol64: one 8-byte word, passed by value in one register.
struct CSymbol64 {
    std::uint64_t value;
};
static_assert(sizeof(CSymbol64) == 0x8);

struct SActor {
    std::uint64_t value;
};
static_assert(sizeof(SActor) == 0x8);

class CncaGameSpace;

// NRadEngine::CEventDB::FireEvent<NRadEngine::SComponentEvent> @ 0x1921278 (thunk 0x101bac0).
// Entry: x0 this (the CEventDB at game space +0xbd8), x1 CSymbol64, x2 event reference, w3 flags.
class CEventDB {
public:
    template <typename TEvent>
    void FireEvent(CSymbol64 symbol, const TEvent& event, std::uint32_t flags);
};

struct SActorAlias;
template <typename T, unsigned int Max>
struct CTempTable;

// NRadEngine::CActorAliasCS::NumAliases @ 0x14611bc (thunk 0xfed760): x0 game space, x1 SActor, count in x0.
// NRadEngine::CActorAliasCS::Aliases @ 0x1461318 (thunk 0x1021010): x0 table, x1 game space, x2 SActor.
struct CActorAliasCS {
    static std::int64_t NumAliases(CncaGameSpace* game_space, SActor actor);
    static void Aliases(CTempTable<SActorAlias, 4294967295u>& table,
                        CncaGameSpace* game_space, SActor actor);
};

// NRadEngine::CSysThread::GetTLS @ 0x108a22c (thunk 0x104b720).
struct CSysThread {
    static void* GetTLS(int index);
};

// NRadEngine::CStackAllocator::DirectAlloc @ 0x12df278 (thunk 0x10412a0);
// NRadEngine::CStackAllocator::DirectFree @ 0x12df3fc (thunk 0x1003d40).
// DirectAlloc is a member: the allocator is this (x0), bytes and alignment are x1 and x2.
struct CStackAllocator {
    void* DirectAlloc(std::size_t bytes, std::size_t alignment);
    void DirectFree();
};

class CncaGameSpace {
public:
    // VA 0x190281c: the 2-argument overload. Entry: x0 this, x1 event reference, w2 flags.
    void SendComponentEventGlobal(const SComponentEvent& event, std::uint32_t flags);
};

void CncaGameSpace::SendComponentEventGlobal(const SComponentEvent& event, std::uint32_t flags) {
    // Offsets read through this: +0x8 holder of the entry array, +0xc40 16-bit flags, +0xbd8 CEventDB.
    auto* const self = reinterpret_cast<std::byte*>(this);

    if ((LoadAt<std::uint16_t>(self, 0xc40) & 0x180u) != 0) {
        reinterpret_cast<CEventDB*>(self + 0xbd8)->FireEvent<SComponentEvent>(CSymbol64{event.at_00}, event, flags);
    }

    // Fire into each entry of the holder's array. Each step loads the pointer at
    // array + slot (slot starts at 8, step 0x10); the CEventDB is at that pointer + 0xbd8.
    std::byte* holder = LoadAt<std::byte*>(self, 0x8);
    if (LoadAt<std::uint64_t>(holder, 0x1f78) != 0) {
        std::uint64_t index = 0;
        std::size_t slot = 0x8;
        do {
            std::byte* const array = LoadAt<std::byte*>(holder, 0x1f48);
            std::byte* const entry = LoadAt<std::byte*>(array, slot);
            reinterpret_cast<CEventDB*>(entry + 0xbd8)->FireEvent<SComponentEvent>(CSymbol64{event.at_00}, event, flags);
            holder = LoadAt<std::byte*>(self, 0x8);
            ++index;
            slot += 0x10;
        } while (index < LoadAt<std::uint64_t>(holder, 0x1f78));
    }

    if (static_cast<std::int16_t>(event.at_10) != -1) {
        const SActor actor{event.at_10};
        const std::int64_t count = CActorAliasCS::NumAliases(this, actor);
        if (count != 0) {
            // The 0x20-byte table Aliases fills: +0x00 entries, +0x08 the NumAliases count,
            // +0x10 the TLS allocator, +0x18 the entries written (zeroed before the call).
            alignas(8) std::byte alias_table[0x20]{};
            void* const allocator = CSysThread::GetTLS(static_cast<int>(DAT_038611b8));
            StoreAt<std::uint64_t>(alias_table, 0x08, static_cast<std::uint64_t>(count));
            StoreAt<void*>(alias_table, 0x10, allocator);
            void* const entries = static_cast<CStackAllocator*>(allocator)->DirectAlloc(
                static_cast<std::size_t>(count) << 4, 8);
            StoreAt<void*>(alias_table, 0x00, entries);
            StoreAt<std::uint64_t>(alias_table, 0x18, 0);
            CActorAliasCS::Aliases(
                *reinterpret_cast<CTempTable<SActorAlias, 4294967295u>*>(alias_table), this, actor);

            // Each alias is a 16-byte element: +0x00 its game space, +0x08 its SActor word.
            // The recursive call runs on the alias game space with a copy of the event.
            const std::uint64_t filled = LoadAt<std::uint64_t>(alias_table, 0x18);
            if ((filled << 4) != 0) {
                SComponentEvent copy = event;
                std::byte* cursor = LoadAt<std::byte*>(alias_table, 0x00);
                std::byte* const end = cursor + (filled << 4);
                do {
                    std::byte* const element = cursor;
                    cursor += 0x10;
                    CncaGameSpace* const alias_space = LoadAt<CncaGameSpace*>(element, 0x00);
                    copy.at_08 = LoadAt<std::uint64_t>(
                        reinterpret_cast<const std::byte*>(alias_space), 0x58);
                    copy.at_10 = LoadAt<std::uint64_t>(element, 0x08);
                    alias_space->SendComponentEventGlobal(copy, flags);
                } while (cursor != end);
            }

            if (allocator != nullptr) {
                static_cast<CStackAllocator*>(allocator)->DirectFree();
            }
        }
    }
}

}  // namespace NRadEngine

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// The game's level tables on the game object (the enabled table and the loaded table), as both platforms read them
// (runtime/content/config_alias_hook.cpp, quest/sentinel/config_alias_levels.cpp). Each is a CTable: a data pointer at the
// table's base, then a growth step (+0x20), a capacity (+0x28) and a count (+0x30), counted in 16-byte entries.
//
// The game sets no maximum of its own. CTable::GrowBy (echovr.exe 0x1400D6C50) grows the allocation whenever count + n would
// pass the capacity word (`[table+0x28]`) and then adds to it, and the insert helper 0x1404FA170 calls it for each new
// level; searched there and in EnableGameSpace (0x1404EF130) for a fixed array size or a bound check and found none. So the
// bound a reader holds to is the table's own capacity read at run time: a count over it is not a table.
// On Quest the tables are the same CTable. Addresses below are ReVault VAs for libr15.so (the file vaddr is 0x100000 less;
// quest/sentinel/config_alias_hook.h writes the same function in file vaddrs): CncaGame::EnableGameSpace 0x18DC124 and
// DisableGameSpace 0x18DC424 insert through the PLT stub 0x1045A90 into
// CTable<CSimpleKey<CncaGameSpace*,CSymbol64>>::Insert (0x191204C), which reads the count at +0x30 and the capacity at +0x28
// and grows the table when count + n passes it; the tables sit at +0x1F48 (enabled) and +0x1F88 (loaded).
//
// Which entry counts as a loaded level is also one rule, here. The game looks a level up by its key alone (EnableGameSpace
// 0x1404EF130 binary-searches the key against the resource's name at +0x58 and has no case for key 0), so the game itself
// does not exclude a zero key. This code treats key 0 as "no level" everywhere else (core/config_alias.cpp drops a zero main
// level and zero armed levels), so a zero key is not listed, and neither is an entry with no resource.
namespace nevr_game_space_table {

// Entries are {CSymbol64 level, level resource}: two 8-byte words.
inline constexpr std::size_t kEntryWords = 2;
inline constexpr std::size_t kEntryBytes = kEntryWords * sizeof(std::uint64_t);
inline constexpr std::size_t kResourceWord = 1;  // the resource follows the level symbol

// The capacity word sits one word before the count word.
inline constexpr std::size_t kCapacityWordsBeforeCount = 1;
inline constexpr std::size_t kCapacityBytesBeforeCount = kCapacityWordsBeforeCount * sizeof(std::uint64_t);

// True when a table with this data pointer, count and capacity can be read: no entries, or a data pointer and a count the
// table's own capacity covers.
inline constexpr bool Readable(const void* data, std::uint64_t count, std::uint64_t capacity) {
  return count == 0 || (data != nullptr && count <= capacity);
}

// True when an entry is a loaded level: a level key other than 0 with a resource.
inline constexpr bool Listed(std::uint64_t level, std::uint64_t resource) { return level != 0 && resource != 0; }

// Calls sink(level) for each listed entry of `count` entries at `entries`, in table order, until sink returns false.
// Returns false if sink stopped it. The caller has checked Readable.
template <class Sink>
bool ForEachListed(const unsigned char* entries, std::uint64_t count, Sink sink) {
  for (std::uint64_t i = 0; i < count; ++i) {
    std::uint64_t words[kEntryWords];
    std::memcpy(words, entries + i * kEntryBytes, sizeof(words));
    if (Listed(words[0], words[kResourceWord]) && !sink(words[0])) return false;
  }
  return true;
}

}  // namespace nevr_game_space_table

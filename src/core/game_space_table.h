#pragma once

#include <cstddef>
#include <cstdint>

// The game's level tables on the game object (the enabled table and the loaded table), as both platforms read them
// (runtime/content/config_alias_hook.cpp, quest/sentinel/config_alias_levels.cpp). Each is a CTable: a data pointer at the
// table's base, then a growth step (+0x20), a capacity (+0x28) and a count (+0x30), counted in 16-byte entries.
//
// The game sets no maximum of its own. CTable::GrowBy (echovr.exe 0x1400D6C50) grows the allocation whenever count + n would
// pass the capacity word (`[table+0x28]`) and then adds to it, and the insert helper 0x1404FA170 calls it for each new
// level; searched there and in EnableGameSpace (0x1404EF130) for a fixed array size or a bound check and found none. So the
// bound a reader holds to is the table's own capacity read at run time: a count over it is not a table.
// On Quest the count (+0x30) and the sorted-flag byte (+0x38) are at the same offsets as on PC (libr15
// CncaGame::EnableGameSpace 0x18DC124); the capacity word is not read there, it is taken from the shared layout.
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

}  // namespace nevr_game_space_table

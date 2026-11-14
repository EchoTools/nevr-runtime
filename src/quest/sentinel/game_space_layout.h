#pragma once
// The game object's layout that the Quest ConfigAlias reads (src/quest/sentinel/config_alias_levels.cpp). Kept apart from
// pinned_targets.h, which includes callback_thunk.h, a header that refuses any translation unit built with exceptions: the
// levels file is built with them and needs these offsets. pinned_targets.h includes this header, so there is one definition.

#include <cstddef>

namespace sentinel::pinned::game_layout {

// The game space QuitOnError sends its event to: *(*(self + 8) + 0x7af0) (null before the game space exists). The net game's
// game object sits at +8 (ReVault libr15 CR15NetGame, file vaddr convention of pinned_targets.h).
inline constexpr std::size_t kGameOffset = 8;

// The two game-space tables of the game object: the enabled table and the loaded table, each a data pointer with its count
// 0x30 further on (a CTable). Read from CncaGame::EnableGameSpace (ReVault libr15 0x18DC124 = this libr15's 0x17DC124; the
// file vaddr is the disassembly VA less 0x100000): `self+0x1F88` data and `self+0x1FB8` count select the loaded table,
// `self+0x1F48` and `self+0x1F78` the enabled. Entries are 16 bytes: {CSymbol64 level, CncaGameSpace* space}.
inline constexpr std::size_t kEnabledSpaceTableOffset = 0x1F48;
inline constexpr std::size_t kEnabledSpaceCountOffset = 0x1F78;
inline constexpr std::size_t kLoadedSpaceTableOffset = 0x1F88;
inline constexpr std::size_t kLoadedSpaceCountOffset = 0x1FB8;

}  // namespace sentinel::pinned::game_layout

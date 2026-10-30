// matchtag (#1737 row G6A-073): the in-match player tag shows its COMBAT page (name and team) by default, as the q15t combat
// build's T1/T1b do. game.MatchTagDisplay is bit 13 (0x2000) of the SR15GameSettings flags word.
//
// The game (libr15, ReVault VA = ELF vaddr + 0x100000):
//   - CR15Game's constructor (ReVault 0x12fce34) copies a 16-byte .rodata literal {0, 0x1003, 1, 0} to this+0x8c04 (0x12fd21c,
//     `str q1, [x27, #0x10fc]` with x27 = this+0x7b08), so the live flags word is this+0x8c08 = 0x1003 (bit 13 clear); and it
//     starts the pending copy from {0x1003, 1, 0, 0xa} at this+0x8ce4 (0x12fd28c, `str q0, [x27, #0x11dc]`).
//   - CR15Game::LoadSettings (ReVault 0x12f3e88, reached only through its JUMP_SLOT at ELF 0x36ce030) reads
//     settings_mp_v2_quest.json into the live block (SR15GameSettings::FromJson on this+0x8c08); a key missing from the file keeps
//     what the word already holds, and with no file it returns early without touching either word.
//
// The q15t build changes the two .rodata literals (0x1003 -> 0x3003). The sentinel never writes a code or .rodata page (ADR
// 0021), so this module gives the same default from the data side: a GOT wrapper on LoadSettings sets bit 13 in both words on
// the first call, before the original reads the file. A saved "MatchTagDisplay": false still wins, and the settings menu row
// "In-Match Tag Display" still toggles it. The bit is set only while both words still hold the constructor's 0x1003, so a
// changed layout is refused and logged, never written.
//
// Frequency: LoadSettings runs at boot (ConfigureEngine) and once more from ReVault 0x13021a4 (begin-multiplayer); not per frame.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

inline constexpr std::uint32_t kMatchTagDisplayBit = 0x2000;
inline constexpr std::uint32_t kStockSettingsFlags = 0x1003;  // the constructor's literal, both copies
inline constexpr std::uintptr_t kLiveFlagsOffset = 0x8c08;
inline constexpr std::uintptr_t kPendingFlagsOffset = 0x8ce4;

// Kill file `matchtag.off` in the plugin data directory (module_switch.h): nothing installed. Logs one
// `q15t_matchtag_install` record.
InstallResult InstallMatchTag(std::uintptr_t base);

struct MatchTagStats {
  std::uint32_t calls, applied, refused;
};
MatchTagStats GetMatchTagStats();
void ResetMatchTagForTest();

}  // namespace q15t

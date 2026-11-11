/* The pinned slots, functions, code words and data facts the sound-symbol guard relies on (the guard drops a post whose CSoundSymbol is 0 or the
 * invalid id; the Quest side is tracked in #1737).
 *
 * Every address is an ELF virtual address in the store libr15 (the store file's build ID is `kLibR15StockBuildId` in pinned_targets.h); the disassembly's address is
 * the ELF address plus 0x100000. src/quest/tests/sound_symbol_guard_pinned_test.cpp checks each slot, word and fact against the real file
 * (`just test-quest-hooks-pinned`, which needs the store APK). The store libr15 (sha256 8dd9a961...) relocations, from `llvm-readelf -rW`:
 *
 *   CSoundCS::PlaySound        JUMP_SLOT 0x36dd210 and GLOB_DAT 0x37148e0, both -> 0x1706904 (the GLOB_DAT is a function-pointer word)
 *   CSoundPlayer::Play3DSound  JUMP_SLOT 0x3701090 -> 0x134b468
 *
 * The bullet site, `bl` at 0x21230d0 inside CR15NetBulletCS::Update, calls Play3DSound through the PLT stub at 0xf53350 and returns to 0x21230d4.
 * The .bss word 0x37af76c is the default of the CSoundSymbol it passes: CR15NetBulletCS::Update loads it into w28 at 0x2122fdc and `mov w1, w28`
 * (0x21230c4) makes it the symbol argument. One path replaces w28 first: when the table lookup at 0x2123014..0x2123084 finds an index other than -1
 * (`cmn x11, #1` / `b.eq` at 0x2123084 not taken), `ldr w28, [x8, #0x8]` at 0x21230a0 loads that table entry's word. So the word is the
 * default, not the only source. `_INIT_355` (ReVault's name for 0xf70e14, listed in .init_array) writes the word once, at load, with the
 * .rodata word 0x31c8330 (0x1406b30a), so the default is 0x1406b30a and not zero. ReVault (libr15.so) lists no other write to 0x37af76c.
 *
 * The crash offset 0x11d1cac from upstream's notes is the return address after the `bl` at 0x11d1ca8, which calls the C1 constructor of
 * CConsoleStream through a PLT stub, inside OutputError (0x11d1b38, 1268 bytes). It is not a faulting instruction: a crash that logs +0x11d1cac is
 * a return from that call, so the fault is inside a callee.
 *
 * Upstream: `kotorvr/echoquest-combat` at `c5a67e8`, runtime/native/afkxr/afk_fx.cpp (the sound guards, around lines 5148-5264).
 *
 * PC (echovr.exe): echovr.exe CSoundCS::PlaySound is VA 0x1403f94c0, its play call is 0x14020b780, OutputError (0x14151ee70) is called at
 * 0x14020b8c1 after PostSoundEvent with no test of the symbol for 0, and whether that call faults on PC is not established.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/pinned_words.h"

namespace nevr_quest::combat::sound_symbol_guard::targets {

inline constexpr const char* kPlaySymbol =
    "_ZN10NRadEngine8CSoundCS9PlaySoundENS_18SCompactPoolHandleENS_12CSoundSymbolEfNS_15SSoundDebugInfoE";
inline constexpr const char* kPlay3DSymbol = "_ZN10NRadEngine12CSoundPlayer11Play3DSoundENS_12CSoundSymbolEjf";

// The slots the guard would hook or check, and the function each holds in the pinned build.
struct Pins {
  std::uint64_t playSlot;      // PlaySound's JUMP_SLOT
  std::uint64_t playDataSlot;  // PlaySound's GLOB_DAT (a function-pointer word for the same function)
  std::uint64_t playFn;        // CSoundCS::PlaySound
  std::uint64_t play3DSlot;    // Play3DSound's JUMP_SLOT
  std::uint64_t play3DFn;      // CSoundPlayer::Play3DSound
};
inline constexpr Pins kPins = {0x36dd210ULL, 0x37148e0ULL, 0x1706904ULL, 0x3701090ULL, 0x134b468ULL};

// The code words the guard relies on: the bullet site's path to Play3DSound, the default symbol's reads, and the writer of the default
// (its base, its source and the store). The last entry is the .rodata word the writer copies, a data word the same check reads.
inline constexpr pinned::Word kWords[] = {
    {"Play3DSound stub: adrp x16, 0x3701000", 0xf53350, 0xd0013d70},
    {"Play3DSound stub: ldr x17, [x16, #0x90]", 0xf53354, 0xf9404a11},
    {"Play3DSound stub: add x16, x16, #0x90", 0xf53358, 0x91024210},
    {"Play3DSound stub: br x17", 0xf5335c, 0xd61f0220},
    {"bullet site: bl Play3DSound@plt", 0x21230d0, 0x97b8c0a0},
    {"bullet site: the return address (ldr x8, [x19, #0x278])", 0x21230d4, 0xf9413e68},
    {"default symbol read: adrp x8, 0x37af000", 0x2122fd4, 0xb000b468},
    {"default symbol read: ldr w28, [x8, #0x76c]", 0x2122fdc, 0xb9476d1c},
    {"default symbol writer (_INIT_355): adrp x29, 0x37af000", 0xf70e3c, 0xf00141fd},
    {"default symbol writer (_INIT_355): add x29, x29, #0x75c", 0xf70e4c, 0x911d73bd},
    {"default symbol source (_INIT_355): adrp x10, 0x31c8000", 0xf712d0, 0xf00112aa},
    {"default symbol source (_INIT_355): add x10, x10, #0x337", 0xf712d4, 0x910cdd4a},
    {"default symbol source (_INIT_355): and x10, x10, #-8", 0xf712d8, 0x927df14a},
    {"default symbol source (_INIT_355): ldr w9, [x10]", 0xf712e0, 0xb9400149},
    {"default symbol writer (_INIT_355): str w9, [x29, #0x10] (writes 0x37af76c)", 0xf712ec, 0xb90013a9},
    {"default symbol source word: .rodata 0x1406b30a", 0x31c8330, 0x1406b30a},
};
inline constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
inline constexpr pinned::Pins kWordPins = {kWords, kWordCount, nullptr, 0};

// The default CSoundSymbol: a .bss word (the test checks it lies in .bss), and the function listed in .init_array that writes it at load.
inline constexpr std::uint64_t kDefaultSymbolVaddr = 0x37af76c;
inline constexpr std::uint64_t kDefaultWriterFn = 0xf70e14;

}  // namespace nevr_quest::combat::sound_symbol_guard::targets

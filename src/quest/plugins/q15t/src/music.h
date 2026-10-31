// music (source build T-48, the source build's music; default ON, option Q15T_MUSIC): one combat music instance at a time, and a log line
// for every music-bank post.
//
// Wraps libr15's JUMP_SLOT for CWwiseEngine::PostSoundEvent (ELF 0x36d76e8 -> the function at ELF 0x2549264, disassembly 0x2649264).
// Before one of the 2 combat-music Play events the wrapper posts BOTH music Stop events on the same game object (and on the
// previous music Play's object when that differs) through the real PostSoundEvent, then lets the Play through unchanged. It logs
// every post of a music-bank event (24 ids). MusicTick (game thread, 0.5 s) posts the two Stops once when no combat map is loaded
// any more after a music Play. Data-page hook only; no code word is written. Details, evidence, prediction: docs/quest/q15t/modules/music.md.
#pragma once

#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

namespace q15t {

// ELF vaddrs of the pinned build (R0_readelf_r.txt line 118641, see docs/quest/q15t/modules/music.md).
inline constexpr std::uintptr_t kMusicSlot = 0x36d76e8;
inline constexpr std::uintptr_t kMusicReal = 0x2549264;

// The first 29 words of PostSoundEvent (disassembly 0x2649264..0x26492d4 - 0x100000): the prologue and everything up to the seek test
// that fixes the argument layout the extra Stop posts use (x0 self, x1 event, w2 object, x3 &flags, x4 callback, x5 cookie,
// w6 seek ms, x7 &playing id). Read before the slot is swapped. The source build pins no code word; these are an addition.
// No MD5 or other file-hash gate.
inline constexpr WordPin kMusicWordPins[] = {
    {0x2549264, 0xd10183ff},  // sub sp, sp, #0x60
    {0x2549268, 0xa9016bfb},  // stp x27, x26, [sp, #0x10]
    {0x254926c, 0xa90263f9},  // stp x25, x24, [sp, #0x20]
    {0x2549270, 0xa9035bf7},  // stp x23, x22, [sp, #0x30]
    {0x2549274, 0xa90453f5},  // stp x21, x20, [sp, #0x40]
    {0x2549278, 0xa9057bf3},  // stp x19, x30, [sp, #0x50]
    {0x254927c, 0xb9400068},  // ldr w8, [x3]            flags = *x3
    {0x2549280, 0x2a0203e9},  // mov w9, w2              object
    {0x2549284, 0x3100045f},  // cmn w2, #0x1
    {0x2549288, 0xaa0103f4},  // mov x20, x1             event
    {0x254928c, 0xda9f1135},  // csinv x21, x9, xzr, ne  object, or all ones for 0xffffffff
    {0x2549290, 0xaa0703f6},  // mov x22, x7             &playing id
    {0x2549294, 0x2a0603f3},  // mov w19, w6             seek ms
    {0x2549298, 0xaa0503f7},  // mov x23, x5             cookie
    {0x254929c, 0xaa0403f8},  // mov x24, x4             callback
    {0x25492a0, 0xaa0303f9},  // mov x25, x3
    {0x25492a4, 0xaa0003fa},  // mov x26, x0             self
    {0x25492a8, 0x2a1403e0},  // mov w0, w20
    {0x25492ac, 0xaa1503e1},  // mov x1, x21
    {0x25492b0, 0x2a0803e2},  // mov w2, w8
    {0x25492b4, 0xaa0403e3},  // mov x3, x4
    {0x25492b8, 0xaa0503e4},  // mov x4, x5
    {0x25492bc, 0x2a1f03e5},  // mov w5, wzr
    {0x25492c0, 0xaa1f03e6},  // mov x6, xzr
    {0x25492c4, 0x2a1f03e7},  // mov w7, wzr
    {0x25492c8, 0x9410088e},  // bl 0x02a4b500
    {0x25492cc, 0xb90002c0},  // str w0, [x22]           *playing id = result
    {0x25492d0, 0x340003c0},  // cbz w0, 0x02649348
    {0x25492d4, 0x34000b73},  // cbz w19, 0x02649440     seek 0: no seek call
};
inline constexpr std::size_t kMusicWordPinCount = sizeof kMusicWordPins / sizeof kMusicWordPins[0];
// True when every pin matches at base + its vaddr (one ERROR line naming the first that does not).
bool MusicWordPinsMatch(std::uintptr_t base);

// The combat-music events of the source build's music bank (the source build's music:54-58): 2 Play, 2 Stop, 20 SetState.
inline constexpr std::uint32_t kMusicPlay[2] = {0x2f22c384, 0xb1b06cfb};
inline constexpr std::uint32_t kMusicStop[2] = {0xde586538, 0x2acba759};
inline constexpr std::uint32_t kMusicState[20] = {0x045d3c23, 0x215e97af, 0x4a61827d, 0x6a599cee, 0x7a9959a5, 0x98a1b868, 0xa3117938,
                                                  0xaee8f745, 0xd3282d2b, 0xd9a48db0, 0xd9a48db1, 0xd9a48db3, 0xdb7c7cf9, 0xe34a79b4,
                                                  0xe34a79b5, 0xe34a79b6, 0xe44bd2d0, 0xf0a25298, 0xf7898c0b, 0xffae8834};

// Installs the wrapper. Refused (nothing swapped, one ERROR/INFO line) on: `music.off` in the q15t data directory (reason
// "off_file", INFO), a code word that differs (reason "word_pin_mismatch", ERROR naming pin/found/expected), a slot that does not
// hold base + 0x2549264 (status unexpected_original, ERROR naming pin/found/expected). There is no chaining with another wrapper.
InstallResult InstallMusic(std::uintptr_t base);
InstallResult InstallMusicAt(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr);

// The game-thread tick (register with AddGameTick when the install succeeded; the module does not register itself): every 0.5 s,
// when no combat map is loaded any more after a music Play, posts the two Stops once on the last Play's object.
void MusicTick(std::uintptr_t game);

// Counters: music-bank posts seen, extra Stop posts made, ticks that posted Stops.
struct MusicStats {
  std::uint64_t bank_posts, extra_stops, tick_stops;
};
MusicStats GetMusicStats();

// Test seams: counters/state back to zero (and the real function forgotten); the tick's 0.5 s schedule forgotten; the tick state
// forced.
void ResetMusicForTest();
void ResetMusicScheduleForTest();
void MusicForceStateForTest(bool playing, std::uint64_t self, std::uint32_t obj);

}  // namespace q15t

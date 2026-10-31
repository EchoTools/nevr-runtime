// fxtrail (source build RE-1, the source build's fxtrail; row R29; default ON, option Q15T_FXTRAIL): a bullet whose trail effect has no
// instance ends its trail, so the bullet can be freed.
//
// Wraps libr15's JUMP_SLOT for CParticleEffectCS::EffectTime (ELF 0x36e7b98 -> the function at ELF 0x16647e4). The wrapper
// calls the real function and returns its value, except for an effect with no instance (the lookup EffectTime itself does:
// [cs+0xc8] u16 table at 4*(h&0xffff) -> index, [cs+0xf8] + index*0x18, +0x10 == null), where it returns FLT_MAX: a trail that
// never existed has run its course. A read that fails, or an effect with an instance, returns the real value untouched.
// Data-page hook only; no code word is written. Details, evidence and prediction: docs/quest/q15t/modules/fxtrail.md.
#pragma once

#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

namespace q15t {

// ELF vaddrs of the pinned build (R0_readelf_r.txt, see docs/quest/q15t/modules/fxtrail.md).
inline constexpr std::uintptr_t kFxTrailSlot = 0x36e7b98;
inline constexpr std::uintptr_t kFxTrailReal = 0x16647e4;

// The code words of EffectTime the wrapper repeats the lookup of (docs/quest/q15t/modules/fxtrail.md): read before the slot is swapped. The
// source build pins none; these are an addition. No MD5 or other file-hash gate.
inline constexpr WordPin kFxTrailWordPins[] = {
    {0x16647e4, 0xf9406408},
    {0x16647e8, 0x92403c29},
    {0x16647ec, 0xd37ef529},
    {0x16647f0, 0x5280030a},
    {0x16647f4, 0x78696908},
    {0x16647f8, 0xf9407c09},
    {0x1664800, 0x9b0a2508},
    {0x1664804, 0xf9400908},
    {0x1664808, 0xb4000068},
    {0x166480c, 0xbd434100},
    {0x1664810, 0xd65f03c0},
};
inline constexpr std::size_t kFxTrailWordPinCount = sizeof kFxTrailWordPins / sizeof kFxTrailWordPins[0];
// True when every pin matches at base + its vaddr (one ERROR line naming the first that does not).
bool FxTrailWordPinsMatch(std::uintptr_t base);

// `<q15t data dir>/fxtrail.off` present: refused (reason "off_file", one INFO line), nothing swapped.
InstallResult InstallFxTrail(std::uintptr_t base);
InstallResult InstallFxTrailAt(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr);

// Trails ended so far (calls that returned FLT_MAX).
std::uint64_t FxTrailEnded();

// Test seams: the directory the kill file is looked up in; counters back to zero.
void SetFxTrailDirForTest(const char* dir);
void ResetFxTrailForTest();

}  // namespace q15t

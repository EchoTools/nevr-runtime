// fxguard (issue #1737 row G6A-031): two crash and frame-time guards on libr15's sound posts and bullet fire. Data-page hooks only
// (GOT words, got_swap.h); no code page is written.
//
// Sound guard (switch fxsndguard.off turns it off):
//   CSoundCS::PlaySound(this, SCompactPoolHandle, CSoundSymbol, float, SSoundDebugInfo) is called directly (JUMP_SLOT 0x36dd210) and
//   through the method pointer CSoundCS::DeferredPlaySound queues (GLOB_DAT 0x37148e0, read at ReVault 0x166c7f4). A call whose symbol
//   is 0 in its low 32 bits, or the whole 64-bit 0x0f986c58c7b3d4fb (no Wwise event has it; the game's symbol table names it
//   "SoundPlay"), is answered with the result PlaySound gives for a failed post, without calling it. A failed post makes the game print a
//   symbolised callstack (several ms on the main thread; on the audio thread, which has no log writer, the print faults).
//   CSoundPlayer::Play3DSound(CSoundSymbol, unsigned, float) (JUMP_SLOT 0x3701090) is reached without PlaySound from
//   CR15NetBulletCS::Update (the `bl` at ELF 0x21230d0): a call with the whole invalid symbol is dropped from any caller, and from that one
//   call site also a symbol equal to the default Update loads on a table miss (ELF 0x37af76c, read per call) or equal to 1.
//   Switch fxsndguardid.off keeps only the symbol-0 drop.
// Fire guard (switch fxfireguard.off turns it off):
//   CR15NetBulletCS::FireBullet (JUMP_SLOT 0x36f3cf8) normalises the velocity with no zero test (ELF 0x210ccc4 `fdiv s3, s3, s4`), so a
//   velocity of length ~0 stores a NaN direction. Such a call, or one whose velocity is not finite, is skipped. The slot is shared with
//   dronemuzzle: fxguard installs after it and chains on whatever the slot holds (InstallChainedImportHook), and passes every other call
//   on as a tail call, so the return address dronemuzzle's thunk compares is the game's.
// Whole module off: fxguard.off. Details and evidence: docs/quest/q15t/modules/fxguard.md.
#pragma once

#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

namespace q15t {

struct FxGuardSlots {
  std::uintptr_t play_slot, play_data_slot, play_fn;  // CSoundCS::PlaySound: JUMP_SLOT, GLOB_DAT, function
  std::uintptr_t play3d_slot, play3d_fn;              // CSoundPlayer::Play3DSound: JUMP_SLOT, function
  std::uintptr_t bullet_ret, bullet_default;          // the return address after Update's `bl` to Play3DSound; the miss default's .bss word
  std::uintptr_t fire_slot, fire_fn;                  // CR15NetBulletCS::FireBullet: JUMP_SLOT, function
};
extern const FxGuardSlots kFxGuardPinned;

inline constexpr std::uint64_t kFxGuardInvalidSymbol = 0x0f986c58c7b3d4fbull;

// The words PlaySound's failure result was reconstructed from (ReVault 0x1806904..0x1806a20, ELF = VA - 0x100000).
inline constexpr WordPin kFxGuardSoundPins[] = {
    {0x1706914, 0xf9406408},  // ldr x8, [x0, #0xc8]       the handle's index table
    {0x1706920, 0x5280180a},  // mov w10, #0xc0            the pool entry stride
    {0x1706924, 0x78696908},  // ldrh w8, [x8, x9]         the entry index
    {0x1706928, 0xf9407c09},  // ldr x9, [x0, #0xf8]       the pool
    {0x1706934, 0x39400d4a},  // ldrb w10, [x10, #3]
    {0x1706938, 0x3708014a},  // tbnz w10, #1, ...         a released entry: x0 = ~0, x1 = 0
    {0x170697c, 0xb94006a8},  // ldr w8, [x21, #4]         the emitter id, the low half of x0
    {0x1706a00, 0x12800000},  // mov w0, #-1               a failed post: the high half of x0
    {0x1706a04, 0x52800021},  // mov w1, #1
};
// The bullet call site and the miss default it loads (ReVault 0x22230d0, 0x2222fd4, 0x2222fdc). A mismatch turns off the site rule only.
inline constexpr WordPin kFxGuardBulletSitePins[] = {
    {0x21230d0, 0x97b8c0a0},  // bl Play3DSound (PLT)
    {0x2122fd4, 0xb000b468},  // adrp x8, 0x37af000
    {0x2122fdc, 0xb9476d1c},  // ldr w28, [x8, #0x76c]
};
// FireBullet's unguarded normalisation (ReVault 0x220ccc4).
inline constexpr WordPin kFxGuardFirePins[] = {
    {0x210ccc4, 0x1e241863},  // fdiv s3, s3, s4
};

struct FxGuardInstall {
  InstallResult play, play_data, play3d, fire;  // per hook; status kNullSlot with reason "off" when that guard is switched off
  bool sound_on = false, bullet_site_on = false, fire_on = false;
  InstallResult summary;  // for ReportModule: kOk when every guard that is on installed
};
FxGuardInstall InstallFxGuard(std::uintptr_t base, const FxGuardSlots& slots = kFxGuardPinned);

// Once a frame on the game thread: every 10 s, when a count changed, one INFO line with the totals.
void FxGuardTick(std::uintptr_t game);

struct FxGuardStats {
  std::uint32_t sound_zero, sound_invalid, play3d_dropped, site_calls, site_dropped, fire_skipped;
};
FxGuardStats GetFxGuardStats();

// Tests: the pure decisions, and the clock and switch directory of the module.
bool FxGuardDropsPlaySound(std::uint64_t symbol, bool invalid_id_on);
bool FxGuardDropsPlay3D(std::uint64_t symbol, bool invalid_id_on, bool from_bullet_site, std::uint32_t site_default);
bool FxGuardSkipsFire(const float* velocity);
void SetFxGuardClockForTest(std::uint64_t (*now_ms)());
void ResetFxGuardForTest();

}  // namespace q15t

#pragma once
// The sound-symbol rule: whether a sound post's CSoundSymbol is one the game's sound engine cannot play, so the post is only cost. The 32-bit
// CSoundSymbol is the Wwise event id the post hands to PostEvent unchanged (Quest: Play3DSound masks it, `and x1, x21, #0xffffffff` at libr15
// ELF 0x134b5d8; PostSoundEvent passes it as the event id, 0x25492a8; quest/combat/sound_symbol_guard_targets.h pins both). PostEvent answers 0
// for an id no loaded bank defines, and PostSoundEvent then records the "not a valid wwise identifier" error and returns its status, which
// Play3DSound hands to OutputError (which logs the callstack and writes the record when that status is neither 0 nor -1).
//
// Two classes of symbol fail that way on every post:
//   kZero          the symbol 0, Wwise's invalid id (AK_INVALID_UNIQUE_ID): no event may have it (the Wwise SDK's contract; the banks
//                  were not searched for 0).
//   kUnregistered  a symbol in kUnregistered[]: measured absent from every bank the game ships. Each entry carries its evidence.
// No other symbol is classed: a symbol is listed only on a measurement of the banks, never on another project's word.
//
// This header is the one definition, for both builds: quest/combat/sound_symbol_guard.cpp drops a post of such a symbol. PC calls nothing here
// yet (echovr.exe calls OutputError on the same chain; whether that matters on PC is not established). It reads no memory and calls nothing of the
// game's.
//
// Origin: the sound guards of kotorvr's echoquest-combat, runtime/native/afkxr/afk_fx.cpp (its "fxsndguard" switch), read as text. The class
// list is the tree's own measurement, not upstream's: upstream's third value, 0x0f986c58c7b3d4fb (low word 0xc7b3d4fb), is the error record's
// type key, not a sound symbol (sound_symbol_guard_targets.h), and is not listed.
#include <cstddef>
#include <cstdint>

namespace nevr_combat::sound_symbol_guard {

enum class Drop : std::uint8_t {
  kNone,          // a symbol the banks may define: the post goes ahead
  kZero,          // the invalid id 0
  kUnregistered,  // a symbol of kUnregistered[]
};

// Wwise's invalid id (AK_INVALID_UNIQUE_ID).
inline constexpr std::uint32_t kInvalidSymbol = 0;

// Symbols measured absent from the game's banks.
inline constexpr std::uint32_t kUnregistered[] = {
    // CR15NetBulletCS::Update's default bullet sound: the .bss word libr15 ELF 0x37af76c, written once at load from the .rodata word 0x31c8330
    // (sound_symbol_guard_targets.h). tools/cmd/wwise-events over the two stock packages of the store OBB (48037dc70b0ecab2_0, 2b47aab238f60515_0,
    // each equal to its digest in tools/quest_stock_data.pin): 34 banks, 1295 distinct event ids, and 0x1406b30a is no event and no HIRC object of
    // any type; its four bytes occur nowhere in either decompressed package.
    0x1406b30aU,
};
inline constexpr std::size_t kUnregisteredCount = sizeof(kUnregistered) / sizeof(kUnregistered[0]);

// The class of `symbol` (the low 32 bits of the post's symbol register).
constexpr Drop Classify(std::uint32_t symbol) noexcept {
  if (symbol == kInvalidSymbol) return Drop::kZero;
  for (std::size_t i = 0; i < kUnregisteredCount; ++i) {
    if (kUnregistered[i] == symbol) return Drop::kUnregistered;
  }
  return Drop::kNone;
}

// True when a post of `symbol` can only fail: the symbol is 0 or in kUnregistered[].
constexpr bool Drops(std::uint32_t symbol) noexcept { return Classify(symbol) != Drop::kNone; }

// The word a log line uses for a class.
constexpr const char* DropName(Drop d) noexcept {
  return d == Drop::kZero ? "zero" : d == Drop::kUnregistered ? "unregistered" : "none";
}

}  // namespace nevr_combat::sound_symbol_guard

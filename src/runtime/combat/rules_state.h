#pragma once

// The RULES mailbox: the bytes in echovr.exe's .data tail that the ported VeilNet and the script hooks share (#600, the
// scripts plan's D5). One definition of where it is and what each byte is, over the same bytes VeilNet has always
// used, so veilnet.cpp keeps working unchanged in behaviour; it names these offsets from here.
//
// The mailbox is the .data page end (0x2105000 for the supported build) less 0x140 (veilnet.cpp RulesMailboxRva, which
// computes it at run time; the image guard makes the build the supported one). The fields the script hooks use:
//  +0x00, +0x04  starting scores, blue and orange (float points; 0 is the stock 0; the game-mode script's publish nodes
//                write them, the capture point hooks read them)
//  +0x08         T, the RULES page's two time rows as one MM:SS value in seconds (float; set when its bits as an int32 are
//                above 0; the time page hooks write it)
//  +0x0C         the capture time, T mod 60 (float; the time page hooks write it, VeilNet reads it)
//  +0x14..+0x16  station cells shown per gear type (u8 each; VeilNet writes, the station hooks read)
//  +0x2D         POINT RATE shown (u8, quarters: 4 is x1, 0 not set; VeilNet writes, the point rate hook reads)
//  +0x2E         PAUSE shown (u8: 0 off, 1 paused, 2 the 5 s resume countdown; VeilNet writes, on server and client)
//  +0x2F         match live (u8: 1 once the spawn orbs read a set spawn_access other than pre_match; the orb hook
//                writes, VeilNet reads for TEAM READY -> PAUSE)
// The bytes a client takes from the server's 'G' word (runtime/combat/match_control_rules.h Apply): +0x11 LOADOUTS, +0x2C
// SERIES-10 ONLY, +0x2D POINT RATE, +0x2E PAUSE, +0x30 payload map, +0x31 PAYLOAD SPEED, +0x32 checkpoint used, +0x33 driving to
// the checkpoint, +0x34 PAUSE queued; and T at +0x08 from its 'R'. The mailbox is kMailboxSize bytes: the EC page's veil use
// counter follows it (runtime/patch/ec_rules.h kVeilUse, page +0xF00 = mailbox +0x40).
// Every byte field is written and read whole by one store or load, as VeilNet's code does; the two scores are floats,
// each one aligned 4-byte store or load.

#include <cstdint>

namespace nevr_combat::rules {

inline constexpr std::uintptr_t kMailboxRva = 0x2104ec0;
inline constexpr std::uint32_t kMailboxSize = 0x40;
inline constexpr std::uint32_t kBlueStartScore = 0x00;
inline constexpr std::uint32_t kOrangeStartScore = 0x04;
inline constexpr std::uint32_t kRoundTotal = 0x08;
inline constexpr std::uint32_t kCaptureTime = 0x0C;
inline constexpr std::uint32_t kStationCells = 0x14;
inline constexpr std::uint32_t kPointRate = 0x2d;
inline constexpr std::uint32_t kPausedShown = 0x2e;
inline constexpr std::uint32_t kMatchLive = 0x2f;
// The bytes a new session starts with (#2242, EchoCombat 2.0.3): LOADOUTS all, SERIES-10 ONLY off, POINT RATE and PAYLOAD SPEED
// x1 (4 quarters), PAUSE off and nothing queued. The LOADOUTS byte is veilnet.cpp's LOADOUT_ALL, the mailbox's +11h.
inline constexpr std::uint32_t kClassicShown = 0x11;
inline constexpr std::uint32_t kChassisShown = 0x2C;
inline constexpr std::uint32_t kPspeedShown = 0x31;
inline constexpr std::uint32_t kPauseQueuedShown = 0x34;
inline constexpr std::uint32_t kPayloadMap = 0x30;
inline constexpr std::uint32_t kCheckpointShown = 0x32;
inline constexpr std::uint32_t kSkipShown = 0x33;
inline constexpr std::uint8_t kLoadoutAll = 0;
inline constexpr std::uint8_t kRateDefault = 4;

enum class Pause : std::uint8_t { kOff = 0, kOn = 1, kResuming = 2 };
enum class Team : std::uint8_t { kBlue, kOrange };

/// A typed view of the mailbox at `mailbox` (game base + kMailboxRva). A null mailbox reads as all zero and drops writes.
class RulesState {
 public:
  explicit RulesState(volatile std::uint8_t* mailbox) : m_(mailbox) {}
  Pause Paused() const { return m_ != nullptr ? static_cast<Pause>(m_[kPausedShown]) : Pause::kOff; }
  std::uint8_t PointRate() const { return m_ != nullptr ? m_[kPointRate] : 0; }
  void SetMatchLive(bool live) const {
    if (m_ != nullptr) m_[kMatchLive] = live ? 1 : 0;
  }
  /// A team's starting score, the float at +0x00 (blue) or +0x04 (orange); 0 with no mailbox.
  float StartScore(Team team) const {
    return m_ != nullptr ? *reinterpret_cast<const volatile float*>(m_ + (team == Team::kBlue ? kBlueStartScore
                                                                                              : kOrangeStartScore))
                         : 0.0f;
  }
  void SetStartScores(float blue, float orange) const {
    if (m_ == nullptr) return;
    *reinterpret_cast<volatile float*>(m_ + kBlueStartScore) = blue;
    *reinterpret_cast<volatile float*>(m_ + kOrangeStartScore) = orange;
  }
  /// T, and whether it is set: its bits as an int32 above 0, as his `cmp dword [T], 0; jg` reads it.
  float RoundTotal() const { return m_ != nullptr ? *reinterpret_cast<const volatile float*>(m_ + kRoundTotal) : 0.0f; }
  bool RoundTotalSet() const {
    return m_ != nullptr && *reinterpret_cast<const volatile std::int32_t*>(m_ + kRoundTotal) > 0;
  }
  void SetRoundTime(float total, float capture) const {
    if (m_ == nullptr) return;
    *reinterpret_cast<volatile float*>(m_ + kRoundTotal) = total;
    *reinterpret_cast<volatile float*>(m_ + kCaptureTime) = capture;
  }
  const volatile std::uint8_t* StationCells() const { return m_ != nullptr ? m_ + kStationCells : nullptr; }

 private:
  volatile std::uint8_t* m_;
};

// Writes a new session's defaults into the mailbox at `mailbox` (game base + kMailboxRva): the four scores and T and the
// capture time read as zero, and the shown bytes of the defaults above. Nothing else of the mailbox is touched.
inline void ApplyDefaults(volatile std::uint8_t* mailbox) {
  *reinterpret_cast<volatile float*>(mailbox + kBlueStartScore) = 0.0f;
  *reinterpret_cast<volatile float*>(mailbox + kOrangeStartScore) = 0.0f;
  *reinterpret_cast<volatile float*>(mailbox + kRoundTotal) = 0.0f;
  *reinterpret_cast<volatile float*>(mailbox + kCaptureTime) = 0.0f;
  mailbox[kClassicShown] = kLoadoutAll;
  mailbox[kChassisShown] = 0;
  mailbox[kPointRate] = kRateDefault;
  mailbox[kPspeedShown] = kRateDefault;
  mailbox[kPausedShown] = static_cast<std::uint8_t>(Pause::kOff);
  mailbox[kPauseQueuedShown] = 0;
}

// VeilNet's once-per-session watch: `generation` is the session store's count (nevr_arena_round_time::SessionGeneration). True
// when a session has been seen before and this one is another; the first session the process sees is not a new one, and no
// session yet (generation 0) changes nothing. `seen` takes the generation.
inline bool NewSessionSeen(std::uint64_t& seen, std::uint64_t generation) {
  if (generation == 0 || generation == seen) return false;
  const bool again = seen != 0;
  seen = generation;
  return again;
}

}  // namespace nevr_combat::rules

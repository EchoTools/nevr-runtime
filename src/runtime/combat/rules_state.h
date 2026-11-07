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
//  +0x2E         PAUSE shown (u8: 0 off, 1 paused, 2 the 5 s resume countdown; VeilNet writes, on server and client)
//  +0x2F         match live (u8: 1 once the spawn orbs read a set spawn_access other than pre_match; the orb hook
//                writes, VeilNet reads for TEAM READY -> PAUSE)
// Every byte field is written and read whole by one store or load, as VeilNet's code does; the two scores are floats,
// each one aligned 4-byte store or load.

#include <cstdint>

namespace nevr_combat::rules {

inline constexpr std::uintptr_t kMailboxRva = 0x2104ec0;
inline constexpr std::uint32_t kBlueStartScore = 0x00;
inline constexpr std::uint32_t kOrangeStartScore = 0x04;
inline constexpr std::uint32_t kRoundTotal = 0x08;
inline constexpr std::uint32_t kCaptureTime = 0x0C;
inline constexpr std::uint32_t kStationCells = 0x14;
inline constexpr std::uint32_t kPausedShown = 0x2e;
inline constexpr std::uint32_t kMatchLive = 0x2f;

enum class Pause : std::uint8_t { kOff = 0, kOn = 1, kResuming = 2 };
enum class Team : std::uint8_t { kBlue, kOrange };

/// A typed view of the mailbox at `mailbox` (game base + kMailboxRva). A null mailbox reads as all zero and drops writes.
class RulesState {
 public:
  explicit RulesState(volatile std::uint8_t* mailbox) : m_(mailbox) {}
  Pause Paused() const { return m_ != nullptr ? static_cast<Pause>(m_[kPausedShown]) : Pause::kOff; }
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

}  // namespace nevr_combat::rules

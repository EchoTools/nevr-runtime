#pragma once

// The RULES mailbox: the bytes in echovr.exe's .data tail that the ported VeilNet and the script hooks share (#600, the
// scripts plan's D5). One definition of where it is and what each byte is, over the same bytes VeilNet has always
// used, so veilnet.cpp keeps working unchanged in behaviour; it names these offsets from here.
//
// The mailbox is the .data page end (0x2105000 for the supported build) less 0x140 (veilnet.cpp RulesMailboxRva, which
// computes it at run time; the image guard makes the build the supported one). The fields the script hooks use:
//  +0x14..+0x16  station cells shown per gear type (u8 each; VeilNet writes, the station hooks read)
//  +0x2E         PAUSE shown (u8: 0 off, 1 paused, 2 the 5 s resume countdown; VeilNet writes, on server and client)
//  +0x2F         match live (u8: 1 once the spawn orbs read a set spawn_access other than pre_match; the orb hook
//                writes, VeilNet reads for TEAM READY -> PAUSE)
// Every field is one byte, written and read whole by one store or load, as VeilNet's code does.

#include <cstdint>

namespace nevr_combat::rules {

inline constexpr std::uintptr_t kMailboxRva = 0x2104ec0;
inline constexpr std::uint32_t kStationCells = 0x14;
inline constexpr std::uint32_t kPausedShown = 0x2e;
inline constexpr std::uint32_t kMatchLive = 0x2f;

enum class Pause : std::uint8_t { kOff = 0, kOn = 1, kResuming = 2 };

/// A typed view of the mailbox at `mailbox` (game base + kMailboxRva). A null mailbox reads as all zero and drops writes.
class RulesState {
 public:
  explicit RulesState(volatile std::uint8_t* mailbox) : m_(mailbox) {}
  Pause Paused() const { return m_ != nullptr ? static_cast<Pause>(m_[kPausedShown]) : Pause::kOff; }
  void SetMatchLive(bool live) const {
    if (m_ != nullptr) m_[kMatchLive] = live ? 1 : 0;
  }
  const volatile std::uint8_t* StationCells() const { return m_ != nullptr ? m_ + kStationCells : nullptr; }

 private:
  volatile std::uint8_t* m_;
};

}  // namespace nevr_combat::rules

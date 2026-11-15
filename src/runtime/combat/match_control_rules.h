#pragma once
// VeilNet's match control on the wire (#2242, EchoCombat nevr-pack 82ad7ba, plugin/veilnet.cpp RulesWord and Handle's 'L', 'R' and
// 'G' branches): the server's rules word, what a client writes into the RULES mailbox (rules_state.h) from 'G' and 'R', the
// mailbox fields a client puts back when its server does not state EchoCombat, and the map indices 'L' carries. One definition for
// both builds: PC's runtime/combat/veilnet.cpp and the Quest's quest/combat/ec.cpp call it; each keeps its own sockets, its check
// that a datagram is the current server's, its log lines and what a map switch does. test_veilnet_rules.cpp (PC) and ec_test.cpp
// (Quest) drive it.
//
// The 'G' word: LOADOUTS in bits 0-1 (0 ALL, 1 CLASSIC, 2 NEW; 3 reads as ALL), SERIES-10 ONLY in bit 2, PAUSE in bits 3-4 (0 off,
// 1 on, 2 the resume countdown; 3 reads as off), the payload map in bit 5 and PAYLOAD SPEED (quarters) in bits 16-23
// (payload_speed_rules.h), driving to the checkpoint in bit 6, PAUSE queued in bit 7, POINT RATE (quarters) in bits 8-15, and the
// checkpoint used in bits 24-25 (no server sets them since CHECKPOINT USED was dropped; a client still copies them).
// 'R' carries T, the RULES page's two time rows as one float of seconds (rules_state.h kRoundTotal), as its bits.
// 'L' carries a map index, kMapCount of them.
#include <cmath>
#include <cstdint>
#include <cstring>

#include "runtime/combat/payload_speed_rules.h"
#include "runtime/combat/rules_state.h"

namespace nevr_combat::match_control {

inline constexpr std::uint8_t kLoadoutAll = rules::kLoadoutAll;
inline constexpr std::uint8_t kLoadoutClassic = 1;
inline constexpr std::uint8_t kLoadoutNew = 2;

inline constexpr std::uint32_t kLoadoutMask = 3;
inline constexpr unsigned kChassisShift = 2;
inline constexpr unsigned kPauseShift = 3;
inline constexpr std::uint32_t kPauseMask = 3;
inline constexpr unsigned kSkipShift = 6;
inline constexpr unsigned kPauseQueuedShift = 7;
inline constexpr unsigned kRateShift = 8;
inline constexpr unsigned kCheckpointShift = 24;
inline constexpr std::uint32_t kCheckpointMask = 3;
inline constexpr std::uint32_t kBitMask = 1;
inline constexpr std::uint32_t kByteMask = 0xff;

/// The rules the server's 'G' word states.
struct RulesWord {
  std::uint8_t loadout = kLoadoutAll;
  bool chassis = false;  // SERIES-10 ONLY
  rules::Pause paused = rules::Pause::kOff;
  bool payloadMap = false;
  bool skipping = false;     // the server drives the payload to the checkpoint
  bool pauseQueued = false;  // a PAUSE waits for a checkpoint
  std::uint8_t pointRate = 0;     // quarters; 0 is not set
  std::uint8_t payloadSpeed = 0;  // quarters; 0 is not set
  std::uint8_t checkpoint = 0;    // decoded only: no server packs it
};

/// The server's word (veilnet.cpp RulesWord). The checkpoint is not packed.
inline std::uint32_t Pack(const RulesWord& r) {
  return (static_cast<std::uint32_t>(r.loadout) & kLoadoutMask) | (r.chassis ? kBitMask : 0u) << kChassisShift |
         (static_cast<std::uint32_t>(r.paused) & kPauseMask) << kPauseShift | payload_speed::PackWord(r.payloadMap, r.payloadSpeed) |
         (r.skipping ? kBitMask : 0u) << kSkipShift | (r.pauseQueued ? kBitMask : 0u) << kPauseQueuedShift |
         static_cast<std::uint32_t>(r.pointRate) << kRateShift;
}

/// What a client reads from the word, as veilnet.cpp's 'G' branch: a LOADOUTS of 3 reads as ALL, a PAUSE of 3 as off.
inline RulesWord Decode(std::uint32_t word) {
  RulesWord r;
  const std::uint32_t m = word & kLoadoutMask;
  r.loadout = m <= kLoadoutNew ? static_cast<std::uint8_t>(m) : kLoadoutAll;
  r.chassis = (word >> kChassisShift & kBitMask) != 0;
  const std::uint32_t z = word >> kPauseShift & kPauseMask;
  r.paused = z <= static_cast<std::uint32_t>(rules::Pause::kResuming) ? static_cast<rules::Pause>(z) : rules::Pause::kOff;
  r.payloadMap = payload_speed::WordMap(word);
  r.skipping = (word >> kSkipShift & kBitMask) != 0;
  r.pauseQueued = (word >> kPauseQueuedShift & kBitMask) != 0;
  r.pointRate = static_cast<std::uint8_t>(word >> kRateShift & kByteMask);
  r.payloadSpeed = payload_speed::WordQuarters(word);
  r.checkpoint = static_cast<std::uint8_t>(word >> kCheckpointShift & kCheckpointMask);
  return r;
}

inline const char* LoadoutName(std::uint32_t m) { return m == kLoadoutClassic ? "CLASSIC" : m == kLoadoutNew ? "NEW" : "ALL"; }
inline const char* PauseName(rules::Pause p) {
  return p == rules::Pause::kOn ? "on" : p == rules::Pause::kResuming ? "resuming" : "off";
}

/// Which of the logged rules a 'G' changed.
struct Applied {
  bool rate = false, payload = false, pause = false, loadout = false, chassis = false;
  bool Any() const { return rate || payload || pause || loadout || chassis; }
};

/// A client's 'G' from its server: the shown bytes of the mailbox at `mailbox`, in the order veilnet.cpp's branch writes them. The
/// two payload-progress bytes (driving to the checkpoint, PAUSE queued) are written every time and are not reported.
inline Applied Apply(volatile std::uint8_t* mailbox, std::uint32_t word) {
  const RulesWord r = Decode(word);
  Applied a;
  if (mailbox[rules::kPointRate] != r.pointRate) {
    mailbox[rules::kPointRate] = r.pointRate;
    a.rate = true;
  }
  const std::uint8_t pm = r.payloadMap ? 1 : 0;
  if (mailbox[rules::kPayloadMap] != pm || mailbox[rules::kPspeedShown] != r.payloadSpeed ||
      mailbox[rules::kCheckpointShown] != r.checkpoint) {
    mailbox[rules::kPayloadMap] = pm;
    mailbox[rules::kPspeedShown] = r.payloadSpeed;
    mailbox[rules::kCheckpointShown] = r.checkpoint;
    a.payload = true;
  }
  mailbox[rules::kSkipShown] = r.skipping ? 1 : 0;
  mailbox[rules::kPauseQueuedShown] = r.pauseQueued ? 1 : 0;
  const std::uint8_t z = static_cast<std::uint8_t>(r.paused);
  if (mailbox[rules::kPausedShown] != z) {
    mailbox[rules::kPausedShown] = z;
    a.pause = true;
  }
  if (mailbox[rules::kClassicShown] != r.loadout) {
    mailbox[rules::kClassicShown] = r.loadout;
    a.loadout = true;
  }
  const std::uint8_t c = r.chassis ? 1 : 0;
  if (mailbox[rules::kChassisShown] != c) {
    mailbox[rules::kChassisShown] = c;
    a.chassis = true;
  }
  return a;
}

/// A client whose server does not state EchoCombat (veilnet.cpp UpdateGearLists): the 'G' bytes back to ALL, off, not set. True
/// when a byte was not zero already. T (+0x08) is left as it is, as on PC.
inline bool ClearServerRules(volatile std::uint8_t* mailbox) {
  constexpr std::uint32_t kFields[] = {rules::kClassicShown, rules::kChassisShown,     rules::kPointRate,
                                       rules::kPausedShown,  rules::kPayloadMap,       rules::kPspeedShown,
                                       rules::kCheckpointShown, rules::kSkipShown,    rules::kPauseQueuedShown};
  bool was = false;
  for (std::uint32_t at : kFields) {
    was |= mailbox[at] != 0;
    mailbox[at] = 0;
  }
  return was;
}

/// A client's 'R' from its server: T's bits into the mailbox (+0x08), one aligned 4-byte store. True when they changed.
inline bool ApplyRoundTotal(volatile std::uint8_t* mailbox, std::uint32_t bits) {
  auto* const t = reinterpret_cast<volatile std::uint32_t*>(mailbox + rules::kRoundTotal);
  if (*t == bits) return false;
  *t = bits;
  return true;
}

inline constexpr int kSecondsPerMinute = 60;
/// T's two rows: POINT UNLOCK in minutes and CAPTURE TIME in seconds. A T that is not a finite number of seconds from 0 up to
/// kMaxRoundSeconds reads as 0 and 0.
inline constexpr float kMaxRoundSeconds = 1.0e6f;
struct RoundParts {
  int unlockMinutes = 0;
  int captureSeconds = 0;
};
inline RoundParts Parts(std::uint32_t bits) {
  float t = 0.0f;
  std::memcpy(&t, &bits, sizeof t);
  if (!std::isfinite(t) || t < 0.0f || t > kMaxRoundSeconds) return {};
  const int total = static_cast<int>(t);
  return {total / kSecondsPerMinute, total % kSecondsPerMinute};
}

// ---- 'L': the map the server switches to (the post-match podium's four buttons)
inline constexpr int kMapCount = 4;
inline constexpr const char* kMapNames[kMapCount] = {"DYSON", "COMBUSTION", "FISSION", "SURGE"};  // SURGE = mpl_combat_gauss
inline bool MapIndexValid(std::uint32_t id) { return id < static_cast<std::uint32_t>(kMapCount); }

}  // namespace nevr_combat::match_control

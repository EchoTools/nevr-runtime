#pragma once

#include <array>
#include <cstdint>
#include <map>

#include "runtime/patch/lobby_full_backoff_rules.h"

// Spacing for a lobby join the server refused with "lobby full" (#1001, the Quest twin of #889).
//
// The game re-issues a lobby join from its own state machine on every tick after a refusal. On the pinned
// libr15.so build, CR15NetClientLobby::Join is at 0x1240ba8 and its refusal callback,
// CR15NetClientLobby::LobbySessionFailureCB, is at 0x1240cc8 (link-time addresses, quest/social/lobby_full_hooks.h).
// Nothing in the game waits between attempts.
//
// The key is the 16 bytes the join passes on to CNSLobby::RequestJoin (the message's field at +0x18). That the key
// names the lobby is not measured yet: the device run in #1001 logs it (quest/social/lobby_full_hooks.cpp).
//
// This class has no clock and no lock: the caller passes the time and serializes calls.
// A refusal k (1-based) lets the next send go after min(kFirstWaitMs * 2^(k-1), kMaxWaitMs). The refusal that reaches
// kGiveUpAfter starts a cooldown of kCooldownMs for that lobby, the same shape as the PC policy (runtime/patch/
// lobby_full_backoff_rules.h): no send until the cooldown ends, then the count starts again from the first wait.
namespace quest_social::lobby_full {

using LobbyKey = std::array<std::uint8_t, 16>;

class FullLobbyBackoff {
 public:
  // The numbers are the PC policy's (runtime/patch/lobby_full_backoff_rules.h), one definition for both platforms.
  static constexpr std::uint64_t kFirstWaitMs = nevr_lobby_full_backoff::kBaseDelayMs;
  static constexpr std::uint64_t kMaxWaitMs = nevr_lobby_full_backoff::kMaxDelayMs;
  static constexpr std::uint32_t kGiveUpAfter = static_cast<std::uint32_t>(nevr_lobby_full_backoff::kGiveUpAfter);
  static constexpr std::uint64_t kCooldownMs = nevr_lobby_full_backoff::kGiveUpCooldownMs;

  enum class Verdict { kSend, kWait, kGiveUp };

  struct Decision {
    Verdict verdict = Verdict::kSend;
    std::uint64_t waitMs = 0;    // kWait and kGiveUp: milliseconds until the next send is allowed
    std::uint32_t refusals = 0;  // lobby-full refusals recorded for this lobby so far
  };

  // A join for `lobby` is about to go out. kSend: let the game's Join run. kWait: skip it this tick, the cooldown or
  // the wait after a refusal has not ended.
  Decision OnJoin(const LobbyKey& lobby, std::uint64_t nowMs) const {
    Decision d;
    const auto it = entries_.find(lobby);
    if (it == entries_.end()) return d;
    d.refusals = it->second.refusals;
    if (nowMs < it->second.nextAllowedMs) {
      d.verdict = Verdict::kWait;
      d.waitMs = it->second.nextAllowedMs - nowMs;
    }
    return d;
  }

  // The server refused a join for `lobby` with lobby full. The returned decision is what follows the refusal:
  // kWait with the wait before the next send, or kGiveUp on the refusal that starts the cooldown (waitMs is the cooldown).
  Decision OnFull(const LobbyKey& lobby, std::uint64_t nowMs) {
    Entry& e = entries_[lobby];
    ++e.refusals;
    Decision d;
    d.refusals = e.refusals;
    if (e.refusals >= kGiveUpAfter) {
      d.verdict = Verdict::kGiveUp;
      d.waitMs = kCooldownMs;
      e.nextAllowedMs = nowMs + kCooldownMs;
      e.refusals = 0;
      return d;
    }
    const unsigned shift = e.refusals - 1;
    std::uint64_t wait = kFirstWaitMs << shift;
    if (wait > kMaxWaitMs) wait = kMaxWaitMs;
    e.nextAllowedMs = nowMs + wait;
    d.verdict = Verdict::kWait;
    d.waitMs = wait;
    return d;
  }

  // The join succeeded: forget the lobby, so a later refusal starts again at the first wait.
  void OnSuccess(const LobbyKey& lobby) { entries_.erase(lobby); }

 private:
  struct Entry {
    std::uint32_t refusals = 0;
    std::uint64_t nextAllowedMs = 0;
  };
  std::map<LobbyKey, Entry> entries_;
};

}  // namespace quest_social::lobby_full

#pragma once
// The retry policy for a party-follow join the lobby refused as "lobby full" (#1011). The game queues
// CR15NetGame::JoinPartyLobby again every frame while the follow data still names the lobby, and nothing in the
// refusal clears that data (failure callback 0x14017d380, echovr.exe). This policy decides when the next join may
// go out. The hooks that apply it are in lobby_full_backoff.cpp.
//
// A refusal k (1-based) holds the next send for min(1000 ms * 2^(k-1), 30000 ms): 1, 2, 4, 8, 16, 30 s. The seventh
// refusal gives up and holds sends for a cooldown, after which the count starts again. Sends in the first 61 s of
// a refused follow are therefore 7 (at 0, 1, 3, 7, 15, 31, 61 s).

#include <algorithm>
#include <cstdint>

namespace nevr_lobby_full_backoff {

constexpr std::uint64_t kBaseDelayMs = 1000;
constexpr std::uint64_t kMaxDelayMs = 30000;
constexpr int kGiveUpAfter = 7;
constexpr std::uint64_t kGiveUpCooldownMs = 60000;

struct Refusal {
  int count;            // the refusal's number in this run of refusals, 1-based
  std::uint64_t delayMs;  // how long the next send is held
  bool gaveUp;          // true when this refusal ended the run and started the cooldown
};

class Policy {
 public:
  // A join may go out at nowMs. Before any refusal, and after a success, always true.
  bool MaySend(std::uint64_t nowMs) const { return nowMs >= nextSendMs_; }

  // The lobby refused a join with code 8 at nowMs.
  Refusal OnLobbyFull(std::uint64_t nowMs) {
    ++refusals_;
    if (refusals_ >= kGiveUpAfter) {
      const Refusal given{refusals_, kGiveUpCooldownMs, true};
      refusals_ = 0;
      nextSendMs_ = nowMs + kGiveUpCooldownMs;
      return given;
    }
    // refusals_ is 1 to 6 here, so the shift cannot overflow.
    const std::uint64_t delay = std::min(kBaseDelayMs << (refusals_ - 1), kMaxDelayMs);
    nextSendMs_ = nowMs + delay;
    return Refusal{refusals_, delay, false};
  }

  // The lobby accepted the join: the next refusal starts a new run.
  void OnSuccess() {
    refusals_ = 0;
    nextSendMs_ = 0;
  }

  int refusals() const { return refusals_; }

 private:
  int refusals_ = 0;
  std::uint64_t nextSendMs_ = 0;
};

}  // namespace nevr_lobby_full_backoff

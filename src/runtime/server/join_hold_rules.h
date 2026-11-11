#pragma once

// Join hold on the PC dedicated server (#1737 row 101f; docs/reference/join-hold.md): the pure decisions. The service's
// accept of an entrant is held while the session's level set is loading, and forwarded once it is enabled. No game
// image, no clock of its own (the caller passes milliseconds), no lock (join_hold.cpp holds one around the Gate).

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nevr_join_hold {

/// How long after the last level-load activity the gate waits before it opens (a dated figure: the Enabling lines
/// follow the state switch within this window in the one run read; docs/reference/join-hold.md "Live-run row owed").
constexpr std::uint64_t kSettleMs = 200;

/// network.join_hold_ms: 0 is off. A value from 1 to kMinHoldMs - 1 is raised to kMinHoldMs and one above kMaxHoldMs
/// is lowered to it, each with a problem text; the default is off (owner decision D4).
constexpr std::uint32_t kDefaultHoldMs = 0;
constexpr std::uint32_t kMinHoldMs = 500;
constexpr std::uint32_t kMaxHoldMs = 15000;

/// Parses network.join_hold_ms. Null or empty is the default. A text that is not a plain whole number is 0 (off) with
/// `problem` set; a number out of range is clamped with `problem` set.
inline std::uint32_t ParseJoinHoldMs(const char* text, std::string* problem) {
  if (problem != nullptr) problem->clear();
  if (text == nullptr || text[0] == '\0') return kDefaultHoldMs;
  std::uint64_t value = 0;
  bool over = false;
  for (const char* c = text; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9') {
      if (problem != nullptr) *problem = "not a whole number of milliseconds";
      return 0;
    }
    if (!over) {
      value = value * 10 + static_cast<std::uint64_t>(*c - '0');
      if (value > kMaxHoldMs) over = true;
    }
  }
  if (over) {
    if (problem != nullptr) *problem = "above the limit of " + std::to_string(kMaxHoldMs);
    return kMaxHoldMs;
  }
  if (value == 0) return 0;
  if (value < kMinHoldMs) {
    if (problem != nullptr) *problem = "below the limit of " + std::to_string(kMinHoldMs);
    return kMinHoldMs;
  }
  return static_cast<std::uint32_t>(value);
}

/// One held accept: the encoded bytes the server would have forwarded, how many entrants it names, when it arrived.
struct Held {
  std::vector<std::uint8_t> bytes;
  std::size_t entrants = 0;
  std::uint64_t arrivedMs = 0;
};

enum class ReleaseReason { kNone, kSettled, kTimeout };

inline const char* ReleaseReasonName(ReleaseReason r) {
  switch (r) {
    case ReleaseReason::kSettled: return "settled";
    case ReleaseReason::kTimeout: return "timeout";
    case ReleaseReason::kNone: break;
  }
  return "none";
}

struct Release {
  std::vector<Held> accepts;  // in arrival order
  ReleaseReason reason = ReleaseReason::kNone;
};

/// The hold's state. Closed while a session is armed and its levels are not seen enabled; open otherwise.
///
/// A session create arms it. A LoadLevel entry closes it (the set is not enabled) and a LoadLevel exit counts the load
/// done. An EnableGameSpaces return opens it only when no load is in flight and a load was seen since the arm, so an
/// EnableGameSpaces left over from the lobby cannot open it during the session's own load. It stays closed for
/// kSettleMs after the last activity.
class Gate {
 public:
  void Configure(std::uint32_t timeoutMs) { timeoutMs_ = timeoutMs; }
  bool Enabled() const { return timeoutMs_ != 0; }
  std::uint32_t TimeoutMs() const { return timeoutMs_; }

  /// A session was created: arm and expect a load. A load already in flight stays counted (and counts as seen).
  /// Returns the accepts of an earlier session that were still held; they are dropped.
  std::size_t Arm(std::uint64_t nowMs) {
    const std::size_t dropped = queue_.size();
    queue_.clear();
    armed_ = true;
    loadSeen_ = pending_ > 0;
    enabledSeen_ = false;
    armedMs_ = nowMs;
    lastActivityMs_ = nowMs;
    return dropped;
  }

  /// The session ended or the server is shutting down: disarm and drop what is held. Returns how many were dropped.
  std::size_t Clear() {
    const std::size_t dropped = queue_.size();
    queue_.clear();
    armed_ = false;
    loadSeen_ = false;
    enabledSeen_ = false;
    return dropped;
  }

  void OnLoadBegin(std::uint64_t nowMs) {
    ++pending_;
    loadSeen_ = true;
    enabledSeen_ = false;
    lastActivityMs_ = nowMs;
  }

  void OnLoadEnd(std::uint64_t nowMs) {
    if (pending_ > 0) --pending_;
    lastActivityMs_ = nowMs;
  }

  /// EnableGameSpaces returned. True when this marks the session's level set enabled.
  bool OnSetEnabled(std::uint64_t nowMs) {
    if (!armed_ || pending_ != 0 || !loadSeen_) return false;
    enabledSeen_ = true;
    lastActivityMs_ = nowMs;
    return true;
  }

  /// Open: accepts are forwarded. Not armed is open.
  bool Ready(std::uint64_t nowMs) const {
    if (!armed_) return true;
    return enabledSeen_ && pending_ == 0 && nowMs - lastActivityMs_ >= kSettleMs;
  }

  /// An accept arrived. False: forward it now (the queue is empty and the gate is open). True: it was queued; once
  /// anything is queued every later accept queues behind it, so none overtakes another.
  bool Admit(Held&& accept) {
    if (queue_.empty() && Ready(accept.arrivedMs)) return false;
    queue_.push_back(std::move(accept));
    return true;
  }

  /// What is due now: everything, in order, when the gate is open (settled) or when the front waited the timeout (the
  /// timeout fails open: the load count is reset and the gate disarms, which also ends a count stuck by a load that
  /// never returned).
  Release TakeDue(std::uint64_t nowMs) {
    Release out;
    if (queue_.empty()) return out;
    if (Ready(nowMs)) {
      out.reason = ReleaseReason::kSettled;
    } else if (nowMs - queue_.front().arrivedMs >= timeoutMs_) {
      out.reason = ReleaseReason::kTimeout;
      pending_ = 0;
      armed_ = false;
      loadSeen_ = false;
      enabledSeen_ = false;
    } else {
      return out;
    }
    out.accepts = std::move(queue_);
    queue_.clear();
    return out;
  }

  bool Armed() const { return armed_; }
  std::size_t Pending() const { return pending_; }
  std::size_t QueueSize() const { return queue_.size(); }
  std::uint64_t ArmedMs() const { return armedMs_; }

 private:
  std::uint32_t timeoutMs_ = 0;
  bool armed_ = false;
  bool loadSeen_ = false;
  bool enabledSeen_ = false;
  std::size_t pending_ = 0;
  std::uint64_t armedMs_ = 0;
  std::uint64_t lastActivityMs_ = 0;
  std::vector<Held> queue_;
};

/// A line the install writes (the server_bots form): the text and whether it is a Warning.
struct Line {
  bool warning = false;
  std::string text;
};

struct InstallPlan {
  std::uint32_t timeoutMs = 0;  // 0: no hook is installed
  std::vector<Line> lines;
};

/// What the install does for the config text: the lines to log and the timeout to arm (0 = leave the game alone).
/// `text` is network.join_hold_ms as written (null when absent).
inline InstallPlan PlanInstall(bool isServer, const char* text) {
  InstallPlan out;
  if (!isServer) {
    out.lines.push_back({false, "[NEVR.GAMESERVER] join hold off reason=client (accepts are forwarded at once)"});
    return out;
  }
  std::string problem;
  const std::uint32_t ms = ParseJoinHoldMs(text, &problem);
  if (!problem.empty()) {
    out.lines.push_back({true, "[NEVR.GAMESERVER] network.join_hold_ms is " + problem + "; using " + std::to_string(ms) +
                                   (ms == 0 ? " (off)" : "")});
  }
  if (ms == 0) {
    const bool absent = text == nullptr || text[0] == '\0';
    out.lines.push_back({false, std::string("[NEVR.GAMESERVER] join hold off reason=") +
                                    (!problem.empty() ? "config_invalid" : (absent ? "absent" : "config_0")) +
                                    " (accepts are forwarded at once)"});
    return out;
  }
  out.timeoutMs = ms;
  return out;
}

}  // namespace nevr_join_hold

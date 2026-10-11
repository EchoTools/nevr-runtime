#include "runtime/compat/self_check.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>

namespace nevr_self_check {
namespace {

struct Check {
  std::string name;
  std::string expected;
  Probe probe = nullptr;
  std::size_t reported = 0;  // results this check has produced this session (the cap counts them)
};

// A result waiting to be sent. The string is built when it is sent, so it carries the user the service named
// at login even when the result happened before it.
struct Pending {
  std::string check;
  std::string expected;
  std::string observed;
  bool pass = false;
  uint64_t seq = 0;
  int suppressedAfter = -1;  // >= 0 on the "capped" result
  int dropped = -1;          // >= 0 on the note that the queue lost results
};

struct State {
  std::mutex mutex;
  bool enabled = false;
  bool loggedIn = false;
  nevr_evr_codec::UserId user;
  Sender sender = nullptr;
  LogSink logSink = nullptr;
  std::string build;
  std::vector<Check> checks;
  std::deque<Pending> queue;      // oldest first
  std::size_t dropped = 0;        // results the bounded queue lost since the last frame
  uint64_t seq = 0;
};

// Never destroyed: the unit is used from threads that outlive static destruction, and the Quest sentinel may
// not register an atexit destructor (tools/check_quest_static_init.sh).
State& S() {
  static State* const state = new State();
  return *state;
}

std::string Truncate(std::string_view text) {
  return std::string(text.substr(0, std::min(text.size(), kMaxTextBytes)));
}

std::string Dump(const nlohmann::json& j) {
  // A text with bytes that are not UTF-8 must not make the dump throw: replace them.
  return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// Builds the result string from what was recorded and the user known now.
std::string BuildResult(const Pending& p, bool loggedIn, const nevr_evr_codec::UserId& user, const std::string& build) {
  nlohmann::json j;
  j["message"] = "nevr_self_check";
  j["message_type"] = "NEVR_SELF_CHECK";
  if (loggedIn) {
    j["userid"] = std::string(nevr_evr_codec::PlatformPrefix(user.platformCode)) + "-" + std::to_string(user.accountId);
  }
  j["check"] = p.check;
  j["pass"] = p.pass;
  j["expected"] = p.expected;
  j["observed"] = p.observed;
  j["seq"] = p.seq;
  if (!build.empty()) j["build"] = build;
  if (p.suppressedAfter >= 0) j["suppressed_after"] = p.suppressedAfter;
  if (p.dropped >= 0) j["dropped"] = p.dropped;
  return Dump(j);
}

// Caller holds the mutex.
void Enqueue(State& s, Pending result) {
  if (s.queue.size() >= kMaxQueued) {
    s.queue.pop_front();
    ++s.dropped;
  }
  s.queue.push_back(std::move(result));
}

}  // namespace

void SetEnabled(bool enabled) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.enabled = enabled;
}

bool Enabled() {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.enabled;
}

void SetSender(Sender sender) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.sender = sender;
}

void SetLogSink(LogSink sink) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.logSink = sink;
}

void SetBuild(std::string_view build) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.build = Truncate(build);
}

void SetLoggedIn(bool loggedIn, const nevr_evr_codec::UserId& user) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.loggedIn = loggedIn;
  if (loggedIn) s.user = user;
  if (!loggedIn) {
    // A new session starts the per-check caps over; what is already queued stays queued.
    for (Check& c : s.checks) c.reported = 0;
  }
}

CheckId Register(const CheckSpec& spec) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (spec.name == nullptr || s.checks.size() >= kMaxChecks) return -1;
  Check c;
  c.name = spec.name;
  c.expected = spec.expected != nullptr ? spec.expected : "";
  c.probe = spec.probe;
  s.checks.push_back(std::move(c));
  return static_cast<CheckId>(s.checks.size() - 1);
}

void Report(CheckId id, std::string_view observed, bool pass) {
  State& s = S();
  LogSink sink = nullptr;
  LogRecord record;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled || id < 0 || static_cast<std::size_t>(id) >= s.checks.size()) return;
    Check& c = s.checks[static_cast<std::size_t>(id)];
    if (c.reported > kMaxResultsPerCheck) return;  // already said it was capped
    Pending p;
    p.check = c.name;
    p.expected = Truncate(c.expected);
    p.pass = pass;
    p.seq = ++s.seq;
    if (c.reported == kMaxResultsPerCheck) {
      ++c.reported;
      p.observed = "capped";
      p.suppressedAfter = static_cast<int>(kMaxResultsPerCheck);
    } else {
      ++c.reported;
      p.observed = Truncate(observed);
    }
    record.name = p.check;
    record.expected = p.expected;
    record.observed = p.observed;
    record.pass = pass;
    sink = s.logSink;
    Enqueue(s, std::move(p));
  }
  if (sink != nullptr) sink(record);
}

void Flush() {
  State& s = S();
  // Probes first, outside the lock (a probe reads atomics; Report takes the lock itself).
  std::vector<std::pair<CheckId, Probe>> probes;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.enabled) return;
    for (std::size_t i = 0; i < s.checks.size(); ++i) {
      if (s.checks[i].probe != nullptr) probes.emplace_back(static_cast<CheckId>(i), s.checks[i].probe);
    }
  }
  for (const auto& entry : probes) {
    Observation o;
    if (entry.second(&o)) Report(entry.first, o.observed, o.pass);
  }

  for (;;) {
    std::vector<Pending> taken;
    std::vector<std::string> batch;
    nevr_evr_codec::UserId user;
    Sender sender = nullptr;
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      if (!s.loggedIn || s.sender == nullptr || (s.queue.empty() && s.dropped == 0)) return;
      sender = s.sender;
      user = s.user;
      while (!s.queue.empty() && taken.size() < kMaxStringsPerFrame) {
        taken.push_back(std::move(s.queue.front()));
        s.queue.pop_front();
      }
      if (s.dropped != 0 && taken.size() < kMaxStringsPerFrame) {
        Pending note;
        note.check = "self_check";
        note.expected = "no results lost before login";
        note.observed = "queue full";
        note.pass = true;
        note.seq = ++s.seq;
        note.dropped = static_cast<int>(s.dropped);
        s.dropped = 0;
        taken.push_back(std::move(note));
      }
      for (const Pending& p : taken) batch.push_back(BuildResult(p, true, user, s.build));
    }
    if (batch.empty()) return;
    const std::string frame = nevr_evr_codec::BuildRemoteLogSet(user, kRemoteLogLevelInfo, batch);
    if (!sender(frame)) {
      // Keep them, in order, ahead of anything queued since; the next Flush tries again.
      std::lock_guard<std::mutex> lock(s.mutex);
      for (auto it = taken.rbegin(); it != taken.rend(); ++it) {
        if (s.queue.size() >= kMaxQueued) {
          ++s.dropped;
        } else {
          s.queue.push_front(std::move(*it));
        }
      }
      return;
    }
  }
}

void ResetForTest() {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.enabled = false;
  s.loggedIn = false;
  s.user = nevr_evr_codec::UserId();
  s.sender = nullptr;
  s.logSink = nullptr;
  s.build.clear();
  s.checks.clear();
  s.queue.clear();
  s.dropped = 0;
  s.seq = 0;
}

}  // namespace nevr_self_check

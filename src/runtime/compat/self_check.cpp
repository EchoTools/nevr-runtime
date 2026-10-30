#include "runtime/compat/self_check.h"

#include <algorithm>
#include <atomic>
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
  int sessionDropped = -1;   // >= 0 on the note that a session ended with results unsent
  bool hasUser = false;      // the user the result belongs to, when it was known as it happened
  nevr_evr_codec::UserId user;
};

struct State {
  std::mutex mutex;
  std::atomic<bool> enabled{false};  // read without the mutex, so a build with the unit off pays nothing
  bool loggedIn = false;
  nevr_evr_codec::UserId user;
  Sender sender = nullptr;
  LogSink logSink = nullptr;
  std::string build;
  std::vector<Check> checks;
  std::deque<Pending> queue;      // oldest first
  std::vector<SessionReset> resets;  // functions that clear a check's per-session state
  std::size_t dropped = 0;        // results the bounded queue lost since the last frame
  std::size_t sessionDropped = 0;  // results unsent when a session ended: dropped, never stored
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
  // The user the result belonged to when it happened, else the user the service named by the time it is sent.
  const nevr_evr_codec::UserId& who = p.hasUser ? p.user : user;
  if (p.hasUser || loggedIn) {
    j["userid"] = std::string(nevr_evr_codec::PlatformPrefix(who.platformCode)) + "-" + std::to_string(who.accountId);
  }
  j["check"] = p.check;
  j["pass"] = p.pass;
  j["expected"] = p.expected;
  j["observed"] = p.observed;
  j["seq"] = p.seq;
  if (!build.empty()) j["build"] = build;
  if (p.suppressedAfter >= 0) j["suppressed_after"] = p.suppressedAfter;
  if (p.dropped >= 0) j["dropped"] = p.dropped;
  if (p.sessionDropped >= 0) j["dropped_at_session_end"] = p.sessionDropped;
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

void SetEnabled(bool enabled) { S().enabled.store(enabled, std::memory_order_release); }

bool Enabled() { return S().enabled.load(std::memory_order_acquire); }

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

void RegisterSessionReset(SessionReset reset) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (reset == nullptr) return;
  for (SessionReset r : s.resets) {
    if (r == reset) return;
  }
  s.resets.push_back(reset);
}

void SetLoggedIn(bool loggedIn, const nevr_evr_codec::UserId& user) {
  State& s = S();
  std::vector<SessionReset> resets;
  bool endingSession = false;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    resets = s.resets;
    endingSession = s.loggedIn;
  }
  // The ending session's own state is reported (and attributed) before the boundary moves.
  for (SessionReset reset : resets) reset();
  // One delivery attempt while the session still stands; what it cannot send is dropped below.
  if (endingSession) Flush();
  LogSink sink = nullptr;
  LogRecord record;
  std::unique_lock<std::mutex> lock(s.mutex);
  // A session that ends with results unsent drops them and counts them: nothing is stored for a later
  // session. Results queued before the first login belong to no session yet and stay.
  if (s.loggedIn && !s.queue.empty()) {
    s.sessionDropped += s.queue.size();
    record.name = "self_check";
    record.expected = "no results unsent when a session ends";
    record.observed = std::to_string(s.queue.size()) + " dropped";
    record.pass = false;
    sink = s.logSink;
    s.queue.clear();
  }
  s.loggedIn = loggedIn;
  if (loggedIn) s.user = user;
  // A new session starts the per-check caps over.
  for (Check& c : s.checks) c.reported = 0;
  lock.unlock();
  if (sink != nullptr) sink(record);
}

CheckId Register(const CheckSpec& spec) {
  State& s = S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (spec.name == nullptr) return -1;
  for (std::size_t i = 0; i < s.checks.size(); ++i) {
    if (s.checks[i].name == spec.name) return static_cast<CheckId>(i);
  }
  if (s.checks.size() >= kMaxChecks) return -1;
  Check c;
  c.name = spec.name;
  c.expected = spec.expected != nullptr ? spec.expected : "";
  c.probe = spec.probe;
  s.checks.push_back(std::move(c));
  return static_cast<CheckId>(s.checks.size() - 1);
}

void Report(CheckId id, std::string_view observed, bool pass) {
  State& s = S();
  if (!s.enabled.load(std::memory_order_acquire)) return;
  LogSink sink = nullptr;
  LogRecord record;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (id < 0 || static_cast<std::size_t>(id) >= s.checks.size()) return;
    Check& c = s.checks[static_cast<std::size_t>(id)];
    if (c.reported > kMaxResultsPerCheck) return;  // already said it was capped
    Pending p;
    p.check = c.name;
    p.expected = Truncate(c.expected);
    p.pass = pass;
    p.seq = ++s.seq;
    if (s.loggedIn) {
      p.hasUser = true;
      p.user = s.user;
    }
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
  if (!s.enabled.load(std::memory_order_acquire)) return;
  // Probes first, outside the lock (a probe reads atomics; Report takes the lock itself).
  std::vector<std::pair<CheckId, Probe>> probes;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
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
      if (!s.loggedIn || s.sender == nullptr || (s.queue.empty() && s.dropped == 0 && s.sessionDropped == 0)) return;
      sender = s.sender;
      user = s.user;
      while (!s.queue.empty() && taken.size() < kMaxStringsPerFrame) {
        taken.push_back(std::move(s.queue.front()));
        s.queue.pop_front();
      }
      if ((s.dropped != 0 || s.sessionDropped != 0) && taken.size() < kMaxStringsPerFrame) {
        Pending note;
        note.check = "self_check";
        note.expected = "no results lost";
        note.observed = s.sessionDropped != 0 ? "unsent when a session ended" : "queue full";
        note.pass = true;
        note.seq = ++s.seq;
        if (s.dropped != 0) note.dropped = static_cast<int>(s.dropped);
        if (s.sessionDropped != 0) note.sessionDropped = static_cast<int>(s.sessionDropped);
        s.dropped = 0;
        s.sessionDropped = 0;
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
  s.enabled.store(false);
  s.loggedIn = false;
  s.user = nevr_evr_codec::UserId();
  s.sender = nullptr;
  s.logSink = nullptr;
  s.build.clear();
  s.resets.clear();
  s.checks.clear();
  s.queue.clear();
  s.dropped = 0;
  s.sessionDropped = 0;
  s.seq = 0;
}

}  // namespace nevr_self_check

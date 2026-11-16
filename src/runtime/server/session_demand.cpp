#include "runtime/server/session_demand.h"

#include <winsock2.h>  // before windows.h
#include <windows.h>

#include <condition_variable>
#include <cstdio>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "core/logging.h"
#include "runtime/lifecycle/cdn_start.h"
#include "runtime/server/game_type_table.h"
#include "runtime/server/session_demand_rules.h"

namespace nevr_session_demand {
namespace {

// The level's text in the start's log line: "none", or "0x" and 16 hex digits and a NUL (19 bytes). Chosen, not a game record size; 32
// leaves room for it and keeps the line's buffer on the stack.
constexpr std::size_t kLevelTextBytes = 32;

// What the worker is asked to resolve: one hold's ticket and the session's demand.
struct Job {
  std::uint64_t ticket = 0;
  std::uint64_t level = 0;
  std::string gameType;
};

// What the worker answers. Stored under the mutex; Poll (game thread) reads it.
struct Answer {
  std::uint64_t ticket = 0;
  bool ok = false;
  std::string reason;
};

// Game-thread state: the hold, the held frame and its accepts, the session they belong to.
Hold g_hold{kSessionDemandHoldMs};
std::vector<std::uint8_t> g_frame;
std::vector<QueuedAccept> g_accepts;
std::string g_sessionId;
std::uint64_t g_begunMs = 0;

// The mailbox, shared with the worker. One slot each way; a newer job replaces a job not yet taken.
std::mutex g_mutex;
std::condition_variable g_cv;
std::optional<Job> g_job;
std::optional<Answer> g_answer;
std::uint64_t g_activeTicket = 0;  // the ticket whose resolve still wants its result; 0 when none
bool g_cancel = false;
bool g_workerStarted = false;

std::uint64_t NowMs() { return static_cast<std::uint64_t>(GetTickCount64()); }

// The resolve's keep_going: true only while the job's ticket is the active one and the server is not shutting down.
bool KeepGoing(std::uint64_t ticket) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return !g_cancel && g_activeTicket == ticket;
}

// The worker: one resolve at a time. It stores its answer and never calls into the game.
void WorkerLoop() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(g_mutex);
      g_cv.wait(lock, [] { return g_job.has_value() || g_cancel; });
      if (g_cancel) return;
      job = *g_job;
      g_job.reset();
    }
    bool ok = false;
    std::string reason;
    try {
      const std::uint64_t ticket = job.ticket;
      const nevr_cdn_start::SessionDemandOutcome outcome = nevr_cdn_start::ResolveDemandForSession(
          job.level, job.gameType, [ticket] { return KeepGoing(ticket); });
      ok = outcome.ok;
      reason = outcome.reason;
    } catch (const std::exception&) {
      ok = false;
      reason = "exception";
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_cancel) g_answer = Answer{job.ticket, ok, reason};
  }
}

}  // namespace

bool HoldSessionStart(const std::uint8_t* frame, std::size_t size, const std::string& sessionId) {
  const std::optional<nevr_game_type_table::DemandSettings> settings =
      nevr_game_type_table::DemandSettingsOfSessionStart(frame, size);
  if (!settings) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] session demand skipped session=%s reason=unreadable_frame consequence=\"the start is forwarded "
        "without a content check\"",
        sessionId.c_str());
    return false;
  }
  const std::uint64_t now = NowMs();
  const std::uint64_t ticket = g_hold.Begin(now);
  if (g_hold.Replaced()) {
    // N3: a second start while one is held replaces it. Logged before the held state is overwritten, so the old session and the
    // accepts it drops are still named here; the old start is never forwarded.
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] session demand replaced session=%s by=%s dropped_accepts=%zu consequence=\"the earlier held start is "
        "never forwarded to the game, and its queued accepts are dropped\"",
        g_sessionId.c_str(), sessionId.c_str(), g_accepts.size());
  }
  g_frame.assign(frame, frame + size);
  g_accepts.clear();
  g_sessionId = sessionId;
  g_begunMs = now;
  const std::string gameType = settings->gameType.empty() ? "none" : settings->gameType;
  char level[kLevelTextBytes];
  if (settings->level == 0) {
    std::snprintf(level, sizeof(level), "none");
  } else {
    std::snprintf(level, sizeof(level), "0x%llx", static_cast<unsigned long long>(settings->level));
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.GAMESERVER] session demand started session=%s level=%s game_type=%s max_wait_ms=%llu consequence=\"the session's "
      "start is held until its content is mounted\"",
      sessionId.c_str(), level, gameType.c_str(), static_cast<unsigned long long>(kSessionDemandHoldMs));
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_activeTicket = ticket;
    g_answer.reset();
    g_job = Job{ticket, settings->level, settings->gameType};
    if (!g_workerStarted) {
      g_workerStarted = true;
      std::thread(WorkerLoop).detach();
    }
  }
  g_cv.notify_one();
  return true;
}

bool Active() { return g_hold.Holding(); }

bool HoldAccept(const void* encoded, std::size_t size, std::size_t entrants, const char* sessionId) {
  if (!g_hold.Admit()) return false;
  const auto* bytes = static_cast<const std::uint8_t*>(encoded);
  g_accepts.push_back(QueuedAccept{std::vector<std::uint8_t>(bytes, bytes + size), entrants, sessionId != nullptr ? sessionId : ""});
  return true;
}

Due Poll() {
  Due due;
  std::optional<Answer> answer;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    answer = g_answer;
    g_answer.reset();
  }
  const std::uint64_t now = NowMs();
  Step step = Step::kNone;
  bool fromAnswer = false;
  if (answer) {
    step = g_hold.Finish(answer->ticket, answer->ok);
    fromAnswer = step != Step::kNone;
  }
  if (step == Step::kNone) step = g_hold.Poll(now);

  if (step == Step::kForward) {
    due.action = DueAction::kRelease;
    due.frame = std::move(g_frame);
    due.accepts = std::move(g_accepts);
    g_frame.clear();
    g_accepts.clear();
    Log(EchoVR::LogLevel::Info,
        "[NEVR.GAMESERVER] session demand released session=%s after_ms=%llu held_accepts=%zu consequence=\"the session's start "
        "was forwarded to the game\"",
        g_sessionId.c_str(), static_cast<unsigned long long>(now - g_begunMs), due.accepts.size());
  } else if (step == Step::kFail) {
    due.action = DueAction::kFail;
    due.reason = fromAnswer ? answer->reason : std::string("deadline");
    const std::size_t dropped = g_accepts.size();
    due.accepts = std::move(g_accepts);
    g_frame.clear();
    g_accepts.clear();
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] session demand failed session=%s reason=%s dropped_accepts=%zu consequence=\"the session ends and the "
        "server stays up\"",
        g_sessionId.c_str(), due.reason.c_str(), dropped);
  }
  return due;
}

void OnSessionEnd(const char* cause, const char* sessionId) {
  // F1 (PLAN-v5 R6-a-2b review): only the held session's own end drops its start. An end that names another session (a late
  // CODE_ENDED for an earlier one, a game-side end of another) changes nothing here: the hold, its answer and its resolve stand.
  const std::string ended = sessionId != nullptr ? sessionId : "";
  if (!g_hold.Holding() || !EndsHeldSession(g_sessionId, ended)) return;
  const std::uint64_t ticket = g_hold.Ticket();
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_activeTicket = 0;
    g_answer.reset();
  }
  g_hold.Clear();
  // A Warning: the session's start was held and is now never forwarded, so the session is not started. The operator sees it here.
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.GAMESERVER] session demand dropped session=%s ticket=%llu cause=%s dropped_accepts=%zu consequence=\"the held start "
      "is never forwarded to the game, so this session is not started\"",
      g_sessionId.c_str(), static_cast<unsigned long long>(ticket), cause != nullptr ? cause : "unknown", g_accepts.size());
  g_frame.clear();
  g_accepts.clear();
}

void Shutdown() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cancel = true;
  }
  g_cv.notify_all();
}

}  // namespace nevr_session_demand

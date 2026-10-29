#pragma once
// #1156 (PR-D slice D1), #906, docs/adr/0017-invite-codes-for-the-create-game-wizard.md: the worker that posts the wizard's
// planned request to the game service and keeps the answer for the menu.
//
// One thread. The game's thread calls Enqueue(plan) (a lock and a push, never a socket), the worker takes a request, reads the
// token at that moment, writes the request line, posts through the Transport, writes the answer line and puts an Answer on a
// bounded list the game's thread empties (TakeAnswers, one call per frame) or a test waits on. Both lists are bounded and a
// refusal or a loss is a line: nothing is dropped silently.
//
// Pure: std only, no game, no socket, no clock of its own. The platform hands in the Transport (libcurl in the runtime), the
// token (nevr_token_auth), the log sink and the clock; src/runtime/tests/test_invite_worker.cpp runs the real thread against
// a fake transport.
//
// What a line may carry (docs/standards/logging.md, [NEVR.INVITE]): op, code_len, a salted fingerprint, the status, the
// latency and the result token. Never the code, the token, the request body or the answer's text. The answer's body is given
// back to the caller only when the call succeeded (ParseCreated / ParseResolved read it), and is never logged.
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "runtime/patch/invite_wizard.h"

namespace nevr_invite {

/// Requests waiting behind the one in flight. A player makes one request at a time with a click; four is more than any
/// play needs and a bound on what a stuck service can pile up.
inline constexpr std::size_t kMaxQueued = 4;
/// Answers nobody has taken yet. When the list is full the oldest goes, with a line.
inline constexpr std::size_t kMaxAnswers = 8;
static_assert(kMaxAnswers > kMaxQueued, "every queued request must still have room for its answer");

enum class LogLevel { Info, Warning };

struct HttpAnswer {
  int status = 0;  // 0: no answer at all (the connection failed, timed out, or the body was refused)
  std::string body;
};

/// One POST of a JSON body to the RPC `rpc` with `bearer`. Called on the worker thread only; it may block, up to its own
/// timeout. It may throw; the worker treats that as no answer.
class Transport {
 public:
  virtual ~Transport() = default;
  virtual HttpAnswer Post(std::string_view rpc, std::string_view body, std::string_view bearer) = 0;
};

struct Answer {
  Op op = Op::Create;
  bool sent = false;  // false: nothing went out (no token)
  int status = 0;
  Result result = Result::Unavailable;
  bool via_status = false;  // a refusal the service did not name: the HTTP status alone decided the result
  std::string refusal_message;  // via_status only: for the scenario endpoint, never logged or shown
  std::string body;  // a success only
};

enum class Submit { Queued, NoRequest, NotConfigured, Busy, Stopped };

struct WorkerParts {
  Transport* transport = nullptr;
  std::function<std::string()> token;                         // the current Bearer token, "" when none; any thread
  std::function<void(LogLevel, const std::string&)> log;      // any thread
  std::function<std::int64_t()> clock_ms;                     // monotonic milliseconds; the worker thread
  std::string salt;                                           // per run: see CodeFingerprint
  bool configured = false;                                    // the config.yaml key (N133); false: the worker never starts
};

class Worker {
 public:
  explicit Worker(WorkerParts parts) : parts_(std::move(parts)) {
    if (parts_.configured && parts_.transport != nullptr) thread_ = std::thread([this] { Loop(); });
  }
  ~Worker() { Stop(); }
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;

  /// Hands the plan's request to the worker. Never blocks on the network. A refusal is logged and returned.
  Submit Enqueue(const Plan& plan) {
    if (!plan.request.has_value()) return Submit::NoRequest;
    if (!parts_.configured || parts_.transport == nullptr) {
      Say(LogLevel::Info, NotSentLine(Skip::NotConfigured, plan.code_len));
      return Submit::NotConfigured;
    }
    Job job;
    job.plan = plan;
    job.fingerprint = plan.op == Op::Resolve ? CodeFingerprint(plan.code, parts_.salt) : std::string();
    Skip refused = Skip::None;
    {
      std::lock_guard<std::mutex> lock(m_);
      if (stop_) {
        refused = Skip::Stopped;
      } else if (jobs_.size() >= kMaxQueued) {
        refused = Skip::Busy;
      } else {
        jobs_.push_back(std::move(job));
      }
    }
    if (refused != Skip::None) {
      Say(LogLevel::Info, NotSentLine(refused, plan.code_len));
      return refused == Skip::Stopped ? Submit::Stopped : Submit::Busy;
    }
    cv_.notify_all();
    return Submit::Queued;
  }

  /// The answers that have arrived, oldest first; each is handed over once. Non-blocking.
  std::vector<Answer> TakeAnswers() {
    std::lock_guard<std::mutex> lock(m_);
    return DrainLocked();
  }

  /// As TakeAnswers, but waits up to `timeout` for the first answer.
  std::vector<Answer> WaitForAnswers(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_);
    answered_.wait_for(lock, timeout, [&] { return !answers_.empty(); });
    return DrainLocked();
  }

  /// Waits for the request in flight, drops the queued ones with a line and joins the thread. One caller; a second call
  /// does nothing.
  void Stop() {
    std::size_t dropped = 0;
    {
      std::lock_guard<std::mutex> lock(m_);
      if (!stop_) {
        stop_ = true;
        dropped = jobs_.size();  // the loop sees stop_ before it takes another, so these are never sent
      }
    }
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.join();
      if (dropped > 0) Say(LogLevel::Info, "[NEVR.INVITE] worker stopped dropped=" + std::to_string(dropped));
    }
  }

 private:
  struct Job {
    Plan plan;
    std::string fingerprint;
  };

  void Say(LogLevel level, const std::string& line) const {
    if (parts_.log) parts_.log(level, line);
  }

  std::vector<Answer> DrainLocked() {
    std::vector<Answer> out(std::make_move_iterator(answers_.begin()), std::make_move_iterator(answers_.end()));
    answers_.clear();
    return out;
  }

  void Loop() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
        if (stop_) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      Answer answer = Run(job);
      Deliver(std::move(answer));
    }
  }

  Answer Run(const Job& job) {
    Answer answer;
    answer.op = job.plan.op;
    const std::string token = parts_.token ? parts_.token() : std::string();
    if (token.empty()) {
      Say(LogLevel::Info, NotSentLine(Skip::NoToken, job.plan.code_len));
      return answer;  // sent=false, Unavailable
    }
    answer.sent = true;
    Say(LogLevel::Info, SentLine(job.plan, job.fingerprint));
    const std::int64_t started = parts_.clock_ms();
    HttpAnswer http;
    try {
      http = parts_.transport->Post(job.plan.request->rpc, job.plan.request->body, token);
    } catch (const std::exception&) {
      http = HttpAnswer{};  // no answer at all; the line says status=0
    }
    const long latency = static_cast<long>(parts_.clock_ms() - started);
    answer.status = http.status;
    answer.result = ResultFromAnswer(http.status, http.body);
    answer.via_status = IsFailure(answer.result) && http.status > 0 && !AnswerNamesResult(http.status, http.body);
    if (answer.via_status) answer.refusal_message = RefusalMessage(http.body);
    if (answer.result == Result::Ok) answer.body = std::move(http.body);
    Say(IsFailure(answer.result) ? LogLevel::Warning : LogLevel::Info,
        AnswerLine(job.plan.op, answer.status, latency, answer.result, answer.via_status));
    return answer;
  }

  void Deliver(Answer answer) {
    bool dropped = false;
    {
      std::lock_guard<std::mutex> lock(m_);
      if (answers_.size() >= kMaxAnswers) {
        answers_.pop_front();
        dropped = true;
      }
      answers_.push_back(std::move(answer));
    }
    answered_.notify_all();
    if (dropped) Say(LogLevel::Warning, "[NEVR.INVITE] answers dropped reason=not_taken kept=" + std::to_string(kMaxAnswers));
  }

  WorkerParts parts_;
  std::mutex m_;
  std::condition_variable cv_;        // a job arrived, or stop
  std::condition_variable answered_;  // an answer arrived
  std::deque<Job> jobs_;
  std::deque<Answer> answers_;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace nevr_invite

#include "runtime/patch/invite_service.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "runtime/patch/invite_answer_text.h"
#include "runtime/patch/invite_transport.h"
#include "runtime/patch/invite_match_api.h"

namespace nevr_invite_service {

namespace {

// Every object with a non-trivial destructor is a function-local static, not a namespace-scope object: a namespace-scope
// one makes the compiler emit a load-time _GLOBAL__sub_I_* function that registers its destructor, and the Quest
// sentinel's static-init check rejects that (tools/check_quest_static_init.sh). The statics are built on first use.
std::mutex& ServiceMutex() {
  static std::mutex mutex;
  return mutex;
}
std::unique_ptr<nevr_invite::Transport>& TransportSlot() {
  static std::unique_ptr<nevr_invite::Transport> transport;
  return transport;
}
std::unique_ptr<nevr_invite::Worker>& WorkerSlot() {
  static std::unique_ptr<nevr_invite::Worker> worker;
  return worker;
}
bool g_started = false;
bool g_shutdown = false;  // Shutdown ran and no Start since
std::atomic<bool> g_active{false};  // on and not shut down; read without the lock by the per-frame applier
std::atomic<bool> g_answers_pending{false};  // an answer waits in the worker (AnswersPending); read without the lock
std::atomic<std::uint64_t> g_lock_takes{0};  // every take of the service mutex, counted for the host tests (ServiceLockTakesForTest)

// The sink is a function-local static (see ServiceMutex above for why no namespace-scope object has a destructor).
std::function<void(nevr_invite::LogLevel, const std::string&)>& LogSink() {
  static std::function<void(nevr_invite::LogLevel, const std::string&)> sink;
  return sink;
}

void Say(nevr_invite::LogLevel level, const std::string& line) {
  if (LogSink()) LogSink()(level, line);
}

// Every take of the service mutex goes through here, so the count is the number of times the service lock was taken.
std::lock_guard<std::mutex> LockService() {
  g_lock_takes.fetch_add(1, std::memory_order_relaxed);
  return std::lock_guard<std::mutex>(ServiceMutex());
}

}  // namespace

void Start(Settings settings) {
  auto lock = LockService();
  LogSink() = settings.log;
  if (g_started) {
    Say(nevr_invite::LogLevel::Warning, "[NEVR.INVITE] service start ignored reason=already_started");
    return;
  }
  g_started = true;
  g_shutdown = false;

  const std::string source = std::string(" source=") + nevr_invite::SourceName(settings.invite_source);
  const char* off = nullptr;
  nevr_invite::LogLevel offLevel = nevr_invite::LogLevel::Info;
  if (settings.invite_codes == nevr_invite::Switch::Unrecognised) {
    off = "unrecognised_value";
    offLevel = nevr_invite::LogLevel::Warning;
  } else if (settings.invite_codes == nevr_invite::Switch::Off) {
    // Off from config.yaml is a player's choice; off with no config behind it (a host whose default is off) is not configured.
    off = settings.invite_source == nevr_invite::Source::Config ? "turned_off" : "not_configured";
  } else if (settings.hooks_refused) {
    // An observer this service serves did not install. That observer's own Warning carries the address and the bytes (Rule 6).
    off = "hook_prologue_mismatch";
    offLevel = nevr_invite::LogLevel::Warning;
  } else if (settings.http_uri.empty()) {
    off = "no_service_address";
    offLevel = nevr_invite::LogLevel::Warning;
  } else {
    // The token goes to this address, so the address is checked now and the feature stays off when it may not receive it.
    const nevr_invite::Endpoint probe = nevr_invite::BuildRpcUrl(settings.http_uri, "invite/resolve");
    if (probe.check != nevr_invite::UrlCheck::Ok) {
      Say(nevr_invite::LogLevel::Warning,
          std::string("[NEVR.INVITE] feature off reason=service_address_refused detail=") + nevr_invite::UrlCheckName(probe.check) +
              source);
      off = "";
    }
  }

  nevr_invite::WorkerParts parts;
  parts.token = std::move(settings.token);
  parts.log = settings.log;
  parts.salt = std::move(settings.salt);
  parts.answers_pending = &g_answers_pending;
  if (off == nullptr) {
    nevr_invite::TransportOptions options = std::move(settings.transport);
    options.log = settings.log;
    if (settings.transport_factory) {
      TransportSlot() = settings.transport_factory(settings.http_uri, options);
    } else {
      TransportSlot() = std::make_unique<nevr_invite::CurlTransport>(settings.http_uri, std::move(options));
    }
    if (!TransportSlot()) {
      off = "transport_unavailable";
      offLevel = nevr_invite::LogLevel::Warning;
    }
  }
  if (off == nullptr) {
    parts.transport = TransportSlot().get();
    parts.clock_ms = [] {
      return static_cast<std::int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    parts.configured = true;
    g_active.store(true, std::memory_order_release);
    Say(nevr_invite::LogLevel::Info, "[NEVR.INVITE] service on" + source);
  } else if (off[0] != '\0') {
    Say(offLevel, std::string("[NEVR.INVITE] feature off reason=") + off + source);
  }
  WorkerSlot() = std::make_unique<nevr_invite::Worker>(std::move(parts));
}

bool Active() { return g_active.load(std::memory_order_acquire); }

nevr_invite::Submit Submit(const nevr_invite::Plan& plan) {
  auto lock = LockService();
  if (!WorkerSlot()) {
    if (!g_shutdown) return nevr_invite::Submit::NotConfigured;  // Start has not run: there is no log to write to either
    Say(nevr_invite::LogLevel::Info, nevr_invite::NotSentLine(nevr_invite::Skip::Stopped, plan.code_len));
    return nevr_invite::Submit::Stopped;
  }
  return WorkerSlot()->Enqueue(plan);
}

std::vector<nevr_invite::Answer> TakeAnswers() {
  auto lock = LockService();
  return WorkerSlot() ? WorkerSlot()->TakeAnswers() : std::vector<nevr_invite::Answer>();
}

void NoteDroppedAnswer(const nevr_invite::Answer& answer) {
  Say(nevr_invite::LogLevel::Warning,
      std::string("[NEVR.INVITE] answer dropped by the join op=") + (answer.op == nevr_invite::Op::Create ? "create" : "resolve") +
          " result=" + nevr_invite::ResultName(answer.result));
}

}  // namespace nevr_invite_service

namespace {

// Resolved match ids the frame path has not taken yet, filled from the service's answers. A function-local static, as the
// rest of this file keeps its non-trivial objects (see ServiceMutex above).
struct MatchQueue {
  std::mutex mutex;
  std::deque<std::string> matches;
};
MatchQueue& Matches() {
  static MatchQueue queue;
  return queue;
}

// The texts the answers asked for and the frame path has not taken yet (ShareText), oldest first.
struct TextQueue {
  std::mutex mutex;
  std::deque<nevr_invite_match::ShareText> texts;
};
TextQueue& Texts() {
  static TextQueue queue;
  return queue;
}

// The number of ids in the queue, readable without its mutex: ResolvedMatchPending reads it on every frame.
std::atomic<std::size_t> g_matchesWaiting{0};

}  // namespace

bool nevr_invite_match::ServiceActive() noexcept { return nevr_invite_service::Active(); }

bool nevr_invite_match::ResolvedMatchPending() noexcept {
  return g_matchesWaiting.load(std::memory_order_acquire) != 0 || nevr_invite_service::AnswersPending();
}

bool nevr_invite_match::TakeResolvedMatch(char* out, std::size_t capacity, std::size_t* length) noexcept {
  if (out == nullptr || length == nullptr) return false;
  MatchQueue& queue = Matches();
  std::lock_guard<std::mutex> lock(queue.mutex);
  if (queue.matches.empty()) {
    for (const nevr_invite::Answer& answer : nevr_invite_service::TakeAnswers()) {
      // One answer, two readers of it: the texts it asks the wizard's pages to show (a created code, a refusal), and the match a
      // resolved code names. An answer that is neither is the one the join drops.
      bool used = false;
      for (const nevr_invite_text::Item& item : nevr_invite_text::TextsFor(answer)) {
        nevr_invite_match::ShareText text;
        text.actor = item.request.target.actor;
        text.element = item.request.target.element;
        const std::string field = nevr_invite_text::FieldName(item.field);
        std::memcpy(text.field, field.data(), std::min(field.size(), sizeof(text.field) - 1));
        text.length = std::min(item.request.text.size(), kMaxShareTextBytes);
        std::memcpy(text.text, item.request.text.data(), text.length);
        std::lock_guard<std::mutex> textLock(Texts().mutex);
        Texts().texts.push_back(text);
        used = true;
      }
      if (answer.op == nevr_invite::Op::Resolve && answer.result == nevr_invite::Result::Ok) {
        const std::optional<nevr_invite::Resolved> resolved = nevr_invite::ParseResolved(answer.body);
        const bool fits = resolved.has_value() && resolved->match_id.size() <= kMaxMatchIdBytes;
        queue.matches.push_back(fits ? resolved->match_id : std::string());
        used = true;
      }
      if (!used) nevr_invite_service::NoteDroppedAnswer(answer);
    }
    g_matchesWaiting.store(queue.matches.size(), std::memory_order_release);
  }
  if (queue.matches.empty()) return false;
  const std::string& next = queue.matches.front();
  std::size_t n = next.size();
  if (n > capacity) n = 0;
  if (n > 0) std::memcpy(out, next.data(), n);
  *length = n;
  queue.matches.pop_front();
  g_matchesWaiting.store(queue.matches.size(), std::memory_order_release);
  return true;
}

bool nevr_invite_match::TakeShareText(ShareText* out) noexcept {
  if (out == nullptr) return false;
  TextQueue& queue = Texts();
  std::lock_guard<std::mutex> lock(queue.mutex);
  if (queue.texts.empty()) return false;
  *out = queue.texts.front();
  queue.texts.pop_front();
  return true;
}

namespace nevr_invite_service {

std::vector<nevr_invite::Answer> WaitForAnswers(std::chrono::milliseconds timeout) {
  nevr_invite::Worker* worker = nullptr;
  {
    auto lock = LockService();
    worker = WorkerSlot().get();
  }
  return worker != nullptr ? worker->WaitForAnswers(timeout) : std::vector<nevr_invite::Answer>();
}

void Shutdown() {
  std::unique_ptr<nevr_invite::Worker> worker;
  std::unique_ptr<nevr_invite::Transport> transport;
  {
    auto lock = LockService();
    g_active.store(false, std::memory_order_release);
    if (TransportSlot()) TransportSlot()->Abort();  // a request in flight returns at once instead of waiting out its timeout
    worker = std::move(WorkerSlot());
    transport = std::move(TransportSlot());
    g_started = false;
    g_shutdown = true;
  }
  if (worker) worker->Stop();  // joins; the transport outlives it
  g_answers_pending.store(false, std::memory_order_release);  // the stopped worker's dropped answers are not waiting for anyone
}

bool AnswersPending() { return g_answers_pending.load(std::memory_order_acquire); }

bool WaitForAnswerPendingForTest(std::chrono::milliseconds timeout) {
  nevr_invite::Worker* worker = nullptr;
  {
    auto lock = LockService();
    worker = WorkerSlot().get();
  }
  return worker != nullptr && worker->WaitPendingForTest(timeout);
}

std::uint64_t ServiceLockTakesForTest() { return g_lock_takes.load(std::memory_order_relaxed); }

}  // namespace nevr_invite_service

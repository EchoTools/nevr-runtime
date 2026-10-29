#include "runtime/patch/invite_service.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "runtime/patch/invite_transport.h"

namespace nevr_invite_service {

namespace {

std::mutex g_m;
std::unique_ptr<nevr_invite::Transport> g_transport;
std::unique_ptr<nevr_invite::Worker> g_worker;
std::function<void(nevr_invite::LogLevel, const std::string&)> g_log;
bool g_started = false;
bool g_shutdown = false;  // Shutdown ran and no Start since
std::atomic<bool> g_active{false};  // on and not shut down; read without the lock by the per-frame applier
std::atomic<bool> g_answers_pending{false};  // an answer waits in the worker (AnswersPending); read without the lock
std::atomic<std::uint64_t> g_lock_takes{0};  // every take of g_m, counted for the host tests (ServiceLockTakesForTest)

void Say(nevr_invite::LogLevel level, const std::string& line) {
  if (g_log) g_log(level, line);
}

// Every take of g_m goes through here, so the count is the number of times the service lock was taken.
std::lock_guard<std::mutex> LockService() {
  g_lock_takes.fetch_add(1, std::memory_order_relaxed);
  return std::lock_guard<std::mutex>(g_m);
}

}  // namespace

void Start(Settings settings) {
  auto lock = LockService();
  g_log = settings.log;
  if (g_started) {
    Say(nevr_invite::LogLevel::Warning, "[NEVR.INVITE] service start ignored reason=already_started");
    return;
  }
  g_started = true;
  g_shutdown = false;

  const char* off = nullptr;
  nevr_invite::LogLevel offLevel = nevr_invite::LogLevel::Info;
  if (settings.invite_codes == nevr_invite::Switch::Unrecognised) {
    off = "unrecognised_value";
    offLevel = nevr_invite::LogLevel::Warning;
  } else if (settings.invite_codes == nevr_invite::Switch::Off) {
    off = "not_configured";
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
          std::string("[NEVR.INVITE] feature off reason=service_address_refused detail=") + nevr_invite::UrlCheckName(probe.check));
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
      g_transport = settings.transport_factory(settings.http_uri, options);
    } else {
      g_transport = std::make_unique<nevr_invite::CurlTransport>(settings.http_uri, std::move(options));
    }
    if (!g_transport) {
      off = "transport_unavailable";
      offLevel = nevr_invite::LogLevel::Warning;
    }
  }
  if (off == nullptr) {
    parts.transport = g_transport.get();
    parts.clock_ms = [] {
      return static_cast<std::int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    parts.configured = true;
    g_active.store(true, std::memory_order_release);
    Say(nevr_invite::LogLevel::Info, "[NEVR.INVITE] service on");
  } else if (off[0] != '\0') {
    Say(offLevel, std::string("[NEVR.INVITE] feature off reason=") + off);
  }
  g_worker = std::make_unique<nevr_invite::Worker>(std::move(parts));
}

bool Active() { return g_active.load(std::memory_order_acquire); }

nevr_invite::Submit Submit(const nevr_invite::Plan& plan) {
  auto lock = LockService();
  if (!g_worker) {
    if (!g_shutdown) return nevr_invite::Submit::NotConfigured;  // Start has not run: there is no log to write to either
    Say(nevr_invite::LogLevel::Info, nevr_invite::NotSentLine(nevr_invite::Skip::Stopped, plan.code_len));
    return nevr_invite::Submit::Stopped;
  }
  return g_worker->Enqueue(plan);
}

std::vector<nevr_invite::Answer> TakeAnswers() {
  auto lock = LockService();
  return g_worker ? g_worker->TakeAnswers() : std::vector<nevr_invite::Answer>();
}

std::vector<nevr_invite::Answer> WaitForAnswers(std::chrono::milliseconds timeout) {
  nevr_invite::Worker* worker = nullptr;
  {
    auto lock = LockService();
    worker = g_worker.get();
  }
  return worker != nullptr ? worker->WaitForAnswers(timeout) : std::vector<nevr_invite::Answer>();
}

void Shutdown() {
  std::unique_ptr<nevr_invite::Worker> worker;
  std::unique_ptr<nevr_invite::Transport> transport;
  {
    auto lock = LockService();
    g_active.store(false, std::memory_order_release);
    if (g_transport) g_transport->Abort();  // a request in flight returns at once instead of waiting out its timeout
    worker = std::move(g_worker);
    transport = std::move(g_transport);
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
    worker = g_worker.get();
  }
  return worker != nullptr && worker->WaitPendingForTest(timeout);
}

std::uint64_t ServiceLockTakesForTest() { return g_lock_takes.load(std::memory_order_relaxed); }

}  // namespace nevr_invite_service

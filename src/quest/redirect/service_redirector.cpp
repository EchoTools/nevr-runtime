#include "quest/redirect/service_redirector.h"

#include <cstring>
#include <exception>
#include <optional>
#include <string>

#include "hook_log.h"

namespace nevr_quest::redirect {
namespace {

constexpr const char* kServiceHostKeys[] = {
    "config_host",        "configservice_host",      // libr15 Initialize
    "login_host",         "loginservice_host",       // libr15 VerifyServerLoginConnection, BeginLogIn
    "transaction_host",   "transactionservice_host", // libr15 LogInSuccess, BeginMultiplayer
    "matchmaker_host",    "matchingservice_host",    // libpnsradmatchmaking MatchmakerUri, ConnectMatchmaker
};

// Log-only classification of a URL's scheme. It never influences a decision.
const char* SchemeToken(const char* url) {
  if (std::strncmp(url, "wss://", 6) == 0) return "wss";
  if (std::strncmp(url, "ws://", 5) == 0) return "ws";
  if (std::strncmp(url, "https://", 8) == 0) return "https";
  if (std::strncmp(url, "http://", 7) == 0) return "http";
  return "other";
}

void LogFailure(const char* key, const char* status) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kError, "service_redirect",
                      {{"key", key}, {"decision", "failed"}, {"status", status},
                       {"action", "return_original"}});
}

}  // namespace

bool IsServiceHostKey(const char* key) noexcept {
  if (key == nullptr) return false;
  for (const char* candidate : kServiceHostKeys) {
    if (std::strcmp(key, candidate) == 0) return true;
  }
  return false;
}

ServiceRedirector::ServiceRedirector(const nevr_quest::ResolvedConfig& config, InternFn intern,
                                     BridgeProbe bridge)
    : config_(config),
      intern_(intern),
      bridge_(bridge),
      active_(intern != nullptr && config.effective.redirect) {}

const char* ServiceRedirector::Resolve(const char* key, const char* result, std::size_t length,
                                       BridgeState bridge, Outcome* outcome) {
  Entry* slot = nullptr;
  for (Entry& e : cache_) {
    if (!e.used) {
      if (slot == nullptr) slot = &e;
      continue;
    }
    const bool sameBridge = e.bridgeReady == bridge.ready && e.bridgePort == bridge.port;
    if (sameBridge && e.length == length && std::memcmp(e.original.data(), result, length) == 0) {
      if (e.redirected == nullptr) {
        *outcome = Outcome::kPassThrough;
        return result;
      }
      *outcome = Outcome::kRedirected;
      return e.redirected;
    }
    // A full cache may recycle an entry computed for an older bridge state.
    if (slot == nullptr && !sameBridge) slot = &e;
  }

  policyRuns_.fetch_add(1, std::memory_order_relaxed);
  const std::optional<std::string> replacement = nevr_quest::ResolveQuestRedirect(
      config_, std::string(result, length), bridge.ready, bridge.port);

  const char* published = nullptr;  // null: leave the original
  if (replacement && *replacement != std::string_view(result, length)) {
    const nevr_runtime::lifecycle::InternResult interned = intern_(*replacement);
    if (interned.status != nevr_runtime::lifecycle::InternStatus::kSuccess || interned.pointer == nullptr) {
      LogFailure(key, nevr_runtime::lifecycle::InternStatusName(interned.status));
      *outcome = Outcome::kFailed;
      return result;
    }
    published = interned.pointer;
  }

  if (slot != nullptr) {
    slot->used = true;
    slot->bridgeReady = bridge.ready;
    slot->bridgePort = bridge.port;
    slot->length = length;
    std::memcpy(slot->original.data(), result, length);
    slot->redirected = published;
  }

  if (published == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "service_redirect",
                        {{"key", key}, {"decision", "pass"}, {"from", SchemeToken(result)}});
    *outcome = Outcome::kPassThrough;
    return result;
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "service_redirect",
                      {{"key", key}, {"decision", "redirect"}, {"from", SchemeToken(result)},
                       {"to", SchemeToken(published)}, {"via", bridge.ready ? "bridge" : "target"},
                       {"cached", slot != nullptr ? 1 : 0}});
  *outcome = Outcome::kRedirected;
  return published;
}

const char* ServiceRedirector::Apply(const char* key, const char* result) noexcept {
  if (!active_ || key == nullptr || result == nullptr || !IsServiceHostKey(key)) return result;
  calls_.fetch_add(1, std::memory_order_relaxed);

  const std::size_t length = strnlen(result, kMaxValueBytes + 1);
  if (length > kMaxValueBytes) {
    failures_.fetch_add(1, std::memory_order_relaxed);
    LogFailure(key, "value_too_long");
    return result;
  }

  Outcome outcome = Outcome::kFailed;
  const char* chosen = result;
  try {
    BridgeState bridge;
    if (bridge_ != nullptr) bridge = bridge_();
    const std::lock_guard<std::mutex> lock(mutex_);
    chosen = Resolve(key, result, length, bridge, &outcome);
  } catch (const std::exception&) {
    failures_.fetch_add(1, std::memory_order_relaxed);
    LogFailure(key, "exception");
    return result;
  }

  if (outcome == Outcome::kRedirected) redirected_.fetch_add(1, std::memory_order_relaxed);
  if (outcome == Outcome::kFailed) failures_.fetch_add(1, std::memory_order_relaxed);
  return chosen;
}

void ServiceRedirector::Prewarm() noexcept {
  if (!active_) return;
  for (const BuiltinDefault& d : kBuiltinDefaults) (void)Apply(d.key, d.value);
}

Counters ServiceRedirector::counters() const noexcept {
  Counters c;
  c.calls = calls_.load(std::memory_order_relaxed);
  c.redirected = redirected_.load(std::memory_order_relaxed);
  c.failures = failures_.load(std::memory_order_relaxed);
  c.policyRuns = policyRuns_.load(std::memory_order_relaxed);
  return c;
}

}  // namespace nevr_quest::redirect

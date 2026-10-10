#include "quest/redirect/service_redirector.h"

#include <cstring>
#include <exception>
#include <optional>
#include <string>


namespace nevr_quest::redirect {
namespace {

constexpr const char* kServiceHostKeys[] = {
    "config_host",        "configservice_host",      // libr15 Initialize
    "login_host",         "loginservice_host",       // libr15 VerifyServerLoginConnection, BeginLogIn
    "transaction_host",   "transactionservice_host", // libr15 LogInSuccess, BeginMultiplayer
    "matchmaker_host",    "matchingservice_host",    // libpnsradmatchmaking MatchmakerUri, ConnectMatchmaker
};

// key == prefix + <at least one character> + suffix.
bool IsFormattedKey(const char* key, const char* prefix, const char* suffix) noexcept {
  const std::size_t keyLen = std::strlen(key);
  const std::size_t prefixLen = std::strlen(prefix);
  const std::size_t suffixLen = std::strlen(suffix);
  if (keyLen < prefixLen + 1 + suffixLen) return false;
  return std::memcmp(key, prefix, prefixLen) == 0 &&
         std::memcmp(key + keyLen - suffixLen, suffix, suffixLen) == 0;
}

}  // namespace

RedirectCounters& GlobalCounters() noexcept {
  static RedirectCounters counters;
  return counters;
}

void ResetCountersForTest() noexcept {
  RedirectCounters& c = GlobalCounters();
  c.reads.store(0);
  c.redirected.store(0);
  c.policyRuns.store(0);
  c.valueTooLong.store(0);
  c.poolRefused.store(0);
  c.exceptions.store(0);
}

bool IsServiceHostKey(const char* key) noexcept {
  if (key == nullptr) return false;
  for (const char* candidate : kServiceHostKeys) {
    if (std::strcmp(key, candidate) == 0) return true;
  }
  return IsFormattedKey(key, "matchmaker_", "_host") || IsFormattedKey(key, "matchingservice_", "_host");
}

bool IsApiBaseUrl(const char* url) noexcept {
  // "https://api." and "https://api-", the two real hosts: a bare "https://api" prefix would also take
  // "https://apiary.example" (#413 tightened the PC hook the same way).
  return url != nullptr && (std::strncmp(url, "https://api.", 12) == 0 || std::strncmp(url, "https://api-", 12) == 0);
}

ServiceRedirector::ServiceRedirector(const nevr_quest::ResolvedConfig& config, InternFn intern,
                                     BridgeProbe bridge)
    : config_(config),
      intern_(intern),
      bridge_(bridge),
      active_(intern != nullptr && config.effective.redirect) {}

const char* ServiceRedirector::Resolve(const char* result, std::size_t length, BridgeState bridge,
                                       Outcome* outcome) {
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

  GlobalCounters().policyRuns.fetch_add(1, std::memory_order_relaxed);
  const std::optional<std::string> replacement = nevr_quest::ResolveQuestRedirect(
      config_, std::string(result, length), bridge.ready, bridge.port);

  const char* published = nullptr;  // null: leave the original
  if (replacement && *replacement != std::string_view(result, length)) {
    const nevr::lifecycle::InternResult interned = intern_(*replacement);
    if (interned.status != nevr::lifecycle::InternStatus::kSuccess || interned.pointer == nullptr) {
      GlobalCounters().poolRefused.fetch_add(1, std::memory_order_relaxed);
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
    *outcome = Outcome::kPassThrough;
    return result;
  }
  *outcome = Outcome::kRedirected;
  return published;
}

const char* ServiceRedirector::Apply(const char* key, const char* result) noexcept {
  if (!active_ || key == nullptr || result == nullptr || !IsServiceHostKey(key)) return result;
  return ApplyChecked(result);
}

const char* ServiceRedirector::ApplyUrl(const char* url) noexcept {
  if (!active_ || !IsApiBaseUrl(url)) return url;
  return ApplyChecked(url);
}

const char* ServiceRedirector::ApplyChecked(const char* result) noexcept {
  RedirectCounters& counters = GlobalCounters();
  counters.reads.fetch_add(1, std::memory_order_relaxed);

  const std::size_t length = strnlen(result, kMaxValueBytes + 1);
  if (length > kMaxValueBytes) {
    counters.valueTooLong.fetch_add(1, std::memory_order_relaxed);
    return result;
  }

  Outcome outcome = Outcome::kFailed;
  const char* chosen = result;
  try {
    BridgeState bridge;
    if (bridge_ != nullptr) bridge = bridge_();
    const std::lock_guard<std::mutex> lock(mutex_);
    chosen = Resolve(result, length, bridge, &outcome);
  } catch (const std::exception&) {
    counters.exceptions.fetch_add(1, std::memory_order_relaxed);
    return result;
  }

  if (outcome == Outcome::kRedirected) counters.redirected.fetch_add(1, std::memory_order_relaxed);
  return chosen;
}

void ServiceRedirector::Prewarm() noexcept {
  if (!active_) return;
  for (const BuiltinDefault& d : kBuiltinDefaults) (void)Apply(d.key, d.value);
}

Counters ServiceRedirector::counters() const noexcept {
  const RedirectCounters& g = GlobalCounters();
  Counters c;
  c.calls = g.reads.load(std::memory_order_relaxed);
  c.redirected = g.redirected.load(std::memory_order_relaxed);
  c.failures = g.valueTooLong.load(std::memory_order_relaxed) + g.poolRefused.load(std::memory_order_relaxed) +
               g.exceptions.load(std::memory_order_relaxed);
  c.policyRuns = g.policyRuns.load(std::memory_order_relaxed);
  return c;
}

}  // namespace nevr_quest::redirect

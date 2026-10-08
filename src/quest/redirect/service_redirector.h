// Config-string redirect for the Quest client: the decision half of the CJson::TString hook.
//
// The game reads each service endpoint with NRadEngine::CJson::TString(key, fallback, flag) and
// passes the returned char const* straight to the next lookup and to CUriContainer::Parse, so the
// pointer has to stay valid after the call returns (docs/adr/0003, "Config-string seam"). This
// class takes the key and the value the original returned and answers with the pointer the game
// should use: the original pointer untouched, or a pointer into the process-lifetime pool
// (runtime/lifecycle/stable_string_pool) holding the redirected URL.
//
// Policy is not decided here. A value is redirected only when the shared policy
// (nevr_quest::ResolveQuestRedirect -> nevr_cfg::ResolveRedirect, the same source the PCVR runtime
// compiles) returns a replacement, and only for the endpoint keys in IsServiceHostKey. PCVR
// redirects by value for every key; Quest narrows to the keys the binaries read for the config,
// login, transaction and matchmaker hosts, so no unrelated config string is ever rewritten.
// Why not by value for every key: the two libraries make 175 and 62 TString calls, 87 and 28 of them
// with a key that is not a literal this analysis could recover, many from per-frame script readers;
// a by-value rule would take the cache lock and run the policy for any ws:// or https:// string any
// of them reads. The key rule is exhaustive for the host keys: the only `_host` key formats in the
// three Quest libraries' strings are the eight literals and the two matchmaker per-type formats
// below (`strings -a | grep '%s.*host'`).
//
// Allocation. Apply copies nothing and allocates nothing for a key outside the list, for a value
// it has already seen, or when redirect is off. The first sight of each distinct value (at most
// kMaxCachedValues are remembered) runs the shared policy, which allocates, and may intern a new
// pool string. Prewarm() runs that work for the game's built-in endpoint defaults at install time,
// so the normal login and matchmaking reads are cache hits. Config reads happen at connect time,
// not per frame.
//
// Full cache. A value is remembered only if a slot is free or holds an entry computed for another
// bridge state. When all kMaxCachedValues slots were computed under the current bridge state, a
// further distinct value is not remembered: every read of it runs the policy again and allocates
// (the pool interns it once, so pool growth is bounded by the distinct redirect targets). That is
// acceptable here because the game reads a host key a handful of times per connect, and the
// policyRuns counter shows it if it ever happens.
//
// Failure. Any doubt returns the original pointer: a value longer than kMaxValueBytes, a pool
// refusal, an exception from the policy or the pool. Apply never logs: it runs inside a hooked
// game call, where logging is unsafe (hook_log.h). It only increments the process-wide counters
// below, which RegisterRedirectCounters hands to the sentinel's reporter thread; the install path
// logs the rest. No counter or line carries a URL, host or credential.
//
// Residual. Apply catches std::exception (which covers bad_alloc and system_error) around the
// policy, the lock and the pool. It does not catch(...) (repo rule), so an exception of another
// type would reach Apply's noexcept boundary and terminate; nothing it calls throws one.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

#include "quest/sentinel/quest_config.h"
#include "runtime/lifecycle/stable_string_pool.h"

namespace nevr_quest::redirect {

inline constexpr std::size_t kMaxCachedValues = 16;
inline constexpr std::size_t kMaxValueBytes = 512;

// The keys the pinned libr15.so / libpnsradmatchmaking.so read for service endpoints
// (docs/adr/0003; each is the first argument of a CJson::TString call whose fallback is the
// game's built-in wss:// default or ''). The eight literals, plus `matchmaker_<type>_host` and
// `matchingservice_<type>_host` with a non-empty <type> (the two formats libpnsradmatchmaking
// passes to TString for each match type). Exact, case-sensitive, allocation-free.
bool IsServiceHostKey(const char* key) noexcept;

// The built-in defaults the game passes as fallback for those keys. Used to prewarm the cache.
struct BuiltinDefault {
  const char* key;    // the primary key read with this fallback
  const char* value;  // the fallback string in the binary's rodata
};
inline constexpr BuiltinDefault kBuiltinDefaults[] = {
    {"config_host", "wss://config.readyatdawn.com/rad/rad15_live"},
    {"login_host", "wss://login.readyatdawn.com/rad/rad15_live"},
    {"transaction_host", "wss://transaction.readyatdawn.com/rad/rad15_live"},
    {"matchmaker_host", "wss://matchmaker.readyatdawn.com/rad/rad15_live"},
};

struct BridgeState {
  bool ready = false;
  unsigned port = 0;
};
// Current state of the local bridge; nullptr means no bridge exists yet.
using BridgeProbe = BridgeState (*)();
using InternFn = nevr_runtime::lifecycle::InternResult (*)(std::string_view);

enum class Outcome : std::uint8_t {
  kPassThrough,   // original pointer returned
  kRedirected,    // pool pointer returned
  kFailed,        // doubt or failure: original pointer returned
};

// Process-wide, because the reporter holds pointers to them from before any redirector exists.
struct RedirectCounters {
  std::atomic<std::uint64_t> reads{0};          // Apply calls for a service key with a value
  std::atomic<std::uint64_t> redirected{0};     // returned a pool pointer
  std::atomic<std::uint64_t> policyRuns{0};     // cache misses that ran the shared policy
  std::atomic<std::uint64_t> valueTooLong{0};   // fault: value over kMaxValueBytes
  std::atomic<std::uint64_t> poolRefused{0};    // fault: the pool returned a non-success status
  std::atomic<std::uint64_t> exceptions{0};     // fault: std::exception inside Apply
};
RedirectCounters& GlobalCounters() noexcept;
void ResetCountersForTest() noexcept;

struct Counters {
  std::uint64_t calls = 0;
  std::uint64_t redirected = 0;
  std::uint64_t failures = 0;    // valueTooLong + poolRefused + exceptions
  std::uint64_t policyRuns = 0;
};

class ServiceRedirector {
 public:
  // Copies `config`. `intern` is the pool (production: InternStableCStr); `bridge` may be null.
  ServiceRedirector(const nevr_quest::ResolvedConfig& config, InternFn intern, BridgeProbe bridge);

  ServiceRedirector(const ServiceRedirector&) = delete;
  ServiceRedirector& operator=(const ServiceRedirector&) = delete;

  // True when the redirect feature is effective; otherwise Apply is a pass-through.
  bool active() const noexcept { return active_; }

  // Decides for one TString result. Never throws; returns `result` itself unless it returns a
  // pool pointer. Safe to call from any thread.
  const char* Apply(const char* key, const char* result) noexcept;

  // Runs the policy for the built-in defaults so the first game reads are cache hits.
  void Prewarm() noexcept;

  Counters counters() const noexcept;

 private:
  struct Entry {
    bool used = false;
    bool bridgeReady = false;
    unsigned bridgePort = 0;
    std::size_t length = 0;
    const char* redirected = nullptr;  // null: policy declined, return the original
    std::array<char, kMaxValueBytes> original{};
  };

  const char* Resolve(const char* result, std::size_t length, BridgeState bridge, Outcome* outcome);

  const nevr_quest::ResolvedConfig config_;
  const InternFn intern_;
  const BridgeProbe bridge_;
  const bool active_;

  std::mutex mutex_;
  std::array<Entry, kMaxCachedValues> cache_{};

};

}  // namespace nevr_quest::redirect

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
// redirects by value for every key; Quest narrows to the eight keys the binaries read for the
// service, config, login, transaction and matchmaker hosts, so no unrelated config string is ever
// rewritten.
//
// Allocation. Apply copies nothing and allocates nothing for a key outside the list, for a value
// it has already seen, or when redirect is off. The first sight of each distinct value (at most
// kMaxCachedValues are remembered) runs the shared policy, which allocates, and may intern a new
// pool string. Prewarm() runs that work for the game's built-in endpoint defaults at install time,
// so the normal login and matchmaking reads are cache hits. Config reads happen at connect time,
// not per frame.
//
// Failure. Any doubt returns the original pointer: a value longer than kMaxValueBytes, a pool
// refusal, an exception from the policy or the pool. Each failure logs one structured line with
// the key name and a status token; no line contains a URL, host or credential.
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
// game's built-in wss:// default). Exact, case-sensitive match.
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

struct Counters {
  std::uint64_t calls = 0;        // Apply calls for a service key with a value
  std::uint64_t redirected = 0;   // returned a pool pointer
  std::uint64_t failures = 0;     // returned the original because of doubt or an error
  std::uint64_t policyRuns = 0;   // cache misses that ran the shared policy
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

  const char* Resolve(const char* key, const char* result, std::size_t length, BridgeState bridge,
                      Outcome* outcome);

  const nevr_quest::ResolvedConfig config_;
  const InternFn intern_;
  const BridgeProbe bridge_;
  const bool active_;

  std::mutex mutex_;
  std::array<Entry, kMaxCachedValues> cache_{};

  std::atomic<std::uint64_t> calls_{0};
  std::atomic<std::uint64_t> redirected_{0};
  std::atomic<std::uint64_t> failures_{0};
  std::atomic<std::uint64_t> policyRuns_{0};
};

}  // namespace nevr_quest::redirect

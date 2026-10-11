#pragma once
// Self-check "status_redirect" (#408, docs/engine/remote-log.md): where the game's HTTP connections go.
//
// The game's service-status request (GET status/services,news?env=%s&projectid=rad14, echovr.exe 0x1416dfe50,
// libr15.so 0x2baa0d2) and its other REST calls open their connection through one connect function that the
// runtime hooks and decides the host for. #408's failing shape on the headset was "Couldn't resolve host name":
// the request went to a host the redirect does not cover. So EVERY connect the game opens is counted by class:
//   api       the game's API base (https://api.<host> / https://api-<env>.<host>): redirected, or left alone
//             (passthrough: a failure);
//   graph     the matchmaker queue's base (https://graph.oculus.com), counted apart (#414);
//   uncovered a host of the game's own service domain (readyatdawn.com) that is not an API or graph base and
//             that the redirect left alone: the failing shape, a failure, named by host only;
//   external  any other host (not a failure).
// The check cannot see the HTTP response: whether the status request succeeded, and the game's own `Service
// status request failed` line, stay invisible to the runtime.
//
// NoteConnect runs inside the hooked connect function: relaxed atomic increments and one bounded copy of a host
// name into a fixed buffer, nothing else (no logging, no allocation, no lock). Take runs on the self-check flush.
//
// What is said (policy): the first time a redirect has been seen and nothing is wrong, one pass; a failure each
// time the failure count (passthrough + uncovered) first reaches 1, 2, 4, 8, ... carrying the running counts, so
// they survive the per-check cap; nothing when nothing changed; a late redirect after a failure is not a pass.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "runtime/compat/self_check.h"

namespace nevr_status_redirect_check {

struct Snapshot {
  std::uint64_t apiRedirected = 0;
  std::uint64_t apiPassthrough = 0;
  std::uint64_t uncovered = 0;
  std::uint64_t graph = 0;
  std::uint64_t external = 0;
};

namespace detail {

constexpr std::size_t kHostSlots = 4;
constexpr std::size_t kHostBytes = 48;

// The host part of "scheme://host[:port][/path][?query]" in lower case; false when there is none.
inline bool HostOf(const char* uri, char* out, std::size_t outBytes) noexcept {
  if (uri == nullptr) return false;
  const char* p = std::strstr(uri, "://");
  if (p == nullptr) return false;
  p += 3;
  std::size_t n = 0;
  while (p[n] != '\0' && p[n] != ':' && p[n] != '/' && p[n] != '?' && p[n] != '#') ++n;
  if (n == 0 || n >= outBytes) return false;
  for (std::size_t i = 0; i < n; ++i) {
    const char c = p[i];
    out[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  out[n] = '\0';
  return true;
}

inline bool EndsWith(const char* text, const char* suffix) noexcept {
  const std::size_t t = std::strlen(text);
  const std::size_t s = std::strlen(suffix);
  return t >= s && std::memcmp(text + t - s, suffix, s) == 0;
}

// The game's API base: "https://api.<host>" or "https://api-<env>.<host>" (the same rule as
// nevr::lifecycle::IsGameApiBaseUrl and the Quest redirector's IsApiBaseUrl).
inline bool IsApiBase(const char* uri) noexcept {
  return std::strncmp(uri, "https://api.", 12) == 0 || std::strncmp(uri, "https://api-", 12) == 0;
}

inline bool IsGraphBase(const char* uri) noexcept {
  constexpr char kGraph[] = "https://graph.oculus.com";
  constexpr std::size_t kLength = sizeof(kGraph) - 1;
  return std::strncmp(uri, kGraph, kLength) == 0 && (uri[kLength] == '\0' || uri[kLength] == '/');
}

inline std::uint64_t FloorPowerOfTwo(std::uint64_t n) noexcept {
  std::uint64_t p = 1;
  while (n >= p * 2 && p < (std::uint64_t{1} << 62)) p *= 2;
  return n == 0 ? 0 : p;
}

}  // namespace detail

class Ledger {
 public:
  /// One game HTTP connect: `gameUri` is what the game asked for, `finalUri` what it will be sent to.
  void NoteConnect(const char* gameUri, const char* finalUri) noexcept {
    if (gameUri == nullptr || gameUri[0] == '\0') return;
    const bool redirected = finalUri != nullptr && std::strcmp(gameUri, finalUri) != 0;
    if (detail::IsApiBase(gameUri)) {
      (redirected ? apiRedirected_ : apiPassthrough_).fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (detail::IsGraphBase(gameUri)) {
      graph_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    char host[detail::kHostBytes];
    if (detail::HostOf(gameUri, host, sizeof(host)) &&
        (std::strcmp(host, "readyatdawn.com") == 0 || detail::EndsWith(host, ".readyatdawn.com"))) {
      if (redirected) return;  // covered
      uncovered_.fetch_add(1, std::memory_order_relaxed);
      RememberHost(host);
      return;
    }
    external_.fetch_add(1, std::memory_order_relaxed);
  }

  Snapshot Counts() const noexcept {
    Snapshot s;
    s.apiRedirected = apiRedirected_.load(std::memory_order_relaxed);
    s.apiPassthrough = apiPassthrough_.load(std::memory_order_relaxed);
    s.uncovered = uncovered_.load(std::memory_order_relaxed);
    s.graph = graph_.load(std::memory_order_relaxed);
    s.external = external_.load(std::memory_order_relaxed);
    return s;
  }

  /// The observation to report now, if there is one (see the policy above).
  bool Take(nevr_self_check::Observation* out) {
    const Snapshot s = Counts();
    const std::uint64_t failures = s.apiPassthrough + s.uncovered;
    bool say = false;
    bool pass = false;
    if (failures > 0) {
      const std::uint64_t level = detail::FloorPowerOfTwo(failures);
      if (level > failureLevel_.load(std::memory_order_relaxed)) {
        failureLevel_.store(level, std::memory_order_relaxed);
        say = true;
      }
    } else if (s.apiRedirected > 0 && !passSaid_.load(std::memory_order_relaxed)) {
      passSaid_.store(true, std::memory_order_relaxed);
      say = true;
      pass = true;
    }
    if (!say) return false;
    std::string text = "api_connects=" + std::to_string(s.apiRedirected + s.apiPassthrough) +
                       " redirected=" + std::to_string(s.apiRedirected) +
                       " passthrough=" + std::to_string(s.apiPassthrough) + " uncovered=" + std::to_string(s.uncovered) +
                       " graph=" + std::to_string(s.graph) + " external=" + std::to_string(s.external);
    if (s.uncovered > 0) {
      std::string hosts;
      for (std::size_t i = 0; i < detail::kHostSlots; ++i) {
        if (!ready_[i].load(std::memory_order_acquire)) continue;
        if (!hosts.empty()) hosts += ',';
        hosts += hosts_[i];
      }
      if (!hosts.empty()) text += " hosts=" + hosts;
    }
    out->observed = std::move(text);
    out->pass = pass;
    return true;
  }

  void Reset() noexcept {
    apiRedirected_.store(0);
    apiPassthrough_.store(0);
    uncovered_.store(0);
    graph_.store(0);
    external_.store(0);
    failureLevel_.store(0);
    passSaid_.store(false);
    next_.store(0);
    for (std::size_t i = 0; i < detail::kHostSlots; ++i) ready_[i].store(false);
  }

 private:
  void RememberHost(const char* host) noexcept {
    for (std::size_t i = 0; i < detail::kHostSlots; ++i) {
      if (ready_[i].load(std::memory_order_acquire) && std::strcmp(hosts_[i], host) == 0) return;  // seen
    }
    const std::uint32_t slot = next_.fetch_add(1, std::memory_order_relaxed);
    if (slot >= detail::kHostSlots) return;  // the first four distinct hosts are kept
    std::strncpy(hosts_[slot], host, detail::kHostBytes - 1);
    hosts_[slot][detail::kHostBytes - 1] = '\0';
    ready_[slot].store(true, std::memory_order_release);
  }

  std::atomic<std::uint64_t> apiRedirected_{0};
  std::atomic<std::uint64_t> apiPassthrough_{0};
  std::atomic<std::uint64_t> uncovered_{0};
  std::atomic<std::uint64_t> graph_{0};
  std::atomic<std::uint64_t> external_{0};
  std::atomic<std::uint64_t> failureLevel_{0};
  std::atomic<bool> passSaid_{false};
  std::atomic<std::uint32_t> next_{0};
  char hosts_[detail::kHostSlots][detail::kHostBytes] = {};
  std::atomic<bool> ready_[detail::kHostSlots] = {};
};

/// The process-wide ledger the PC connect hook and the Quest redirector feed.
inline Ledger& Process() noexcept {
  static Ledger* const ledger = new Ledger();  // never destroyed (the Quest sentinel registers no atexit)
  return *ledger;
}

inline void NoteConnect(const char* gameUri, const char* finalUri) noexcept { Process().NoteConnect(gameUri, finalUri); }

inline bool ProcessProbe(nevr_self_check::Observation* out) { return Process().Take(out); }

inline constexpr char kName[] = "status_redirect";
inline constexpr char kExpected[] =
    "every connection the game opens to its API base is sent to the configured http service, and none to an "
    "uncovered service-domain host (passthrough=0 uncovered=0)";

/// Registers the check with the unit; the unit calls `probe` on every Flush.
inline nevr_self_check::CheckId Register(nevr_self_check::Probe probe) {
  return nevr_self_check::Register({kName, kExpected, probe});
}

// Test seam.
inline void ResetForTest() { Process().Reset(); }

}  // namespace nevr_status_redirect_check

// service_map.h — the PURE, testable core of the config.cpp cutover (N133 S3).
//
// This is the single source of truth for two things the migration must get
// exactly right:
//   1. the legacy-flat-key -> config.yaml dotted-path map, and
//   2. the service-endpoint resolution logic (host fallback + scheme redirect)
//      that config.cpp's hooks used to run against the game JSON.
//
// Everything here is a pure function of a `nevr::NevrConfig` (+ scalar bridge
// state) — no game functions, no windows.h, no singleton, no I/O — so a gtest
// (test_service_map) links it directly against nevr_core + yaml-cpp with zero
// stubs. The impure half (the config.yaml singleton, discovery, interning, and
// the C-string accessors config.cpp actually calls) lives in service_config.*.
//
// yaml-cpp is NOT pulled in: nevr_config.h is pImpl. This header is safe to
// include from a SKIP_PCH translation unit that has defined NOMINMAX.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/nevr_config.h"

namespace nevr_cfg {

/// Map a legacy flat NEVR/service config key (the string the game JSON was keyed
/// by, e.g. "matchingservice_host") to its config.yaml dotted path
/// (e.g. "services.matchmaking"). Returns "" for a key that is deliberately NOT
/// migrated to config.yaml in S3 (e.g. "nevr_http_uri", owned by S4/S5) — the
/// caller then keeps that key's old source. This map IS the cutover.
std::string FlatKeyToYamlPath(const std::string& flatKey);

/// Read a migrated flat key from `cfg`. nullopt when the key is unmapped or the
/// path is absent; may return an empty string when the key is present-but-empty
/// (the caller applies the same `[0] != '\0'` check the JSON readers did).
std::optional<std::string> LookupFlat(const nevr::NevrConfig& cfg, const std::string& flatKey);

/// Which source satisfied a service-host lookup — preserved so the adapter can
/// reproduce config.cpp's three distinct Debug log lines verbatim.
enum class HostSource {
  kPrimary,        // the requested service key was present + non-empty
  kLoginFallback,  // fell back to loginservice_host (services.login)
  kNone,           // neither present -> caller uses its hardcoded default
};

struct ServiceHostResult {
  std::optional<std::string> value;  // nullopt iff source == kNone
  HostSource source = HostSource::kNone;
};

/// Service host with the loginservice_host fallback — the exact chain
/// GetServiceHostWithFallback ran (primary key -> loginservice_host -> default).
/// kNone means "no override": the caller returns its own default URL, which is
/// the *unchanged* game default, preserving today's behaviour for an absent key.
ServiceHostResult ResolveServiceHost(const nevr::NevrConfig& cfg, const std::string& flatServiceKey);

/// Pure scheme-based redirect — the core of RedirectServiceUrl. Given the URL the
/// game produced (`result`) and the two configured redirect targets, decide the
/// replacement, or nullopt to leave `result` untouched.
///   socketTarget : nevr_socket_uri, migrated to config.yaml (services.socket_uri)
///   httpTarget   : nevr_http_uri, NOT migrated in S3 — the caller passes the raw
///                  early-JSON value so the https branch is byte-for-byte unchanged
/// A ws/wss `result` with the bridge active rewrites to ws://127.0.0.1:<port>;
/// otherwise the raw target passes through. https redirects never hit the bridge.
std::optional<std::string> ResolveRedirect(const std::string& result,
                                           const std::optional<std::string>& socketTarget,
                                           const std::optional<std::string>& httpTarget,
                                           bool bridgeActive, unsigned bridgePort);

}  // namespace nevr_cfg

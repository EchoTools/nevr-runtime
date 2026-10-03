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
#include <map>
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

/// Built-in defaults embedded at build time, keyed by flat key (non-empty values only).
using FlatDefaults = std::map<std::string, std::string>;

/// LookupFlat layered over the embedded defaults. The config.yaml value wins when it is
/// present and non-empty after interpolation; otherwise the embedded default is used; with
/// no default the file's own answer is returned unchanged (nullopt, or a present-but-empty
/// string). A null section (`auth:` with every key commented out) and a value that
/// interpolates to empty both count as "no override", so the default survives — the same
/// "an empty secret is unset" rule the rest of the config layer follows. Lookup-time
/// layering, not tree merging: the parsed file is never modified.
std::optional<std::string> LookupFlatWithDefaults(const nevr::NevrConfig& cfg,
                                                  const FlatDefaults& defaults,
                                                  const std::string& flatKey);

/// Read a LIST-shaped migrated flat key (guilds, regions) as a CSV string — the
/// shape the game-JSON readers built into `guilds=%s` / `regions=%s`. A config.yaml
/// list `[a, b]` and a scalar CSV `"a,b"` both resolve to "a,b". nullopt when the
/// key is unmapped or the path is absent (an empty present value resolves to "").
std::optional<std::string> LookupFlatCsv(const nevr::NevrConfig& cfg, const std::string& flatKey);

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

/// Issue #21 — the value NEVR supplies for a key the STOCK ENGINE reads from its
/// own JSON config (never a NEVR setting), used only when no config anywhere
/// provided one. _local/config.json is optional now, and before that it was the
/// only source of these keys. Owner-chosen value: publisher_lock = "echotools".
/// nullopt for every other key (the engine keeps its own default).
std::optional<std::string> GameNativeDefault(const std::string& key);

/// The game-native config the runtime supplies when no `_local/config.json` exists, as JSON text.
/// Today that is the `social_plugin` block the game's social layer (friends, parties, presence,
/// matchmaking UI) reads: endpoint and port come from the Nakama HTTP base (`httpUri`, e.g.
/// https://host:7350), the key is the Nakama server key, device auth, auto-create, every feature on
/// (the same shape a hand-written config.json carried). nullopt when either input is empty or the
/// URL has no host, so a build with nothing embedded supplies nothing. Built with nlohmann::json,
/// never by string concatenation.
std::optional<std::string> BuildGameNativeConfigJson(const std::string& httpUri,
                                                     const std::string& serverKey);

/// Gate for the social façade that stands in when the platform provider has no
/// Social object (pnsrad exports none). On by default; `social.facade: false`
/// turns it off. A missing or invalid value leaves it on. It never replaces a
/// provider's own Social object, only a null one.
bool SocialFacadeEnabled(const nevr::NevrConfig& cfg);

}  // namespace nevr_cfg

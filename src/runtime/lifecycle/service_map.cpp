// service_map.cpp — see service_map.h. PURE: nevr_config + std only. Compiled
// with SKIP_PRECOMPILE_HEADERS and NO windows.h, so the min/max macros never
// exist here and the test can link it standalone.

#include "runtime/lifecycle/service_map.h"

#include <unordered_map>

namespace nevr_cfg {

std::string FlatKeyToYamlPath(const std::string& flatKey) {
  // The cutover map. Left column = the exact key the game-JSON readers used;
  // right column = the config.yaml dotted path that now owns it. A flat key
  // ABSENT from this table is NOT migrated in S3 (caller keeps its old source):
  // in particular nevr_http_uri (RedirectServiceUrl's https branch) is S4/S5.
  static const std::unordered_map<std::string, std::string> kMap = {
      // services — endpoint redirects (config.cpp GetServiceHostWithFallback /
      // HttpConnectHook / RedirectServiceUrl)
      {"loginservice_host", "services.login"},
      {"serverdb_host", "services.serverdb"},
      {"matchingservice_host", "services.matchmaking"},
      {"apiservice_host", "services.api"},
      {"api_host", "services.api_legacy"},
      {"configservice_host", "services.config"},
      {"transactionservice_host", "services.transaction"},
      {"graph_host", "services.graph"},
      {"graphservice_host", "services.graph_service"},
      {"nevr_socket_uri", "services.socket_uri"},
      // S4b — gameserver.cpp's ServerDB DIAL URI (the token route, e.g. /nevr).
      // DISTINCT from serverdb_host -> services.serverdb (S3, the redirect HOST):
      // one is the address the gameserver dials, the other is a service-endpoint
      // override the game's URL producer rewrites through. Different keys, no
      // collision — both live under services with distinct child names.
      {"nevr_serverdb_uri", "services.serverdb_uri"},
      // identity / auth (S4a — ws_bridge login injection; S4b — gameserver auth
      // POST + ServerDB dial). Left column = the exact key the JSON readers used.
      // auth.password and auth.http_key are SECRETS: the ${VAR:?} form fails loud
      // on an unset env in server mode (see service_config.cpp NevrCfg()).
      {"nevr_discord_id", "identity.discord_id"},
      {"nevr_password", "auth.password"},
      // S4b — gameserver auth POST (AuthenticateServer) + OAuth2 refresh exchange.
      // http_uri is also read (unmigrated) by config.cpp's https redirect (S5
      // owns that residual); adding it here only routes gameserver's read.
      {"nevr_http_uri", "auth.http_uri"},
      {"nevr_http_key", "auth.http_key"},  // SECRET
      // S5 — token_auth (device-code auth) reads these three via the module
      // config accessor (ctx->config_get), not the game JSON. server_key is a
      // SECRET and, like http_key, is only ever read on a CLIENT (token_auth is
      // disabled in server mode), so it never fatals a headless server.
      {"nevr_server_key", "auth.server_key"},  // SECRET
      // network (config.cpp LoadLocalConfigHook; S4b adds regions)
      {"external_ip", "network.external_ip"},
      {"internal_ip", "network.internal_ip"},
      {"upnp", "network.upnp"},
      {"upnp_port", "network.upnp_port"},
      // S4b — nevr_regions: registration metadata appended to the ServerDB dial
      // URI (regions=...), read CSV via LookupFlatCsv. SCHEMA GAP: the plan schema
      // had no regions home; network.regions is the S4b choice (no new top-level
      // allowlist key), owner-reversible (a top-level regions:, parallel to
      // guilds:, is the arguably-more-consistent alternative).
      {"nevr_regions", "network.regions"},
      // S4b — telemetry (nevr-stream). OPTIONAL: an absent uri/token is a correct
      // disabled state, never fatal. No ${VAR:?} is forced on these.
      {"telemetry_uri", "telemetry.uri"},
      {"telemetry_token", "telemetry.token"},
      // S4b — nevr_guilds: registration metadata (guilds=...), list-shaped in the
      // schema, read CSV via LookupFlatCsv (a scalar stays a single element).
      {"nevr_guilds", "guilds"},
      // assets
      {"asset_cdn_url", "assets.cdn_url"},
      // arena rule overrides (floats)
      {"arena_round_time", "arena.round_time"},
      {"arena_celebration_time", "arena.celebration_time"},
      {"arena_mercy_score", "arena.mercy_score"},
      // misc — deprecated + inert (default TRUE, config only ever sets TRUE;
      // -noexitonerror is the live control). No first-class section is invented
      // for a deprecated flag: it rides the schema's x- extension namespace.
      {"exitonerror", "x-exitonerror"},
  };
  const auto it = kMap.find(flatKey);
  return it == kMap.end() ? std::string() : it->second;
}

std::optional<std::string> LookupFlat(const nevr::NevrConfig& cfg, const std::string& flatKey) {
  const std::string path = FlatKeyToYamlPath(flatKey);
  if (path.empty()) return std::nullopt;  // not a migrated key
  return cfg.GetString(path);
}

std::optional<std::string> LookupFlatCsv(const nevr::NevrConfig& cfg, const std::string& flatKey) {
  // For LIST-shaped keys (guilds, regions). GetStringList turns a sequence into
  // its elements and a lone scalar into a single element, so a config.yaml list
  // `[a, b]` and a scalar CSV `"a,b"` both yield the same "a,b" the game-JSON
  // readers built into `guilds=%s` / `regions=%s`. nullopt when unmapped or the
  // path is absent/empty (an empty scalar yields one empty element -> "").
  const std::string path = FlatKeyToYamlPath(flatKey);
  if (path.empty()) return std::nullopt;  // not a migrated key
  const std::vector<std::string> items = cfg.GetStringList(path);
  if (items.empty()) return std::nullopt;  // absent
  std::string joined;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) joined += ",";
    joined += items[i];
  }
  return joined;
}

ServiceHostResult ResolveServiceHost(const nevr::NevrConfig& cfg, const std::string& flatServiceKey) {
  // Primary service key.
  if (const std::optional<std::string> v = LookupFlat(cfg, flatServiceKey); v && !v->empty()) {
    return {v, HostSource::kPrimary};
  }
  // Fallback: loginservice_host.
  if (const std::optional<std::string> v = LookupFlat(cfg, "loginservice_host"); v && !v->empty()) {
    return {v, HostSource::kLoginFallback};
  }
  // Neither present -> caller uses its hardcoded default (unchanged behaviour).
  return {std::nullopt, HostSource::kNone};
}

namespace {
bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
}  // namespace

std::optional<std::string> ResolveRedirect(const std::string& result,
                                           const std::optional<std::string>& socketTarget,
                                           const std::optional<std::string>& httpTarget,
                                           bool bridgeActive, unsigned bridgePort) {
  const bool isWebSocket = StartsWith(result, "wss://") || StartsWith(result, "ws://");
  const bool isReadyAtDawn = result.find("readyatdawn.com") != std::string::npos;

  // Only ws/wss URLs (any host) or https readyatdawn.com URLs are redirected;
  // everything else passes through unchanged.
  if (!isWebSocket && !isReadyAtDawn) return std::nullopt;

  const std::optional<std::string>& target = isWebSocket ? socketTarget : httpTarget;
  if (!target || target->empty()) return std::nullopt;

  // ws/wss with the bridge up: route through the in-process relay. https never
  // hits the bridge (the game's native TLS can reach the raw http target).
  if (bridgeActive && isWebSocket) {
    return std::string("ws://127.0.0.1:") + std::to_string(bridgePort);
  }
  return *target;
}

}  // namespace nevr_cfg

// test_service_map.cpp — behaviour-equivalence tests for the config.cpp cutover
// (N133 S3). Locks the two things the migration had to preserve exactly:
//
//   1. the legacy-flat-key -> config.yaml dotted-path map (FlatKeyToYamlPath), and
//   2. the service-endpoint resolution the config.cpp hooks used to run against
//      the game JSON — host fallback (ResolveServiceHost) and scheme redirect
//      (ResolveRedirect) — including the "absent key -> unchanged default" case
//      for each of the ~10 service keys, which is HARD RISK #2 (no default drift).
//
// Pure: links service_map.cpp + nevr_core + yaml-cpp, no game stubs. Built under
// -DBUILD_TESTING=ON and run under Wine by `just test-auth-unit` (`just verify`).

#include <optional>
#include <string>

#include <gtest/gtest.h>

#include "core/nevr_config.h"
#include "runtime/lifecycle/service_map.h"

namespace {

using nevr_cfg::FlatKeyToYamlPath;
using nevr_cfg::HostSource;
using nevr_cfg::LookupFlat;
using nevr_cfg::ResolveRedirect;
using nevr_cfg::ResolveServiceHost;

// Every migrated NEVR key present, at the value config.json used to carry (the
// two ws hosts) plus representative values for the rest. Mirrors the on-disk
// echovr/_local/config.yaml sample where they overlap.
const char* kFullYaml = R"YAML(
version: "1"
services:
  login: "wss://login.example/nevr"
  serverdb: "wss://serverdb.example/nevr"
  matchmaking: "ws://127.0.0.1"
  api: "https://api.example"
  api_legacy: "https://api-legacy.example"
  config: "https://config.example"
  transaction: "https://transaction.example"
  graph: "https://graph.example"
  graph_service: "https://graphsvc.example"
  socket_uri: "ws://g.echovrce.com:80/spr"
network:
  external_ip: "154.36.247.148"
  internal_ip: "10.0.0.5"
  upnp: true
  upnp_port: 6789
assets:
  cdn_url: "https://cdn.example/assets"
arena:
  round_time: 240
  celebration_time: 12.5
  mercy_score: 17
x-exitonerror: "true"
)YAML";

// ---------------------------------------------------------------------------
// 1. The cutover map is the single source of truth for the flat->path mapping.
// ---------------------------------------------------------------------------
TEST(ServiceMap, FlatKeyToYamlPath_EveryMigratedKey) {
  // services (the ~10 endpoint keys)
  EXPECT_EQ(FlatKeyToYamlPath("loginservice_host"), "services.login");
  EXPECT_EQ(FlatKeyToYamlPath("serverdb_host"), "services.serverdb");
  EXPECT_EQ(FlatKeyToYamlPath("matchingservice_host"), "services.matchmaking");
  EXPECT_EQ(FlatKeyToYamlPath("apiservice_host"), "services.api");
  EXPECT_EQ(FlatKeyToYamlPath("api_host"), "services.api_legacy");
  EXPECT_EQ(FlatKeyToYamlPath("configservice_host"), "services.config");
  EXPECT_EQ(FlatKeyToYamlPath("transactionservice_host"), "services.transaction");
  EXPECT_EQ(FlatKeyToYamlPath("graph_host"), "services.graph");
  EXPECT_EQ(FlatKeyToYamlPath("graphservice_host"), "services.graph_service");
  EXPECT_EQ(FlatKeyToYamlPath("nevr_socket_uri"), "services.socket_uri");
  // identity / auth (S4a — ws_bridge login injection)
  EXPECT_EQ(FlatKeyToYamlPath("nevr_discord_id"), "identity.discord_id");
  EXPECT_EQ(FlatKeyToYamlPath("nevr_password"), "auth.password");
  // network
  EXPECT_EQ(FlatKeyToYamlPath("external_ip"), "network.external_ip");
  EXPECT_EQ(FlatKeyToYamlPath("internal_ip"), "network.internal_ip");
  EXPECT_EQ(FlatKeyToYamlPath("upnp"), "network.upnp");
  EXPECT_EQ(FlatKeyToYamlPath("upnp_port"), "network.upnp_port");
  // assets / arena / misc
  EXPECT_EQ(FlatKeyToYamlPath("asset_cdn_url"), "assets.cdn_url");
  EXPECT_EQ(FlatKeyToYamlPath("arena_round_time"), "arena.round_time");
  EXPECT_EQ(FlatKeyToYamlPath("arena_celebration_time"), "arena.celebration_time");
  EXPECT_EQ(FlatKeyToYamlPath("arena_mercy_score"), "arena.mercy_score");
  EXPECT_EQ(FlatKeyToYamlPath("exitonerror"), "x-exitonerror");
}

TEST(ServiceMap, FlatKeyToYamlPath_UnmigratedKeysAreEmpty) {
  // nevr_http_uri is deliberately NOT migrated in S4a (gameserver.cpp / S4b + S5
  // own it): the https branch of RedirectServiceUrl and the auth POST must keep
  // reading the game JSON until then.
  EXPECT_EQ(FlatKeyToYamlPath("nevr_http_uri"), "");
  // S4b/S5 keys and anything unknown are not this step's business. (nevr_discord_id
  // and nevr_password moved to the migrated set above in S4a.)
  EXPECT_EQ(FlatKeyToYamlPath("nevr_serverdb_uri"), "");
  EXPECT_EQ(FlatKeyToYamlPath("telemetry_uri"), "");
  EXPECT_EQ(FlatKeyToYamlPath("telemetry_token"), "");
  EXPECT_EQ(FlatKeyToYamlPath("bogus_key"), "");
  EXPECT_EQ(FlatKeyToYamlPath(""), "");
}

// ---------------------------------------------------------------------------
// 2. Every migrated key present resolves to its configured value.
// ---------------------------------------------------------------------------
TEST(ServiceMap, LookupFlat_PresentKeysResolve) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(kFullYaml);
  EXPECT_EQ(LookupFlat(cfg, "matchingservice_host").value_or(""), "ws://127.0.0.1");
  EXPECT_EQ(LookupFlat(cfg, "nevr_socket_uri").value_or(""), "ws://g.echovrce.com:80/spr");
  EXPECT_EQ(LookupFlat(cfg, "serverdb_host").value_or(""), "wss://serverdb.example/nevr");
  EXPECT_EQ(LookupFlat(cfg, "api_host").value_or(""), "https://api-legacy.example");
  EXPECT_EQ(LookupFlat(cfg, "external_ip").value_or(""), "154.36.247.148");
  EXPECT_EQ(LookupFlat(cfg, "upnp").value_or(""), "true");
  EXPECT_EQ(LookupFlat(cfg, "upnp_port").value_or(""), "6789");
  EXPECT_EQ(LookupFlat(cfg, "arena_round_time").value_or(""), "240");
  EXPECT_EQ(LookupFlat(cfg, "exitonerror").value_or(""), "true");
  // Unmigrated key -> nullopt regardless of the config content.
  EXPECT_FALSE(LookupFlat(cfg, "nevr_http_uri").has_value());
}

TEST(ServiceMap, LookupFlat_AbsentKeyIsNullopt) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString("version: \"1\"\n");
  EXPECT_FALSE(LookupFlat(cfg, "matchingservice_host").has_value());
  EXPECT_FALSE(LookupFlat(cfg, "external_ip").has_value());
}

// ---------------------------------------------------------------------------
// S4a — ws_bridge login-injection keys resolve through the same flat->path map.
// The discord_id fixture is a SHORT fake on purpose: an 18-20 digit literal in
// src/ trips the N20 identity-literal sensor (justfile). The real id lives only
// in echovr/_local/config.yaml.
// ---------------------------------------------------------------------------
TEST(ServiceMap, LookupFlat_IdentityAndAuthResolve) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(
      "identity:\n  discord_id: \"1234567890\"\nauth:\n  password: \"s3cr3t-pw\"\n");
  EXPECT_EQ(LookupFlat(cfg, "nevr_discord_id").value_or(""), "1234567890");
  EXPECT_EQ(LookupFlat(cfg, "nevr_password").value_or(""), "s3cr3t-pw");
}

TEST(ServiceMap, LookupFlat_IdentityAndAuthAbsentIsNullopt) {
  // Absent identity/auth -> nullopt, so ws_bridge keeps its no-injection default
  // (guard on cfgDiscordId[0]/cfgPassword[0]) — Hazard C, no default change.
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString("version: \"1\"\n");
  EXPECT_FALSE(LookupFlat(cfg, "nevr_discord_id").has_value());
  EXPECT_FALSE(LookupFlat(cfg, "nevr_password").has_value());
}

// ---------------------------------------------------------------------------
// 3. Service host fallback chain: primary -> loginservice_host -> default.
//    The kNone case is HARD RISK #2 — an absent key must resolve to the SAME
//    (unchanged) default it does today, which the adapter delivers by returning
//    its own defaultUrl when source == kNone.
// ---------------------------------------------------------------------------
TEST(ServiceMap, ResolveServiceHost_PrimaryPresent) {
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString(kFullYaml);
  const auto r = ResolveServiceHost(cfg, "matchingservice_host");
  EXPECT_EQ(r.source, HostSource::kPrimary);
  EXPECT_EQ(r.value.value_or(""), "ws://127.0.0.1");
}

TEST(ServiceMap, ResolveServiceHost_FallsBackToLogin) {
  // Only services.login present; a specific service key falls back to it.
  const nevr::NevrConfig cfg =
      nevr::NevrConfig::LoadFromString("services:\n  login: \"wss://login.example/nevr\"\n");
  const auto r = ResolveServiceHost(cfg, "serverdb_host");
  EXPECT_EQ(r.source, HostSource::kLoginFallback);
  EXPECT_EQ(r.value.value_or(""), "wss://login.example/nevr");

  // loginservice_host itself hits its own key as the primary, not the fallback.
  const auto r2 = ResolveServiceHost(cfg, "loginservice_host");
  EXPECT_EQ(r2.source, HostSource::kPrimary);
  EXPECT_EQ(r2.value.value_or(""), "wss://login.example/nevr");
}

TEST(ServiceMap, ResolveServiceHost_AbsentEveryServiceKeyYieldsDefault) {
  // Empty config: EVERY service key must resolve to kNone (caller keeps its own
  // hardcoded default — no default drift). This is the absent-key half of the
  // task's "each service key present -> value, each absent -> today-default".
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString("version: \"1\"\n");
  for (const char* key : {"loginservice_host", "serverdb_host", "matchingservice_host",
                          "apiservice_host", "api_host", "configservice_host",
                          "transactionservice_host", "graph_host", "graphservice_host",
                          "nevr_socket_uri"}) {
    const auto r = ResolveServiceHost(cfg, key);
    EXPECT_EQ(r.source, HostSource::kNone) << "service key resolved to a non-default: " << key;
    EXPECT_FALSE(r.value.has_value()) << key;
  }
}

TEST(ServiceMap, ResolveServiceHost_EmptyStringIsNotAnOverride) {
  // A present-but-empty value must not count as an override (matches the old
  // `host[0] != '\0'` check), so it falls through to login, then to default.
  const nevr::NevrConfig cfg = nevr::NevrConfig::LoadFromString("services:\n  matchmaking: \"\"\n");
  const auto r = ResolveServiceHost(cfg, "matchingservice_host");
  EXPECT_EQ(r.source, HostSource::kNone);
}

// ---------------------------------------------------------------------------
// 4. Scheme redirect (RedirectServiceUrl core).
// ---------------------------------------------------------------------------
TEST(ServiceMap, ResolveRedirect_WsBridgeInactivePassesTarget) {
  const auto r = ResolveRedirect("wss://login.readyatdawn.com/rad/rad15_live",
                                 std::optional<std::string>("ws://g.echovrce.com:80/spr"),
                                 std::nullopt, /*bridgeActive=*/false, /*bridgePort=*/0);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, "ws://g.echovrce.com:80/spr");
}

TEST(ServiceMap, ResolveRedirect_WsBridgeActiveRewritesToLoopback) {
  // The behaviour the baseline run showed: a ws/wss URL with the bridge up is
  // rewritten to ws://127.0.0.1:<port>, whatever the target host was.
  for (const char* url : {"wss://login.readyatdawn.com/rad/rad15_live",
                          "ws://127.0.0.1:53748"}) {
    const auto r = ResolveRedirect(url, std::optional<std::string>("ws://g.echovrce.com:80/spr"),
                                   std::nullopt, /*bridgeActive=*/true, /*bridgePort=*/53748);
    ASSERT_TRUE(r.has_value()) << url;
    EXPECT_EQ(*r, "ws://127.0.0.1:53748") << url;
  }
}

TEST(ServiceMap, ResolveRedirect_HttpsReadyAtDawnUsesHttpTargetNotBridge) {
  // https readyatdawn.com redirects to the http target and NEVER hits the bridge,
  // even when the bridge is active.
  const auto r = ResolveRedirect("https://config.readyatdawn.com/rad/rad15_live", std::nullopt,
                                 std::optional<std::string>("https://g.echovrce.com:7350"),
                                 /*bridgeActive=*/true, /*bridgePort=*/53748);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, "https://g.echovrce.com:7350");
}

TEST(ServiceMap, ResolveRedirect_UnrelatedUrlUnchanged) {
  // Not ws/wss and not readyatdawn.com -> leave it alone.
  EXPECT_FALSE(ResolveRedirect("https://api.somewhere.com/v1", std::nullopt,
                               std::optional<std::string>("https://g.echovrce.com:7350"), true, 1)
                   .has_value());
}

TEST(ServiceMap, ResolveRedirect_NoTargetLeavesUnchanged) {
  // A redirect-eligible URL but no configured target -> unchanged (never send an
  // empty/garbage host).
  EXPECT_FALSE(ResolveRedirect("wss://login.readyatdawn.com/x", std::nullopt, std::nullopt, false, 0)
                   .has_value());
  EXPECT_FALSE(ResolveRedirect("wss://login.readyatdawn.com/x", std::optional<std::string>(""),
                               std::nullopt, false, 0)
                   .has_value());
}

}  // namespace

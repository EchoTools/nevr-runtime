#include "runtime/server/gameserver.h"
#include "core/hex_dump.h"
#include "core/curl_global.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "core/auth_token.h"
#include "auth_token_refresh.h"
#include "runtime/lifecycle/return_to_lobby.h"
#include "runtime/server/constants.h"
#include "runtime/server/failure_detail.h"
#include "runtime/server/protobuf_transport.h"
#include "runtime/server/registration_envelope.h"
#include "runtime/server/serialized_mint.h"
#include "runtime/server/serverdb_uri.h"
#include "runtime/server/session_success_dispatch.h"
#include "runtime/server/session_unregister.h"
#include "runtime/server/callback_unregistration.h"
#include "runtime/log/security_diagnostics.h"
#include "runtime/log/url_diagnostics.h"
#include "abi/echovr.h"
#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "core/login_session.h"
#include "core/build_identity.h"  // N112: NEVR build identity
#include "runtime/server/messages.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/service_config.h"  // NevrCfgGetFlat / NevrCfgGetFlatCsv (N133 S4b: config.yaml reads)
#include "runtime/compat/ws_bridge.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/hook/patching.h"
#include "nevr_curl.h"
#include "core/pch.h"
#include "runtime/server/upnp.h"
#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/gameserver_internal.h"

#include "core/logging.h"

using namespace GameServer;

// ServerDB socket: server authentication, token acquisition, registration and unregistration (#47).

// Authenticate the server with Nakama using the operator's discord_id + password.
// Returns a session access JWT, or empty string on failure.
//
// Uses the non-interactive password RPC (docs/adr/0001-serverdb-token-auth.md):
//   POST {nevr_http_uri}/v2/rpc/account/authenticate/password?http_key=<key>&unwrap
//   body {"discord_id","password"} -> {"token","refresh_token"}
// (nakama server/evr_runtime_rpc.go:1137 AuthenticatePasswordRPC, RequireAuth:false;
//  http_key is the transport credential, not server_key Basic auth.) The access
// token's uid is the operator's discord-linked account, which carries the
// server-host role checked at registration (gg.IsServerHost). Verified live
// 2026-06-29: uid=metis.sprock, access token (no vrs.refresh), TTL ~1h.
static std::string AuthenticateServer(std::string& reason) {
    // N133 S4b: config.yaml (nevr_config), not the game JSON. auth.http_key is a
    // SECRET; its ${VAR:?} form fails loud at config load in server mode.
    const char* httpUri = NevrCfgGetFlat("nevr_http_uri");
    const char* httpKey = NevrCfgGetFlat("nevr_http_key");
    const char* discordId = NevrCfgGetFlat("nevr_discord_id");
    const char* password = NevrCfgGetFlat("nevr_password");

    if (!httpUri || !httpKey || !discordId || !password ||
        httpUri[0] == '\0' || httpKey[0] == '\0' || discordId[0] == '\0' || password[0] == '\0') {
        std::string missingKeysCsv;
        if (!httpUri || httpUri[0] == '\0') missingKeysCsv += "nevr_http_uri,";
        if (!httpKey || httpKey[0] == '\0') missingKeysCsv += "nevr_http_key,";
        if (!discordId || discordId[0] == '\0') missingKeysCsv += "nevr_discord_id,";
        if (!password || password[0] == '\0') missingKeysCsv += "nevr_password,";
        if (!missingKeysCsv.empty()) missingKeysCsv.pop_back();  // drop trailing comma
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.GAMESERVER] cannot authenticate — missing config keys: %s", missingKeysCsv.c_str());
        reason = "password auth: missing config keys " + missingKeysCsv;
        return "";
    }

    std::string url = std::string(httpUri) +
        "/v2/rpc/account/authenticate/password?http_key=" + httpKey + "&unwrap";
    nlohmann::json body;
    body["discord_id"] = discordId;
    body["password"] = password;

    nevr::EnsureCurlGlobalInit();
    CURL* curl = curl_easy_init();
    if (!curl) {
        reason = "password auth: curl_easy_init failed";
        return "";
    }

    std::string response;
    std::string postData = body.dump();
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nevr::CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
#ifdef NEVR_INSECURE_SKIP_TLS_VERIFY
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
#endif

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        const std::string diagnostic =
            LogDiagnostics::FormatCurlFailureDiagnostic("[NEVR.GAMESERVER] Server auth failed ", static_cast<int>(res));
        Log(EchoVR::LogLevel::Warning, "%s", diagnostic.c_str());
        reason = FailureDetail::PasswordAuthRequestFailed(httpUri, curl_easy_strerror(res), static_cast<int>(res));
        return "";
    }

    if (http_code != 200) {
        LogDiagnostics::LogHttpResponseSummary(EchoVR::LogLevel::Warning,
                                               "[NEVR.GAMESERVER] Server auth rejected ", http_code, response);
        reason = FailureDetail::PasswordAuthHttpStatus(httpUri, http_code);
        return "";
    }

    std::string token = FailureDetail::ExtractAuthToken(response, reason);
    if (token.empty()) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Server auth response carried no usable token");
    } else {
        Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Server authenticated (token acquired)");
    }
    return token;
}

// Mints a ServerDB access token: refresh-token exchange first, password auth as
// the fallback. Returns "" when neither produced one. Called from
// RequestRegistration (game thread) and, since #39, from the WebSocketClient's
// token refresher on ixwebsocket's thread after ServerDB answers 401. Config
// reads go through NevrCfgGetFlat's mutex-guarded intern pool. RefreshAuthToken
// also rewrites the on-disk credential cache (auth_token_refresh.h SaveAuthToken)
// without a lock, and three callers can mint at once (this function on the game
// thread, the ServerDB socket's 401 refresher, the telemetry socket's), so the
// whole mint runs under ServerDbAuth::RunSerializedMint.
static std::string AcquireServerDbTokenUnserialized(std::string& reason);

std::string AcquireServerDbToken(std::string& reason) {
    return ServerDbAuth::RunSerializedMint([&reason]() { return AcquireServerDbTokenUnserialized(reason); });
}

static std::string AcquireServerDbTokenUnserialized(std::string& reason) {
    std::string token;
    auto auth = LoadCachedAuthToken();
    if (auth.HasValidToken()) {
        token = auth.token;
        Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Using cached auth token for ServerDB");
    } else if (auth.HasValidRefreshToken()) {
        // N106: exchange the refresh token for an access token.
        //
        // This branch was MISSING, which made the OAuth2 device-flow path
        // unreachable on a dedicated server. 571a41b ("access token in-memory
        // only, 60s lifetime, refresh token on disk") stopped persisting the
        // access token — SaveAuthToken writes only refresh_token — so
        // LoadCachedAuthToken().token is ALWAYS empty and HasValidToken() is
        // ALWAYS false in a fresh process. The consumer here was never updated to
        // match, so every server fell through to password auth while a perfectly
        // valid refresh token sat unused on disk.
        //
        // Nothing else refreshes in server mode either: nevr_token_auth::Init returns
        // early on is_server, before the background refresh thread starts. This
        // function is the only place a server can mint an access token.
        const char* httpUri = NevrCfgGetFlat("nevr_http_uri");
        const char* httpKey = NevrCfgGetFlat("nevr_http_key");
        if (httpUri && httpKey && httpUri[0] != '\0' && httpKey[0] != '\0' &&
            RefreshAuthToken(auth, httpUri, httpKey)) {
            token = auth.token;
            Log(EchoVR::LogLevel::Info,
                "[NEVR.GAMESERVER] ServerDB auth via refreshed OAuth2 token (device-flow credential)");
        } else {
            Log(EchoVR::LogLevel::Warning,
                "[NEVR.GAMESERVER] Refresh token present but exchange failed — falling back to password auth");
            reason = "refresh-token exchange failed; ";
        }
    }

    if (token.empty()) {
        std::string passwordReason;
        token = AuthenticateServer(passwordReason);
        reason += passwordReason;
    }
    return token;
}

// N133 S4b: NEVR keys are read from config.yaml (nevr_config), not from the game-passed
// config JSON. The IServerLib vtable slot passes the game's localConfig pointer, but the
// runtime does not read NEVR keys from it, so the parameter is intentionally unnamed.
VOID GameServerLib::RequestRegistration(INT64 serverId, CHAR*, EchoVR::SymbolId regionId, EchoVR::SymbolId versionLock,
                                        const EchoVR::Json*) {
  // Update session state
  SessionState state = m_context->GetSessionState();
  state.serverId = serverId;
  state.regionId = regionId;
  state.versionLock = versionLock;
  m_context->UpdateSessionState(state);

  // Issue #21: the engine derives these from its own config, and version_lock is
  // what ServerDB groups servers by. Logged so a config change (e.g. an absent or
  // different _local/config.json) can be checked for a changed registration
  // identity from the log alone, instead of by inference.
  Log(EchoVR::LogLevel::Info,
      "[NEVR.GAMESERVER] RequestRegistration server_id=%lld region=0x%016llx version_lock=0x%016llx",
      static_cast<long long>(serverId), static_cast<unsigned long long>(regionId),
      static_cast<unsigned long long>(versionLock));

  // Get serverdb URI from config. If not explicitly set, construct from
  // nevr_socket_uri + nevr_discord_id + nevr_password (the common config pattern).
  const char* serverDbUri = NevrCfgGetFlat("serverdb_host");

  // Acquire a session JWT for the operator's server-host account (token auth, BAC-1).
  // Re-auth each registration: the access token TTL is ~1h (BAC-5).
  std::string tokenFailureReason;
  std::string wsToken = AcquireServerDbToken(tokenFailureReason);
  // N102: no token means every ServerDB connection below will be rejected.
  // Continuing produces a server that logs connection failures forever
  // instead of exiting with a cause.
  if (wsToken.empty()) {
    const std::string message = FailureDetail::WithCause(
        "Server authentication failed — no valid token for ServerDB connection", tokenFailureReason);
    ServerFatal("%s", message.c_str());
  }

  // Owns the constructed URI for the rest of this call; Connect() copies it
  // (websocket_client.cpp setUrl(std::string(uri))).
  std::string constructedUri;
  if (!serverDbUri || serverDbUri[0] == '\0') {
    const auto orEmpty = [](const char* value) { return value ? std::string_view(value) : std::string_view(); };
    // guilds/regions are list-shaped registration metadata; read CSV so a yaml
    // list `[a, b]` and a scalar CSV both build the same guilds=/regions= param.
    const char* guilds = NevrCfgGetFlatCsv("nevr_guilds");
    const char* regions = NevrCfgGetFlatCsv("nevr_regions");
    // Token auth is opt-in: only when nevr_serverdb_uri (the token route, e.g. /nevr) is
    // configured AND a token was acquired. Otherwise fall back to the legacy url-param
    // path so a deploy that hasn't set nevr_serverdb_uri keeps working (no footgun).
    const char* tokenUri = NevrCfgGetFlat("nevr_serverdb_uri");

    if (tokenUri && tokenUri[0] != '\0' && !wsToken.empty()) {
      // Token auth (BAC-2): identity via the Bearer JWT (sent by Connect()); discord_id/
      // password dropped; guilds/regions stay as registration metadata. nevr_serverdb_uri
      // points at the token route that forwards the real Authorization header
      // (docs/adr/0001-serverdb-token-auth.md).
      // Issue #41: query values are percent-encoded by ServerDbUri, not snprintf.
      std::optional<std::string> built =
          ServerDbUri::BuildTokenRouteUri(tokenUri, orEmpty(guilds), orEmpty(regions));
      if (!built) {
        Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] could not percent-encode the token-route serverdb URI");
        ServerFatal("Could not build the ServerDB URI (token route)");
        return;
      }
      constructedUri = std::move(*built);
      serverDbUri = constructedUri.c_str();
      const std::string diagnostic = LogDiagnostics::FormatRedactedUrlDiagnostic(
          "[NEVR.GAMESERVER] constructed serverdb URI for token auth: ", constructedUri);
      Log(EchoVR::LogLevel::Debug, "%s", diagnostic.c_str());
    } else {
      // Legacy url-param auth (no nevr_serverdb_uri configured): connect via
      // nevr_socket_uri with discord_id+password — the pre-token-auth behavior.
      const char* socketUri = NevrCfgGetFlat("nevr_socket_uri");
      const char* discordId = NevrCfgGetFlat("nevr_discord_id");
      const char* password = NevrCfgGetFlat("nevr_password");
      if (socketUri && socketUri[0] != '\0' && discordId && discordId[0] != '\0') {
        // Issue #41: every value is percent-encoded, so a password containing
        // '&', '=', '#', '%', '+' or whitespace can no longer rewrite the query.
        std::optional<std::string> built = ServerDbUri::BuildLegacyUri(
            socketUri, discordId, orEmpty(password), orEmpty(guilds), orEmpty(regions));
        if (!built) {
          Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] could not percent-encode the legacy serverdb URI");
          ServerFatal("Could not build the ServerDB URI (legacy url-param auth)");
          return;
        }
        constructedUri = std::move(*built);
        serverDbUri = constructedUri.c_str();
        // Do NOT log constructedUri here — this branch embeds the operator's
        // password in the query string. Presence only, never the value.
        Log(EchoVR::LogLevel::Debug,
            "[NEVR.GAMESERVER] constructed serverdb URI (legacy url-param auth, percent-encoded): "
            "discord_id=%s password=%s",
            discordId, (password && password[0] != '\0') ? "present (redacted)" : "absent");
      } else {
        serverDbUri = "ws://localhost:777/serverdb";
        const std::string diagnostic = LogDiagnostics::FormatRedactedUrlDiagnostic(
            "[NEVR.GAMESERVER] No nevr_serverdb_uri/nevr_socket_uri — using default serverdb URI: ", serverDbUri);
        Log(EchoVR::LogLevel::Warning, "%s", diagnostic.c_str());
      }
    }
  }

  // #39: the header is stored once, and ixwebsocket's automatic reconnect
  // re-presents it. Once the ~1h token has expired, ServerDB answers every
  // reconnect with 401; this lets the client mint a fresh token instead.
  m_wsClient->SetBearerTokenRefresher([]() {
    std::string reason;  // the refresher has no operator to tell; each step already logged its cause
    return AcquireServerDbToken(reason);
  });

  // Connect with the JWT as Authorization: Bearer; the token route forwards it
  // to Nakama's acceptor, which sets the operator identity (BAC-2/3).
  if (!m_wsClient->Connect(serverDbUri, wsToken)) {
    // serverDbUri may be the password-bearing legacy-auth URI at this point
    // (see the constructedUri branch above) — redact before logging.
    const std::string diagnostic = LogDiagnostics::FormatRedactedUrlDiagnostic(
        "[NEVR.GAMESERVER] failed to initiate WebSocket connection uri=", serverDbUri ? serverDbUri : "");
    Log(EchoVR::LogLevel::Error, "%s", diagnostic.c_str());
    return;
  }

  // Build registration request
  auto* broadcaster = m_context->GetBroadcaster();
  if (!broadcaster || !broadcaster->data) {
    Log(EchoVR::LogLevel::Error, "[NEVR.GAMESERVER] broadcaster unavailable — initial registration aborted");
    return;
  }

  sockaddr_in gameServerAddr = *reinterpret_cast<sockaddr_in*>(&broadcaster->data->addr);
  uint16_t broadcasterPort = broadcaster->data->broadcastSocketInfo.port;

  // Resolve IP addresses and apply UPnP / config overrides
  std::string internalIp = Ipv4ToString(gameServerAddr.sin_addr.S_un.S_addr);
  std::string externalIp;  // empty = let UPnP fill it, or falls back to internalIp

  NevrUPnPConfig upnpCfg = {};
  if (ReadUPnPConfig(upnpCfg)) {
    if (upnpCfg.internalIp[0] != '\0') internalIp = upnpCfg.internalIp;
    if (upnpCfg.externalIp[0] != '\0') externalIp = upnpCfg.externalIp;

    if (upnpCfg.enabled) {
      uint16_t extPort = (upnpCfg.port != 0) ? upnpCfg.port : broadcasterPort;
      if (UPnPHelper::OpenPort(broadcasterPort, extPort, externalIp)) {
        broadcasterPort = extPort;
      } else {
        Log(EchoVR::LogLevel::Warning,
            "[NEVR.GAMESERVER] UPnP port mapping failed (internal=%u external=%u) — using raw broadcaster port %u",
            broadcasterPort, extPort, broadcasterPort);
      }
    } else {
      // N122. The disabled branch was SILENT, so a server that never attempted a
      // mapping looked identical in the log to one where the attempt was never
      // reached at all. Registration announces this port to ServerDB either way,
      // so whether it was forwarded is exactly the thing an operator needs to know.
      Log(EchoVR::LogLevel::Info,
          "[NEVR.UPNP] disabled — announcing raw broadcaster port %u, no mapping attempted "
          "(set \"upnp\" in config or pass -upnp)", broadcasterPort);
    }
  }

  if (externalIp.empty()) externalIp = internalIp;

  // Build protobuf registration request
  const nevr_build_identity::Info& buildId = nevr_build_identity::Get();  // N112: commit hash and build type in the version
  GameServer::RegistrationParams params;
  params.loginSessionId = GuidToUuidString(nevr_login_session::Get());
  params.serverId = static_cast<uint64_t>(serverId);
  params.externalIp = externalIp;  // public-facing IP
  params.port = static_cast<uint32_t>(broadcasterPort);
  params.regionId = regionId;
  params.versionLock = versionLock;
  params.timeStepUsecs = state.defaultTimeStepUsecs;
  params.version = GameServer::FormatRegistrationVersion(buildId.git_describe, buildId.git_commit, buildId.build_type);
  const gameservice::v1::Envelope envelope = GameServer::BuildRegistrationEnvelope(params);

  if (!SendProtobufEnvelope(this, envelope)) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] protobuf serialize failed for initial registration");
  }

  ConnectTelemetry(wsToken);

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] requested game server registration server_id=%lld region=0x%llX",
      static_cast<long long>(serverId), static_cast<unsigned long long>(regionId));
}

VOID GameServerLib::Unregister() {
  // IServerLib entry point: the game calls this on its own thread.
  UnregisterFromServerDb(true);
}

void GameServerLib::ShutdownUnregisterOnGameThread() {
  EndSession();
  UnregisterFromServerDb(true);
}

void GameServerLib::ShutdownUnregisterOffGameThread() {
  // Everything here is safe off the game thread: ServerContext state is
  // mutex-guarded and WebSocketClient is documented thread-safe. The callback
  // registry is not, so it is skipped (GH #44).
  EndSession();
  UnregisterFromServerDb(false);
}

void GameServerLib::UnregisterFromServerDb(bool touchCallbackRegistry) {
  const auto sendEnvelope = [this](const gameservice::v1::Envelope& envelope) {
    return GameServer::SendProtobufEnvelope(*m_wsClient, envelope);
  };
  GameServer::ServerLifecycleAction unregisterCallbacks;
  if (touchCallbackRegistry) unregisterCallbacks = [this]() { UnregisterAllCallbacks(); };
  const auto endResult = GameServer::UnregisterRegisteredServer(
      *m_context, sendEnvelope, [this]() { m_wsClient->DiscardPendingMessages(); }, unregisterCallbacks,
      [this]() { m_wsClient->Disconnect(); });
  if (endResult.attempted && endResult.sendResult == GameServer::ProtobufSendResult::TransportRejected) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.SERVER] CODE_ENDED transport rejected during unregister");
  } else if (endResult.sendResult == GameServer::ProtobufSendResult::AcceptedQueued) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.SERVER] CODE_ENDED was queued; ServerDB delivery is unconfirmed");
  }

  // Remove UPnP port mapping if we added one
  UPnPHelper::ClosePort();

  // Disconnect telemetry before unregistering
  if (m_telemetry) {
    m_telemetry->Stop();
    m_telemetry->Disconnect();
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] Unregistered game server");
}

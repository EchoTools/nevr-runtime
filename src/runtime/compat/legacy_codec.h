#pragma once
// Translator between the message family a legacy game client speaks (the b2 lobby builds: Summer 2019 and
// Halloween 2018) and the current family the game service speaks.
//
// The game service only ever sees current messages. The ws_bridge sits between it and a legacy game client
// and rewrites one message at a time through this file (nevr-runtime #417).
//
// Like evr_codec.h it is platform neutral: no Windows headers, no sockets, no logging, no game state. What a
// legacy message does not carry (the login session, the channel, the build's symbol tables) is passed in
// through Context and BuildTables, so every translation is a pure function of its arguments.
//
// Layout sources are listed per row in docs/reference/legacy-translator.md. A row whose layout is not
// established is not translated; it comes back as Outcome::Unsupported.
//
// Integers are little endian. A GUID is 16 raw bytes and is the same in both families.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/compat/evr_codec.h"

namespace nevr_legacy_codec {

using Guid = std::array<uint8_t, 16>;
using nevr_evr_codec::UserId;

// ---- legacy symbols ---------------------------------------------------------------------------
// Named as the game names them; each constant is the CSymbol64 of its name (checked in the tests).
inline constexpr uint64_t kSNSLoginRequest = 0xa5add1bb1b0cce40ULL;
inline constexpr uint64_t kSNSLoginProfileResult = 0x236ccbefa38074c0ULL;
inline constexpr uint64_t kSNSLoginClientSettings = 0x208da2538a66a18dULL;
inline constexpr uint64_t kSNSRefreshProfile = 0x3aff685d5e06bc6dULL;
inline constexpr uint64_t kSNSRefreshProfileResult = 0xb48e249377fe5e02ULL;
inline constexpr uint64_t kSNSProfileRequestv2 = 0x7a24e0b22443b982ULL;
inline constexpr uint64_t kSNSProfileRequest = 0xc9b31861e72836daULL;
inline constexpr uint64_t kSNSProfileResponsev2 = 0x2261fd8d8f56fad6ULL;
inline constexpr uint64_t kSNSProfileResponse = 0xb852ca51d02dbadfULL;
inline constexpr uint64_t kSNSLeaderboardRequest = 0x0d9ba32b0b8e9eecULL;
inline constexpr uint64_t kSNSLeaderboardResponse = 0x4757a48a7af6aef7ULL;
inline constexpr uint64_t kSNSMatchEnded = 0xc7ba60cd3bc9ee9cULL;
inline constexpr uint64_t kSNSMatchEndedv2 = 0x80119c19ac72d692ULL;
inline constexpr uint64_t kSNSLobbyFindSessionRequestv8 = 0x2a56739e56e6fb7aULL;
inline constexpr uint64_t kSNSLobbyCreateSessionRequestv7 = 0x599a6b1bbda3cc1dULL;
inline constexpr uint64_t kSNSLobbyJoinSessionRequestv6 = 0x2f03468f77ffb210ULL;
inline constexpr uint64_t kSNSLobbyPlayerSessionsRequestv3 = 0x9af2fab2a0c81a03ULL;
inline constexpr uint64_t kSNSLobbyPendingSessionCancel = 0x70f2da850f25105aULL;
inline constexpr uint64_t kSNSLobbySessionSuccessv4 = 0x6d4de3650ee3110eULL;
inline constexpr uint64_t kSNSLobbySessionFailurev3 = 0x4ae8365ebc45f96bULL;
inline constexpr uint64_t kSNSLobbySessionFailurev2 = 0x4ae8365ebc45f96aULL;

// ---- current symbols the translator needs beyond evr_codec.h (CSymbol64 of the name) -----------
inline constexpr uint64_t kSNSLobbySessionSuccessv5 = 0x6d4de3650ee3110fULL;
inline constexpr uint64_t kSNSLobbySessionFailurev4 = 0x4ae8365ebc45f96cULL;
inline constexpr uint64_t kSNSConfigSuccessv2 = 0xb9cdaf586f7bd012ULL;
inline constexpr uint64_t kSNSReconcileIAP = 0x1bd0fc454c85573cULL;
inline constexpr uint64_t kSNSReconcileIAPResult = 0x0dabc24265508a82ULL;
inline constexpr uint64_t kSNSLobbyMatchmakerStatus = 0x8f28cf33dabfbecbULL;
inline constexpr uint64_t kSNSLobbyPlayerSessionsSuccessv3 = 0xa1b9cae1f8588969ULL;

// The result byte of the legacy login reply (SNSLoginProfileResult). Values learned from reading EchoRelay's
// message documentation; unverified against a binary.
inline constexpr uint8_t kLoginAccepted = 0x0B;
inline constexpr uint8_t kLoginBadRequest = 0;
inline constexpr uint8_t kLoginCredentialsRejected = 7;
inline constexpr uint8_t kLoginBlocked = 8;

// ---- what the caller knows --------------------------------------------------------------------

// The symbols that differ between the two generations of the b2 lobby message set, plus the build's own
// symbol dictionary. The package carries its table; the codec never works out which build it is talking to.
struct BuildTables {
  uint64_t loginSettingsSymbol = nevr_evr_codec::kSymLoginSettings;  // SNSLoginSettings or SNSLoginClientSettings
  uint64_t profileRequestSymbol = kSNSProfileRequestv2;
  uint64_t profileResponseSymbol = kSNSProfileResponsev2;
  uint64_t matchEndedSymbol = kSNSMatchEndedv2;
  uint64_t sessionFailureSymbol = kSNSLobbySessionFailurev3;  // v3 carries the game type, v2 does not
  // Legacy symbol -> current symbol. An unlisted symbol passes through unchanged in both directions.
  std::vector<std::pair<uint64_t, uint64_t>> modes;
  std::vector<std::pair<uint64_t, uint64_t>> levels;
  std::vector<std::pair<uint64_t, uint64_t>> platforms;
};

// The b2 lobby message set whose profile messages carry the v2 symbols, and the one that carries the original
// (v1) profile symbols. Neither includes a per-build dictionary.
BuildTables LobbyB2ProfileV2Tables();
BuildTables LobbyB2ProfileV1Tables();

// What a legacy message does not carry and the bridge already knows.
struct Context {
  Guid loginSession{};  // from LoginSuccess; zero before it
  UserId self;          // the logged-in account
  Guid channel{};       // the channel of the last find/create, for the reply direction
  // Which encoder-flag layout the game service writes in SessionSuccess: false = the PC layout a legacy game
  // client reads; true = the Quest layout, which is converted.
  bool currentUsesQuestFlags = false;
  // When non-empty, replaces the JSON of the legacy login request with the bridge's own profile (which is
  // where the game client's version is reported). Empty keeps the JSON the game sent.
  std::string loginProfileJson;
  // The request JSON carried by the current-family profile requests; it names the fields the game service returns.
  std::string profileRequestJson = "{}";
};

// Which legacy reply a current-family profile answer becomes. The bridge knows which request is outstanding.
enum class ProfileReply {
  LoginProfileResult,    // answers the login
  RefreshProfileResult,  // answers SNSRefreshProfile
  ProfileResponse,       // answers SNSProfileRequest[v2]
};

// ---- results ----------------------------------------------------------------------------------

struct OutMessage {
  uint64_t symbol = 0;
  std::string payload;
};

enum class Outcome {
  Translated,   // the messages in toGameService / toGameClient replace the input
  Passthrough,  // same symbol and layout in both families; forward the input unchanged
  Dropped,      // consumed; forwarding it would make the game service discard the rest of its packet
  Local,        // the bridge answers the game client itself; the reply is in toGameClient
  Unsupported,  // the symbol is not a row of this translator
  Malformed,    // the symbol is a row but the payload does not parse as its layout
};

struct Translation {
  Outcome outcome = Outcome::Unsupported;
  std::vector<OutMessage> toGameService;  // to the game service (current family)
  std::vector<OutMessage> toGameClient;   // to the legacy game client (legacy family)
};

// One message from a legacy game client, headed for the game service.
Translation LegacyToCurrent(uint64_t symbol, std::string_view payload, const Context& ctx, const BuildTables& tables);

// One message from the game service, headed for a legacy game client. `reply` chooses which legacy profile
// reply a current-family profile answer becomes; it is ignored for every other symbol.
Translation CurrentToLegacy(uint64_t symbol, std::string_view payload, const Context& ctx, const BuildTables& tables,
                            ProfileReply reply = ProfileReply::LoginProfileResult);

// The LoggedInUserProfileRequest the bridge sends once the login succeeds; a legacy game client never asks.
OutMessage BuildLoggedInUserProfileRequest(const Context& ctx);

// ---- whole frames -----------------------------------------------------------------------------

struct FrameTranslation {
  std::string toGameService;  // frame for the game service; empty when nothing remains
  std::string toGameClient;   // frame for the legacy game client; empty when nothing remains
  std::size_t translated = 0;
  std::size_t passed = 0;
  std::size_t dropped = 0;      // Dropped rows
  std::size_t unsupported = 0;  // not forwarded in either direction
  std::size_t malformed = 0;
};

// Translates every message of a WebSocket binary frame. An Unsupported or Malformed message is not
// forwarded: the game service stops reading a packet at the first symbol it does not know.
FrameTranslation TranslateFrameFromGameClient(const std::string& frame, const Context& ctx, const BuildTables& tables);
FrameTranslation TranslateFrameFromGameService(const std::string& frame, const Context& ctx, const BuildTables& tables,
                                               ProfileReply reply);

}  // namespace nevr_legacy_codec

#pragma once
// The legacy <-> current EVR message translator for the Summer 2019 / Halloween 2018 builds.
//
// A legacy client talks the message family its build shipped with; the game service (nakama) only ever
// sees the current build's family. This file rewrites one message at a time between the two, so the
// ws_bridge can sit between them without nakama changing (nevr-runtime #417).
//
// Platform neutral by construction, like evr_codec.h: no Windows headers, no Winsock, no logging, no game
// state. Everything a legacy message lacks (the login session, the channel, the build's symbol tables) comes
// in through Context and BuildTables, so a translation is a pure function of its arguments.
//
// Layouts: the legacy side follows the build's own message layouts; the current side follows the decode
// order of the game service's codec. Each row's layout source is cited in docs/reference/legacy-translator.md.
// Rows whose layout is not established are not translated; they come back as Outcome::Unsupported.
//
// Integers are little endian. A GUID is 16 raw bytes and is the same on both sides.

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

// ---- legacy symbols (named as the game names them; each equals CSymbol64 of its name)
// ------------------------------------------------------------------------- The symbols the legacy builds put on the
// wire. The current symbols are in evr_codec.h.
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

// The profile result code the legacy client treats as a successful login.
inline constexpr uint8_t kProfileResultSuccess = 0x0B;
inline constexpr uint8_t kProfileResultInvalid = 0;
inline constexpr uint8_t kProfileResultAuthFailed = 7;
inline constexpr uint8_t kProfileResultRestricted = 8;

// ---- what the caller knows --------------------------------------------------------------------

// What one build ships: the symbols that differ between the Summer and Halloween 2018 families and the
// build's own symbol dictionary. The package carries the table; the codec never infers a build.
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

// The Summer 2019 and Halloween 2018 symbol families (everything but the per-build dictionaries).
BuildTables Summer2019Tables();
BuildTables Halloween2018Tables();

// What a legacy message lacks and the bridge holds.
struct Context {
  Guid loginSession{};  // from LoginSuccess; zero before it
  UserId self;          // the logged-in account
  Guid channel{};       // the channel of the last find/create, for the reply direction
  // The current build's encoder flag layout in SessionSuccess: false = the PC layout the legacy client
  // reads; true = the Quest layout, which is converted.
  bool currentUsesQuestFlags = false;
  // When non-empty, replaces the legacy LoginRequest's JSON (the bridge's own profile, which names the
  // client version). Empty keeps the game's JSON as sent.
  std::string loginProfileJson;
  // The request JSON the current profile requests carry; the game service reads field names from it.
  std::string profileRequestJson = "{}";
};

// Which legacy reply a current profile answer becomes. The bridge knows which request is outstanding.
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
  Translated,   // the messages in toService / toGame replace the input
  Passthrough,  // same symbol and layout both sides; the output is the input
  Dropped,      // consumed; nothing is sent (forwarding it would make the service drop its whole packet)
  Local,        // the bridge answers itself: the reply is in toGame
  Unsupported,  // the symbol is not a row of this translator
  Malformed,    // the symbol is a row but the payload does not parse as its layout
};

struct Translation {
  Outcome outcome = Outcome::Unsupported;
  std::vector<OutMessage> toService;  // to the game service (current family)
  std::vector<OutMessage> toGame;     // to the game (legacy family)
};

// One message from the legacy game, to the game service.
Translation LegacyToCurrent(uint64_t symbol, std::string_view payload, const Context& ctx, const BuildTables& tables);

// One message from the game service, to the legacy game. `reply` picks the legacy profile reply for the
// current profile answers; it is ignored for every other symbol.
Translation CurrentToLegacy(uint64_t symbol, std::string_view payload, const Context& ctx, const BuildTables& tables,
                            ProfileReply reply = ProfileReply::LoginProfileResult);

// The LoggedInUserProfileRequest the bridge sends after LoginSuccess (the legacy client never asks).
OutMessage BuildLoggedInUserProfileRequest(const Context& ctx);

// ---- whole frames -----------------------------------------------------------------------------

struct FrameTranslation {
  std::string toService;  // frame for the service; empty when nothing remains
  std::string toGame;     // frame for the game; empty when nothing remains
  std::size_t translated = 0;
  std::size_t passed = 0;
  std::size_t dropped = 0;      // Dropped rows
  std::size_t unsupported = 0;  // not forwarded either way
  std::size_t malformed = 0;
};

// Translates every message of a WebSocket binary frame. A message that is Unsupported or Malformed is
// not forwarded, because the service stops reading a packet at the first symbol it does not know.
FrameTranslation TranslateFrameFromGame(const std::string& frame, const Context& ctx, const BuildTables& tables);
FrameTranslation TranslateFrameFromService(const std::string& frame, const Context& ctx, const BuildTables& tables,
                                           ProfileReply reply);

}  // namespace nevr_legacy_codec

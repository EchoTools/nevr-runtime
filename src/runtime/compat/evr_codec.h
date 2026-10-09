#pragma once
// The EVR wire codec: frame parse/build, the LoginRequest payload layout, and the small pure
// policy functions that decide what a login frame and its upgrade request carry.
//
// Platform neutral by construction: no Windows headers, no Winsock, no logging, no game state.
// The PCVR runtime (compat/ws_bridge.cpp) and the Quest target (src/quest) compile this one file,
// so a framing or login-layout fix lands once (ADR 0003, contract 2).
//
// Wire format of one message:   [marker(8)][symbol(8)][length(8)][payload(length)]
// A WebSocket binary frame carries one or more messages back to back. Integers are little endian.
// LoginRequest payload:         [session UUID(16)][platform code(8)][account id(8)][profile JSON][NUL]
// The profile JSON is built by LoginProfile::BuildLoginProfileJson (nlohmann::json); this codec only
// frames it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace EvrCodec {

constexpr std::size_t kMarkerSize = 8;
constexpr std::size_t kHeaderSize = 24;  // marker + symbol + length
constexpr std::size_t kUuidSize = 16;
constexpr std::size_t kLoginRequestFixedSize = kUuidSize + 8 + 8;  // UUID + platform + account

inline constexpr uint8_t kMarker[kMarkerSize] = {0xf6, 0x40, 0xbb, 0x78, 0xa2, 0xe7, 0x8c, 0xbb};

inline constexpr uint64_t kSymLoginRequest = 0xbdb41ea9e67b200aULL;
inline constexpr uint64_t kSymLoginSuccess = 0xa5acc1a90d0cce47ULL;
inline constexpr uint64_t kSymLoginFailure = 0xa5b9d5a3021ccf51ULL;
// The friend-list subscribe the bridge sends after LoginSuccess (payload ignored by the server).
inline constexpr uint64_t kSymFriendListSubscribe = 0xcdc02fd1dbee3aaaULL;
constexpr std::size_t kFriendListSubscribePayloadSize = 0x20;

// The platform the bridge logs in as. It MUST equal the provider the bridge forces into the game's own
// CNSUser, because the game names itself with that platform in every later request and Nakama looks the
// requester up under the platform the LoginRequest carried. OVR_ORG in the game's numbering.
inline constexpr uint64_t kBridgeLoginPlatform = 4;

// ---- building -------------------------------------------------------------------------------

// One message: marker, symbol, payload length, payload.
std::string BuildMessage(uint64_t symbol, std::string_view payload);

// A LoginRequest message with an all-zero session UUID (no previous session). nullopt when
// profileJson contains a NUL byte: the payload is NUL terminated, so the server would read a
// truncated profile. LoginProfile::BuildLoginProfileJson never emits one (JSON escapes it).
std::optional<std::string> BuildLoginRequest(uint64_t platformCode, uint64_t accountId,
                                             std::string_view profileJson);

// The LoginSuccess a dedicated server synthesizes when the service answers LoginRequest with a
// failure: a "NEVRSRVR" session id, then platform and account.
std::string BuildLoginSuccess(uint64_t platformCode, uint64_t accountId);

std::string BuildFriendListSubscribe();

// ---- parsing --------------------------------------------------------------------------------

struct Message {
  uint64_t symbol = 0;
  uint64_t length = 0;               // as declared by the header
  const uint8_t* payload = nullptr;  // points into the caller's buffer; valid when status is Ok
};

enum class ReadStatus {
  Ok,         // `out` holds a message that lies wholly inside the buffer
  End,        // fewer than kHeaderSize bytes remain: nothing (more) to read
  BadMarker,  // a header's worth of bytes that do not start with the marker
  Truncated,  // the header declares more payload than remains; out->symbol/length are set
};

// Reads the message at byte `offset` of `frame`. `*out` is reset first, so `payload` is nullptr unless the
// status is Ok. On Ok the message occupies kHeaderSize + out->length bytes (which cannot overflow: it fits
// in what remains of the frame).
ReadStatus ReadMessage(const std::string& frame, std::size_t offset, Message* out);

// The first message's symbol, or 0 when the frame is shorter than a header. Does not check the marker.
uint64_t FirstSymbol(const std::string& frame);

struct LoginFailure {
  uint64_t statusCode = 0;
  std::size_t messageBytes = 0;  // length of the server's message text; the text is never read out
};

// The numeric diagnostics of a LoginFailure that is the frame's first message; nullopt when the frame is
// not one or is truncated.
std::optional<LoginFailure> ParseLoginFailure(const std::string& frame);

// ---- login policy ---------------------------------------------------------------------------

// Short platform name for an XPID prefix, from the game's 1-indexed provider enum, which is also
// Nakama's PlatformCode: STM=1, DSC=2, XBX=3, OVR_ORG=4, OVR=5, BOT=6, DMO=7; anything else is "UNK".
// Code 2 is "PSN" in the game's string table and reads "DSC" only after PatchDscProvider rewrites it;
// the game's own fallback prefix for an unknown provider is "???".
const char* PlatformPrefix(uint64_t platformCode);

// The platform code the bridge sends. The arguments do not influence the result.
uint64_t SelectPlatformCode(bool hasUrlCredentials, bool noOvr);

// Which Bearer goes on the remote upgrade. The game front's /nevr ingress forwards the caller's
// Authorization header unchanged. A token-auth client sends its JWT, which authenticates the session.
// A client logging in with URL credentials (discordid/password) sends the SERVER KEY instead: Nakama
// treats a token equal to the server key as the legacy unauthenticated session and then authenticates it
// from the discordid/password query parameters, as the /ws catch-all does. Empty means "attach no
// Authorization header".
std::string SelectRemoteBearer(bool hasUrlCredentials, const std::string& jwt, const std::string& serverKey);

// True when the URL's path is the /ws catch-all, whose front replaces the client's Bearer with the server
// key: a token-auth login sent there arrives unauthenticated.
bool IsBearerReplacingPath(const std::string& url);

}  // namespace EvrCodec

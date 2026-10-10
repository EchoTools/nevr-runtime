#include "runtime/compat/legacy_codec.h"

#include <zlib.h>
#include <zstd.h>

#include <cstring>
#include <nlohmann/json.hpp>
#include <optional>

namespace nevr_legacy_codec {

namespace {

using nevr_evr_codec::AppendLE64;
using nevr_evr_codec::ReadLE64;
using nlohmann::json;

// A decompressed profile is a few tens of KiB; this bounds what a hostile length field can make us allocate.
constexpr std::size_t kMaxInflatedBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxEntrants = 16;
constexpr std::size_t kGuidSize = 16;
constexpr std::size_t kUserSize = 16;

// SessionSuccess: [mode(8)][lobby(16)][group(16), current only][endpoint(10)][team(2)][flags(1)][pad(3)]
// [server encoder flags(8)][client encoder flags(8)][server seq(8)][server keys][client seq(8)][client keys]
constexpr std::size_t kSuccessLobbyEnd = 8 + kGuidSize;
constexpr std::size_t kSuccessEndpointAndFlagsSize = 10 + 2 + 1 + 3;
// SessionFailure (current): [mode(8)][channel(16)][code(4)][unk(4)][message(64)][expiry(8)]
constexpr std::size_t kFailureMessageSize = 64;
constexpr std::size_t kFailureCurrentSize = 8 + kGuidSize + 4 + 4 + kFailureMessageSize + 8;

// Profile result codes the legacy client maps from the service's HTTP-style login failure status.
constexpr uint64_t kStatusBadRequest = 400;
constexpr uint64_t kStatusUnauthorized = 401;
constexpr uint64_t kStatusForbidden = 403;

// ---- reading and writing ---------------------------------------------------------------------

// Reads little-endian fields from a payload. A short read sets ok() false and every later read returns zero,
// so a row parses straight through and checks once at the end.
class Reader {
 public:
  explicit Reader(std::string_view in) : in_(in) {}

  bool ok() const { return ok_; }
  std::size_t remaining() const { return in_.size() - pos_; }

  uint8_t U8() { return static_cast<uint8_t>(Take(1)[0]); }
  uint32_t U32() {
    const std::string_view b = Take(4);
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<uint8_t>(b[static_cast<std::size_t>(i)]);
    return v;
  }
  uint64_t U64() {
    const std::string_view b = Take(8);
    return ok_ ? ReadLE64(reinterpret_cast<const uint8_t*>(b.data())) : 0;
  }
  int16_t I16() {
    const std::string_view b = Take(2);
    return static_cast<int16_t>(static_cast<uint16_t>(static_cast<uint8_t>(b[0]) | (static_cast<uint8_t>(b[1]) << 8)));
  }
  Guid ReadGuid() {
    Guid g{};
    const std::string_view b = Take(kGuidSize);
    if (ok_) std::memcpy(g.data(), b.data(), kGuidSize);
    return g;
  }
  UserId User() {
    UserId u;
    u.platformCode = U64();
    u.accountId = U64();
    return u;
  }
  std::string Bytes(std::size_t n) { return std::string(Take(n)); }
  std::string Rest() { return std::string(Take(remaining())); }
  // A NUL-terminated string; the NUL is consumed and not returned. Fails when there is no NUL.
  std::string CString() {
    const std::size_t nul = in_.find('\0', pos_);
    if (nul == std::string_view::npos) {
      ok_ = false;
      return {};
    }
    std::string s(in_.substr(pos_, nul - pos_));
    pos_ = nul + 1;
    return s;
  }

 private:
  std::string_view Take(std::size_t n) {
    static const char kZeros[16] = {};
    if (!ok_ || n > remaining()) {
      ok_ = false;
      return std::string_view(kZeros, n < sizeof(kZeros) ? n : sizeof(kZeros));
    }
    const std::string_view v = in_.substr(pos_, n);
    pos_ += n;
    return v;
  }

  std::string_view in_;
  std::size_t pos_ = 0;
  bool ok_ = true;
};

void PutU8(std::string& out, uint8_t v) { out.push_back(static_cast<char>(v)); }
void PutU32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void PutU64(std::string& out, uint64_t v) { AppendLE64(out, v); }
void PutI16(std::string& out, int16_t v) {
  const uint16_t u = static_cast<uint16_t>(v);
  out.push_back(static_cast<char>(u & 0xff));
  out.push_back(static_cast<char>(u >> 8));
}
void PutGuid(std::string& out, const Guid& g) { out.append(reinterpret_cast<const char*>(g.data()), g.size()); }
void PutUser(std::string& out, const UserId& u) {
  PutU64(out, u.platformCode);
  PutU64(out, u.accountId);
}
void PutZeros(std::string& out, std::size_t n) { out.append(n, '\0'); }
void PutCString(std::string& out, std::string_view s) {
  out.append(s);
  out.push_back('\0');
}

// ---- maps -------------------------------------------------------------------------------------

using SymbolMap = std::vector<std::pair<uint64_t, uint64_t>>;

uint64_t ToCurrent(const SymbolMap& map, uint64_t legacy) {
  for (const auto& [l, c] : map) {
    if (l == legacy) return c;
  }
  return legacy;
}

uint64_t ToLegacy(const SymbolMap& map, uint64_t current) {
  for (const auto& [l, c] : map) {
    if (c == current) return l;
  }
  return current;
}

// ---- compression and JSON ---------------------------------------------------------------------

std::optional<std::string> ZlibDeflate(std::string_view raw) {
  uLongf n = compressBound(static_cast<uLong>(raw.size()));
  std::string out(n, '\0');
  if (compress(reinterpret_cast<Bytef*>(out.data()), &n, reinterpret_cast<const Bytef*>(raw.data()),
               static_cast<uLong>(raw.size())) != Z_OK) {
    return std::nullopt;
  }
  out.resize(n);
  return out;
}

std::optional<std::string> ZlibInflate(std::string_view z, uint64_t rawSize) {
  if (rawSize > kMaxInflatedBytes) return std::nullopt;
  std::string out(static_cast<std::size_t>(rawSize), '\0');
  uLongf n = static_cast<uLongf>(rawSize);
  if (uncompress(reinterpret_cast<Bytef*>(out.data()), &n, reinterpret_cast<const Bytef*>(z.data()),
                 static_cast<uLong>(z.size())) != Z_OK ||
      n != rawSize) {
    return std::nullopt;
  }
  return out;
}

std::optional<std::string> ZstdDeflate(std::string_view raw) {
  std::string out(ZSTD_compressBound(raw.size()), '\0');
  const std::size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), 3);
  if (ZSTD_isError(n) != 0) return std::nullopt;
  out.resize(n);
  return out;
}

std::optional<std::string> ZstdInflate(std::string_view z, uint64_t rawSize) {
  if (rawSize > kMaxInflatedBytes) return std::nullopt;
  std::string out(static_cast<std::size_t>(rawSize), '\0');
  const std::size_t n = ZSTD_decompress(out.data(), out.size(), z.data(), z.size());
  if (ZSTD_isError(n) != 0 || n != rawSize) return std::nullopt;
  return out;
}

std::optional<json> ParseJson(std::string_view text) {
  json doc = json::parse(text.begin(), text.end(), nullptr, false);
  if (doc.is_discarded()) return std::nullopt;
  return doc;
}

std::string DumpJson(const json& doc) { return doc.dump(-1, ' ', false, json::error_handler_t::replace); }

// A current profile body: u32 length of the JSON including its NUL, then a zstd frame of JSON + NUL.
// The service's StreamJson appends the NUL; it is stripped here and put back by PackCurrentJson.
std::optional<json> UnpackCurrentJson(Reader& r) {
  const uint32_t length = r.U32();
  const std::string frame = r.Rest();
  if (!r.ok() || length == 0) return std::nullopt;
  const std::optional<std::string> raw = ZstdInflate(frame, length);
  if (!raw || raw->empty()) return std::nullopt;
  std::string_view text(*raw);
  if (text.back() == '\0') text.remove_suffix(1);
  return ParseJson(text);
}

bool PackCurrentJson(std::string& out, const json& doc) {
  std::string raw = DumpJson(doc);
  raw.push_back('\0');
  const std::optional<std::string> frame = ZstdDeflate(raw);
  if (!frame) return false;
  PutU32(out, static_cast<uint32_t>(raw.size()));
  out.append(*frame);
  return true;
}

// ---- results ----------------------------------------------------------------------------------

Translation Make(Outcome outcome) {
  Translation t;
  t.outcome = outcome;
  return t;
}

Translation ToService(uint64_t symbol, std::string payload) {
  Translation t = Make(Outcome::Translated);
  t.toService.push_back({symbol, std::move(payload)});
  return t;
}

Translation ToGame(uint64_t symbol, std::string payload) {
  Translation t = Make(Outcome::Translated);
  t.toGame.push_back({symbol, std::move(payload)});
  return t;
}

Translation Bad() { return Make(Outcome::Malformed); }

std::string GuidBytes(const Guid& g) { return std::string(g.begin(), g.end()); }

// ---- login --------------------------------------------------------------------------------------

Translation LoginRequestToCurrent(std::string_view payload, const Context& ctx) {
  Reader r(payload);
  const Guid session = r.ReadGuid();
  const UserId user = r.User();
  r.Bytes(8);  // locale: the current request has no slot for it
  std::string profile = r.CString();
  if (!r.ok()) return Bad();
  if (!ctx.loginProfileJson.empty()) profile = ctx.loginProfileJson;
  std::string out;
  PutGuid(out, session);
  PutUser(out, user);
  PutCString(out, profile);
  return ToService(nevr_evr_codec::kSymLoginRequest, std::move(out));
}

Translation LoginRequestToLegacy(std::string_view payload) {
  Reader r(payload);
  const Guid session = r.ReadGuid();
  const UserId user = r.User();
  const std::string profile = r.CString();
  if (!r.ok()) return Bad();
  std::string out;
  PutGuid(out, session);
  PutUser(out, user);
  out.append("en");
  PutZeros(out, 6);  // the locale field is 8 bytes
  PutCString(out, profile);
  return ToGame(kSNSLoginRequest, std::move(out));
}

uint8_t ResultCodeForStatus(uint64_t status) {
  if (status == kStatusUnauthorized) return kProfileResultAuthFailed;
  if (status == kStatusForbidden) return kProfileResultRestricted;
  return kProfileResultInvalid;
}

uint64_t StatusForResultCode(uint8_t code) {
  if (code == kProfileResultAuthFailed) return kStatusUnauthorized;
  if (code == kProfileResultRestricted) return kStatusForbidden;
  return kStatusBadRequest;
}

// The 0x30-byte header the client requires even on a failure: it drops anything shorter.
std::string ProfileResultFailure(const Guid& session, const UserId& user, uint8_t code) {
  std::string out;
  PutGuid(out, session);
  PutUser(out, user);
  PutU32(out, 0);
  PutU8(out, code);
  PutZeros(out, 3);
  PutU64(out, 0);
  return out;
}

Translation LoginFailureToLegacy(std::string_view payload, const Context& ctx) {
  Reader r(payload);
  const UserId user = r.User();
  const uint64_t status = r.U64();
  if (!r.ok()) return Bad();
  return ToGame(kSNSLoginProfileResult, ProfileResultFailure(ctx.loginSession, user, ResultCodeForStatus(status)));
}

std::optional<std::string> ProfileResultSuccess(const Context& ctx, const UserId& user, const json& client,
                                                const json& server) {
  std::string raw = DumpJson(client);
  raw.push_back('\0');
  raw.append(DumpJson(server));
  const std::optional<std::string> z = ZlibDeflate(raw);
  if (!z) return std::nullopt;
  std::string out;
  PutGuid(out, ctx.loginSession);
  PutUser(out, user);
  PutU32(out, 0);
  PutU8(out, kProfileResultSuccess);
  PutZeros(out, 3);
  PutU64(out, raw.size());
  out.append(*z);
  return out;
}

json Member(const json& doc, const char* key) {
  if (doc.is_object()) {
    const auto it = doc.find(key);
    if (it != doc.end()) return *it;
  }
  return json::object();
}

// The legacy reply header shared by RefreshProfileResult: user | u32 | u8 result | 3 pad | JSON NUL.
std::string RefreshResult(const UserId& user, const json& profile) {
  std::string out;
  PutUser(out, user);
  PutU32(out, 0);
  PutU8(out, kProfileResultSuccess);
  PutZeros(out, 3);
  PutCString(out, DumpJson(profile));
  return out;
}

std::string ProfileResponse(const UserId& user, const json& profile) {
  std::string out;
  PutUser(out, user);
  PutCString(out, DumpJson(profile));
  return out;
}

Translation LoggedInProfileSuccessToLegacy(std::string_view payload, const Context& ctx, const BuildTables& tables,
                                           ProfileReply reply) {
  Reader r(payload);
  const UserId user = r.User();
  const std::optional<json> doc = UnpackCurrentJson(r);
  if (!r.ok() || !doc) return Bad();
  const json client = Member(*doc, "client");
  const json server = Member(*doc, "server");
  switch (reply) {
    case ProfileReply::LoginProfileResult: {
      std::optional<std::string> out = ProfileResultSuccess(ctx, user, client, server);
      if (!out) return Bad();
      return ToGame(kSNSLoginProfileResult, std::move(*out));
    }
    case ProfileReply::RefreshProfileResult:
      return ToGame(kSNSRefreshProfileResult, RefreshResult(user, server));
    case ProfileReply::ProfileResponse:
      return ToGame(tables.profileResponseSymbol, ProfileResponse(user, server));
  }
  return Make(Outcome::Unsupported);
}

Translation OtherProfileSuccessToLegacy(std::string_view payload, const BuildTables& tables, ProfileReply reply) {
  Reader r(payload);
  const UserId user = r.User();
  const std::optional<json> profile = UnpackCurrentJson(r);
  if (!r.ok() || !profile) return Bad();
  switch (reply) {
    case ProfileReply::ProfileResponse:
      return ToGame(tables.profileResponseSymbol, ProfileResponse(user, *profile));
    case ProfileReply::RefreshProfileResult:
      return ToGame(kSNSRefreshProfileResult, RefreshResult(user, *profile));
    case ProfileReply::LoginProfileResult:
      break;  // an OtherUser answer never answers the login
  }
  return Make(Outcome::Unsupported);
}

Translation ProfileResultToCurrent(std::string_view payload, const Context& ctx) {
  (void)ctx;
  Reader r(payload);
  r.ReadGuid();  // the legacy session: the service has none to keep
  const UserId user = r.User();
  r.U32();
  const uint8_t result = r.U8();
  r.Bytes(3);
  const uint64_t length = r.U64();
  if (!r.ok()) return Bad();
  if (result != kProfileResultSuccess) {
    std::string out;
    PutUser(out, user);
    PutU64(out, StatusForResultCode(result));
    PutCString(out, "");
    return ToService(nevr_evr_codec::kSymLoginFailure, std::move(out));
  }
  const std::optional<std::string> raw = ZlibInflate(r.Rest(), length);
  if (!raw) return Bad();
  const std::size_t split = raw->find('\0');
  std::string_view clientText(*raw);
  std::string_view serverText;
  if (split != std::string::npos) {
    clientText = std::string_view(*raw).substr(0, split);
    serverText = std::string_view(*raw).substr(split + 1);
    while (!serverText.empty() && serverText.back() == '\0') serverText.remove_suffix(1);
  }
  const std::optional<json> client = ParseJson(clientText);
  std::optional<json> server = json::object();
  if (!serverText.empty()) server = ParseJson(serverText);
  if (!client || !server) return Bad();
  std::string out;
  PutUser(out, user);
  if (!PackCurrentJson(out, json{{"client", *client}, {"server", *server}})) return Bad();
  return ToService(nevr_evr_codec::kSymLoggedInUserProfileSuccess, std::move(out));
}

// RefreshProfileResult and ProfileResponse(v2): the user id then a profile JSON (after a 8-byte header for
// the former). Both become OtherUserProfileSuccess.
Translation LegacyProfileBodyToCurrent(std::string_view payload, bool refreshHeader) {
  Reader r(payload);
  const UserId user = r.User();
  if (refreshHeader) {
    r.U32();
    r.U8();
    r.Bytes(3);
  }
  const std::string text = r.CString();
  if (!r.ok()) return Bad();
  const std::optional<json> profile = ParseJson(text);
  if (!profile) return Bad();
  std::string out;
  PutUser(out, user);
  if (!PackCurrentJson(out, *profile)) return Bad();
  return ToService(nevr_evr_codec::kSymOtherUserProfileSuccess, std::move(out));
}

Translation SettingsToCurrent(std::string_view payload, const BuildTables& tables, uint64_t symbol) {
  (void)tables;
  if (symbol == nevr_evr_codec::kSymLoginSettings) return Make(Outcome::Passthrough);
  return ToService(nevr_evr_codec::kSymLoginSettings, std::string(payload));
}

std::string CurrentProfileRequestJson(const Context& ctx) {
  return ctx.profileRequestJson.empty() ? std::string("{}") : ctx.profileRequestJson;
}

Translation RefreshProfileToCurrent(std::string_view payload, const Context& ctx) {
  Reader r(payload);
  const Guid session = r.ReadGuid();
  const UserId user = r.User();
  if (!r.ok()) return Bad();  // the trailing u64 flags are optional and carry nothing the service needs
  std::string out;
  if (user.platformCode == ctx.self.platformCode && user.accountId == ctx.self.accountId) {
    PutGuid(out, session);
    PutUser(out, user);
    PutCString(out, CurrentProfileRequestJson(ctx));
    return ToService(nevr_evr_codec::kSymLoggedInUserProfileRequest, std::move(out));
  }
  PutUser(out, user);
  PutCString(out, CurrentProfileRequestJson(ctx));
  return ToService(nevr_evr_codec::kSymOtherUserProfileRequest, std::move(out));
}

Translation ProfileRequestToCurrent(std::string_view payload, const Context& ctx) {
  Reader r(payload);
  r.U64();
  const UserId user = r.User();
  if (!r.ok()) return Bad();
  std::string out;
  PutUser(out, user);
  PutCString(out, CurrentProfileRequestJson(ctx));
  return ToService(nevr_evr_codec::kSymOtherUserProfileRequest, std::move(out));
}

Translation LoggedInProfileRequestToLegacy(std::string_view payload) {
  Reader r(payload);
  const Guid session = r.ReadGuid();
  const UserId user = r.User();
  if (!r.ok()) return Bad();
  std::string out;
  PutGuid(out, session);
  PutUser(out, user);
  PutU64(out, 1);  // the clients' flag
  return ToGame(kSNSRefreshProfile, std::move(out));
}

Translation OtherProfileRequestToLegacy(std::string_view payload, const BuildTables& tables) {
  Reader r(payload);
  const UserId user = r.User();
  if (!r.ok()) return Bad();
  std::string out;
  PutU64(out, 0);
  PutUser(out, user);
  return ToGame(tables.profileRequestSymbol, std::move(out));
}

Translation LeaderboardAnswer(std::string_view payload) {
  Reader r(payload);
  const uint64_t tag = r.U64();
  if (!r.ok()) return Bad();
  const std::string raw = "[]";
  const std::optional<std::string> z = ZlibDeflate(raw);
  if (!z) return Bad();
  std::string out;
  PutU64(out, tag);
  PutU64(out, raw.size());
  out.append(*z);
  Translation t = Make(Outcome::Local);
  t.toGame.push_back({kSNSLeaderboardResponse, std::move(out)});
  return t;
}

// ---- matching -----------------------------------------------------------------------------------

constexpr uint8_t kRequestFlagsNone = 0;

// The tail the current requests end with: the legacy i16 team index, which the game service reads as the
// entrant's role. A legacy request that ends at the user id has no team: -1.
void PutTeamTail(std::string& out, int16_t team) { PutI16(out, team); }

int16_t TeamFromTail(std::string_view tail) {
  if (tail.size() < 2) return -1;
  const uint16_t v = static_cast<uint16_t>(static_cast<uint8_t>(tail[0]) | (static_cast<uint8_t>(tail[1]) << 8));
  return static_cast<int16_t>(v);
}

Translation FindToCurrent(std::string_view payload, const Context& ctx, const BuildTables& tables) {
  Reader r(payload);
  const uint64_t versionLock = r.U64();
  const uint64_t mode = r.U64();
  const uint64_t level = r.U64();
  const uint64_t platform = r.U64();
  r.U8();
  r.U8();
  r.Bytes(6);
  const Guid channel = r.ReadGuid();
  const std::string settings = r.CString();
  const UserId user = r.User();
  if (!r.ok()) return Bad();
  std::string out;
  PutU64(out, versionLock);
  PutU64(out, ToCurrent(tables.modes, mode));
  PutU64(out, ToCurrent(tables.levels, level));
  PutU64(out, ToCurrent(tables.platforms, platform));
  PutGuid(out, ctx.loginSession);
  PutU8(out, 1);  // entrant count
  PutU32(out, kRequestFlagsNone);
  PutZeros(out, 3);
  PutZeros(out, kGuidSize);  // no lobby to leave
  PutGuid(out, channel);
  PutCString(out, settings);
  PutUser(out, user);
  return ToService(nevr_evr_codec::kSymFindSessionRequest, std::move(out));
}

Translation FindToLegacy(std::string_view payload, const Context& ctx, const BuildTables& tables) {
  Reader r(payload);
  const uint64_t versionLock = r.U64();
  const uint64_t mode = r.U64();
  const uint64_t level = r.U64();
  const uint64_t platform = r.U64();
  r.ReadGuid();  // login session
  const uint8_t count = r.U8();
  r.U32();
  r.Bytes(3);
  r.ReadGuid();  // current lobby
  const Guid group = r.ReadGuid();
  const std::string settings = r.CString();
  if (!r.ok() || count > kMaxEntrants) return Bad();
  UserId user = ctx.self;
  for (uint8_t i = 0; i < count; ++i) {
    const UserId entrant = r.User();
    if (i == 0) user = entrant;
  }
  if (!r.ok()) return Bad();
  std::string out;
  PutU64(out, versionLock);
  PutU64(out, ToLegacy(tables.modes, mode));
  PutU64(out, ToLegacy(tables.levels, level));
  PutU64(out, ToLegacy(tables.platforms, platform));
  PutU8(out, 0);
  PutU8(out, 0);
  PutZeros(out, 6);
  PutGuid(out, group);
  PutCString(out, settings);
  PutUser(out, user);
  return ToGame(kSNSLobbyFindSessionRequestv8, std::move(out));
}

Translation JoinToCurrent(std::string_view payload, const Context& ctx, const BuildTables& tables) {
  Reader r(payload);
  const Guid lobby = r.ReadGuid();
  const uint64_t versionLock = r.U64();
  const uint64_t platform = r.U64();
  r.U64();
  r.U64();
  const std::string settings = r.CString();
  const UserId user = r.User();
  if (!r.ok()) return Bad();
  const int16_t team = r.remaining() >= 2 ? r.I16() : static_cast<int16_t>(-1);
  std::string out;
  PutGuid(out, lobby);
  PutU64(out, versionLock);
  PutU64(out, ToCurrent(tables.platforms, platform));
  PutGuid(out, ctx.loginSession);
  PutU32(out, 1);  // entrant count in the low byte
  PutZeros(out, 4);
  PutU64(out, 0);
  PutCString(out, settings);
  PutUser(out, user);
  PutTeamTail(out, team);
  return ToService(nevr_evr_codec::kSymJoinSessionRequest, std::move(out));
}

Translation JoinToLegacy(std::string_view payload, const Context& ctx, const BuildTables& tables) {
  Reader r(payload);
  const Guid lobby = r.ReadGuid();
  const uint64_t versionLock = r.U64();
  const uint64_t platform = r.U64();
  r.ReadGuid();  // login session
  const uint32_t flags = r.U32();
  r.Bytes(4);
  r.U64();
  const std::string settings = r.CString();
  const std::size_t count = flags & 0xff;
  if (!r.ok() || count > kMaxEntrants) return Bad();
  UserId user = ctx.self;
  for (std::size_t i = 0; i < count; ++i) {
    const UserId entrant = r.User();
    if (i == 0) user = entrant;
  }
  if (!r.ok()) return Bad();
  const int16_t team = TeamFromTail(r.Rest());
  std::string out;
  PutGuid(out, lobby);
  PutU64(out, versionLock);
  PutU64(out, ToLegacy(tables.platforms, platform));
  PutU64(out, 0);
  PutU64(out, 0);
  PutCString(out, settings);
  PutUser(out, user);
  PutI16(out, team);
  return ToGame(kSNSLobbyJoinSessionRequestv6, std::move(out));
}

// The legacy create request: five u64, lobby type, then bytes whose meaning is not established, the channel,
// the settings JSON, the user id and the team. The unmapped run's length is not fixed in the captures we
// have, so the JSON is located from the end: it is followed by exactly a user id (and optionally a team),
// and the channel is the 16 bytes before it.
struct CreateV7 {
  uint64_t region = 0;
  uint64_t versionLock = 0;
  uint64_t mode = 0;
  uint64_t level = 0;
  uint64_t platform = 0;
  uint8_t lobbyType = 0;
  std::string unmapped;
  Guid channel{};
  std::string settings;
  UserId user;
  int16_t team = -1;
};

std::optional<CreateV7> ParseCreateV7(std::string_view payload) {
  Reader head(payload);
  CreateV7 m;
  m.region = head.U64();
  m.versionLock = head.U64();
  m.mode = head.U64();
  m.level = head.U64();
  m.platform = head.U64();
  if (!head.ok()) return std::nullopt;
  const std::string rest = head.Rest();
  // rest = lobbyType(1) unmapped(n) channel(16) json NUL user(16) [team(2)]
  for (std::size_t brace = 1 + kGuidSize; brace < rest.size(); ++brace) {
    if (rest[brace] != '{') continue;
    const std::size_t nul = rest.find('\0', brace);
    if (nul == std::string::npos) return std::nullopt;
    const std::size_t after = rest.size() - (nul + 1);
    if (after != kUserSize && after != kUserSize + 2) continue;
    if (!ParseJson(std::string_view(rest).substr(brace, nul - brace))) continue;  // a 0x7b inside the channel
    m.lobbyType = static_cast<uint8_t>(rest[0]);
    m.unmapped = rest.substr(1, brace - kGuidSize - 1);
    std::memcpy(m.channel.data(), rest.data() + brace - kGuidSize, kGuidSize);
    m.settings = rest.substr(brace, nul - brace);
    Reader tail(std::string_view(rest).substr(nul + 1));
    m.user = tail.User();
    m.team = tail.remaining() >= 2 ? tail.I16() : static_cast<int16_t>(-1);
    return m;
  }
  return std::nullopt;
}

Translation CreateToCurrent(std::string_view payload, const Context& ctx, const BuildTables& tables) {
  const std::optional<CreateV7> m = ParseCreateV7(payload);
  if (!m) return Bad();
  std::string out;
  PutU64(out, m->region);
  PutU64(out, m->versionLock);
  PutU64(out, ToCurrent(tables.modes, m->mode));
  PutU64(out, ToCurrent(tables.levels, m->level));
  PutU64(out, ToCurrent(tables.platforms, m->platform));
  PutGuid(out, ctx.loginSession);
  PutU8(out, 1);
  PutZeros(out, 7);
  PutU8(out, m->lobbyType);
  PutZeros(out, 3);
  PutU32(out, kRequestFlagsNone);
  PutGuid(out, m->channel);
  PutCString(out, m->settings);
  PutUser(out, m->user);
  PutTeamTail(out, m->team);
  return ToService(nevr_evr_codec::kSymCreateSessionRequest, std::move(out));
}

Translation CreateToLegacy(std::string_view payload, const Context& ctx, const BuildTables& tables) {
  Reader r(payload);
  const uint64_t region = r.U64();
  const uint64_t versionLock = r.U64();
  const uint64_t mode = r.U64();
  const uint64_t level = r.U64();
  const uint64_t platform = r.U64();
  r.ReadGuid();
  const uint8_t count = r.U8();
  r.Bytes(7);
  const uint8_t lobbyType = r.U8();
  r.Bytes(3);
  r.U32();
  const Guid group = r.ReadGuid();
  const std::string settings = r.CString();
  if (!r.ok() || count > kMaxEntrants) return Bad();
  UserId user = ctx.self;
  for (uint8_t i = 0; i < count; ++i) {
    const UserId entrant = r.User();
    if (i == 0) user = entrant;
  }
  if (!r.ok()) return Bad();
  const int16_t team = TeamFromTail(r.Rest());
  std::string out;
  PutU64(out, region);
  PutU64(out, versionLock);
  PutU64(out, ToLegacy(tables.modes, mode));
  PutU64(out, ToLegacy(tables.levels, level));
  PutU64(out, ToLegacy(tables.platforms, platform));
  PutU8(out, lobbyType);
  PutZeros(out, 7);
  PutGuid(out, group);
  PutCString(out, settings);
  PutUser(out, user);
  PutI16(out, team);
  return ToGame(kSNSLobbyCreateSessionRequestv7, std::move(out));
}

Translation PlayerSessionsToCurrent(std::string_view payload, const Context& ctx, const BuildTables& tables) {
  Reader r(payload);
  const Guid matched = r.ReadGuid();
  const uint64_t platform = r.U64();
  const uint64_t count = r.U64();
  if (!r.ok() || count > kMaxEntrants) return Bad();
  std::vector<UserId> ids;
  for (uint64_t i = 0; i < count; ++i) ids.push_back(r.User());
  if (!r.ok()) return Bad();
  std::string out;
  PutGuid(out, ctx.loginSession);
  PutUser(out, ids.empty() ? ctx.self : ids[0]);
  PutGuid(out, matched);
  PutU64(out, ToCurrent(tables.platforms, platform));
  PutU64(out, count);
  for (const UserId& id : ids) PutUser(out, id);
  return ToService(nevr_evr_codec::kSymPlayerSessionsRequest, std::move(out));
}

Translation PlayerSessionsToLegacy(std::string_view payload, const BuildTables& tables) {
  Reader r(payload);
  r.ReadGuid();
  r.User();
  const Guid lobby = r.ReadGuid();
  const uint64_t platform = r.U64();
  const uint64_t count = r.U64();
  if (!r.ok() || count > kMaxEntrants) return Bad();
  std::vector<UserId> ids;
  for (uint64_t i = 0; i < count; ++i) ids.push_back(r.User());
  if (!r.ok()) return Bad();
  std::string out;
  PutGuid(out, lobby);
  PutU64(out, ToLegacy(tables.platforms, platform));
  PutU64(out, count);
  for (const UserId& id : ids) PutUser(out, id);
  return ToGame(kSNSLobbyPlayerSessionsRequestv3, std::move(out));
}

// ---- session success ------------------------------------------------------------------------------

constexpr uint64_t kEncoderSizeMask = 0x0fff;

// The three key sizes an encoder-flags word declares, in the PC layout (mac at bit 26, encryption at 38,
// random at 50).
std::size_t KeyBytesOf(uint64_t pcFlags) {
  return static_cast<std::size_t>(((pcFlags >> 26) & kEncoderSizeMask) + ((pcFlags >> 38) & kEncoderSizeMask) +
                                  ((pcFlags >> 50) & kEncoderSizeMask));
}

// The Quest layout is the PC layout shifted up one bit with bit 0 set ("initialized").
uint64_t QuestToPcFlags(uint64_t flags) { return flags >> 1; }
uint64_t PcToQuestFlags(uint64_t flags) { return (flags << 1) | 1; }

// Everything after the group: validates the length against the key sizes the two flag words declare and
// converts the flag words between layouts. `fromQuest` says the input words are Quest layout.
std::optional<std::string> ConvertSuccessTail(std::string_view tail, bool fromQuest, bool toQuest) {
  if (tail.size() < kSuccessEndpointAndFlagsSize + 16) return std::nullopt;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(tail.data());
  uint64_t server = ReadLE64(bytes + kSuccessEndpointAndFlagsSize);
  uint64_t client = ReadLE64(bytes + kSuccessEndpointAndFlagsSize + 8);
  if (fromQuest) {
    server = QuestToPcFlags(server);
    client = QuestToPcFlags(client);
  }
  const std::size_t expected = kSuccessEndpointAndFlagsSize + 16 + 8 + KeyBytesOf(server) + 8 + KeyBytesOf(client);
  if (tail.size() != expected) return std::nullopt;
  if (toQuest) {
    server = PcToQuestFlags(server);
    client = PcToQuestFlags(client);
  }
  std::string out(tail.substr(0, kSuccessEndpointAndFlagsSize));
  PutU64(out, server);
  PutU64(out, client);
  out.append(tail.substr(kSuccessEndpointAndFlagsSize + 16));
  return out;
}

Translation SuccessV5ToLegacy(std::string_view payload, const Context& ctx) {
  if (payload.size() < kSuccessLobbyEnd + kGuidSize) return Bad();
  const std::optional<std::string> tail =
      ConvertSuccessTail(payload.substr(kSuccessLobbyEnd + kGuidSize), ctx.currentUsesQuestFlags, false);
  if (!tail) return Bad();
  std::string out(payload.substr(0, kSuccessLobbyEnd));
  out.append(*tail);
  return ToGame(kSNSLobbySessionSuccessv4, std::move(out));
}

Translation SuccessV4ToCurrent(std::string_view payload, const Context& ctx) {
  if (payload.size() < kSuccessLobbyEnd) return Bad();
  const std::optional<std::string> tail =
      ConvertSuccessTail(payload.substr(kSuccessLobbyEnd), false, ctx.currentUsesQuestFlags);
  if (!tail) return Bad();
  std::string out(payload.substr(0, kSuccessLobbyEnd));
  PutGuid(out, ctx.channel);
  out.append(*tail);
  return ToService(kSNSLobbySessionSuccessv5, std::move(out));
}

Translation FailureToLegacy(std::string_view payload, const BuildTables& tables) {
  if (payload.size() < kFailureCurrentSize) return Bad();
  Reader r(payload);
  const uint64_t mode = r.U64();
  const Guid channel = r.ReadGuid();
  const uint32_t code = r.U32();
  const uint32_t unk = r.U32();
  std::string out;
  if (tables.sessionFailureSymbol == kSNSLobbySessionFailurev2) {
    PutGuid(out, channel);
    PutU32(out, code);
    return ToGame(kSNSLobbySessionFailurev2, std::move(out));
  }
  PutU64(out, ToLegacy(tables.modes, mode));
  PutGuid(out, channel);
  PutU32(out, code);
  PutU32(out, unk);
  return ToGame(kSNSLobbySessionFailurev3, std::move(out));
}

Translation FailureToCurrent(std::string_view payload, uint64_t symbol, const BuildTables& tables) {
  Reader r(payload);
  uint64_t mode = 0;
  uint32_t unk = 0;
  if (symbol == kSNSLobbySessionFailurev3) mode = r.U64();
  const Guid channel = r.ReadGuid();
  const uint32_t code = r.U32();
  if (symbol == kSNSLobbySessionFailurev3) unk = r.U32();
  if (!r.ok()) return Bad();
  std::string out;
  PutU64(out, ToCurrent(tables.modes, mode));
  PutGuid(out, channel);
  PutU32(out, code);
  PutU32(out, unk);
  PutZeros(out, kFailureMessageSize);
  PutU64(out, 0);
  return ToService(kSNSLobbySessionFailurev4, std::move(out));
}

// ---- one frame's messages -------------------------------------------------------------------------

bool IsPassthrough(uint64_t symbol) {
  switch (symbol) {
    case nevr_evr_codec::kSymLoginSuccess:
    case nevr_evr_codec::kSymUpdateProfile:
    case nevr_evr_codec::kSymConfigRequest:
    case kSNSConfigSuccessv2:
    case nevr_evr_codec::kSymConfigFailure:
    case kSNSReconcileIAP:
    case kSNSReconcileIAPResult:
    case nevr_evr_codec::kSymMatchmakerStatusRequest:
    case kSNSLobbyMatchmakerStatus:
    case kSNSLobbyPlayerSessionsSuccessv3:
    case nevr_evr_codec::kSymConnectionUnrequire:
      return true;
    default:
      return false;
  }
}

}  // namespace

BuildTables Summer2019Tables() { return BuildTables{}; }

BuildTables Halloween2018Tables() {
  BuildTables t;
  t.loginSettingsSymbol = kSNSLoginClientSettings;
  t.profileRequestSymbol = kSNSProfileRequest;
  t.profileResponseSymbol = kSNSProfileResponse;
  t.matchEndedSymbol = kSNSMatchEnded;
  t.sessionFailureSymbol = kSNSLobbySessionFailurev2;
  return t;
}

Translation LegacyToCurrent(uint64_t symbol, std::string_view payload, const Context& ctx, const BuildTables& tables) {
  if (IsPassthrough(symbol)) return Make(Outcome::Passthrough);
  switch (symbol) {
    case nevr_evr_codec::kSymLoginSettings:
      return Make(Outcome::Passthrough);
    case kSNSLoginClientSettings:
      return SettingsToCurrent(payload, tables, symbol);
    case kSNSLoginRequest:
      return LoginRequestToCurrent(payload, ctx);
    case kSNSLoginProfileResult:
      return ProfileResultToCurrent(payload, ctx);
    case kSNSRefreshProfile:
      return RefreshProfileToCurrent(payload, ctx);
    case kSNSRefreshProfileResult:
      return LegacyProfileBodyToCurrent(payload, true);
    case kSNSProfileRequestv2:
    case kSNSProfileRequest:
      return ProfileRequestToCurrent(payload, ctx);
    case kSNSProfileResponsev2:
    case kSNSProfileResponse:
      return LegacyProfileBodyToCurrent(payload, false);
    case kSNSLeaderboardRequest:
      return LeaderboardAnswer(payload);
    case nevr_evr_codec::kSymTelemetryEvent:
    case kSNSMatchEnded:
    case kSNSMatchEndedv2:
      return Make(Outcome::Dropped);
    case kSNSLobbyFindSessionRequestv8:
      return FindToCurrent(payload, ctx, tables);
    case kSNSLobbyCreateSessionRequestv7:
      return CreateToCurrent(payload, ctx, tables);
    case kSNSLobbyJoinSessionRequestv6:
      return JoinToCurrent(payload, ctx, tables);
    case kSNSLobbyPlayerSessionsRequestv3:
      return PlayerSessionsToCurrent(payload, ctx, tables);
    case kSNSLobbyPendingSessionCancel: {
      if (payload.empty()) return Bad();
      return ToService(nevr_evr_codec::kSymPendingSessionCancel, GuidBytes(ctx.loginSession));
    }
    case kSNSLobbySessionSuccessv4:
      return SuccessV4ToCurrent(payload, ctx);
    case kSNSLobbySessionFailurev3:
    case kSNSLobbySessionFailurev2:
      return FailureToCurrent(payload, symbol, tables);
    default:
      return Make(Outcome::Unsupported);
  }
}

Translation CurrentToLegacy(uint64_t symbol, std::string_view payload, const Context& ctx, const BuildTables& tables,
                            ProfileReply reply) {
  if (IsPassthrough(symbol)) return Make(Outcome::Passthrough);
  switch (symbol) {
    case nevr_evr_codec::kSymLoginSettings:
      if (tables.loginSettingsSymbol == symbol) return Make(Outcome::Passthrough);
      return ToGame(tables.loginSettingsSymbol, std::string(payload));
    case nevr_evr_codec::kSymLoginRequest:
      return LoginRequestToLegacy(payload);
    case nevr_evr_codec::kSymLoginFailure:
      return LoginFailureToLegacy(payload, ctx);
    case nevr_evr_codec::kSymLoggedInUserProfileSuccess:
      return LoggedInProfileSuccessToLegacy(payload, ctx, tables, reply);
    case nevr_evr_codec::kSymOtherUserProfileSuccess:
      return OtherProfileSuccessToLegacy(payload, tables, reply);
    case nevr_evr_codec::kSymLoggedInUserProfileRequest:
      return LoggedInProfileRequestToLegacy(payload);
    case nevr_evr_codec::kSymOtherUserProfileRequest:
      return OtherProfileRequestToLegacy(payload, tables);
    case nevr_evr_codec::kSymFindSessionRequest:
      return FindToLegacy(payload, ctx, tables);
    case nevr_evr_codec::kSymCreateSessionRequest:
      return CreateToLegacy(payload, ctx, tables);
    case nevr_evr_codec::kSymJoinSessionRequest:
      return JoinToLegacy(payload, ctx, tables);
    case nevr_evr_codec::kSymPlayerSessionsRequest:
      return PlayerSessionsToLegacy(payload, tables);
    case nevr_evr_codec::kSymPendingSessionCancel: {
      if (payload.size() < kGuidSize) return Bad();
      return ToGame(kSNSLobbyPendingSessionCancel, std::string(1, '\0'));
    }
    case kSNSLobbySessionSuccessv5:
      return SuccessV5ToLegacy(payload, ctx);
    case kSNSLobbySessionFailurev4:
      return FailureToLegacy(payload, tables);
    default:
      return Make(Outcome::Unsupported);
  }
}

OutMessage BuildLoggedInUserProfileRequest(const Context& ctx) {
  std::string payload;
  PutGuid(payload, ctx.loginSession);
  PutUser(payload, ctx.self);
  PutCString(payload, CurrentProfileRequestJson(ctx));
  return {nevr_evr_codec::kSymLoggedInUserProfileRequest, std::move(payload)};
}

namespace {

// Walks the messages of a frame, calling `translate(symbol, payload)` for each and routing its output.
template <typename Translate>
FrameTranslation WalkFrame(const std::string& frame, bool fromGame, Translate translate) {
  FrameTranslation result;
  std::string& same = fromGame ? result.toService : result.toGame;  // where a passthrough goes
  std::size_t offset = 0;
  for (;;) {
    nevr_evr_codec::Message message;
    const nevr_evr_codec::ReadStatus status = nevr_evr_codec::ReadMessage(frame, offset, &message);
    if (status == nevr_evr_codec::ReadStatus::End) break;
    if (status != nevr_evr_codec::ReadStatus::Ok) {
      ++result.malformed;
      break;
    }
    const std::string_view payload(reinterpret_cast<const char*>(message.payload),
                                   static_cast<std::size_t>(message.length));
    const Translation t = translate(message.symbol, payload);
    switch (t.outcome) {
      case Outcome::Translated:
      case Outcome::Local:
        ++result.translated;
        break;
      case Outcome::Passthrough:
        ++result.passed;
        same.append(nevr_evr_codec::BuildMessage(message.symbol, payload));
        break;
      case Outcome::Dropped:
        ++result.dropped;
        break;
      case Outcome::Unsupported:
        ++result.unsupported;
        break;
      case Outcome::Malformed:
        ++result.malformed;
        break;
    }
    for (const OutMessage& m : t.toService) result.toService.append(nevr_evr_codec::BuildMessage(m.symbol, m.payload));
    for (const OutMessage& m : t.toGame) result.toGame.append(nevr_evr_codec::BuildMessage(m.symbol, m.payload));
    offset += nevr_evr_codec::kHeaderSize + static_cast<std::size_t>(message.length);
  }
  return result;
}

}  // namespace

FrameTranslation TranslateFrameFromGame(const std::string& frame, const Context& ctx, const BuildTables& tables) {
  return WalkFrame(frame, true, [&](uint64_t symbol, std::string_view payload) {
    return LegacyToCurrent(symbol, payload, ctx, tables);
  });
}

FrameTranslation TranslateFrameFromService(const std::string& frame, const Context& ctx, const BuildTables& tables,
                                           ProfileReply reply) {
  return WalkFrame(frame, false, [&](uint64_t symbol, std::string_view payload) {
    return CurrentToLegacy(symbol, payload, ctx, tables, reply);
  });
}

}  // namespace nevr_legacy_codec

#include "runtime/compat/evr_codec.h"

#include <cctype>
#include <cstring>
#include <limits>

#include "abi/symbol_hash.h"

namespace nevr_evr_codec {

uint64_t ReadLE64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
  return value;
}

void AppendLE64(std::string& buffer, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    buffer.push_back(static_cast<char>(value & 0xFF));
    value >>= 8;
  }
}

namespace {

constexpr std::size_t kLoginFailureFixedPayloadSize = 24;

ReadStatus ReadMessageAt(const uint8_t* data, std::size_t remaining, Message* out) {
  *out = Message{};
  if (remaining < kHeaderSize) return ReadStatus::End;
  if (std::memcmp(data, kMarker, kMarkerSize) != 0) return ReadStatus::BadMarker;
  out->symbol = ReadLE64(data + 8);
  out->length = ReadLE64(data + 16);
  if (out->length > remaining - kHeaderSize) return ReadStatus::Truncated;
  out->payload = data + kHeaderSize;
  return ReadStatus::Ok;
}

}  // namespace

std::string BuildMessage(uint64_t symbol, std::string_view payload) {
  std::string message;
  message.reserve(kHeaderSize + payload.size());
  message.append(reinterpret_cast<const char*>(kMarker), kMarkerSize);
  AppendLE64(message, symbol);
  AppendLE64(message, payload.size());
  message.append(payload);
  return message;
}

std::optional<std::string> BuildLoginRequest(uint64_t platformCode, uint64_t accountId,
                                             std::string_view profileJson) {
  if (profileJson.find('\0') != std::string_view::npos) return std::nullopt;

  std::string payload;
  payload.reserve(kLoginRequestFixedSize + profileJson.size() + 1);
  payload.append(kUuidSize, '\0');  // no previous session
  AppendLE64(payload, platformCode);
  AppendLE64(payload, accountId);
  payload.append(profileJson);
  payload.push_back('\0');
  return BuildMessage(kSymLoginRequest, payload);
}

std::string BuildLoginSuccess(uint64_t platformCode, uint64_t accountId) {
  std::string payload("NEVRSRVR", 8);
  payload.append(8, '\0');
  AppendLE64(payload, platformCode);
  AppendLE64(payload, accountId);
  return BuildMessage(kSymLoginSuccess, payload);
}

std::string BuildLoginRemovedNotify(const UserId& user, uint8_t reason, std::string_view json) {
  std::string payload;
  payload.reserve(kLoginRemovedFixedSize + json.size());
  if (kLoginRemovedUserIdSwapped) {
    AppendLE64(payload, user.accountId);
    AppendLE64(payload, user.platformCode);
  } else {
    AppendLE64(payload, user.platformCode);
    AppendLE64(payload, user.accountId);
  }
  for (int i = 0; i < 4; ++i) payload.push_back(static_cast<char>((kLoginRemovedWord10 >> (8 * i)) & 0xff));
  payload.push_back(static_cast<char>(reason));
  payload.append(kLoginRemovedFixedSize - payload.size(), '\0');
  payload.append(json.data(), json.size());
  return BuildMessage(kSymLoginRemovedNotify, payload);
}

std::optional<UserId> ParseLoginSuccessUserId(const std::string& frame) {
  Message message;
  if (ReadMessage(frame, 0, &message) != ReadStatus::Ok || message.symbol != kSymLoginSuccess ||
      message.length < kUuidSize + 16) {
    return std::nullopt;
  }
  UserId id;
  id.platformCode = ReadLE64(message.payload + kUuidSize);
  id.accountId = ReadLE64(message.payload + kUuidSize + 8);
  return id;
}

std::string BuildRemoteLogSet(const UserId& user, uint64_t level, const std::vector<std::string>& logs) {
  std::string payload;
  AppendLE64(payload, user.platformCode);
  AppendLE64(payload, user.accountId);
  for (int i = 0; i < 4; ++i) AppendLE64(payload, 0);  // the session UUID and the 16 text bytes
  AppendLE64(payload, level);
  const auto appendLE32 = [&payload](uint32_t value) {
    for (int i = 0; i < 4; ++i) payload.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
  };
  appendLE32(static_cast<uint32_t>(logs.size()));
  uint32_t offset = 0;
  for (const std::string& log : logs) {
    appendLE32(offset);
    offset += static_cast<uint32_t>(log.size() + 1);
  }
  for (const std::string& log : logs) {
    payload.append(log);
    payload.push_back('\0');
  }
  return BuildMessage(kSymRemoteLogSet, payload);
}

std::string BuildFriendListSubscribe() {
  return BuildMessage(kSymFriendListSubscribe, std::string(kFriendListSubscribePayloadSize, '\0'));
}

ReadStatus ReadMessage(const std::string& frame, std::size_t offset, Message* out) {
  if (offset > frame.size()) {
    *out = Message{};
    return ReadStatus::End;
  }
  return ReadMessageAt(reinterpret_cast<const uint8_t*>(frame.data()) + offset, frame.size() - offset, out);
}

uint64_t FirstSymbol(const std::string& frame) {
  if (frame.size() < kHeaderSize) return 0;
  return ReadLE64(reinterpret_cast<const uint8_t*>(frame.data()) + 8);
}

std::optional<LoginFailure> ParseLoginFailure(const std::string& frame) {
  if (frame.size() < kHeaderSize) return std::nullopt;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(frame.data());
  const uint64_t symbol = ReadLE64(bytes + 8);
  const uint64_t payloadLength = ReadLE64(bytes + 16);
  if (symbol != kSymLoginFailure ||
      payloadLength <= static_cast<uint64_t>(kLoginFailureFixedPayloadSize) ||
      payloadLength > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max() - kHeaderSize)) {
    return std::nullopt;
  }
  const std::size_t payloadSize = static_cast<std::size_t>(payloadLength);
  if (payloadSize > frame.size() - kHeaderSize) return std::nullopt;
  return LoginFailure{ReadLE64(bytes + kHeaderSize + 16), payloadSize - kLoginFailureFixedPayloadSize};
}

const char* PlatformPrefix(uint64_t platformCode) {
  switch (platformCode) {
    case 1: return "STM";
    case 2: return "DSC";
    case 3: return "XBX";
    case 4: return "OVR-ORG";
    case 5: return "OVR";
    case 6: return "BOT";
    case 7: return "DMO";
    default: return "UNK";
  }
}

uint64_t SelectPlatformCode(bool /*hasUrlCredentials*/, bool /*noOvr*/) { return kBridgeLoginPlatform; }

std::string SelectRemoteBearer(bool hasUrlCredentials, const std::string& jwt, const std::string& serverKey) {
  return hasUrlCredentials ? serverKey : jwt;
}

bool IsBearerReplacingPath(const std::string& url) {
  const std::size_t scheme = url.find("://");
  const std::size_t hostStart = scheme == std::string::npos ? 0 : scheme + 3;
  const std::size_t pathStart = url.find('/', hostStart);
  if (pathStart == std::string::npos) return false;
  const std::size_t pathEnd = url.find_first_of("?#", pathStart);
  return url.compare(pathStart, pathEnd == std::string::npos ? std::string::npos : pathEnd - pathStart, "/ws") == 0;
}

// ---- config request and reply keys --------------------------------------------------------------

uint64_t ConfigSymbol(std::string_view text) {
  if (text.empty()) return 0;
  if (text.size() == 18 && text[0] == '0' && text[1] == 'x') {
    uint64_t value = 0;
    bool hex = true;
    for (std::size_t i = 2; i < 18 && hex; ++i) {
      const char c = text[i];
      uint64_t digit = 0;
      if (c >= '0' && c <= '9') digit = static_cast<uint64_t>(c - '0');
      else if (c >= 'a' && c <= 'f') digit = static_cast<uint64_t>(c - 'a') + 10;
      else if (c >= 'A' && c <= 'F') digit = static_cast<uint64_t>(c - 'A') + 10;
      else hex = false;
      value = (value << 4) | digit;
    }
    if (hex) return value;
  }
  uint64_t hash = EchoVR::kCSymbol64Seed;
  for (const char raw : text) {
    uint8_t byte = static_cast<uint8_t>(raw);
    if (static_cast<uint8_t>(byte - 'A') <= 25) byte = static_cast<uint8_t>(byte + 32);
    // The game reads the name byte with MOVSX (echovr.exe CSymbol64::Lookup) and the service with int64(int8(ch)):
    // a byte >= 0x80 enters the hash sign-extended (nakama#710, #908).
    hash = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int8_t>(byte))) ^ EchoVR::kCSymbol64Table[(hash >> 56) & 0xFF] ^ (hash << 8);
  }
  return hash;
}

namespace {

struct JsonCursor {
  const char* p;
  const char* end;
};

void SkipJsonSpace(JsonCursor& c) {
  while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\n' || *c.p == '\r')) ++c.p;
}

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

bool ReadHex4(const char* p, const char* end, uint32_t* out) {
  if (end - p < 4) return false;
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    const char c = p[i];
    uint32_t d = 0;
    if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a') + 10;
    else if (c >= 'A' && c <= 'F') d = static_cast<uint32_t>(c - 'A') + 10;
    else return false;
    v = (v << 4) | d;
  }
  *out = v;
  return true;
}

// Length of the well-formed UTF-8 sequence at p (1-4), or 0 when the bytes are not one.
std::size_t Utf8SequenceLength(const char* p, const char* end) {
  const uint8_t b0 = static_cast<uint8_t>(*p);
  if (b0 < 0x80) return 1;
  std::size_t n = 0;
  uint32_t cp = 0;
  if (b0 >= 0xC2 && b0 <= 0xDF) { n = 2; cp = b0 & 0x1F; }
  else if (b0 >= 0xE0 && b0 <= 0xEF) { n = 3; cp = b0 & 0x0F; }
  else if (b0 >= 0xF0 && b0 <= 0xF4) { n = 4; cp = b0 & 0x07; }
  else return 0;
  if (static_cast<std::size_t>(end - p) < n) return 0;
  for (std::size_t i = 1; i < n; ++i) {
    const uint8_t b = static_cast<uint8_t>(p[i]);
    if ((b & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (b & 0x3F);
  }
  if (n == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return 0;
  if (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return 0;
  return n;
}

// Decodes the JSON string at c.p (the opening quote) as Go's encoding/json does: escapes resolved, a lone
// surrogate escape or an ill-formed UTF-8 byte becoming U+FFFD, a raw control character an error.
bool ParseJsonString(JsonCursor& c, std::string* out) {
  if (c.p >= c.end || *c.p != '"') return false;
  ++c.p;
  out->clear();
  while (c.p < c.end) {
    const char ch = *c.p;
    if (ch == '"') {
      ++c.p;
      return true;
    }
    if (static_cast<uint8_t>(ch) < 0x20) return false;
    if (ch != '\\') {
      if (static_cast<uint8_t>(ch) < 0x80) {
        out->push_back(ch);
        ++c.p;
        continue;
      }
      const std::size_t n = Utf8SequenceLength(c.p, c.end);
      if (n == 0) {
        AppendUtf8(*out, 0xFFFD);
        ++c.p;
      } else {
        out->append(c.p, n);
        c.p += n;
      }
      continue;
    }
    if (c.end - c.p < 2) return false;
    const char e = c.p[1];
    c.p += 2;
    switch (e) {
      case '"': out->push_back('"'); break;
      case '\\': out->push_back('\\'); break;
      case '/': out->push_back('/'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case 'u': {
        uint32_t cp = 0;
        if (!ReadHex4(c.p, c.end, &cp)) return false;
        c.p += 4;
        if (cp >= 0xD800 && cp < 0xDC00) {
          uint32_t low = 0;
          if (c.end - c.p >= 6 && c.p[0] == '\\' && c.p[1] == 'u' && ReadHex4(c.p + 2, c.end, &low) &&
              low >= 0xDC00 && low < 0xE000) {
            c.p += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else {
            cp = 0xFFFD;
          }
        } else if (cp >= 0xDC00 && cp < 0xE000) {
          cp = 0xFFFD;
        }
        AppendUtf8(*out, cp);
        break;
      }
      default: return false;
    }
  }
  return false;
}

bool SkipJsonValue(JsonCursor& c, int depth) {
  if (depth > 32) return false;
  SkipJsonSpace(c);
  if (c.p >= c.end) return false;
  const char ch = *c.p;
  if (ch == '"') {
    std::string ignored;
    return ParseJsonString(c, &ignored);
  }
  if (ch == '{' || ch == '[') {
    const char close = ch == '{' ? '}' : ']';
    ++c.p;
    SkipJsonSpace(c);
    if (c.p < c.end && *c.p == close) {
      ++c.p;
      return true;
    }
    for (;;) {
      if (ch == '{') {
        std::string key;
        SkipJsonSpace(c);
        if (!ParseJsonString(c, &key)) return false;
        SkipJsonSpace(c);
        if (c.p >= c.end || *c.p != ':') return false;
        ++c.p;
      }
      if (!SkipJsonValue(c, depth + 1)) return false;
      SkipJsonSpace(c);
      if (c.p >= c.end) return false;
      if (*c.p == ',') {
        ++c.p;
        continue;
      }
      if (*c.p == close) {
        ++c.p;
        return true;
      }
      return false;
    }
  }
  const char* start = c.p;
  while (c.p < c.end && (std::isalnum(static_cast<unsigned char>(*c.p)) != 0 || *c.p == '-' || *c.p == '+' || *c.p == '.')) ++c.p;
  return c.p != start;
}

bool FoldEqual(const std::string& a, const char* b) {
  const std::size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (std::size_t i = 0; i < n; ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
    if (x != y) return false;
  }
  return true;
}

// The two string fields `first` and `second` of the JSON object in `json`, as encoding/json fills a struct
// with two string fields: keys matched without regard to ASCII case, the last occurrence winning, a null
// leaving the field as it was, a missing field empty. False when the text is not such an object or a named
// field is not a string or null.
bool ExtractStringFields(std::string_view json, const char* first, const char* second, std::string* a,
                         std::string* b) {
  JsonCursor c{json.data(), json.data() + json.size()};
  a->clear();
  b->clear();
  SkipJsonSpace(c);
  if (c.p >= c.end || *c.p != '{') return false;
  ++c.p;
  SkipJsonSpace(c);
  if (c.p < c.end && *c.p == '}') return true;
  for (;;) {
    std::string key;
    SkipJsonSpace(c);
    if (!ParseJsonString(c, &key)) return false;
    SkipJsonSpace(c);
    if (c.p >= c.end || *c.p != ':') return false;
    ++c.p;
    SkipJsonSpace(c);
    std::string* target = FoldEqual(key, first) ? a : (FoldEqual(key, second) ? b : nullptr);
    if (target != nullptr) {
      if (c.p < c.end && *c.p == '"') {
        if (!ParseJsonString(c, target)) return false;
      } else if (c.end - c.p >= 4 && std::memcmp(c.p, "null", 4) == 0) {
        c.p += 4;
      } else {
        return false;
      }
    } else if (!SkipJsonValue(c, 0)) {
      return false;
    }
    SkipJsonSpace(c);
    if (c.p >= c.end) return false;
    if (*c.p == ',') {
      ++c.p;
      continue;
    }
    return *c.p == '}';
  }
}

}  // namespace

// The service's decoder (encoding/json) differs from this scanner on contrived input: nesting deeper than 32 levels
// in an unrelated field, or a pad byte that is itself '{' (taken here as no pad byte). Such a request gets no key
// (or a different one), and its reply, which carries the service's key, is dropped as unmatched.
std::optional<ConfigKey> ParseConfigRequestKey(std::string_view payload) {
  if (payload.empty()) return std::nullopt;
  // One pad byte first, whatever its value: the game's first request has '{' there, so a payload that starts with
  // '{' is read as it stands first and, when that is not a JSON object, without its first byte.
  std::string type, id;
  if (payload[0] == '{' && ExtractStringFields(payload, "type", "id", &type, &id)) {
    return ConfigKey{ConfigSymbol(type), ConfigSymbol(id)};
  }
  if (!ExtractStringFields(payload.substr(1), "type", "id", &type, &id)) return std::nullopt;
  return ConfigKey{ConfigSymbol(type), ConfigSymbol(id)};
}

std::optional<ConfigKey> ParseConfigReplyKey(uint64_t symbol, std::string_view payload) {
  if (payload.size() < 16) return std::nullopt;
  if (symbol == kSymConfigSuccess) {
    return ConfigKey{ReadLE64(reinterpret_cast<const uint8_t*>(payload.data())),
                     ReadLE64(reinterpret_cast<const uint8_t*>(payload.data()) + 8)};
  }
  if (symbol == kSymConfigFailure) {
    std::string type, identifier;
    if (!ExtractStringFields(payload.substr(16), "type", "identifier", &type, &identifier)) return std::nullopt;
    return ConfigKey{ConfigSymbol(type), ConfigSymbol(identifier)};
  }
  return std::nullopt;
}

}  // namespace nevr_evr_codec

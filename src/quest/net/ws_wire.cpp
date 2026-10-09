#include "quest/net/ws_wire.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace quest_net {

namespace {

// ---- SHA-1 (RFC 3174), only for Sec-WebSocket-Accept; not a security primitive here ---------------

struct Sha1 {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  uint8_t block[64] = {};
  std::size_t fill = 0;
  uint64_t totalBits = 0;

  static uint32_t Rol(uint32_t v, unsigned n) { return (v << n) | (v >> (32u - n)); }

  void Process() {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t temp = Rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = Rol(b, 30);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }

  void Update(std::string_view data) {
    for (const char ch : data) {
      block[fill++] = static_cast<uint8_t>(ch);
      totalBits += 8;
      if (fill == 64) {
        Process();
        fill = 0;
      }
    }
  }

  void Finish(uint8_t out[20]) {
    const uint64_t bits = totalBits;
    block[fill++] = 0x80;
    if (fill > 56) {
      while (fill < 64) block[fill++] = 0;
      Process();
      fill = 0;
    }
    while (fill < 56) block[fill++] = 0;
    for (int i = 7; i >= 0; --i) block[fill++] = static_cast<uint8_t>(bits >> (i * 8));
    Process();
    for (int i = 0; i < 5; ++i) {
      out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
  }
};

std::string Base64(const uint8_t* data, std::size_t size) {
  static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  std::size_t i = 0;
  for (; i + 2 < size; i += 3) {
    const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
    out.push_back(kTable[(v >> 18) & 0x3F]);
    out.push_back(kTable[(v >> 12) & 0x3F]);
    out.push_back(kTable[(v >> 6) & 0x3F]);
    out.push_back(kTable[v & 0x3F]);
  }
  if (size - i == 1) {
    const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    out.push_back(kTable[(v >> 18) & 0x3F]);
    out.push_back(kTable[(v >> 12) & 0x3F]);
    out += "==";
  } else if (size - i == 2) {
    const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    out.push_back(kTable[(v >> 18) & 0x3F]);
    out.push_back(kTable[(v >> 12) & 0x3F]);
    out.push_back(kTable[(v >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool IEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (Lower(a[i]) != Lower(b[i])) return false;
  }
  return true;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// True when a comma-separated header value contains `token` (case-insensitive).
bool HasToken(std::string_view value, std::string_view token) {
  while (!value.empty()) {
    const std::size_t comma = value.find(',');
    const std::string_view part = Trim(value.substr(0, comma));
    if (IEquals(part, token)) return true;
    if (comma == std::string_view::npos) break;
    value.remove_prefix(comma + 1);
  }
  return false;
}

}  // namespace

std::string ComputeAcceptKey(std::string_view clientKey) {
  Sha1 sha;
  sha.Update(clientKey);
  sha.Update("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
  uint8_t digest[20];
  sha.Finish(digest);
  return Base64(digest, sizeof(digest));
}

HandshakeStatus ParseUpgradeRequest(std::string_view buffer, UpgradeRequest* out) {
  const std::size_t end = buffer.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    return buffer.size() >= kMaxHandshakeBytes ? HandshakeStatus::TooLarge : HandshakeStatus::NeedMore;
  }
  if (end + 4 > kMaxHandshakeBytes) return HandshakeStatus::TooLarge;
  const std::string_view head = buffer.substr(0, end);

  std::size_t lineEnd = head.find("\r\n");
  const std::string_view requestLine = head.substr(0, lineEnd);
  // "GET <target> HTTP/1.1". The target is returned verbatim for the access-token check; nothing else
  // uses it (the adapter decides the remote URL from its own configuration, never from this path).
  if (requestLine.substr(0, 4) != "GET " || requestLine.size() < 14 ||
      requestLine.substr(requestLine.size() - 9) != " HTTP/1.1") {
    return HandshakeStatus::Bad;
  }
  const std::string_view target = requestLine.substr(4, requestLine.size() - 4 - 9);
  if (target.empty() || target.find(' ') != std::string_view::npos) return HandshakeStatus::Bad;

  bool upgradeWebSocket = false, connectionUpgrade = false, version13 = false, hasOrigin = false;
  std::string key;
  std::string_view rest = lineEnd == std::string_view::npos ? std::string_view() : head.substr(lineEnd + 2);
  while (!rest.empty()) {
    const std::size_t next = rest.find("\r\n");
    const std::string_view line = rest.substr(0, next);
    rest = next == std::string_view::npos ? std::string_view() : rest.substr(next + 2);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) return HandshakeStatus::Bad;
    const std::string_view name = line.substr(0, colon);
    const std::string_view value = Trim(line.substr(colon + 1));
    if (IEquals(name, "Upgrade")) {
      upgradeWebSocket = HasToken(value, "websocket");
    } else if (IEquals(name, "Connection")) {
      connectionUpgrade = connectionUpgrade || HasToken(value, "Upgrade");
    } else if (IEquals(name, "Sec-WebSocket-Version")) {
      version13 = HasToken(value, "13");
    } else if (IEquals(name, "Sec-WebSocket-Key")) {
      key.assign(value);
    } else if (IEquals(name, "Origin")) {
      hasOrigin = true;
    }
  }
  // A key is 16 random bytes in base64: 24 characters. Anything that cannot be that is not a client.
  if (!upgradeWebSocket || !connectionUpgrade || !version13 || key.size() != 24) return HandshakeStatus::Bad;
  out->key = std::move(key);
  out->target.assign(target);
  out->hasOrigin = hasOrigin;
  out->consumed = end + 4;
  return HandshakeStatus::Ok;
}

std::string BuildUpgradeResponse(std::string_view clientKey) {
  std::string response =
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
  response += ComputeAcceptKey(clientKey);
  response += "\r\n\r\n";
  return response;
}

std::string BuildBadRequestResponse() {
  return "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
}

std::string BuildForbiddenResponse() {
  return "HTTP/1.1 403 Forbidden\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
}

bool ConstantTimeEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  volatile uint8_t diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff = static_cast<uint8_t>(diff | (static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i])));
  }
  return diff == 0;
}

bool TargetCarriesToken(std::string_view target, std::string_view token) {
  if (token.size() != kTokenHexLength) return false;  // no token configured: nothing can match
  const std::size_t q = target.find('?');
  const std::string_view path = target.substr(0, q);
  const std::string_view query = q == std::string_view::npos ? std::string_view() : target.substr(q + 1);

  // Path form: "/<token>" followed by end of path or "/". Always compared, even when the shape is wrong,
  // against a same-length slice so the work does not depend on where the input diverges.
  bool pathOk = false;
  {
    std::string_view candidate = path.size() > 1 && path[0] == '/' ? path.substr(1) : std::string_view();
    const bool shapeOk = candidate.size() >= token.size() &&
                         (candidate.size() == token.size() || candidate[token.size()] == '/');
    candidate = candidate.substr(0, token.size());
    std::string padded(candidate);
    padded.resize(token.size(), '\0');
    pathOk = ConstantTimeEquals(padded, token) && shapeOk;
  }

  // Query form: a "nevr_token=<token>" pair among the '&'-separated pairs.
  bool queryOk = false;
  {
    std::string_view rest = query;
    const std::string_view name = kTokenQueryName;
    while (!rest.empty()) {
      const std::size_t amp = rest.find('&');
      const std::string_view pair = rest.substr(0, amp);
      rest = amp == std::string_view::npos ? std::string_view() : rest.substr(amp + 1);
      if (pair.size() > name.size() && pair.substr(0, name.size()) == name && pair[name.size()] == '=') {
        if (ConstantTimeEquals(pair.substr(name.size() + 1), token)) queryOk = true;
      }
    }
  }
  return pathOk || queryOk;
}

std::string HexEncode(const uint8_t* bytes, std::size_t count) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 0x0F]);
  }
  return out;
}

// ---- frames ------------------------------------------------------------------------------------

void FrameDecoder::Feed(const void* data, std::size_t size) {
  if (failed_ || size == 0) return;
  // Reclaim consumed bytes before growing, so a long-lived connection does not accumulate its history.
  if (offset_ > 0 && (offset_ >= buffer_.size() || offset_ > 4096)) {
    buffer_.erase(0, offset_);
    offset_ = 0;
  }
  buffer_.append(static_cast<const char*>(data), size);
}

DecodeStatus FrameDecoder::Next(Message* out) {
  if (failed_) return DecodeStatus::ProtocolError;
  for (;;) {
    const std::size_t avail = buffer_.size() - offset_;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buffer_.data()) + offset_;
    if (avail < 2) return DecodeStatus::NeedMore;

    const bool fin = (p[0] & 0x80) != 0;
    const bool rsv = (p[0] & 0x70) != 0;
    const uint8_t op = p[0] & 0x0F;
    const bool masked = (p[1] & 0x80) != 0;
    uint64_t len = p[1] & 0x7F;
    std::size_t header = 2;
    if (rsv) {
      failed_ = true;
      return DecodeStatus::ProtocolError;  // no extension was negotiated
    }
    const bool control = (op & 0x8) != 0;
    if (op != 0x0 && op != 0x1 && op != 0x2 && op != 0x8 && op != 0x9 && op != 0xA) {
      failed_ = true;
      return DecodeStatus::ProtocolError;
    }
    if (control && (!fin || len > 125)) {
      failed_ = true;
      return DecodeStatus::ProtocolError;
    }
    if (len == 126) {
      if (avail < 4) return DecodeStatus::NeedMore;
      len = (static_cast<uint64_t>(p[2]) << 8) | p[3];
      header = 4;
    } else if (len == 127) {
      if (avail < 10) return DecodeStatus::NeedMore;
      len = 0;
      for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
      header = 10;
      if ((len >> 63) != 0) {
        failed_ = true;
        return DecodeStatus::ProtocolError;
      }
    }
    // The limit applies to the frame itself and to the message it would complete, before any of the
    // payload is buffered: a declared 4 GiB frame is refused on its header.
    if (len > maxMessageBytes_ || (!control && fragment_.size() + len > maxMessageBytes_)) {
      failed_ = true;
      return DecodeStatus::TooBig;
    }
    const std::size_t maskBytes = masked ? 4 : 0;
    if (avail < header + maskBytes + len) return DecodeStatus::NeedMore;

    const uint8_t* maskKey = p + header;
    const uint8_t* payload = p + header + maskBytes;
    std::string data(reinterpret_cast<const char*>(payload), static_cast<std::size_t>(len));
    if (masked) {
      for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(static_cast<uint8_t>(data[i]) ^ maskKey[i & 3]);
    } else {
      ++unmasked_;
    }
    offset_ += header + maskBytes + static_cast<std::size_t>(len);

    if (control) {
      out->opcode = static_cast<Opcode>(op);
      out->payload = std::move(data);
      out->closeCode = 0;
      if (op == 0x8) {
        if (out->payload.size() == 1) {
          failed_ = true;
          return DecodeStatus::ProtocolError;
        }
        out->closeCode = out->payload.size() >= 2
                             ? static_cast<uint16_t>((static_cast<uint8_t>(out->payload[0]) << 8) |
                                                     static_cast<uint8_t>(out->payload[1]))
                             : static_cast<uint16_t>(1005);
      }
      return DecodeStatus::Message;  // control frames may interleave a fragmented message
    }
    if (op == 0x0) {
      if (!inFragment_) {
        failed_ = true;
        return DecodeStatus::ProtocolError;
      }
      fragment_ += data;
    } else {
      if (inFragment_) {
        failed_ = true;  // a new data frame while a fragmented message is open
        return DecodeStatus::ProtocolError;
      }
      fragmentOpcode_ = static_cast<Opcode>(op);
      fragment_ = std::move(data);
      inFragment_ = true;
    }
    if (fin) {
      out->opcode = fragmentOpcode_;
      out->payload = std::move(fragment_);
      out->closeCode = 0;
      fragment_.clear();
      inFragment_ = false;
      return DecodeStatus::Message;
    }
  }
}

std::string BuildFrame(Opcode opcode, std::string_view payload) {
  std::string frame;
  frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));
  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    frame.push_back(126);
    frame.push_back(static_cast<char>(payload.size() >> 8));
    frame.push_back(static_cast<char>(payload.size() & 0xFF));
  } else {
    frame.push_back(127);
    for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((static_cast<uint64_t>(payload.size()) >> (i * 8)) & 0xFF));
  }
  frame.append(payload);
  return frame;
}

std::string BuildCloseFrame(uint16_t code, std::string_view reason) {
  std::string payload;
  payload.push_back(static_cast<char>(code >> 8));
  payload.push_back(static_cast<char>(code & 0xFF));
  // A close payload is at most 125 bytes (RFC 6455 5.5).
  payload.append(reason.substr(0, 123));
  return BuildFrame(Opcode::Close, payload);
}

std::string BuildMaskedFrame(Opcode opcode, std::string_view payload, const uint8_t maskKey[4], bool fin) {
  std::string frame;
  frame.push_back(static_cast<char>((fin ? 0x80 : 0x00) | static_cast<uint8_t>(opcode)));
  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(0x80 | payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    frame.push_back(static_cast<char>(0x80 | 126));
    frame.push_back(static_cast<char>(payload.size() >> 8));
    frame.push_back(static_cast<char>(payload.size() & 0xFF));
  } else {
    frame.push_back(static_cast<char>(0x80 | 127));
    for (int i = 7; i >= 0; --i) frame.push_back(static_cast<char>((static_cast<uint64_t>(payload.size()) >> (i * 8)) & 0xFF));
  }
  frame.append(reinterpret_cast<const char*>(maskKey), 4);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    frame.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ maskKey[i & 3]));
  }
  return frame;
}

}  // namespace quest_net

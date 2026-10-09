#pragma once
// RFC 6455 server-side wire pieces for the loopback listener the game's redirected connection reaches:
// the HTTP upgrade handshake and an incremental frame decoder/encoder. Pure byte logic: no sockets, no
// threads, no logging, no Android headers, so every partial-read and malformed-input case is a host test.
//
// The game dials ws://127.0.0.2:<port> exactly as it dials the community service on PCVR, so it speaks the
// client half of this protocol (that it does so on Quest is the ADR 0003 premise "Quest and PCVR run the
// same game networking code"; the Quest binary's handshake bytes are not captured yet).
//
// Leniency, stated once: a frame without the mask bit is accepted (RFC 6455 5.1 requires clients to mask).
// The peer is the game on loopback, and a strict check would reject a client whose masking behaviour has
// not been observed. The count of unmasked frames is exposed so the adapter can log it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace quest_net {

// ---- handshake ---------------------------------------------------------------------------------

// Sec-WebSocket-Accept for a client key (base64(SHA-1(key + GUID))).
std::string ComputeAcceptKey(std::string_view clientKey);

enum class HandshakeStatus {
  NeedMore,  // the request line and headers are not complete yet
  Ok,
  Bad,       // not a WebSocket upgrade we can answer
  TooLarge,  // headers exceed kMaxHandshakeBytes without ending
};

inline constexpr std::size_t kMaxHandshakeBytes = 8192;

struct UpgradeRequest {
  std::string key;        // Sec-WebSocket-Key
  std::string target;     // the request target exactly as sent ("/path?query"); may carry the access token
  bool hasOrigin = false;  // an Origin header was present (a browser or webview, never the game)
  std::size_t consumed = 0;  // bytes of the buffer the request occupied; frame bytes may follow
};

// ---- access token -------------------------------------------------------------------------------
// The loopback listener is reachable by every local app, so the upgrade must prove it was dialled with the
// URI the runtime handed the game. The token is 128 random bits as 32 lowercase hex characters. It may
// ride as the first path segment ("/<token>/..." or "/<token>") or as the query parameter
// "nevr_token=<token>": the game's WebSocket client sends the URI's path and query verbatim as the request
// target (CWebSocketCodec::SendHandshakeRequest builds "%s%s%s%s" from path, "?" and query).
inline constexpr std::size_t kTokenHexLength = 32;
inline constexpr char kTokenQueryName[] = "nevr_token";

// Compares without an early exit on the first differing byte. Different lengths are not equal (lengths
// are not secret).
bool ConstantTimeEquals(std::string_view a, std::string_view b);
// True when `target` carries `token` in either supported place. Evaluates both places and every byte.
bool TargetCarriesToken(std::string_view target, std::string_view token);
// 32 lowercase hex characters from 16 raw bytes.
std::string HexEncode(const uint8_t* bytes, std::size_t count);

HandshakeStatus ParseUpgradeRequest(std::string_view buffer, UpgradeRequest* out);
std::string BuildUpgradeResponse(std::string_view clientKey);
// What the listener answers a request it will not upgrade.
std::string BuildBadRequestResponse();
// What the listener answers an upgrade that lacks the access token or carries an Origin header.
std::string BuildForbiddenResponse();

// ---- frames ------------------------------------------------------------------------------------

enum class Opcode : uint8_t { Continuation = 0x0, Text = 0x1, Binary = 0x2, Close = 0x8, Ping = 0x9, Pong = 0xA };

struct Message {
  Opcode opcode = Opcode::Binary;  // Text or Binary for data; Close, Ping, Pong for control
  std::string payload;             // unmasked; reassembled across fragments
  uint16_t closeCode = 0;          // Close only; 1005 when the payload had none
};

enum class DecodeStatus {
  NeedMore,        // feed more bytes
  Message,         // `out` holds a message
  ProtocolError,   // malformed stream: close with 1002 and stop decoding
  TooBig,          // a message exceeds the limit: close with 1009 and stop decoding
};

class FrameDecoder {
 public:
  explicit FrameDecoder(std::size_t maxMessageBytes) : maxMessageBytes_(maxMessageBytes) {}

  // Appends bytes. Call Next until NeedMore after every Feed: Next refuses a frame whose header declares
  // more than the message limit before any of its payload is buffered, so the buffer never holds more
  // than one in-limit frame plus the chunk just fed.
  void Feed(const void* data, std::size_t size);
  // Extracts the next complete message. Call until it returns NeedMore. After ProtocolError or TooBig the
  // decoder stays failed.
  DecodeStatus Next(Message* out);

  std::size_t Buffered() const { return buffer_.size() - offset_; }
  uint64_t UnmaskedFrames() const { return unmasked_; }

 private:
  std::size_t maxMessageBytes_;
  std::string buffer_;
  std::size_t offset_ = 0;
  bool failed_ = false;
  // reassembly of a fragmented data message
  bool inFragment_ = false;
  Opcode fragmentOpcode_ = Opcode::Binary;
  std::string fragment_;
  uint64_t unmasked_ = 0;
};

// A server-to-client frame (never masked). FIN set, no fragmentation.
std::string BuildFrame(Opcode opcode, std::string_view payload);
std::string BuildCloseFrame(uint16_t code, std::string_view reason);
// A client-to-server frame with the given mask key; used by tests and by anything acting as a client.
std::string BuildMaskedFrame(Opcode opcode, std::string_view payload, const uint8_t maskKey[4], bool fin = true);

}  // namespace quest_net

// peer_message_rules.h: nEVR's own control messages between a dedicated server and its clients, carried on the game's
// broadcaster (#2365).
//
// A message is a gameservice.v1.Envelope inside the game message NEVRProtobufMessageV1 (symbol 0x9ee5107d9e29fd63, the
// wrapper the game service already uses for the same Envelope). It travels on the peer broadcaster of the game's net
// game, reliable (`dest = ~0`), so the game's own transport carries, orders and resends it. A stock client has no
// listener for the symbol, and the broadcaster drops a symbol nobody listens for (#2365: InvokeListeners echovr.exe
// 0x140f80860).
//
// Until nevr-proto has a CombatMessage arm, the payload is the Envelope's SNSUnknownMessage { type, data }: `type` names
// the nEVR message, `data` is its JSON body. This slice carries three:
//   - capabilities (server -> each client): {"gear":["smoke",...]}, the gear the server offers, from its features;
//   - ping (either way): {"seq":n};
//   - pong (the answer to a ping, to its sender): {"seq":n}.
//
// What is refused, before anything acts on it, and logged as a Warning by the platform half (peer_message.cpp):
//   - empty or oversize (over kMaxEnvelopeBytes) input; an Envelope that does not parse, or whose arm is not
//     SNSUnknownMessage; an unknown `type`; a body that is not the JSON object the type needs (wrong_sender aside, each
//     is a malformed message and changes nothing);
//   - on a client, a message from any peer but the server's authority peer;
//   - on the server, a message from a peer not in the match, and a capabilities message (only the server sends one).
//
// Header only and free of game and platform dependencies, so the PC runtime and the Quest build share it and a host test
// drives it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "gameservice/v1/gameservice.pb.h"
#include "nevr/symbol_hash.h"

namespace nevr_peer_message {

// The game message the Envelope rides in. The game service wraps the same Envelope in it on the TCP side
// (src/runtime/server/protobuf_transport.h kProtobufMessageSymbol).
inline constexpr std::uint64_t kMessageSymbol = 0x9ee5107d9e29fd63ULL;

// `SNSUnknownMessage.type` of each nEVR message: the game's own 64-bit symbol hash of its name.
inline constexpr std::uint64_t kCapabilitiesType = EchoVR::CSymbol64Hash("nevr_peer_capabilities");
inline constexpr std::uint64_t kPingType = EchoVR::CSymbol64Hash("nevr_peer_ping");
inline constexpr std::uint64_t kPongType = EchoVR::CSymbol64Hash("nevr_peer_pong");

// The largest Envelope taken or sent. One game packet is 0x4f0 bytes (#2365); a control message has no reason to need
// more than one, and a larger one is refused before it is parsed.
inline constexpr std::size_t kMaxEnvelopeBytes = 1024;

// The gear list: at most this many names, each 1 to 32 bytes of [a-z0-9_] (the service's feature-name rule,
// runtime/compat/server_features.h), so a name can be logged as it is.
inline constexpr std::size_t kMaxGear = 32;
inline constexpr std::size_t kMaxGearName = 32;

enum class Kind { kNone, kCapabilities, kPing, kPong };

enum class Refusal {
  kNone,
  kEmpty,          // no bytes
  kOversize,       // more than kMaxEnvelopeBytes
  kUndecodable,    // not an Envelope
  kUnexpectedArm,  // an Envelope, but not SNSUnknownMessage
  kUnknownType,    // SNSUnknownMessage with a type this build does not know
  kBadBody,        // a known type whose body is not the JSON it needs
  kWrongSender,    // on a client: not the authority peer; on the server: not a peer in the match
  kNotForServer,   // a capabilities message received by the server
};

/// The token a log line carries for a refusal (Rule 1: the reason as a word).
inline const char* RefusalName(Refusal r) {
  switch (r) {
    case Refusal::kNone: return "none";
    case Refusal::kEmpty: return "empty";
    case Refusal::kOversize: return "oversize";
    case Refusal::kUndecodable: return "undecodable";
    case Refusal::kUnexpectedArm: return "unexpected_message";
    case Refusal::kUnknownType: return "unknown_type";
    case Refusal::kBadBody: return "bad_body";
    case Refusal::kWrongSender: return "wrong_sender";
    case Refusal::kNotForServer: return "not_for_server";
  }
  return "unknown";
}

inline const char* KindName(Kind k) {
  switch (k) {
    case Kind::kNone: return "none";
    case Kind::kCapabilities: return "capabilities";
    case Kind::kPing: return "ping";
    case Kind::kPong: return "pong";
  }
  return "unknown";
}

struct Message {
  Kind kind = Kind::kNone;
  std::vector<std::string> gear;  // capabilities
  std::uint64_t seq = 0;          // ping, pong
};

struct Decoded {
  Refusal refusal = Refusal::kNone;
  Message message;
};

inline bool IsGearName(std::string_view name) {
  if (name.empty() || name.size() > kMaxGearName) return false;
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

/// The gear a server offers, from the features it states (runtime/compat/server_features.h): EchoCombat brings the
/// three tac mods (UMBRAL VEIL `smoke`, OVERCHARGE `buff`, POINT `point`), and its MEDIGUN feature brings `medigun`.
inline std::vector<std::string> GearFromFeatures(const std::vector<std::string>& features) {
  bool combat = false;
  bool medigun = false;
  for (const std::string& f : features) {
    if (f == "echocombat") combat = true;
    if (f == "echocombat_medigun") medigun = true;
  }
  std::vector<std::string> gear;
  if (combat) {
    gear = {"smoke", "buff", "point"};
    if (medigun) gear.push_back("medigun");
  }
  return gear;
}

namespace detail {

inline std::string EncodeUnknown(std::uint64_t type, const nlohmann::json& body) {
  gameservice::v1::Envelope envelope;
  auto* unknown = envelope.mutable_unknown_message();
  unknown->mutable_type()->set_value(type);
  unknown->set_data(body.dump());
  std::string out;
  if (!envelope.SerializeToString(&out)) out.clear();
  return out;
}

inline bool ReadSeq(const nlohmann::json& body, std::uint64_t* seq) {
  const auto it = body.find("seq");
  if (it == body.end() || !it->is_number_unsigned()) return false;
  *seq = it->get<std::uint64_t>();
  return true;
}

}  // namespace detail

/// The Envelope bytes of a capabilities message. Empty when a gear name breaks the name rule or there are more than
/// kMaxGear names: the sender does not send what a receiver would refuse.
inline std::string EncodeCapabilities(const std::vector<std::string>& gear) {
  if (gear.size() > kMaxGear) return {};
  for (const std::string& g : gear) {
    if (!IsGearName(g)) return {};
  }
  return detail::EncodeUnknown(kCapabilitiesType, nlohmann::json{{"gear", gear}});
}

inline std::string EncodePing(std::uint64_t seq) { return detail::EncodeUnknown(kPingType, nlohmann::json{{"seq", seq}}); }
inline std::string EncodePong(std::uint64_t seq) { return detail::EncodeUnknown(kPongType, nlohmann::json{{"seq", seq}}); }

/// Reads one received Envelope. Never throws: every malformed input is a refusal.
inline Decoded Decode(const void* bytes, std::size_t size) {
  Decoded out;
  if (bytes == nullptr || size == 0) {
    out.refusal = Refusal::kEmpty;
    return out;
  }
  if (size > kMaxEnvelopeBytes) {
    out.refusal = Refusal::kOversize;
    return out;
  }
  gameservice::v1::Envelope envelope;
  if (!envelope.ParseFromArray(bytes, static_cast<int>(size))) {
    out.refusal = Refusal::kUndecodable;
    return out;
  }
  if (envelope.message_case() != gameservice::v1::Envelope::kUnknownMessage) {
    out.refusal = Refusal::kUnexpectedArm;
    return out;
  }
  const auto& unknown = envelope.unknown_message();
  const std::uint64_t type = unknown.type().value();
  Kind kind = Kind::kNone;
  if (type == kCapabilitiesType) kind = Kind::kCapabilities;
  else if (type == kPingType) kind = Kind::kPing;
  else if (type == kPongType) kind = Kind::kPong;
  if (kind == Kind::kNone) {
    out.refusal = Refusal::kUnknownType;
    return out;
  }
  // A body that is not JSON parses to a discarded value, and one that is not an object (an array, a number) has no keys:
  // both fail the key reads below, so either is kBadBody there.
  const nlohmann::json body = nlohmann::json::parse(unknown.data(), nullptr, /*allow_exceptions=*/false);
  out.message.kind = kind;
  if (kind == Kind::kCapabilities) {
    const auto it = body.find("gear");
    if (it == body.end() || !it->is_array() || it->size() > kMaxGear) {
      out.refusal = Refusal::kBadBody;
      return out;
    }
    for (const auto& g : *it) {
      if (!g.is_string() || !IsGearName(g.get_ref<const std::string&>())) {
        out.refusal = Refusal::kBadBody;
        out.message.gear.clear();
        return out;
      }
      out.message.gear.push_back(g.get<std::string>());
    }
    return out;
  }
  if (!detail::ReadSeq(body, &out.message.seq)) out.refusal = Refusal::kBadBody;
  return out;
}

enum class Role { kClient, kServer };

/// Whether a decoded message is taken from `sender`. A client takes messages only from the server's authority peer
/// (`authority` empty: no session yet, so nothing is taken). The server takes them only from a peer in its match, and
/// never takes a capabilities message.
inline Refusal Admit(Role role, Kind kind, std::uint64_t sender, std::optional<std::uint64_t> authority,
                     const std::vector<std::uint64_t>& matchPeers) {
  if (role == Role::kClient) {
    if (!authority || sender != *authority) return Refusal::kWrongSender;
    return Refusal::kNone;
  }
  bool inMatch = false;
  for (const std::uint64_t p : matchPeers) {
    if (p == sender) inMatch = true;
  }
  if (!inMatch) return Refusal::kWrongSender;
  if (kind == Kind::kCapabilities) return Refusal::kNotForServer;
  return Refusal::kNone;
}

/// A peer can send as fast as its link allows, so refusal Warnings are bounded: at most kRefusalLinesPerWindow lines in a
/// kRefusalWindowMs window. The refusals a window does not write are counted, and the next written line carries that
/// count (`suppressed=<n>`), so no refusal goes uncounted.
inline constexpr std::uint64_t kRefusalWindowMs = 10000;
inline constexpr std::uint32_t kRefusalLinesPerWindow = 10;

class RefusalLimiter {
 public:
  /// True when this refusal is written now; `*suppressed` is then the count of refusals not written since the last
  /// written one (0 otherwise).
  bool Take(std::uint64_t nowMs, std::uint64_t* suppressed) {
    if (!started_ || nowMs - windowStartMs_ >= kRefusalWindowMs) {
      started_ = true;
      windowStartMs_ = nowMs;
      written_ = 0;
    }
    if (written_ >= kRefusalLinesPerWindow) {
      ++pending_;
      return false;
    }
    ++written_;
    *suppressed = pending_;
    pending_ = 0;
    return true;
  }

 private:
  bool started_ = false;
  std::uint64_t windowStartMs_ = 0;
  std::uint32_t written_ = 0;
  std::uint64_t pending_ = 0;
};

/// The pings this side sent and not yet heard answered, for the round trip of each pong. Holds at most kMaxOutstanding:
/// the oldest is forgotten first, so a peer that never answers costs a fixed amount of memory.
class PingBook {
 public:
  static constexpr std::size_t kMaxOutstanding = 16;

  std::uint64_t Next(std::uint64_t nowMs) {
    const std::uint64_t seq = ++last_;
    if (count_ == kMaxOutstanding) {
      for (std::size_t i = 1; i < count_; ++i) sent_[i - 1] = sent_[i];
      --count_;
    }
    sent_[count_++] = {seq, nowMs};
    return seq;
  }

  /// The round trip of the ping `seq`, when this side sent it and it is not answered yet; it is then answered.
  std::optional<std::uint64_t> Answer(std::uint64_t seq, std::uint64_t nowMs) {
    for (std::size_t i = 0; i < count_; ++i) {
      if (sent_[i].seq != seq) continue;
      const std::uint64_t rtt = nowMs >= sent_[i].ms ? nowMs - sent_[i].ms : 0;
      for (std::size_t j = i + 1; j < count_; ++j) sent_[j - 1] = sent_[j];
      --count_;
      return rtt;
    }
    return std::nullopt;
  }

 private:
  struct Sent {
    std::uint64_t seq;
    std::uint64_t ms;
  };
  Sent sent_[kMaxOutstanding] = {};
  std::size_t count_ = 0;
  std::uint64_t last_ = 0;
};

/// "smoke,buff,point" for a log line (each name passed IsGearName); "none" for an empty list.
inline std::string GearText(const std::vector<std::string>& gear) {
  if (gear.empty()) return "none";
  std::string out;
  for (const std::string& g : gear) {
    if (!out.empty()) out += ',';
    out += g;
  }
  return out;
}

}  // namespace nevr_peer_message

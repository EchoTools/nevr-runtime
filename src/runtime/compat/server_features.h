// server_features.h: what a server states about itself to the clients that join it, and what a client reads of it (#2333).
//
// A fact about the server is the server's to state. A dedicated server lists its features (today: whether it runs
// EchoCombat, and whether it allows MEDIGUN) in two places:
//   - its ServerDB registration (`features`, GameServerRegistrationMessage field 13), which the game service stores and
//     matches finds and joins against (the match label's `features`);
//   - the lobby settings document it gives the game (SNSLobbySettingsResponse), under the key "nevr_server". The game
//     sends that document to every peer it has accepted, and from its stored copy (NetGame+0x63260) to every peer it
//     accepts later (CNSLobby::AcceptPlayersSuccessCBHost 0x140603e20 -> 0x1401aa750), so a client that joins mid-match
//     gets it too. OnLobbySettings (echovr.exe 0x1401c2a20) reads only parties_unlocked and allowed_levels, and a key it
//     does not know changes nothing it does (#2333, the disassembly read).
// A client reads the key in the game's own lobby-settings callback (CR15NetGame::LobbySettingsCB(SMsg), echovr.exe
// 0x14017de60, libr15.so 0x1367ae0), not from the stored copy: that copy outlives a change of server (only LogOut clears
// it), so the client clears what it read when it starts another session.
//
// Header-only and free of game and platform dependencies, so the PC runtime and the Quest build share it and a test
// drives it.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace nevr_server_features {

inline constexpr const char* kEchoCombat = "echocombat";                // the server runs EchoCombat
inline constexpr const char* kEchoCombatMedigun = "echocombat_medigun";  // ... and allows MEDIGUN
inline constexpr const char* kDocumentKey = "nevr_server";              // the lobby settings key the server writes
inline constexpr const char* kFeaturesKey = "features";                 // its array of feature names

// The game service takes at most 32 names of [a-z0-9_]+ (EchoTools/nakama session_ws.go featurePattern,
// evr_pipeline_gameserver.go sanitizeList(msg.Features, 32, featurePattern)); a client takes the same.
inline constexpr std::size_t kMaxFeatures = 32;
inline constexpr std::size_t kMaxNameBytes = 64;

inline bool ValidName(std::string_view name) {
  if (name.empty() || name.size() > kMaxNameBytes) return false;
  for (const char c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  return true;
}

// The combat module's features, from what it actually started (runtime/combat/combat.cpp): EchoCombat when VeilNet
// started, and MEDIGUN when combat.medigun is on and both of its prerequisites (OVERCHARGE's PowerBoost and VeilNet)
// started, the same condition the MEDIGUN feature line reports.
inline std::vector<std::string> CombatFeatures(bool veilStarted, bool medigunOn, bool overchargeStarted) {
  std::vector<std::string> features;
  if (!veilStarted) return features;
  features.emplace_back(kEchoCombat);
  if (medigunOn && overchargeStarted) features.emplace_back(kEchoCombatMedigun);
  return features;
}

// "a,b" for a log line; "none" for an empty list.
inline std::string Joined(const std::vector<std::string>& features) {
  if (features.empty()) return "none";
  std::string out;
  for (const std::string& f : features) {
    if (!out.empty()) out += ',';
    out += f;
  }
  return out;
}

// The document the server gives the game: `serviceJson` (the game service's document, "" when there is none) with
// "nevr_server" set to {"features": [...]}, every other key kept as the service wrote it. False, and `out` untouched,
// when `serviceJson` is not a JSON object or a name is not valid.
inline bool StateInDocument(const std::string& serviceJson, const std::vector<std::string>& features, std::string* out) {
  if (features.size() > kMaxFeatures) return false;
  for (const std::string& f : features) {
    if (!ValidName(f)) return false;
  }
  nlohmann::json document = nlohmann::json::object();
  if (!serviceJson.empty()) {
    document = nlohmann::json::parse(serviceJson, nullptr, /*allow_exceptions=*/false);
    if (document.is_discarded() || !document.is_object()) return false;
  }
  document[kDocumentKey] = nlohmann::json::object({{kFeaturesKey, features}});
  *out = document.dump();
  return true;
}

// What a client read of one lobby settings document.
struct Statement {
  enum class Kind {
    kNotStated,  // a JSON object without "nevr_server": a stock server's document, or the service's alone
    kStated,     // "nevr_server": {"features": [...]} read; `features` holds the valid names, in order
    kMalformed,  // not a JSON object, or "nevr_server" / "features" of the wrong type
  };
  Kind kind = Kind::kNotStated;
  std::vector<std::string> features;
  std::size_t dropped = 0;  // names refused (not [a-z0-9_]+, too long, past kMaxFeatures, not a string)

  bool Has(std::string_view name) const {
    for (const std::string& f : features) {
      if (f == name) return true;
    }
    return false;
  }
};

inline const char* KindName(Statement::Kind kind) {
  switch (kind) {
    case Statement::Kind::kNotStated: return "not_stated";
    case Statement::Kind::kStated: return "stated";
    case Statement::Kind::kMalformed: return "malformed";
  }
  return "unknown";
}

// Reads the document text the game received (the callback's buffer and size; no terminator needed). Never throws.
inline Statement ReadDocument(const char* text, std::size_t size) {
  Statement s;
  if (text == nullptr) {
    s.kind = Statement::Kind::kMalformed;
    return s;
  }
  const nlohmann::json document = nlohmann::json::parse(text, text + size, nullptr, /*allow_exceptions=*/false);
  if (document.is_discarded() || !document.is_object()) {
    s.kind = Statement::Kind::kMalformed;
    return s;
  }
  const auto server = document.find(kDocumentKey);
  if (server == document.end()) return s;
  if (!server->is_object()) {
    s.kind = Statement::Kind::kMalformed;
    return s;
  }
  const auto list = server->find(kFeaturesKey);
  if (list == server->end() || !list->is_array()) {
    s.kind = Statement::Kind::kMalformed;
    return s;
  }
  s.kind = Statement::Kind::kStated;
  for (const nlohmann::json& item : *list) {
    if (!item.is_string() || s.features.size() >= kMaxFeatures) {
      ++s.dropped;
      continue;
    }
    const std::string& name = item.get_ref<const std::string&>();
    if (!ValidName(name)) {
      ++s.dropped;
      continue;
    }
    s.features.push_back(name);
  }
  return s;
}

// What the gates on a client decide from the current server's statement.
struct Gate {
  bool echoCombat = false;  // UMBRAL VEIL, OVERCHARGE and POINT shown
  bool medigun = false;     // MEDIGUN shown and working
};

inline Gate GateOf(const Statement& s) {
  Gate g;
  if (s.kind != Statement::Kind::kStated) return g;
  g.echoCombat = s.Has(kEchoCombat);
  g.medigun = g.echoCombat && s.Has(kEchoCombatMedigun);
  return g;
}

// The current server's gate on a client: written by the game thread (the callback; the session change), read by any
// thread (the combat module's tick). One word, so a reader never sees half of an update.
class ClientState {
 public:
  using Lobby = std::array<std::uint8_t, 16>;  // the lobby session's GUID, as compat/lobby_frame.h decodes it

  static constexpr std::uint32_t kEchoCombatBit = 1U;
  static constexpr std::uint32_t kMedigunBit = 2U;

  // The gate this document gives. Returns true when it differs from the one in force.
  bool Apply(const Gate& gate) {
    const std::uint32_t next = (gate.echoCombat ? kEchoCombatBit : 0U) | (gate.medigun ? kMedigunBit : 0U);
    return bits_.exchange(next, std::memory_order_acq_rel) != next;
  }
  // Another session begins: nothing is known of its server until its document comes. Returns true when a gate was open.
  bool Clear() { return bits_.exchange(0U, std::memory_order_acq_rel) != 0U; }

  Gate Current() const {
    const std::uint32_t b = bits_.load(std::memory_order_acquire);
    return Gate{(b & kEchoCombatBit) != 0U, (b & kMedigunBit) != 0U};
  }

  // A lobby session success (PC ws_bridge, the Quest session router): a session in another lobby begins, so what the
  // last server stated no longer holds until this server's document comes. A second success for the lobby already in
  // changes nothing. Returns true when the lobby changed; `wasOpen` says whether a gate was open then.
  bool ClearForLobby(const Lobby& lobby, bool* wasOpen) {
    std::lock_guard<std::mutex> lock(lobbyMu_);
    if (lobby == lobby_) return false;
    lobby_ = lobby;
    const bool open = Clear();
    if (wasOpen != nullptr) *wasOpen = open;
    return true;
  }

 private:
  std::atomic<std::uint32_t> bits_{0U};
  std::mutex lobbyMu_;
  Lobby lobby_{};  // guarded by lobbyMu_
};

// The line both writers (PC ws_bridge, the Quest session router) give a ClearForLobby that changed the lobby.
inline std::string FormatCleared(const std::string& lobbyText, bool wasOpen) {
  return "[NEVR.WS] server features cleared lobby=" + lobbyText + " was_stated=" + (wasOpen ? "yes" : "no") +
         " (the EchoCombat tac mods stay hidden until this server's lobby settings state them)";
}

// The one state of this process.
inline ClientState& Client() {
  static ClientState state;
  return state;
}

// The game's lobby-settings message, as CR15NetGame::LobbySettingsCB(const SMsg<SNSLobbySettingsResponse>&) reads it on
// both binaries: the text at +0x18 and its size at +0x20, no terminator (echovr.exe 0x14017de7b MOV R8,[RBX+0x20];
// 0x14017de7f MOV RDX,[RBX+0x18]; libr15.so 0x1367afc ldp x1, x2, [x20, #0x18]).
inline constexpr std::size_t kMsgTextOffset = 0x18;
inline constexpr std::size_t kMsgSizeOffset = 0x20;
// The relay refuses a larger document (server/lobby_settings_relay.h kLobbySettingsMaxJsonBytes); a client reads no more.
inline constexpr std::uint64_t kMaxTextBytes = 1024ULL * 1024ULL;

struct Received {
  Statement statement;
  Gate gate;
  std::uint64_t size = 0;  // the size the message gave
  bool changed = false;    // the gate in force changed
};

// What one lobby-settings message the game receives says, applied to `state`. Each binary's hook calls this and logs
// the result; the game's own call is forwarded unchanged.
inline Received Receive(const void* msg, ClientState& state) {
  Received r;
  const char* text = nullptr;
  if (msg != nullptr) {
    std::memcpy(&text, static_cast<const std::uint8_t*>(msg) + kMsgTextOffset, sizeof(text));
    std::memcpy(&r.size, static_cast<const std::uint8_t*>(msg) + kMsgSizeOffset, sizeof(r.size));
  }
  if (text == nullptr || r.size > kMaxTextBytes) {
    r.statement.kind = Statement::Kind::kMalformed;
  } else {
    r.statement = ReadDocument(text, static_cast<std::size_t>(r.size));
  }
  r.gate = GateOf(r.statement);
  r.changed = state.Apply(r.gate);
  return r;
}

}  // namespace nevr_server_features

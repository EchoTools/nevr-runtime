#pragma once

// Relay of the game service's lobby settings document to the game and to every connected client (#905).
//
// What the message is. The game service sends one frame over the ServerDB socket whose msgId is
// CSymbol64("SNSLobbySettingsResponse") (EchoVR::Symbols::Tcp::LobbySettingsResponse). Its payload is a u64
// little-endian decompressed length followed by a zlib stream holding the settings JSON object. This is the shape
// the retail TCP handler CR15NetGame::OnTcpLobbySettingsResponse (echovr.exe 0x14017dee0) reads: Decompress
// (0x14152a910), CJson_LoadFromBuffer (0x1405f0bd0), then CR15NetGame::OnLobbySettings (0x1401c2a20), which stores
// the document at NetGame+0x63260, and a broadcast of it to all peers (0x1401a93e0).
//
// Why nEVR must do it. PatchEnableServer forces the dedicated server onto the game's local handler set
// (0x140157fb0), so the TCP handler is never registered, and the server reads ServerDB frames through its own
// WebSocketClient, never through the game's TCP broadcaster. Without this relay the frame reaches the "Unhandled
// WebSocket msgId" branch and the document never reaches the game or any client.
//
// What the relay does, in this order, on the game thread (ProcessReceivedMessages runs from Update()):
//   1. decode and validate the frame (nothing is injected when it is bad);
//   2. raise the local event Symbols::LobbySettingsResponse: an 8-byte zero item followed by the plain JSON, which
//      the game's local handler 0x14017de60 parses and stores at NetGame+0x63260;
//   3. send the same shape to every connected peer (Peer_AllPeers), as 0x1401a93e0 does.
// A client accepted after this point gets the stored document from the game itself: CNSLobby::
// AcceptPlayersSuccessCBHost (0x140603e20) raises NSLobbySendClientLobbySettings for each accepted peer and
// 0x1401aa750 answers it from NetGame+0x63260. So there is no re-broadcast on an entrant-count change.
//
// This unit has no game dependency: the two sinks are the game calls, supplied by the caller, so the tests drive
// the whole sequence without a game process.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nevr_game_server {

/// EchoVR::Peer_AllPeers (src/abi/echovr.h); gameserver_callbacks.cpp static_asserts the two are equal. The value
/// is also DAT_142025f28 in echovr.exe (0xFFFFFFFFFFFFFFFD), the peer the retail handler passes to 0x1401a93e0.
inline constexpr uint64_t kLobbySettingsAllPeers = 0xFFFFFFFFFFFFFFFDULL;

/// Upper bound on the decompressed document. The real document is a few KiB; a larger length is refused before
/// anything is allocated, so a corrupt length field cannot ask for gigabytes.
inline constexpr uint64_t kLobbySettingsMaxJsonBytes = 1024ULL * 1024ULL;

/// The item that precedes the JSON in both the local event and the broadcast (0x1401a93e0 passes a pointer to
/// eight zero bytes as the item and the JSON as the buffer; the local adapter 0x14013cd50 skips the first 8).
inline constexpr size_t kLobbySettingsItemBytes = 8;

enum class LobbySettingsDrop {
  kNone,
  kTooShort,        // fewer than 8 bytes: no length field
  kBadLength,       // zero, or above kLobbySettingsMaxJsonBytes
  kInflateFailed,   // zlib refused the stream
  kNotJson,         // inflated bytes are not JSON
  kNotObject,       // JSON, but not an object
  kNoBroadcaster,   // the game has no broadcaster to take the local event
  kException,       // a sink threw
};

/// Stable name used in the log line.
const char* LobbySettingsDropName(LobbySettingsDrop drop);

struct LobbySettingsDecoded {
  LobbySettingsDrop drop = LobbySettingsDrop::kTooShort;
  int zlibResult = 0;  // the zlib return code when drop == kInflateFailed, else 0
  std::string json;    // the inflated document, byte for byte, when drop == kNone
};

/// Decodes one frame payload. Never throws for malformed input. Trailing NUL bytes of the inflated document are
/// dropped (a sender may NUL-terminate its JSON); a NUL anywhere else makes it kNotJson, because the JSON parser
/// stops reading at a NUL and the game would otherwise receive bytes the parser never checked.
LobbySettingsDecoded DecodeLobbySettings(const void* data, uint64_t size);

/// The local-event message: kLobbySettingsItemBytes zero bytes, then `json` with no terminator (the game parses by
/// length: CJson_LoadFromBuffer takes a size).
std::vector<uint8_t> BuildLobbySettingsLocalEvent(const std::string& json);

struct LobbySettingsSinks {
  /// Raises Symbols::LobbySettingsResponse on the broadcaster. False when the game has no broadcaster.
  std::function<bool(void* message, uint64_t size)> raiseLocalEvent;
  /// CBroadcaster::Send of Symbols::LobbySettingsResponse to `peer`; returns the call's result code.
  std::function<int32_t(const void* item, uint64_t itemSize, const void* body, uint64_t bodySize, uint64_t peer)>
      sendToPeer;
};

enum class LobbySettingsLogLevel { kInfo, kWarning };

struct LobbySettingsReport {
  bool relayed = false;
  LobbySettingsDrop drop = LobbySettingsDrop::kNone;
  uint64_t wireBytes = 0;
  uint64_t jsonBytes = 0;
  int32_t sendResult = 0;
  LobbySettingsLogLevel level = LobbySettingsLogLevel::kInfo;
  std::string line;  // the finished log line, tag included; carries no document text
};

/// Runs the sequence above for one frame. `sessionId` is the active lobby session ("" when none).
LobbySettingsReport RelayLobbySettings(const void* data, uint64_t size, const std::string& sessionId,
                                       const LobbySettingsSinks& sinks);

/// What the context remembers about the last document relayed, so that a session which never receives one is
/// visible in the log at its end instead of silently inheriting the previous session's.
struct LobbySettingsRecord {
  bool present = false;
  uint64_t wireBytes = 0;
  uint64_t jsonBytes = 0;
  std::string sessionId;  // the session that was active when the document arrived; "" when none was
};

/// The Info line for the record being dropped at a session boundary. `reason` is "session_start" or "session_end".
std::string FormatLobbySettingsCleared(const LobbySettingsRecord& record, const char* reason,
                                       const std::string& sessionId);

}  // namespace nevr_game_server

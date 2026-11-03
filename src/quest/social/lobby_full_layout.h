#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "quest/social/lobby_full_backoff.h"
#include "runtime/patch/lobby_failure_message_rules.h"

// Where the lobby-full hooks read the game's objects. Header-only and free of the game, so a host test can hand it a fake
// lobby client and a fake message and check which bytes it reads (lobby_full_test.cpp).
//
// Addresses. libr15.so has two numberings. The patch table (quest/patch/libr15_patch_table.h) and ReVault use the same one:
// CR15NetClientLobby::Join 0x1340ba8, LobbySessionFailureCB 0x1340cc8. The pinned ELF vaddr is that minus 0x100000 (0x1240ba8,
// 0x1240cc8; libr15_patch_table_test.cpp pairs them). This file and the hooks name the table/ReVault address.
// Every offset below is read from the ReVault decompilation of LobbySessionFailureCB (libr15.so, binary id 576, 0x1340cc8),
// where `param_1` is the CR15NetClientLobby and `in_x1` is the message:
//   `if (2 < *(int *)(param_1 + 0x448) - 4U) return;`                         the status gate
//   `uVar7 = (ulong)*(char *)(*(long *)(in_x1 + 0x10) + 0x18);`               msg + 0x10 is the data, data + 0x18 the code
//   `pcVar5 = *(char **)(param_1 + 8); puVar4 = (ushort *)(lVar8 + 0x20);`    case 0xb: the CR15NetGame is *(param_1 + 8),
//   `CR15NetGame::SetErrorMessage(pcVar5, pcVar3, (char *)puVar4, pcVar6);`   and the message is data + 0x20
// `*(param_1 + 0x910)` is a different object (a table owner: `plVar10[0x1e]`, `*plVar10 + 0x28d0`), not the game. The error
// block is at the game's kErrorBlockOffset (login_prompt_hook.cpp, 0x63308). The PC callback reads the game the same way
// (`*(lobbyClient + 8)`, runtime/patch/lobby_full_backoff.cpp).
namespace quest_social::lobby_full {

// Join (0x1340ba8) hands its message pointer to `CNSLobby::RequestJoin` unchanged (`mov x1, x20`), so its code does not show
// where the lobby's key lies in the message. The key is taken as the 16 bytes at msg + 0x18; that this names the lobby is
// not measured yet (the device run in #1001 logs it).
constexpr std::size_t kJoinKeyOffset = 0x18;
constexpr std::size_t kFailureDataOffset = 0x10;  // *(msg + 0x10) is the failure data
constexpr std::size_t kFailureCodeOffset = 0x18;  // data + 0x18 is the signed failure code
constexpr std::size_t kLobbyStatusOffset = 0x448;  // the status word the callback's gate tests
constexpr std::size_t kLobbyGameOffset = 0x8;      // *(lobby client + 8) is the CR15NetGame that owns the error block

inline LobbyKey KeyOf(const void* msg) noexcept {
  LobbyKey key{};
  std::memcpy(key.data(), static_cast<const std::uint8_t*>(msg) + kJoinKeyOffset, key.size());
  return key;
}

// The signed failure code of a refusal message. False when the message or its data is null.
inline bool FailureCode(const void* msg, std::int8_t* code) noexcept {
  if (msg == nullptr) return false;
  const void* data = nullptr;
  std::memcpy(&data, static_cast<const std::uint8_t*>(msg) + kFailureDataOffset, sizeof data);
  if (data == nullptr) return false;
  *code = static_cast<std::int8_t>(static_cast<const std::uint8_t*>(data)[kFailureCodeOffset]);
  return true;
}

// The game's status word (+0x448), 0 when there is no object.
inline std::int32_t LobbyStatus(const void* self) noexcept {
  std::int32_t status = 0;
  if (self != nullptr) std::memcpy(&status, static_cast<const std::uint8_t*>(self) + kLobbyStatusOffset, sizeof status);
  return status;
}

// The game's own gate: status - 4 < 3 (unsigned). A refusal the game ignores is neither counted nor shown.
inline bool GateOpenFor(std::int32_t status) noexcept { return static_cast<std::uint32_t>(status - 4) < 3U; }

inline bool StatusGateOpen(const void* self) noexcept { return self != nullptr && GateOpenFor(LobbyStatus(self)); }

// What the hook copies out of the game's objects before the original callback runs: the payload belongs to the game and
// may be released when the callback returns (the PC twin, #1443).
struct RefusalReads {
  bool gateOpen = false;
  std::uint8_t* game = nullptr;  // *(self + kLobbyGameOffset)
  char message[nevr_lobby_failure_message::kMessageSize] = {};
  std::int32_t status = 0;  // the status word the gate was decided from, read once
  bool haveStatus = false;  // false with no lobby client: `status` is then 0 and is not a reading
};

inline RefusalReads ReadRefusal(const void* self, const void* msg) noexcept {
  RefusalReads r;
  if (self == nullptr || msg == nullptr) return r;
  r.status = LobbyStatus(self);
  r.haveStatus = true;
  r.gateOpen = GateOpenFor(r.status);
  std::memcpy(&r.game, static_cast<const std::uint8_t*>(self) + kLobbyGameOffset, sizeof r.game);
  const void* payload = nullptr;
  std::memcpy(&payload, static_cast<const std::uint8_t*>(msg) + kFailureDataOffset, sizeof payload);
  if (payload != nullptr)
    std::memcpy(r.message, static_cast<const std::uint8_t*>(payload) + nevr_lobby_failure_message::kMessageOffset,
                sizeof r.message);
  return r;
}

// The decision for a refusal whose game keeps its page: the reason the service's message is not shown, or kNone when it is
// shown (the PC callback's rule, lobby_failure_message_rules.h).
inline nevr_lobby_failure_message::Decline DeclineOf(const RefusalReads& reads) noexcept {
  return nevr_lobby_failure_message::DeclineFor(reads.game, reads.message, sizeof reads.message);
}

// The decline when the game keeps its own page. Passes it to `write` once per code and reason per process and returns
// true then; a repeat returns false and writes nothing (the caller writes the repeat as a Debug record).
inline bool NoteMessageNotShown(nevr_lobby_failure_message::DeclineLog& log, const RefusalReads& reads,
                                std::uint8_t code, nevr_lobby_failure_message::DeclineSink write) noexcept {
  return nevr_lobby_failure_message::NoteDeclined(log, code, DeclineOf(reads), write);
}

// The Quest's two refusal records that are said once per process: a refusal the game's gate drops (refusal_dropped), and a
// refusal no join went through, so the back-off does not count it (refusal_uncounted). One bit per record, one byte per code.
// Not thread-safe; the caller holds its lock.
class RefusalSayings {
 public:
  enum class Record : std::uint8_t { kDroppedGateClosed = 0x1, kUncountedNoJoin = 0x2 };

  // True the first time `record` is said for `code`, false after.
  bool FirstTime(std::uint8_t code, Record record) noexcept {
    std::uint8_t& seen = seen_[code];
    const auto bit = static_cast<std::uint8_t>(record);
    if ((seen & bit) != 0) return false;
    seen |= bit;
    return true;
  }

 private:
  std::array<std::uint8_t, 256> seen_{};
};

}  // namespace quest_social::lobby_full

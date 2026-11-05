#include "runtime/patch/queued_join_log.h"

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/compat/lobby_frame.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/patch/queued_join_log_rules.h"

namespace nevr_queued_join_log {
namespace {

using nevr_lobby_frame::Guid;

// CR15NetClientLobby::LobbySessionSuccessCB(lobby, msg): MOV R11,RSP; PUSH RBP; PUSH RDI; SUB RSP,0x88. It runs once per
// success the game receives (a join or a find ends in one), never per frame.
constexpr std::uint64_t kSuccessCbVA = 0x14017DB90;
constexpr std::array<std::uint8_t, 12> kSuccessCbPrologue = {0x4C, 0x8B, 0xDC, 0x55, 0x57, 0x48,
                                                             0x81, 0xEC, 0x88, 0x00, 0x00, 0x00};
using SuccessCbFn = void (*)(void* lobby, void* msg);

// The game's "Leave For Match" (echovr.exe 0x140179600, no direct caller; the deferred-call queue reaches it through
// 0x14013c680): PUSH RSI; SUB RSP,0x20; XOR EDX,EDX. It runs once per queued join the script schedules.
constexpr std::uint64_t kLeaveForMatchVA = 0x140179600;
constexpr std::array<std::uint8_t, 8> kLeaveForMatchPrologue = {0x40, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xD2};
using LeaveForMatchFn = void (*)(void* netGame);

// CR15NetClientLobby::SwitchTo(lobby, status): MOV [RSP+0x18],RBX; MOV [RSP+0x20],RSI; PUSH RBP,RDI,R12,R14,R15. It runs
// when the lobby status changes and the game writes a line for each change, so it is not a per-frame call. The hook
// does one int read and forwards.
constexpr std::uint64_t kSwitchToVA = 0x1401B7BB0;
constexpr std::array<std::uint8_t, 18> kSwitchToPrologue = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24,
                                                            0x20, 0x55, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57};
using SwitchToFn = void (*)(void* lobby, std::int32_t status);

// The game's invalid SUuid (echovr.exe 0x142100b20, 16 bytes): what "Leave For Match" writes over the queued id.
constexpr std::uint64_t kInvalidIdVA = 0x142100B20;

constexpr std::size_t kLobbyStatusOffset = 0x448;
constexpr std::size_t kLobbyQueueOffset = 0x6E0;      // the queued payload copy, aligned up to 8 from lobby+0x6e0
constexpr std::size_t kQueuedIdOffset = 8;            // the id inside the payload (the game formats payload+8)
constexpr std::size_t kNetGameLobbyClientOffset = 0x40;  // [netGame+0x40] is the lobby client
constexpr std::size_t kMsgPayloadOffset = 0x10;

std::uintptr_t g_gameBase = 0;
SuccessCbFn g_originalSuccessCb = nullptr;
LeaveForMatchFn g_originalLeaveForMatch = nullptr;
SwitchToFn g_originalSwitchTo = nullptr;
Guid g_invalidId{};  // read at Install; the leave and drop hooks are installed only when it was readable

std::int32_t ReadStatus(const void* lobby) {
  std::int32_t status = 0;
  if (lobby != nullptr) std::memcpy(&status, static_cast<const std::uint8_t*>(lobby) + kLobbyStatusOffset, sizeof(status));
  return status;
}

// The id in the lobby's queue buffer, or the nil id when the buffer holds the game's invalid id (shown as "none").
// queued is set when the buffer holds a real id.
Guid ReadQueuedId(const void* lobby, bool* queued) {
  Guid id{};
  *queued = false;
  if (lobby == nullptr) return id;
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(lobby) + kLobbyQueueOffset;
  const std::uintptr_t aligned = base + ((~base + 1U) & 7U);
  std::memcpy(id.data(), reinterpret_cast<const void*>(aligned + kQueuedIdOffset), id.size());
  if (id == g_invalidId) return Guid{};
  *queued = true;
  return id;
}

void SuccessCbHook(void* lobby, void* msg) {
  const std::int32_t before = ReadStatus(lobby);
  // The payload belongs to the game and may be released when the callback returns: copy the id first.
  Guid session{};
  if (msg != nullptr) {
    const std::uint8_t* payload = nullptr;
    std::memcpy(&payload, static_cast<const std::uint8_t*>(msg) + kMsgPayloadOffset, sizeof(payload));
    if (payload != nullptr) std::memcpy(session.data(), payload + kQueuedIdOffset, session.size());
  }
  g_originalSuccessCb(lobby, msg);
  const std::int32_t after = ReadStatus(lobby);
  Log(EchoVR::LogLevel::Info, "%s", FormatSuccessLine(before, after, session).c_str());
}

void LeaveForMatchHook(void* netGame) {
  void* lobby = nullptr;
  if (netGame != nullptr)
    std::memcpy(&lobby, static_cast<const std::uint8_t*>(netGame) + kNetGameLobbyClientOffset, sizeof(lobby));
  const std::int32_t statusBefore = ReadStatus(lobby);
  bool queuedBefore = false;
  const Guid session = ReadQueuedId(lobby, &queuedBefore);
  g_originalLeaveForMatch(netGame);
  bool queuedAfter = false;
  ReadQueuedId(lobby, &queuedAfter);
  const LeaveResult result = ClassifyLeave(queuedBefore, queuedAfter);
  Log(EchoVR::LogLevel::Info, "%s", FormatLeaveLine(result, session, statusBefore, ReadStatus(lobby)).c_str());
}

void SwitchToHook(void* lobby, std::int32_t status) {
  const std::int32_t from = ReadStatus(lobby);
  const bool drop = IsDrop(from, status);
  bool queued = false;
  const Guid session = drop ? ReadQueuedId(lobby, &queued) : Guid{};
  g_originalSwitchTo(lobby, status);
  if (drop) Log(EchoVR::LogLevel::Info, "%s", FormatDropLine(session, status).c_str());
}

std::string HexBytes(const void* address, std::size_t count) {
  if (address == nullptr) return "unreadable";
  std::string out;
  char byte[4];
  for (std::size_t i = 0; i < count; ++i) {
    std::snprintf(byte, sizeof(byte), "%02x", static_cast<const std::uint8_t*>(address)[i]);
    out += byte;
  }
  return out;
}

template <typename Fn, std::size_t Size>
void HookChecked(std::uint64_t va, const std::array<std::uint8_t, Size>& prologue, Fn& original, PVOID detour,
                 const char* name) {
  void* target = nevr::ResolveVA_Checked(g_gameBase, va);
  if (!nevr::ValidatePrologue(target, prologue.data(), prologue.size())) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MATCH] hook skipped name=%s va=0x%llX reason=prologue_mismatch expected=%s actual=%s (this record is not "
        "written)",
        name, static_cast<unsigned long long>(va), HexBytes(prologue.data(), prologue.size()).c_str(),
        HexBytes(target, prologue.size()).c_str());
    return;
  }
  original = reinterpret_cast<Fn>(target);
  if (PatchDetour(&original, detour, name))
    Log(EchoVR::LogLevel::Info, "[NEVR.MATCH] hooked name=%s va=0x%llX (the record it writes is on)", name,
        static_cast<unsigned long long>(va));
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_gameBase = gameBase;
  HookChecked(kSuccessCbVA, kSuccessCbPrologue, g_originalSuccessCb, reinterpret_cast<PVOID>(&SuccessCbHook),
              "LobbySessionSuccessCB");
  const void* invalid = nevr::ResolveVA_Checked(g_gameBase, kInvalidIdVA);
  if (invalid == nullptr) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.MATCH] queued join hooks skipped reason=invalid_id_unreadable va=0x%llX (the acted-on and dropped records "
        "are not written)",
        static_cast<unsigned long long>(kInvalidIdVA));
    return;
  }
  std::memcpy(g_invalidId.data(), invalid, g_invalidId.size());
  HookChecked(kLeaveForMatchVA, kLeaveForMatchPrologue, g_originalLeaveForMatch,
              reinterpret_cast<PVOID>(&LeaveForMatchHook), "LeaveForMatch");
  HookChecked(kSwitchToVA, kSwitchToPrologue, g_originalSwitchTo, reinterpret_cast<PVOID>(&SwitchToHook),
              "NetClientLobbySwitchTo");
}

}  // namespace nevr_queued_join_log

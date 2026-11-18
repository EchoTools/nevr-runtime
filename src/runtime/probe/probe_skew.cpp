// P-SKEW. Server: every session it hosts is pinned to rad_hash("probe@2") through the owner-JSON setter. Client: the owner
// JSON's "level" and pin are polled on the game thread and logged as they change, beside the "loading level" transition and the
// session level's first Enable; with skew_expect other than none, a pin that differs from the expected one leaves through the
// return-to-lobby path with the lobby-failure notice. The calls are the channel's own (addresses.h CNSLOBBY_OWNER_SYMBOL_*), each
// entry checked before it is called.

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "runtime/content/map_session_pin_notice.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/prologue_head.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"  // g_pGame
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/lifecycle/return_to_lobby.h"
#include "runtime/lifecycle/return_to_lobby_queue.h"
#include "runtime/patch/lobby_failure_message_rules.h"
#include "runtime/probe/probe_game.h"

namespace nevr_map_probe {

namespace {

namespace addr = nevr_patch_addresses;

using OwnerSetSymbolFn = void(__fastcall*)(std::uint64_t lobby, const char* key, std::uint64_t value);
using OwnerGetSymbolFn = std::uint64_t*(__fastcall*)(std::uint64_t lobby, std::uint64_t* out, const char* key,
                                                      std::uint64_t fallback);

constexpr const char* kPinKey = "nevr_map_pin";
constexpr const char* kLevelKey = "level";
constexpr const char* kNoticeTitle = "Map version mismatch";
constexpr const char* kLeaveCaller = "map_probe";
constexpr std::uint64_t kOwnerSymbolAbsent = 0;

// Game thread only.
bool g_callable = false;
OwnerSetSymbolFn g_set = nullptr;
OwnerGetSymbolFn g_get = nullptr;
std::uint64_t g_serverPin = 0;
std::optional<SkewClient> g_client;

bool EntryStock(const std::uint8_t* base, std::uintptr_t rva, const unsigned char* head, std::size_t n) {
  const std::uint8_t* entry = base + rva;
  return nevr_crash_recovery::IsReadableMemory(entry, n) && nevr_prologue_head::EntryOk(entry, head, n);
}

std::uint64_t Lobby() {
  const void* net_game = NetGame();
  if (net_game == nullptr) return 0;
  const void* client_lobby = nevr_return_to_lobby_queue::ReadPointer(net_game, addr::NETGAME_CLIENT_LOBBY_OFFSET);
  return client_lobby == nullptr ? 0 : reinterpret_cast<std::uint64_t>(client_lobby) + addr::CLIENT_LOBBY_LOBBY_OFFSET;
}

std::uint64_t OwnerSymbol(std::uint64_t lobby, const char* key) {
  std::uint64_t value = kOwnerSymbolAbsent;
  g_get(lobby, &value, key, kOwnerSymbolAbsent);
  return value;
}

void ServerTick(std::uint64_t frame) {
  const std::uint64_t lobby = Lobby();
  if (lobby == 0 || OwnerSymbol(lobby, kPinKey) == g_serverPin) return;
  g_set(lobby, kPinKey, g_serverPin);
  Log(EchoVR::LogLevel::Info, "[NEVR.PROBE] probe_skew server_set level=%s pin=%s frame=%llu",
      HexLevel(OwnerSymbol(lobby, kLevelKey)).c_str(), HexLevel(g_serverPin).c_str(), static_cast<unsigned long long>(frame));
}

}  // namespace

void SkewStart(const ProbeConfig& config, const char* hook_refusal) {
  auto* base = reinterpret_cast<std::uint8_t*>(EchoVR::g_GameBaseAddress);
  g_callable = base != nullptr &&
               EntryStock(base, addr::CNSLOBBY_OWNER_SYMBOL_SET, addr::CNSLOBBY_OWNER_SYMBOL_SET_PROLOGUE,
                          sizeof(addr::CNSLOBBY_OWNER_SYMBOL_SET_PROLOGUE)) &&
               EntryStock(base, addr::CNSLOBBY_OWNER_SYMBOL_GET, addr::CNSLOBBY_OWNER_SYMBOL_GET_PROLOGUE,
                          sizeof(addr::CNSLOBBY_OWNER_SYMBOL_GET_PROLOGUE));
  if (!g_callable) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PROBE] probe_skew off reason=owner_json_not_stock (an owner-JSON function's entry differs from the supported "
        "build's: P-SKEW does nothing)");
    return;
  }
  g_set = reinterpret_cast<OwnerSetSymbolFn>(base + addr::CNSLOBBY_OWNER_SYMBOL_SET);
  g_get = reinterpret_cast<OwnerGetSymbolFn>(base + addr::CNSLOBBY_OWNER_SYMBOL_GET);
  if (g_isServer) {
    g_serverPin = ProbePin(kServerProbeVersion);
    Log(EchoVR::LogLevel::Info, "[NEVR.PROBE] probe_skew server version=%u pin=%s", kServerProbeVersion,
        HexLevel(g_serverPin).c_str());
    return;
  }
  g_client.emplace(config);
  Log(EchoVR::LogLevel::Info, "[NEVR.PROBE] probe_skew client expect=%s version=%u hook=%s",
      SkewExpectName(config.skew_expect), config.skew_version, hook_refusal == nullptr ? "installed" : hook_refusal);
}

void SkewTick(std::uint64_t frame, const std::vector<LevelEvent>& events) {
  if (!g_callable) return;
  if (g_isServer) {
    ServerTick(frame);
    return;
  }
  std::vector<std::string> lines;
  for (const LevelEvent& event : events) {
    if (event.enable && event.ok && !event.noop) g_client->OnEnable(event.level, event.frame, &lines);
  }
  SkewInput in;
  in.lobby = Lobby();
  in.state = NetState();
  if (in.lobby != 0) {
    in.level = OwnerSymbol(in.lobby, kLevelKey);
    in.pin = OwnerSymbol(in.lobby, kPinKey);
  }
  const SkewAction action = g_client->Tick(in, frame, &lines);
  if (action.leave) {
    if (!action.notice.empty()) {
      // The block lives on the net game, not in the image (map_session_pin_notice.h).
      nevr_lobby_failure_message::WriteErrorBlock(nevr_map_session_pin_notice::NoticeBaseFor(g_pGame), kNoticeTitle,
                                                  action.notice.c_str(), action.notice.size() + 1);
    }
    const bool scheduled = nevr_return_to_lobby::ScheduleNow(NetGame(), kLeaveCaller);
    g_client->OnLeaveResult(scheduled, frame, &lines);
  }
  LogLines(lines);
}

}  // namespace nevr_map_probe

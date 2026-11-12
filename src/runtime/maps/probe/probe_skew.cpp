// PB3, P-SKEW. Server: every session it hosts is pinned to rad_hash("probe@2") through the owner-JSON setter. Client:
// the owner JSON's "level" and pin are polled on the game thread and logged as they change, beside the "loading level"
// transition and the session level's first Enable; with skew_expect other than none, a pin that differs from the
// expected one leaves through the return-to-lobby path with the lobby-failure notice.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/lifecycle/return_to_lobby.h"
#include "runtime/lifecycle/return_to_lobby_queue.h"
#include "runtime/maps/probe/probe_game.h"
#include "runtime/patch/lobby_failure_message_rules.h"

namespace nevr_maps_probe {

namespace {

// CNSLobby's owner-JSON symbol setter and getter (combat/veilnet.cpp:671, content/map_session_pin_game.cpp). The
// heads are read from the IDA export of echovr.exe 631547.
constexpr std::uintptr_t kOwnerSetSymbolRva = 0x615E60;
constexpr std::uintptr_t kOwnerGetSymbolRva = 0x60D3D0;
const std::uint8_t kOwnerSetSymbolHead[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89,
                                            0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xe9};
const std::uint8_t kOwnerGetSymbolHead[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x81, 0xc1, 0x10, 0x03, 0x00, 0x00};
using OwnerSetSymbolFn = void(__fastcall*)(std::uint64_t lobby, const char* key, std::uint64_t value);
using OwnerGetSymbolFn = std::uint64_t*(__fastcall*)(std::uint64_t lobby, std::uint64_t* out, const char* key,
                                                      std::uint64_t fallback);

// CR15NetClientLobby *(netgame+40h); its CNSLobby at +18h (map_session_pin_game.cpp).
constexpr std::uintptr_t kNetGameClientLobbyOffset = 0x40;
constexpr std::uintptr_t kClientLobbyLobbyOffset = 0x18;

constexpr const char* kPinKey = "nevr_map_pin";
constexpr const char* kLevelKey = "level";
constexpr const char* kNoticeTitle = "Map version mismatch";

// Game thread only.
bool g_callable = false;
OwnerSetSymbolFn g_set = nullptr;
OwnerGetSymbolFn g_get = nullptr;
std::uint64_t g_serverPin = 0;
std::optional<SkewClient> g_client;

// The stock head, or another hook's jmp there whose tail is still stock (map_session_pin_game.cpp EntryOk).
bool EntryOk(const std::uint8_t* entry, const std::uint8_t* head, std::size_t n) {
  if (!nevr_crash_recovery::IsReadableMemory(entry, n)) return false;
  if (std::memcmp(entry, head, n) == 0) return true;
  return entry[0] == 0xe9 && n > 8 && std::memcmp(entry + 5, head + 5, n - 5) == 0;
}

std::uint64_t Lobby() {
  const void* net_game = NetGame();
  if (net_game == nullptr) return 0;
  const void* client_lobby = nevr_return_to_lobby_queue::ReadPointer(net_game, kNetGameClientLobbyOffset);
  return client_lobby == nullptr ? 0 : reinterpret_cast<std::uint64_t>(client_lobby) + kClientLobbyLobbyOffset;
}

std::uint64_t OwnerSymbol(std::uint64_t lobby, const char* key) {
  std::uint64_t value = 0;
  g_get(lobby, &value, key, 0);
  return value;
}

void ServerTick(std::uint64_t frame) {
  const std::uint64_t lobby = Lobby();
  if (lobby == 0 || OwnerSymbol(lobby, kPinKey) == g_serverPin) return;
  g_set(lobby, kPinKey, g_serverPin);
  Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] probe_skew server_set level=%s pin=%s frame=%llu",
      HexLevel(OwnerSymbol(lobby, kLevelKey)).c_str(), HexLevel(g_serverPin).c_str(),
      static_cast<unsigned long long>(frame));
}

}  // namespace

void SkewStart(const ProbeConfig& config, const char* hook_refusal) {
  auto* base = reinterpret_cast<std::uint8_t*>(EchoVR::g_GameBaseAddress);
  g_callable = base != nullptr && EntryOk(base + kOwnerSetSymbolRva, kOwnerSetSymbolHead, sizeof(kOwnerSetSymbolHead)) &&
               EntryOk(base + kOwnerGetSymbolRva, kOwnerGetSymbolHead, sizeof(kOwnerGetSymbolHead));
  if (!g_callable) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.MAPS] probe_skew off reason=owner_json_not_stock");
    return;
  }
  g_set = reinterpret_cast<OwnerSetSymbolFn>(base + kOwnerSetSymbolRva);
  g_get = reinterpret_cast<OwnerGetSymbolFn>(base + kOwnerGetSymbolRva);
  if (g_isServer) {
    g_serverPin = ProbePin(kServerProbeVersion);
    Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] probe_skew server version=%u pin=%s", kServerProbeVersion,
        HexLevel(g_serverPin).c_str());
    return;
  }
  g_client.emplace(config);
  Log(EchoVR::LogLevel::Info, "[NEVR.MAPS] probe_skew client expect=%s version=%u hook=%s",
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
      nevr_lobby_failure_message::WriteErrorBlock(reinterpret_cast<std::uint8_t*>(EchoVR::g_GameBaseAddress),
                                                  kNoticeTitle, action.notice.c_str(), action.notice.size() + 1);
    }
    const bool scheduled = nevr_return_to_lobby::ScheduleNow(NetGame(), "map_probe");
    g_client->OnLeaveResult(scheduled, frame, &lines);
  }
  LogLines(lines);
}

}  // namespace nevr_maps_probe

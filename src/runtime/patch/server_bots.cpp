#include "runtime/patch/server_bots_install.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "runtime/compat/lobby_frame.h"
#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/return_to_lobby_queue.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/patch/server_bots.h"

namespace nevr_server_bots {

static_assert(nevr_patch_addresses::REQUEST_ADD_BOT == kRequestAddBotRva, "RequestAddBot RVA");
static_assert(nevr_patch_addresses::FIND_POOL == kFindPoolRva, "FindPool RVA");

namespace {

using RequestAddBotFn = int(__fastcall*)(void* netGame, const uint8_t* request);
using FindPoolFn = int(__fastcall*)(void* pools, uint64_t body, void** owner, uint64_t* index);

std::atomic<uint32_t> g_armed{0};
Schedule g_schedule;  // the game-loop thread only
bool g_firstFrameLogged = false;
std::vector<uint8_t> g_rewrittenFrame;  // the last rewritten session start; ServerDB's handler runs one at a time
std::mutex g_sessionMutex;              // the session id is written by ServerDB's handler, read on the game loop
std::string g_session = "none";        // the last session start's match id

std::string CurrentSession() {
  std::lock_guard<std::mutex> lock(g_sessionMutex);
  return g_session;
}

template <typename T>
T ReadAt(const void* base, size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const uint8_t*>(base) + offset, sizeof(value));
  return value;
}

std::string Hex(const uint8_t* bytes, size_t n) {
  std::string s;
  char two[3];
  for (size_t i = 0; i < n; ++i) {
    std::snprintf(two, sizeof(two), "%02x", bytes[i]);
    s += two;
  }
  return s;
}

}  // namespace

uint32_t ArmedBots() { return g_armed.load(std::memory_order_acquire); }

void Install(bool isServer) {
  if (!isServer) {
    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] server bots not applied: this is a client (the server adds bots)");
    return;
  }
  const char* text = NevrCfgGetFlat("nevr_bots_per_session");
  std::string problem;
  const uint32_t wanted = ParseBotsPerSession(text, &problem);
  if (!problem.empty()) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] network.bots_per_session=%s is %s (allowed 0..%u); no bots",
        text, problem.c_str(), static_cast<unsigned>(kMaxBots));
  }
  if (wanted == 0) {
    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] server bots off bots_per_session=0 source=%s",
        text != nullptr && text[0] != '\0' ? "config.yaml" : "default");
    return;
  }
  const uint8_t* base = reinterpret_cast<const uint8_t*>(EchoVR::g_GameBaseAddress);
  const size_t bad = FirstMismatch(base);
  if (bad != kWitnessCount) {
    const Witness& w = kWitnesses[bad];
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] server bots off bots_per_session=%u: the game's %s at 0x%llx is not the build this was "
        "read from expected=%s actual=%s",
        static_cast<unsigned>(wanted), w.name, static_cast<unsigned long long>(0x140000000ULL + w.rva),
        Hex(w.bytes, w.size).c_str(), Hex(base + w.rva, w.size).c_str());
    return;
  }
  g_armed.store(wanted, std::memory_order_release);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.GAMESERVER] server bots armed bots_per_session=%u sites=%zu (each in-game session gets up to %u AI "
      "bots, one a second)",
      static_cast<unsigned>(wanted), kWitnessCount, static_cast<unsigned>(wanted));
}

void OnFrame() {
  const uint32_t wanted = ArmedBots();
  if (wanted == 0) return;
  if (!g_firstFrameLogged) {
    g_firstFrameLogged = true;
    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] server bots driver running thread=%lu",
        static_cast<unsigned long>(GetCurrentThreadId()));
  }
  void* netGame = nevr_return_to_lobby_queue::NetGameFromContext(g_pGame);
  if (netGame == nullptr) return;
  const bool inGame = ReadAt<int32_t>(netGame, 0) == kNetGameInGame;
  const uint16_t entrants = ReadAt<uint16_t>(netGame, kNetGameEntrantsOffset);
  const bool pending = ReadAt<int32_t>(netGame, kNetGamePendingOffset) != 0;
  const uint64_t now = GetTickCount64();
  if (Decide(g_schedule, wanted, inGame, entrants, pending, now) != Gate::kAsk) return;

  uint8_t* const base = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress);
  void* pools = ReadAt<void*>(netGame, kNetGamePoolsOffset);
  BodyPool body[2];
  if (pools != nullptr) {
    const auto findPool = reinterpret_cast<FindPoolFn>(base + nevr_patch_addresses::FIND_POOL);
    for (size_t i = 0; i < 2; ++i) {
      void* owner = nullptr;
      uint64_t index = 0;
      body[i].found = findPool(pools, kBotBodies[i], &owner, &index) != 0 && owner != nullptr;
      if (!body[i].found) continue;
      const uint8_t* entries = ReadAt<const uint8_t*>(owner, kPoolOwnerEntriesOffset);
      body[i].found = entries != nullptr;
      if (body[i].found) body[i].free = ReadAt<int64_t>(entries, (index & 0xFFFF) * kPoolEntrySize + kPoolEntryFreeOffset);
    }
  }
  if (!BodyFree(body, 2)) {
    if (NoBodyWarningDue(g_schedule, now)) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.GAMESERVER] server bots: no free bot body in this level session=%s added=%u wanted=%u entrants=%u "
          "bot_arena_pool=%s bot_combat_pool=%s (no more bots this session until one frees)",
          CurrentSession().c_str(), static_cast<unsigned>(g_schedule.added), static_cast<unsigned>(wanted),
          static_cast<unsigned>(entrants), body[0].found ? std::to_string(body[0].free).c_str() : "none",
          body[1].found ? std::to_string(body[1].free).c_str() : "none");
    }
    return;
  }

  uint8_t noPosition[kNoPositionSize];
  std::memcpy(noPosition, base + nevr_patch_addresses::NO_SPAWN_POSITION, kNoPositionSize);
  const Request request = BuildRequest(noPosition);
  // AddBot adds the entrant itself only with the lobby's offline bit set; it is set for this call and put back.
  uint8_t* lobby = ReadAt<uint8_t*>(netGame, kNetGameLobbyOffset);
  if (lobby == nullptr) return;
  uint32_t* hostingFlags = reinterpret_cast<uint32_t*>(lobby + kLobbyFromHolderOffset + kLobbyHostingFlagsOffset);
  const uint32_t saved = *hostingFlags;
  *hostingFlags = saved | kHostingFlagOffline;
  const auto requestAddBot = reinterpret_cast<RequestAddBotFn>(base + nevr_patch_addresses::REQUEST_ADD_BOT);
  const bool accepted = requestAddBot(netGame, request.bytes) != 0;
  *hostingFlags = saved;
  Requested(g_schedule, accepted);
  Log(accepted ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
      "[NEVR.GAMESERVER] server bot %s session=%s n=%u of %u entrants=%u",
      accepted ? "requested" : "request refused by the game", CurrentSession().c_str(),
      static_cast<unsigned>(g_schedule.added),
      static_cast<unsigned>(wanted), static_cast<unsigned>(entrants));
}

void SessionSettings(void** frame, uint64_t* size) {
  const uint32_t wanted = ArmedBots();
  if (wanted == 0) return;
  const uint8_t* bytes = static_cast<const uint8_t*>(*frame);
  const std::string session = *size >= nevr_lobby_frame::kGuidSize
                                  ? nevr_lobby_frame::FormatGuid(nevr_lobby_frame::detail::ReadGuid(bytes))
                                  : std::string("none");
  {
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    g_session = session;
  }
  SettingsRewrite r = RewriteSessionSettings(bytes, static_cast<size_t>(*size), wanted, kPlayersPerTeam);
  if (r.result == SettingsResult::kRewritten) {
    g_rewrittenFrame = std::move(r.frame);
    *frame = g_rewrittenFrame.data();
    *size = g_rewrittenFrame.size();
  }
  Log(IsSettingsFailure(r.result) ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
      "[NEVR.GAMESERVER] server bots session settings %s session=%s numteams=%s players_per_team=%s "
      "settings_bytes=%zu rewritten_bytes=%zu frame_bytes=%llu%s",
      SettingsResultName(r.result), session.c_str(), r.addedNumTeams ? std::to_string(kNumTeams).c_str() : "kept",
      r.addedPlayersPerTeam ? std::to_string(kPlayersPerTeam).c_str() : "kept", r.settingsBytes, r.rewrittenBytes,
      static_cast<unsigned long long>(*size),
      IsSettingsFailure(r.result) ? " (forwarded as it came; bot team placement may fail)" : "");
}

}  // namespace nevr_server_bots

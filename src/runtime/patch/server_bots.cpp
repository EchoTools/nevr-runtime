#include "runtime/patch/server_bots_install.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <cstdint>
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

void LogLine(const Line& line) {
  Log(line.warning ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info, "%s", line.text.c_str());
}

}  // namespace

uint32_t ArmedBots() { return g_armed.load(std::memory_order_acquire); }

void Install(bool isServer) {
  const char* text = isServer ? NevrCfgGetFlat("nevr_bots_per_session") : nullptr;
  const InstallResult r = InstallLines(isServer, text, reinterpret_cast<const uint8_t*>(EchoVR::g_GameBaseAddress));
  g_armed.store(r.armed, std::memory_order_release);
  for (const Line& line : r.lines) LogLine(line);
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
  const NetGameView view = ReadNetGame(netGame);
  const uint16_t entrants = view.entrants;
  const uint64_t now = GetTickCount64();
  const Gate gate = Decide(g_schedule, wanted, view.inGame, entrants, view.pending, now);
  if (PendingWarningDue(g_schedule, gate == Gate::kPending, now)) {
    LogLine(PendingLine(CurrentSession(), g_schedule.added, wanted));
  }
  if (gate != Gate::kAsk) return;

  uint8_t* const base = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress);
  void* pools = ReadAt<void*>(netGame, kNetGamePoolsOffset);
  BodyPool body[2];
  ReadBodyPools(reinterpret_cast<FindPoolFn>(base + nevr_patch_addresses::FIND_POOL), pools, body);
  if (!BodyFree(body, 2)) {
    if (NoBodyWarningDue(g_schedule, now)) {
      LogLine(NoBodyLine(CurrentSession(), g_schedule.added, wanted, entrants, body));
    }
    return;
  }

  uint8_t noPosition[kNoPositionSize];
  std::memcpy(noPosition, base + nevr_patch_addresses::NO_SPAWN_POSITION, kNoPositionSize);
  const Request request = BuildRequest(noPosition);
  // AddBot adds the entrant itself only with the lobby's offline bit set; it is set for this call and put back.
  uint8_t* lobby = ReadAt<uint8_t*>(netGame, kNetGameLobbyOffset);
  if (lobby == nullptr) return;
  const auto requestAddBot = reinterpret_cast<RequestAddBotFn>(base + nevr_patch_addresses::REQUEST_ADD_BOT);
  const bool accepted =
      WithOfflineBit(HostingFlags(lobby), [&] { return requestAddBot(netGame, request.bytes) != 0; });
  Requested(g_schedule, accepted);
  LogLine(RequestLine(accepted, CurrentSession(), g_schedule.added, wanted, entrants));
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
  UseRewrite(r, g_rewrittenFrame, frame, size);
  LogLine(SettingsLine(r, session, *size));
}

}  // namespace nevr_server_bots

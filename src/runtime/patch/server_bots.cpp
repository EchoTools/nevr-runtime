#include "runtime/patch/server_bots_install.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <cstdint>
#include <string>
#include <vector>

#include "abi/echovr_functions.h"
#include "core/logging.h"
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
DriverState g_driver;                   // the game-loop thread only
std::vector<uint8_t> g_rewrittenFrame;  // the last rewritten session start; ServerDB's handler runs one at a time
std::mutex g_sessionMutex;              // ServerDB's handler writes the session, the driver reads it: both run from
                                        // GameServerLib::Update today; the lock keeps that safe if either moves
SessionView g_session;                  // the last session start's id, and whether it gets bots

SessionView CurrentSession() {
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

void OnServerUpdate() {
  const uint32_t wanted = ArmedBots();
  if (wanted == 0) return;
  uint8_t* const base = reinterpret_cast<uint8_t*>(EchoVR::g_GameBaseAddress);
  const auto findPool = reinterpret_cast<FindPoolFn>(base + nevr_patch_addresses::FIND_POOL);
  const auto requestAddBot = reinterpret_cast<RequestAddBotFn>(base + nevr_patch_addresses::REQUEST_ADD_BOT);
  const std::vector<Line> lines =
      Drive(g_driver, wanted, nevr_return_to_lobby_queue::NetGameFromContext(g_pGame),
            base + nevr_patch_addresses::NO_SPAWN_POSITION, GetTickCount64(), CurrentSession(),
            static_cast<unsigned long>(GetCurrentThreadId()), findPool, requestAddBot);
  for (const Line& line : lines) LogLine(line);
}

void SessionSettings(void** frame, uint64_t* size) {
  const uint32_t wanted = ArmedBots();
  if (wanted == 0) return;
  const uint8_t* bytes = static_cast<const uint8_t*>(*frame);
  SettingsRewrite r = RewriteSessionSettings(bytes, static_cast<size_t>(*size), wanted, kPlayersPerTeam);
  const SessionView session{SessionIdOf(bytes, *size), BotsThisSession(r.result)};
  {
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    g_session = session;
  }
  UseRewrite(r, g_rewrittenFrame, frame, size);
  LogLine(SettingsLine(r, session.id, *size));
}

}  // namespace nevr_server_bots

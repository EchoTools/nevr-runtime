#include "runtime/lifecycle/state_machine.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/crash_recovery.h"
#include "runtime/lifecycle/server_bringup.h"
#include "runtime/ext/module_loader.h"
#include "runtime/ext/plugin_loader.h"
#include "core/globals.h"
#include "core/login_session.h"
#include "core/logging.h"
#include "abi/echovr_functions.h"

/// Tracks whether the server has entered a game session (InGame state).
/// Used to detect session completion when the state returns to Lobby.
static BOOL g_serverWasInGame = FALSE;

static_assert(nevr_server_bringup::kLoggingIn == static_cast<uint32_t>(EchoVR::NetGameState::LoggingIn),
              "server_bringup.h state values");
static_assert(nevr_server_bringup::kLoggedIn == static_cast<uint32_t>(EchoVR::NetGameState::LoggedIn),
              "server_bringup.h state values");

namespace {

// Prologues (echovr.exe 631547) of the two game functions the server bring-up calls (#45).
// 0x140104e70, the call queue: push rbp; sub rsp,0x100.
constexpr unsigned char kQueueGameThreadCallPrologue[9] = {0x40, 0x55, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00};
// 0x1400fddd0, begin multiplayer: sub rsp,0x78; xor eax,eax; mov dword [rsp+0x20],...
constexpr unsigned char kBeginMultiplayerLobbyPrologue[8] = {0x48, 0x83, 0xEC, 0x78, 0x33, 0xC0, 0xC7, 0x44};

// The net game's first field is its state, its second the game object.
PVOID GameOf(PVOID netGame) { return *reinterpret_cast<PVOID*>(static_cast<CHAR*>(netGame) + 8); }

// From the game's call queue, on its thread, once the switch to LoggedIn is done.
VOID BeginMultiplayerAfterLogin(PVOID netGame) {
  const int32_t state = *static_cast<const int32_t*>(netGame);
  if (state != static_cast<int32_t>(EchoVR::NetGameState::LoggedIn)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] server bring-up: not beginning multiplayer, the net game is in state %d, not logged in", state);
    return;
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] server logged in — beginning multiplayer (no menu runs on a server to begin it)");
  EchoVR::BeginMultiplayerLobby(GameOf(netGame));
}

// The switch hook runs before the game applies the switch, so the begin goes through the game's
// call queue, as NetGameScheduleReturnToLobby's callback does, and runs after it.
void QueueBeginMultiplayer(PVOID netGame) {
  if (std::memcmp(reinterpret_cast<const void*>(EchoVR::QueueGameThreadCall), kQueueGameThreadCallPrologue,
                  sizeof(kQueueGameThreadCallPrologue)) != 0 ||
      std::memcmp(reinterpret_cast<const void*>(EchoVR::BeginMultiplayerLobby), kBeginMultiplayerLobbyPrologue,
                  sizeof(kBeginMultiplayerLobbyPrologue)) != 0) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] server bring-up skipped reason=prologue_mismatch — the server stays logged in and never "
        "begins multiplayer");
    return;
  }
  EchoVR::GameThreadCall call = {netGame, &BeginMultiplayerAfterLogin};
  EchoVR::QueueGameThreadCall(static_cast<CHAR*>(GameOf(netGame)) + 8, &call);
}

}  // namespace

/// <summary>
/// A detour hook for the game's method it uses to transition from one net game state to another.
/// </summary>
/// <param name="game">A pointer to the game instance.</param>
/// <param name="state">The state to transition to.</param>
/// <returns>None</returns>
VOID NetGameSwitchStateHook(PVOID pGame, EchoVR::NetGameState state) {
  static uint32_t s_prevState = 0;
  const uint32_t fromState = s_prevState;

  // Notify plugins of state change
  {
    NvrGameContext ctx = {};
    ctx.base_addr = (uintptr_t)EchoVR::g_GameBaseAddress;
    ctx.net_game = pGame;
    ctx.game_state = static_cast<uint32_t>(state);
    ctx.flags = NEVR_HOST_HAS_NETGAME;
    if (g_isServer) ctx.flags |= NEVR_HOST_IS_SERVER;
    else ctx.flags |= NEVR_HOST_IS_CLIENT;
    ctx.ctx_size = sizeof(NvrGameContext);
    ctx.get_plugin_count = GetLoadedPluginCount;
    ctx.get_plugin_info = GetLoadedPluginInfo;
    NotifyPluginsStateChange(&ctx, fromState, static_cast<uint32_t>(state));
    {
      NvrModuleContext mctx = {};
      mctx.base_addr = (uintptr_t)EchoVR::g_GameBaseAddress;
      mctx.flags = NEVR_HOST_HAS_NETGAME;
      if (g_isServer) mctx.flags |= NEVR_HOST_IS_SERVER;
      else mctx.flags |= NEVR_HOST_IS_CLIENT;
      NotifyModulesStateChange(&mctx, fromState, static_cast<uint32_t>(state));  // N68
    }
    s_prevState = static_cast<uint32_t>(state);
  }

  if (nevr_server_bringup::BeginsMultiplayer(g_isServer != FALSE, fromState, static_cast<uint32_t>(state))) {
    QueueBeginMultiplayer(pGame);
  }

  if (g_isServer) {
    // Redirect "no network". The actual service traffic goes through the ws_bridge, which handles the
    // WebSocket connection independently. Redirect back to LoadingRoot so the
    // game re-attempts the loading process without ending multiplayer.
    if (state == EchoVR::NetGameState::NoNetwork) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] Suppressed NoNetwork transition — redirecting to LoadingRoot to continue boot.");
      EchoVR::NetGameSwitchState(pGame, EchoVR::NetGameState::LoadingRoot);
      return;
    }

    // Redirect "load level failed" back to lobby instead of getting stuck
    if (state == EchoVR::NetGameState::LoadFailed) {
      Log(EchoVR::LogLevel::Debug,
          "[NEVR.PATCH] Dedicated server failed to load level. Resetting session to keep game server available.");
      EchoVR::NetGameScheduleReturnToLobby(pGame);
      return;
    }

    // Track when we enter a game session
    if (state == EchoVR::NetGameState::InGame) {
      g_serverWasInGame = TRUE;
    }

    // Session ended: we were in-game and now returning to lobby. Exit cleanly
    // so the fleet manager can spawn a fresh instance.
    if (g_serverWasInGame && state == EchoVR::NetGameState::Lobby) {
      const GUID endedSession = nevr_login_session::Get();
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] session ended session_id=%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X "
          "— server exiting via graceful shutdown",
          endedSession.Data1, endedSession.Data2, endedSession.Data3,
          endedSession.Data4[0], endedSession.Data4[1], endedSession.Data4[2],
          endedSession.Data4[3], endedSession.Data4[4], endedSession.Data4[5],
          endedSession.Data4[6], endedSession.Data4[7]);
      PerformGracefulShutdown(0);
      // Unreachable
    }
  }

  // Capture the login session GUID when entering the lobby.
  // By this point the Lobby is initialized and localEntrants contains the server's
  // own login session at entrant[0]. The GUID was set by pnsrad.dll's LoginIdResponseCB
  // after the WebSocket login completed. We read it from the Lobby structure.
  if (state == EchoVR::NetGameState::Lobby && nevr_login_session::Get().Data1 == 0 && g_pGame) {
    // The game's CR15NetGame has a lobby at a known offset. The IServerLib::Initialize
    // already receives the Lobby*. But here we read it from the Lobby's localEntrants
    // pool, which contains LoginSession GUIDs for each entrant.
    // The server's own login session is the first one populated.

    // pnsrad.dll prints "LoginId: <GUID>:" to stdout (bypasses WriteLog), but it
    // also goes to the game's log file in _local/r14logs/. Find the latest log
    // and scan for the LoginId line.
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA("_local\\r14logs\\*.log", &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
      CHAR newestLog[MAX_PATH] = {};
      FILETIME newestTime = {};
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
          if (CompareFileTime(&fd.ftLastWriteTime, &newestTime) > 0) {
            newestTime = fd.ftLastWriteTime;
            snprintf(newestLog, MAX_PATH, "_local\\r14logs\\%s", fd.cFileName);
          }
        }
      } while (FindNextFileA(hFind, &fd));
      FindClose(hFind);

      if (newestLog[0] != '\0') {
        FILE* logFile = fopen(newestLog, "r");
        if (logFile) {
          CHAR line[512];
          while (fgets(line, sizeof(line), logFile)) {
            CHAR* loginIdStr = strstr(line, "LoginId: ");
            if (loginIdStr) {
              loginIdStr += 9;
              unsigned long d1;
              unsigned int d2, d3, d4[8];
              if (sscanf(loginIdStr, "%8lX-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X",
                         &d1, &d2, &d3, &d4[0], &d4[1], &d4[2], &d4[3], &d4[4], &d4[5], &d4[6], &d4[7]) == 11) {
                GUID captured = {};
                captured.Data1 = d1;
                captured.Data2 = static_cast<USHORT>(d2);
                captured.Data3 = static_cast<USHORT>(d3);
                for (int i = 0; i < 8; i++) captured.Data4[i] = static_cast<BYTE>(d4[i]);
                nevr_login_session::Set(captured);
              }
            }
          }
          fclose(logFile);
        }
      }
    }

    const GUID session = nevr_login_session::Get();
    if (session.Data1 != 0) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] Captured login session: %08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
          session.Data1, session.Data2, session.Data3,
          session.Data4[0], session.Data4[1], session.Data4[2],
          session.Data4[3], session.Data4[4], session.Data4[5],
          session.Data4[6], session.Data4[7]);
    } else {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Login session GUID not found in game log");
    }
  }

  // Friends subscription is handled by ws_bridge.cpp — it injects an
  // SNSFriendListSubscribeRequest directly through the WS connection after
  // LOGIN SUCCESS. pnsrad's broadcaster handle (field_0x160) is null so
  // calling CNSRADFriends vtable functions for subscribe/refresh is a no-op.

  // Call the original function
  EchoVR::NetGameSwitchState(pGame, state);
}

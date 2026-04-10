#include "state_machine.h"

#include <windows.h>
#include <cstdio>

#include "cli.h"
#include "config.h"
#include "plugin_loader.h"
#include "common/globals.h"
#include "common/logging.h"
#include "common/echovr_functions.h"

/// Tracks whether the server has entered a game session (InGame state).
/// Used to detect session completion when the state returns to Lobby.
static BOOL g_serverWasInGame = FALSE;

/// <summary>
/// A detour hook for the game's method it uses to transition from one net game state to another.
/// </summary>
/// <param name="game">A pointer to the game instance.</param>
/// <param name="state">The state to transition to.</param>
/// <returns>None</returns>
VOID NetGameSwitchStateHook(PVOID pGame, EchoVR::NetGameState state) {
  // Notify plugins of state change
  {
    static uint32_t s_prevState = 0;
    NvrGameContext ctx = {};
    ctx.base_addr = (uintptr_t)EchoVR::g_GameBaseAddress;
    ctx.net_game = pGame;
    ctx.game_state = static_cast<uint32_t>(state);
    ctx.flags = NEVR_HOST_HAS_NETGAME;
    if (g_isServer) ctx.flags |= NEVR_HOST_IS_SERVER;
    else ctx.flags |= NEVR_HOST_IS_CLIENT;
    NotifyPluginsStateChange(&ctx, s_prevState, static_cast<uint32_t>(state));
    s_prevState = static_cast<uint32_t>(state);
  }

  if (g_isServer) {
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
      Log(EchoVR::LogLevel::Info, "[NEVR] Session ended. Server exiting.");
      ExitProcess(0);
    }
  }

  // Capture the login session GUID when entering the lobby.
  // By this point the Lobby is initialized and localEntrants contains the server's
  // own login session at entrant[0]. The GUID was set by pnsrad.dll's LoginIdResponseCB
  // after the WebSocket login completed. We read it from the Lobby structure.
  if (state == EchoVR::NetGameState::Lobby && g_loginSessionId.Data1 == 0 && g_pGame) {
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
                g_loginSessionId.Data1 = d1;
                g_loginSessionId.Data2 = (USHORT)d2;
                g_loginSessionId.Data3 = (USHORT)d3;
                for (int i = 0; i < 8; i++) g_loginSessionId.Data4[i] = (BYTE)d4[i];
              }
            }
          }
          fclose(logFile);
        }
      }
    }

    if (g_loginSessionId.Data1 != 0) {
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] Captured login session: %08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
          g_loginSessionId.Data1, g_loginSessionId.Data2, g_loginSessionId.Data3,
          g_loginSessionId.Data4[0], g_loginSessionId.Data4[1], g_loginSessionId.Data4[2],
          g_loginSessionId.Data4[3], g_loginSessionId.Data4[4], g_loginSessionId.Data4[5],
          g_loginSessionId.Data4[6], g_loginSessionId.Data4[7]);
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

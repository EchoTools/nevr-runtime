/*
 * pnsnevr - Nakama Social Platform Plugin for NEVR
 *
 * DLL Entry Point
 *
 * This DLL replaces pnsovr.dll/pnsrad.dll to provide social features via Nakama.
 *
 * LOADING MECHANISM:
 * The game loads social plugins via GetPluginNameFromConfig() at address 0x1405fe290.
 * To load pnsnevr instead of the default plugins, you have two options:
 *
 * OPTION 1: Config-based (Recommended - No Binary Patching)
 * ---------------------------------------------------------
 * Modify the game's config.json to specify:
 *   {
 *     "matchmaking_plugin": "pnsnevr",
 *     "server_plugin": "pnsnevr"
 *   }
 *
 * The game's GetPluginNameFromConfig() function reads these values and will
 * load pnsnevr.dll instead of pnsradmatchmaking/pnsradgameserver.
 *
 * OPTION 2: IAT Hook / Detour (For Testing)
 * ------------------------------------------
 * Use Microsoft Detours or similar to hook:
 * - GetPluginNameFromConfig (0x1405fe290) - Return "pnsnevr" for plugin queries
 * - LogMatchmakingPluginLoad (0x14060b810) - Intercept and load pnsnevr.dll
 * - LogServerPluginLoad (0x14060bb70) - Intercept and load pnsnevr.dll
 *
 * OPTION 3: Binary Patch (Permanent)
 * -----------------------------------
 * Patch PlatformModuleDecisionAndInitialize at 0x140157fb0:
 * Add check for "social_platform" config key before existing logic.
 * If social_platform == "nakama", LoadLibrary("pnsnevr.dll") and call DLL_Initialize.
 *
 * FUNCTION EXPORTS:
 * - DLL_Initialize(game_state*, config*) - Called when plugin is loaded
 * - DLL_Shutdown() - Called when plugin is unloaded
 * - DLL_Tick() - Called periodically for async processing (50ms recommended)
 */

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "config.h"
#include "game_bridge.h"
#include "nakama_client.h"

// Global state
static NakamaClient* g_nakamaClient = nullptr;
static GameBridge* g_gameBridge = nullptr;
static NakamaConfig g_config;
static bool g_initialized = false;

// Export macros
#ifdef PNSNEVR_EXPORTS
#define PNSNEVR_API extern "C" __declspec(dllexport)
#else
#define PNSNEVR_API extern "C" __declspec(dllimport)
#endif

// Forward declarations for game structures (from Ghidra analysis)
struct GameState;
struct ConfigData;
struct UdpBroadcaster;
struct TcpBroadcaster;

/*
 * DLL_Initialize - Main entry point called by the game
 *
 * This function is called by LogMatchmakingPluginLoad/LogServerPluginLoad
 * after the DLL is loaded via LoadLibrary.
 *
 * @param game_state Pointer to game state structure (CR15NetGame instance)
 *                   Contains:
 *                   - game_state[0xa30]: TCP broadcaster pointer
 *                   - game_state[0xa32]: UDP broadcaster pointer
 *                   - game_state[0xb68]: Mode flags pointer
 *
 * @param config     Pointer to configuration data
 *
 * @return 0 on success, non-zero error code on failure
 */
PNSNEVR_API int __stdcall DLL_Initialize(void* game_state, void* config) {
  OutputDebugStringA("[pnsnevr] DLL_Initialize called\n");

  if (g_initialized) {
    OutputDebugStringA("[pnsnevr] Already initialized, skipping\n");
    return 0;
  }

  // Parse configuration
  if (!LoadNakamaConfig(config, &g_config)) {
    OutputDebugStringA("[pnsnevr] ERROR: Failed to load Nakama config\n");
    // Use defaults
    g_config.api_endpoint = "http://127.0.0.1";
    g_config.http_port = 7350;
    g_config.server_key = "defaultkey";
    g_config.features.friends = true;
    g_config.features.parties = true;
    g_config.features.matchmaking = true;
  }

  char logBuf[512];
  snprintf(logBuf, sizeof(logBuf), "[pnsnevr] Config: endpoint=%s port=%d key=%s\n", g_config.api_endpoint.c_str(),
           g_config.http_port, g_config.server_key.c_str());
  OutputDebugStringA(logBuf);

  // Create game bridge to access game internals
  g_gameBridge = new GameBridge(game_state);
  if (!g_gameBridge->Initialize()) {
    OutputDebugStringA("[pnsnevr] ERROR: Failed to initialize game bridge\n");
    delete g_gameBridge;
    g_gameBridge = nullptr;
    return -1;
  }

  // Create Nakama client
  g_nakamaClient = new NakamaClient(g_config);
  if (!g_nakamaClient->Initialize()) {
    OutputDebugStringA("[pnsnevr] ERROR: Failed to initialize Nakama client\n");
    delete g_nakamaClient;
    g_nakamaClient = nullptr;
    delete g_gameBridge;
    g_gameBridge = nullptr;
    return -2;
  }

  // Register message handlers with game's broadcaster
  RegisterNakamaMessageHandlers(g_gameBridge, g_nakamaClient);

  // Authenticate with Nakama server
  // For prototype: use device ID authentication
  std::string deviceId = g_gameBridge->GetDeviceId();
  if (!g_nakamaClient->AuthenticateDevice(deviceId)) {
    OutputDebugStringA("[pnsnevr] WARNING: Failed to authenticate with Nakama\n");
    // Continue anyway - will retry later
  }

  g_initialized = true;
  OutputDebugStringA("[pnsnevr] Initialization complete\n");
  return 0;
}

/*
 * DLL_Shutdown - Cleanup when plugin is unloaded
 *
 * Called when the game is shutting down or switching modes.
 */
PNSNEVR_API void __stdcall DLL_Shutdown() {
  OutputDebugStringA("[pnsnevr] DLL_Shutdown called\n");

  if (g_nakamaClient) {
    g_nakamaClient->Disconnect();
    delete g_nakamaClient;
    g_nakamaClient = nullptr;
  }

  if (g_gameBridge) {
    g_gameBridge->UnregisterHandlers();
    delete g_gameBridge;
    g_gameBridge = nullptr;
  }

  g_initialized = false;
  OutputDebugStringA("[pnsnevr] Shutdown complete\n");
}

/*
 * DLL_Tick - Called periodically by the game
 *
 * This pumps the Nakama client's request queue and executes callbacks.
 * The game should call this every 50ms or so in the main game loop.
 *
 * Note: This is a custom export. The original pnsrad plugins may not have
 * this export. If the game doesn't call it, we need to create our own
 * background thread in DLL_Initialize.
 */
PNSNEVR_API void __stdcall DLL_Tick() {
  if (g_nakamaClient) {
    g_nakamaClient->Tick();
  }
}

/*
 * DLL_GetVersion - Returns plugin version
 *
 * For compatibility checking.
 */
PNSNEVR_API const char* __stdcall DLL_GetVersion() { return "pnsnevr v0.1.0 (Nakama Social Plugin)"; }

/*
 * DllMain - Windows DLL entry point
 */
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      OutputDebugStringA("[pnsnevr] DLL_PROCESS_ATTACH\n");
      DisableThreadLibraryCalls(hModule);
      break;

    case DLL_PROCESS_DETACH:
      OutputDebugStringA("[pnsnevr] DLL_PROCESS_DETACH\n");
      // Ensure cleanup on unload
      if (g_initialized) {
        DLL_Shutdown();
      }
      break;
  }
  return TRUE;
}

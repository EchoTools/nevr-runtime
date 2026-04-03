#include "config.h"
#include "cli.h"
#include "common/globals.h"
#include "common/logging.h"
#include "common/echovr_functions.h"
#include "gamepatches_internal.h"

/// <summary>
/// The game instance pointer -- stored globally for social message injection.
/// Set during PreprocessCommandLineHook, used to navigate to the broadcaster.
/// Path: pGame + 0x8518 -> CR15NetGame -> lobby -> +0x008 -> Broadcaster*
/// </summary>
PVOID g_pGame = NULL;

/// <summary>
/// The local config stored in ./_local/config.json.
/// </summary>
EchoVR::Json* g_localConfig = NULL;

/// <summary>
/// Early-loaded config for URI redirect hooks that fire before the game loads its config.
/// Loaded during Initialize() from _local/config.json using the game's JSON parser.
/// </summary>
static EchoVR::Json g_earlyConfig = {NULL, NULL};
EchoVR::Json* g_earlyConfigPtr = NULL;

/// <summary>
/// Early-load _local/config.json so URI redirect hooks work before the game loads its config.
/// The game's JSON loader is available at this point (it's a static function in the EXE).
/// </summary>
VOID LoadEarlyConfig() {
  CHAR configPath[MAX_PATH] = {0};
  CHAR moduleDir[MAX_PATH] = {0};
  GetModuleFileNameA((HMODULE)EchoVR::g_GameBaseAddress, moduleDir, MAX_PATH);
  // Strip the filename to get the directory
  CHAR* lastSlash = strrchr(moduleDir, '\\');
  if (lastSlash) *(lastSlash + 1) = '\0';

  // Try _local/config.json next to the exe first, then walk up parent directories.
  // Supports both flat layouts (bin/win10/_local/) and nested layouts (echovr/_local/).
  const CHAR* searchPaths[] = {
    "%s_local\\config.json",
    "%s..\\_local\\config.json",
    "%s..\\..\\_local\\config.json",
  };

  UINT32 loadResult = 0xFFFFFFFF;
  for (int i = 0; i < 3; i++) {
    snprintf(configPath, MAX_PATH, searchPaths[i], moduleDir);
    loadResult = EchoVR::LoadJsonFromFile(&g_earlyConfig, configPath, 1);
    if (loadResult == 0 && g_earlyConfig.root != NULL) {
      g_earlyConfigPtr = &g_earlyConfig;
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Early config loaded from: %s", configPath);
      return;
    }
  }

  Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to early-load config from: %s_local\\config.json (error %u)",
      moduleDir, loadResult);
}

/// <summary>
/// A detour hook for the game's function to load the local config.json for the game instance.
/// If a custom config path was provided via -config-path, it loads that file directly using
/// the game's internal JSON loading function, bypassing the default _local/config.json.
/// </summary>
/// <param name="pGame">A pointer to the game struct to load the config for.</param>
UINT64 LoadLocalConfigHook(PVOID pGame) {
  UINT64 result;

  // If a custom config.json path was provided, load it directly using the game's JSON loader
  if (g_customConfigPath[0] != '\0') {
    // Resolve to full path so the game's loader can find it regardless of CWD
    CHAR resolvedPath[MAX_PATH] = {0};
    DWORD len = GetFullPathNameA(g_customConfigPath, MAX_PATH, resolvedPath, NULL);
    const CHAR* configPath = (len > 0 && len < MAX_PATH) ? resolvedPath : g_customConfigPath;

    Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Loading custom config from: %s", configPath);

    // Get the config destination pointer (pGame + 0x63240)
    using namespace PatchAddresses;
    EchoVR::Json* configDest = reinterpret_cast<EchoVR::Json*>(static_cast<CHAR*>(pGame) + GAME_LOCAL_CONFIG_OFFSET);

    // Call the game's internal JSON loader directly with our custom path
    // The third parameter (1) indicates the validation level: 1 = standard validation
    UINT32 loadResult = EchoVR::LoadJsonFromFile(configDest, configPath, 1);

    if (loadResult != 0) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] Failed to load custom config file: %s (error %u)", configPath,
          loadResult);
      // Fall back to loading the default config
      result = EchoVR::LoadLocalConfig(pGame);
    } else {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Successfully loaded custom config from: %s", configPath);
      result = 0;  // Success
    }
  } else {
    // No custom config specified — try the default loader first.
    // If it fails (config not next to exe), search parent directories.
    result = EchoVR::LoadLocalConfig(pGame);

    using namespace PatchAddresses;
    EchoVR::Json* configDest = reinterpret_cast<EchoVR::Json*>(static_cast<CHAR*>(pGame) + GAME_LOCAL_CONFIG_OFFSET);
    if (configDest->root == NULL) {
      // Default loader failed — search parent directories for _local/config.json
      CHAR moduleDir[MAX_PATH] = {0};
      GetModuleFileNameA((HMODULE)EchoVR::g_GameBaseAddress, moduleDir, MAX_PATH);
      CHAR* lastSlash = strrchr(moduleDir, '\\');
      if (lastSlash) *(lastSlash + 1) = '\0';

      const CHAR* parentPrefixes[] = {"..\\", "..\\..\\"};
      for (int i = 0; i < 2; i++) {
        CHAR tryPath[MAX_PATH] = {0};
        snprintf(tryPath, MAX_PATH, "%s%s_local\\config.json", moduleDir, parentPrefixes[i]);
        UINT32 loadResult = EchoVR::LoadJsonFromFile(configDest, tryPath, 1);
        if (loadResult == 0 && configDest->root != NULL) {
          Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Game config loaded from: %s", tryPath);
          result = 0;
          break;
        }
      }
    }
  }

  // Store a reference to the local config from the game structure
  using namespace PatchAddresses;
  g_localConfig = reinterpret_cast<EchoVR::Json*>(static_cast<CHAR*>(pGame) + GAME_LOCAL_CONFIG_OFFSET);

  // Configure Asset CDN URL from config.json if specified
  if (g_localConfig != NULL) {
    CHAR* customCdnUrl = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"asset_cdn_url", NULL, false);
    if (customCdnUrl != NULL && customCdnUrl[0] != '\0') {
      // AssetCDN::SetCustomCdnUrl(customCdnUrl);
      // Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Asset CDN URL set from config: %s", customCdnUrl);
    }

    // exitonerror
    CHAR* exitOnErrorVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"exitonerror", NULL, false);
    if (exitOnErrorVal != NULL && (strcmp(exitOnErrorVal, "true") == 0 || strcmp(exitOnErrorVal, "1") == 0)) {
      g_exitOnError = TRUE;
    }

    // upnp
    CHAR* upnpVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"upnp", NULL, false);
    if (upnpVal != NULL && (strcmp(upnpVal, "true") == 0 || strcmp(upnpVal, "1") == 0)) {
      g_upnpEnabled = TRUE;
    }

    // upnp_port (external port override)
    CHAR* upnpPortVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"upnp_port", NULL, false);
    if (upnpPortVal != NULL && upnpPortVal[0] != '\0') {
      UINT32 port = strtoul(upnpPortVal, nullptr, 10);
      if (port > 0 && port <= 65535) g_upnpPort = (UINT16)port;
    }

    // internal_ip override
    CHAR* internalIpVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"internal_ip", NULL, false);
    if (internalIpVal != NULL && internalIpVal[0] != '\0') {
      strncpy(g_internalIpOverride, internalIpVal, sizeof(g_internalIpOverride) - 1);
    }

    // external_ip override
    CHAR* externalIpVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"external_ip", NULL, false);
    if (externalIpVal != NULL && externalIpVal[0] != '\0') {
      strncpy(g_externalIpOverride, externalIpVal, sizeof(g_externalIpOverride) - 1);
    }

    // Arena rule overrides (float values, 0 = use game default)
    CHAR* arenaRoundTimeVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"arena_round_time", NULL, false);
    if (arenaRoundTimeVal != NULL && arenaRoundTimeVal[0] != '\0') {
      g_arenaRoundTime = (FLOAT)atof(arenaRoundTimeVal);
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Arena round time override: %.0f seconds", g_arenaRoundTime);
    }
    CHAR* arenaCelebrationVal =
        EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"arena_celebration_time", NULL, false);
    if (arenaCelebrationVal != NULL && arenaCelebrationVal[0] != '\0') {
      g_arenaCelebrationTime = (FLOAT)atof(arenaCelebrationVal);
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Arena celebration time override: %.1f seconds", g_arenaCelebrationTime);
    }
    CHAR* arenaMercyVal = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"arena_mercy_score", NULL, false);
    if (arenaMercyVal != NULL && arenaMercyVal[0] != '\0') {
      g_arenaMercyScore = (FLOAT)atof(arenaMercyVal);
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Arena mercy score override: %.0f", g_arenaMercyScore);
    }
  }

  return result;
}

/// <summary>
/// Hook for CJson_GetFloat to override arena rule config values at load time.
/// Matches specific JSON path suffixes and returns overridden values from config.json.
/// </summary>
FLOAT CJsonGetFloatHook(PVOID root, const CHAR* path, FLOAT defaultValue, INT32 required) {
  FLOAT result = EchoVR::CJsonGetFloat(root, path, defaultValue, required);

  if (path != NULL) {
    // point_score_celebration_time (but not the _private variant)
    if (g_arenaCelebrationTime > 0.0f && strstr(path, "point_score_celebration_time") != NULL &&
        strstr(path, "_private") == NULL) {
      return g_arenaCelebrationTime;
    }
    // round_time (but not round_time_private)
    if (g_arenaRoundTime > 0.0f && strstr(path, "round_time") != NULL && strstr(path, "_private") == NULL &&
        strstr(path, "round_timer") == NULL && strstr(path, "sudden_death_round_time") == NULL) {
      return g_arenaRoundTime;
    }
    // mercy_win_point_spread (but not the _private variant)
    if (g_arenaMercyScore > 0.0f && strstr(path, "mercy_win_point_spread") != NULL &&
        strstr(path, "_private") == NULL) {
      return g_arenaMercyScore;
    }
  }

  return result;
}

/// <summary>
/// Helper function to get a service host from config.json with fallback logic.
/// Fallback chain: service_key -> loginservice_host -> default_url
/// </summary>
/// <param name="serviceKey">The primary config key to check (e.g., "configservice_host")</param>
/// <param name="defaultUrl">The default URL if no config override is found</param>
/// <returns>The resolved service URL</returns>
static CHAR* GetServiceHostWithFallback(const CHAR* serviceKey, const CHAR* defaultUrl) {
  // Use game config if available, fall back to early-loaded config
  EchoVR::Json* config = g_localConfig ? g_localConfig : g_earlyConfigPtr;
  if (config == NULL) return (CHAR*)defaultUrl;

  // Try primary service key first
  CHAR* host = EchoVR::JsonValueAsString(config, (CHAR*)serviceKey, NULL, false);
  if (host != NULL && host[0] != '\0') {
    Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Service override [%s]: %s", serviceKey, host);
    return host;
  }

  // Fallback to loginservice_host if primary key not found
  host = EchoVR::JsonValueAsString(config, (CHAR*)"loginservice_host", NULL, false);
  if (host != NULL && host[0] != '\0') {
    Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Service fallback [%s → loginservice_host]: %s", serviceKey, host);
    return host;
  }

  // Return default URL
  Log(EchoVR::LogLevel::Debug, "[NEVR.PATCH] Service default [%s]: %s", serviceKey, defaultUrl);
  return (CHAR*)defaultUrl;
}

/// <summary>
/// A detour hook for the game's HTTP(S) connect function. Redirects hardcoded endpoints
/// using config.json overrides with fallback chain: service_key -> loginservice_host -> default.
///
/// The game has NO centralized service endpoint registry. Each service manages its own
/// endpoint independently (CLoginService: loginservice_host, CNSRadMatchmaking:
/// matchingservice_host, CHTTPApi: hardcoded api.readyatdawn.com etc.). URL substring
/// matching is the correct approach given this per-service architecture.
/// </summary>
UINT64 HttpConnectHook(PVOID unk, CHAR* uri) {
  // If we have a local config, check for service overrides with fallback logic
  if (g_localConfig != NULL) {
    CHAR* originalUri = uri;

    // API Service (https://api.*)
    if (!strncmp(uri, "https://api.", 12)) {
      uri = GetServiceHostWithFallback("apiservice_host", uri);
      // Legacy compatibility: also try "api_host"
      if (uri == originalUri) {
        uri = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"api_host", uri, false);
      }
    }
    // Config Service - detect config-related URLs
    else if (strstr(uri, "config") != NULL && strstr(uri, "https://") == uri) {
      uri = GetServiceHostWithFallback("configservice_host", uri);
    }
    // Transaction Service - detect transaction/IAP URLs
    else if ((strstr(uri, "transaction") != NULL || strstr(uri, "iap") != NULL) && strstr(uri, "https://") == uri) {
      uri = GetServiceHostWithFallback("transactionservice_host", uri);
    }
    // Matching Service - detect matchmaking URLs
    else if (strstr(uri, "match") != NULL && strstr(uri, "https://") == uri) {
      uri = GetServiceHostWithFallback("matchingservice_host", uri);
    }
    // ServerDB Service - detect serverdb/registry URLs
    else if ((strstr(uri, "serverdb") != NULL || strstr(uri, "registry") != NULL) && strstr(uri, "https://") == uri) {
      uri = GetServiceHostWithFallback("serverdb_host", uri);
    }
    // Oculus Graph API
    else if (!strncmp(uri, "https://graph.oculus.com", 24)) {
      uri = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"graph_host", uri, false);
      if (uri == originalUri) {
        uri = EchoVR::JsonValueAsString(g_localConfig, (CHAR*)"graphservice_host", uri, false);
      }
    }

    if (uri != originalUri) {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] HTTP(S) connection redirected: %s → %s", originalUri, uri);
    }
  }

  // Call the original function
  return EchoVR::HttpConnect(unk, uri);
}

// ============================================================================
// JsonValueAsString Hook (intercepts config lookups to provide early overrides)
// ============================================================================

/// <summary>
/// Hook for JsonValueAsString. When the game's config doesn't have a key (returns the
/// hardcoded default), check our early-loaded _local/config.json for an override.
/// This is necessary because the game reads config values before LoadLocalConfig runs,
/// so hardcoded defaults (readyatdawn.com URLs, "rad15_live", etc.) always win.
/// </summary>
// Redirect readyatdawn.com service URLs to echovrce.com.
// The game has hardcoded defaults like "wss://login.readyatdawn.com/rad/rad15_live"
// for configservice_host, loginservice_host, etc. These servers are dead.
// We pick the redirect target based on the URL scheme:
//   wss:// URLs → nevr_socket_uri (WebSocket endpoint)
//   https:// URLs → nevr_http_uri (HTTP API endpoint)
static CHAR* RedirectServiceUrl(CHAR* keyName, CHAR* result) {
  if (result == NULL || keyName == NULL) return result;
  if (strstr(result, "readyatdawn.com") == NULL) return result;
  if (g_earlyConfigPtr == NULL) return result;

  // Select the right config key based on scheme
  const CHAR* configKey;
  if (strstr(result, "wss://") == result || strstr(result, "ws://") == result) {
    configKey = "nevr_socket_uri";
  } else {
    configKey = "nevr_http_uri";
  }

  CHAR* target = EchoVR::JsonValueAsString(g_earlyConfigPtr, (CHAR*)configKey, NULL, false);
  if (target == NULL || target[0] == '\0') return result;

  static CHAR redirected[512];

  // All WebSocket connections go through the in-process proxy
  extern bool IsWebSocketBridgeActive();
  extern uint16_t GetWebSocketBridgePort();
  if (IsWebSocketBridgeActive() &&
      (strstr(result, "wss://") == result || strstr(result, "ws://") == result)) {
    snprintf(redirected, sizeof(redirected), "ws://127.0.0.1:%u", GetWebSocketBridgePort());
  } else {
    snprintf(redirected, sizeof(redirected), "%s", target);
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Service redirect [%s]: %s -> %s", keyName, result, redirected);
  return redirected;
}

CHAR* JsonValueAsStringHook(EchoVR::Json* root, CHAR* keyName, CHAR* defaultValue, BOOL reportFailure) {
  // Call the original first
  CHAR* result = EchoVR::JsonValueAsString(root, keyName, defaultValue, reportFailure);

  // Redirect any readyatdawn.com service URLs to echovrce.com
  result = RedirectServiceUrl(keyName, result);

  // If we have an early config, check if it has an override for this key.
  // Only override when the result equals the default (meaning the game's config didn't have it).
  // Don't override lookups against our own early config (avoid infinite loop).
  if (g_earlyConfigPtr != NULL && keyName != NULL && root != g_earlyConfigPtr && result == defaultValue) {
    CHAR* override = EchoVR::JsonValueAsString(g_earlyConfigPtr, keyName, NULL, false);
    if (override != NULL && override[0] != '\0') {
      Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] Config override [%s]: %s -> %s", keyName,
          result ? result : "(null)", override);
      return override;
    }
  }

  return result;
}

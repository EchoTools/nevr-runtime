#pragma once

#include "common/pch.h"
#include "common/echovr.h"

/// <summary>
/// The game instance pointer -- stored globally for social message injection.
/// Set during PreprocessCommandLineHook, used to navigate to the broadcaster.
/// Path: pGame + 0x8518 -> CR15NetGame -> lobby -> +0x008 -> Broadcaster*
/// </summary>
extern PVOID g_pGame;

/// <summary>
/// The local config stored in ./_local/config.json.
/// </summary>
extern EchoVR::Json* g_localConfig;

/// <summary>
/// A detour hook for the game's function to load the local config.json for the game instance.
/// </summary>
UINT64 LoadLocalConfigHook(PVOID pGame);

/// <summary>
/// A detour hook for the game's HTTP(S) connect function. Redirects hardcoded endpoints
/// using config.json overrides with fallback chain: service_key -> loginservice_host -> default.
/// </summary>
UINT64 HttpConnectHook(PVOID unk, CHAR* uri);

/// <summary>
/// Hook for JsonValueAsString. When the game's config doesn't have a key (returns the
/// hardcoded default), check our early-loaded _local/config.json for an override.
/// </summary>
CHAR* JsonValueAsStringHook(EchoVR::Json* root, CHAR* keyName, CHAR* defaultValue, BOOL reportFailure);

/// <summary>
/// Hook for CJson_GetFloat to override arena rule config values at load time.
/// </summary>
FLOAT CJsonGetFloatHook(PVOID root, const CHAR* path, FLOAT defaultValue, INT32 required);

/// <summary>
/// Early-load _local/config.json so URI redirect hooks work before the game loads its config.
/// </summary>
VOID LoadEarlyConfig();

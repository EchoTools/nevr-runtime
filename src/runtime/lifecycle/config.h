#pragma once

#include "core/pch.h"
#include <string>
#include "abi/echovr.h"
#include "runtime/lifecycle/server_timestep.h"

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

/// #515: what writing the server's time step did (outcome kNotApplied until the local config has loaded),
/// with the step written and the value it replaced. boot.cpp reports this, not the configured rate, when it
/// says what step the server runs on.
nevr_server_timestep::StepWrite ServerStepWrite();

/// #1785: sets the server's step for a session of `gameType` (Combat 30 a second, Arena the default 120, an explicit
/// network.tick_rate_hz for all) when the session opens. A client writes nothing.
void ApplySessionTimestep(const std::string& gameType, const std::string& sessionId);

/// #515: logs once what the clock the server runs on holds (its step and mode bits), so a run's log says
/// whether the step NEVR wrote is live. Called from the per-frame tick on one tick (frame/tick.cpp).
void LogServerStepReadBack(long tick);

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
/// Early-load _local/config.json, if one exists. OPTIONAL (issue #21): every NEVR
/// setting lives in config.yaml; config.json only carries keys the stock engine
/// reads natively. Absent -> Info, present-but-unparseable -> Warning, never fatal.
/// </summary>
VOID LoadEarlyConfig();

/// Pointer to the early-loaded config JSON (set by LoadEarlyConfig). NULL when no
/// _local/config.json exists — a supported state, not an error.
extern EchoVR::Json* g_earlyConfigPtr;

/// <summary>
/// Allow RedirectServiceUrl to act. Called once from RunDeferredRuntimeBootstrap
/// at the point g_earlyConfigPtr would become non-null, because redirect
/// resolution is the FIRST access to the lazily loaded config.yaml singleton and
/// must not happen before the bootstrap (see RedirectServiceUrl).
/// </summary>
VOID ArmServiceRedirects();

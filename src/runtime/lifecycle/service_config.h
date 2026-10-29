// service_config.h — C-string accessors over the config.yaml singleton (N133 S3).
//
// config.cpp is compiled WITH the core PCH (windows.h, no NOMINMAX). To keep it
// away from any header that could trip the min/max macro, this interface is
// deliberately plain: no <optional>, no nevr_config.h, no std types in the
// signatures. The std::optional-based pure core is service_map.h (test-linked);
// the singleton, config.yaml discovery, and interning live in service_config.cpp.
//
// Every non-null return is an interned pointer that remains valid through DLL
// unload because the shared pool owner is intentionally process-lifetime. The
// pool quota applies per loaded module generation, not across reloads. A null
// return means genuine absence/unmapped/no redirect; an attempted intern failure
// or std::exception is logged without values and terminates the process rather
// than masquerading as absence. An exception that does not derive from
// std::exception is not caught here.

#pragma once

/// A migrated flat NEVR key read from config.yaml. Null if the key is unmapped or
/// absent; may be a stable "" if the key is present-but-empty (caller checks [0]).
const char* NevrCfgGetFlat(const char* flatKey);

/// Where the config came from, for an operator-facing line (#1254): the config.yaml path that was loaded, or
/// "none found, searched <paths>". Never null; loads the config on first call like every accessor here.
const char* NevrCfgSource();

/// A LIST-shaped migrated flat key (guilds, regions) as a CSV string — a yaml list
/// `[a, b]` and a scalar CSV `"a,b"` both return "a,b" (the shape the game-JSON
/// readers built into guilds=/regions= URL params). Null if unmapped or absent.
const char* NevrCfgGetFlatCsv(const char* flatKey);

/// Service host with the loginservice_host fallback (GetServiceHostWithFallback).
/// Returns the resolved host, or null when neither the service key nor
/// loginservice_host is set. *outSource (if non-null) reports which source hit:
///   0 = the requested service key, 1 = loginservice_host, 2 = none (use default).
const char* NevrCfgServiceHost(const char* flatServiceKey, int* outSource);

/// Scheme redirect (RedirectServiceUrl core). `result` is the URL the game
/// produced; `httpTargetJson` is nevr_http_uri read from the early game JSON by
/// the caller (that key is NOT migrated in S3). Returns the interned replacement,
/// or null to leave `result` unchanged. bridgeActive is 0/1. `serviceKey` is the JSON key the game
/// read `result` under: when the redirect points the config or login service at the bridge, the bridge
/// address carries that service's request target (/config, /login) so the bridge can tell the sockets
/// apart (#520, nevr_socket_role::TagBridgeUrl). Null for a redirect that is not for a game service key.
const char* NevrCfgRedirect(const char* result, const char* httpTargetJson, int bridgeActive,
                            unsigned bridgePort, const char* serviceKey = nullptr);

/// Auto-relay target (AutoRelayThroughBridge): ws://127.0.0.1:<bridgePort> when
/// nevr_socket_uri is configured, else null. With a config or login `serviceKey` the address carries that
/// service's request target, as for NevrCfgRedirect.
const char* NevrCfgAutoRelay(unsigned bridgePort, const char* serviceKey = nullptr);

/// Issue #21: the interned value NEVR supplies for a stock-engine JSON key that
/// no config provided (nevr_cfg::GameNativeDefault), or null to leave the
/// engine's own default. Does not touch config.yaml.
const char* NevrGameNativeDefault(const char* key);

/// True unless config.yaml explicitly sets `social.facade: false`.
bool NevrCfgSocialFacadeEnabled();

/// The player's saved capture device (#412): `audio.capture_device` in config.yaml, an endpoint id or part of a
/// friendly name. Null when the key is absent or empty, which means "use the default rule". Interned, like the
/// other accessors: the pointer stays valid for the process lifetime.
const char* NevrCfgCaptureDevice();

/// Boot logs, once, the built-in defaults this run uses (names only): the keys config.yaml and the
/// environment leave unset that take an embedded value (#843). Reads config.yaml, so call it after it loads.
void NevrCfgLogBuiltinDefaultsTaken();

/// JSON text of the game-native config to hand the game when it found no `_local/config.json`
/// (the `social_plugin` block that turns on friends, parties and presence), built from the
/// Nakama HTTP base and server key via config.yaml, else the build's embedded defaults. nullptr
/// when there is nothing to supply (server mode, or those keys are not configured). The pointer
/// is stable for the process lifetime. Contains the server key: never log it.
const char* NevrCfgGameNativeConfigJson();

/// Chooses the config.yaml of this run, once (#1255): `-config-file`, else the nevr root's. Copies a legacy
/// `_local\config.yaml` into the root when the root has none, logs what it did, and for a command line it refuses
/// (RejectCommandLine, exit code 64) does not return. Boot calls it once the flags are read, before the first read of
/// config.yaml. The accessors call it themselves, so a caller that skips it still gets the same decision.
void NevrCfgResolveConfigYaml();

// service_config.h — C-string accessors over the config.yaml singleton (N133 S3).
//
// config.cpp is compiled WITH the core PCH (windows.h, no NOMINMAX). To keep it
// away from any header that could trip the min/max macro, this interface is
// deliberately plain: no <optional>, no nevr_config.h, no std types in the
// signatures. The std::optional-based pure core is service_map.h (test-linked);
// the singleton, config.yaml discovery, and interning live in service_config.cpp.
//
// Every non-null return is a process-lifetime, interned, stable pointer — the
// game holds these for the process lifetime, exactly as the old JsonValueAsString
// tree pointers were stable. A null return means "not configured": the caller
// keeps its existing default, preserving today's behaviour for an absent key.

#pragma once

/// A migrated flat NEVR key read from config.yaml. Null if the key is unmapped or
/// absent; may be a stable "" if the key is present-but-empty (caller checks [0]).
const char* NevrCfgGetFlat(const char* flatKey);

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
/// or null to leave `result` unchanged. bridgeActive is 0/1.
const char* NevrCfgRedirect(const char* result, const char* httpTargetJson, int bridgeActive,
                            unsigned bridgePort);

/// Auto-relay target (AutoRelayThroughBridge): ws://127.0.0.1:<bridgePort> when
/// nevr_socket_uri is configured, else null.
const char* NevrCfgAutoRelay(unsigned bridgePort);

#pragma once

#include "echovr_functions.h"

#ifndef PROJECT_VERSION          // Set by CMake during the build process
#define PROJECT_VERSION "1.0.0"  // Fallback default version
#endif

#ifndef PROJECT_VERSION_MAJOR    // Set by CMake during the build process
#define PROJECT_VERSION_MAJOR 1  // Fallback default major version
#endif
#ifndef PROJECT_VERSION_MINOR    // Set by CMake during the build process
#define PROJECT_VERSION_MINOR 0  // Fallback default minor version
#endif
#ifndef PROJECT_VERSION_PATCH    // Set by CMake during the build process
#define PROJECT_VERSION_PATCH 0  // Fallback default patch version
#endif

#ifndef GIT_COMMIT_HASH            // Set by CMake during the build process
#define GIT_COMMIT_HASH "unknown"  // Fallback default commit hash
#endif

extern BOOL g_noConsole;
extern BOOL g_isHeadless;
extern BOOL g_exitOnError;
extern UINT32 g_headlessTickRateHz;
extern BOOL g_telemetryEnabled;
extern UINT32 g_telemetryRateHz;
extern BOOL g_telemetryDiag;
extern BOOL g_timestampLogs;
extern BOOL g_upnpEnabled;
extern UINT16 g_upnpPort;
extern CHAR g_internalIpOverride[46];
extern CHAR g_externalIpOverride[46];
/// Login session GUID captured from the game's login response.
/// Populated by NetGameSwitchStateHook when state transitions to LoggedIn.
extern GUID g_loginSessionId;

/// Arena rule overrides (0.0 = use game default). Set by config.json keys.
extern FLOAT g_arenaRoundTime;
extern FLOAT g_arenaCelebrationTime;
extern FLOAT g_arenaMercyScore;

#pragma once

#include "abi/echovr_functions.h"

extern BOOL g_noConsole;
extern BOOL g_isHeadless;
extern BOOL g_exitOnError;
extern UINT32 g_headlessTickRateHz;
extern BOOL g_telemetryEnabled;
extern UINT32 g_telemetryRateHz;
extern BOOL g_telemetryDiag;
extern BOOL g_timestampLogs;
extern BOOL g_noOvr;
extern BOOL g_upnpEnabled;
extern UINT16 g_upnpPort;
extern CHAR g_internalIpOverride[46];
extern CHAR g_externalIpOverride[46];
// The login session GUID lives in core/login_session.h (nevr_login_session::Get / Set).

/// Arena rule overrides (0.0 = use game default). Set by config.json keys.
extern FLOAT g_arenaRoundTime;
extern FLOAT g_arenaCelebrationTime;
extern FLOAT g_arenaMercyScore;

#pragma once

#include "common/pch.h"

/// A CLI argument flag indicating whether the game is booting as a dedicated server.
/// When true, headless and noovr patches are applied automatically.
extern BOOL g_isServer;

/// A CLI argument flag indicating whether the game is booting as an offline client.
extern BOOL g_isOffline;

/// A CLI argument flag indicating whether the game is booting in a windowed mode.
extern BOOL g_isWindowed;

/// Custom config path provided via -config. If empty, the default path is used.
extern CHAR g_customConfigPath[MAX_PATH];

/// Region override from -region or -serverregion CLI args. Empty = default.
extern CHAR g_regionOverride[64];

/// A detour hook for the game's method it uses to build CLI argument definitions.
/// Adds additional definitions to the structure, so that they may be parsed successfully without error.
UINT64 BuildCmdLineSyntaxDefinitionsHook(PVOID pGame, PVOID pArgSyntax);

#pragma once

#include "common/pch.h"

/// <summary>
/// A CLI argument flag indicating whether the game is booting as a dedicated server.
/// </summary>
extern BOOL g_isServer;
/// <summary>
/// A CLI argument flag indicating whether the game is booting as an offline client.
/// </summary>
extern BOOL g_isOffline;
/// <summary>
/// A CLI argument flag indicating whether the game is booting in a windowed mode, rather than with a VR headset.
/// </summary>
extern BOOL g_isWindowed;

/// <summary>
/// Indicates whether the game was launched with `-noovr`.
/// </summary>
extern BOOL g_isNoOVR;

/// <summary>
/// Custom config.json path provided via command-line argument. If empty, the default path is used.
/// </summary>
extern CHAR g_customConfigJsonPath[MAX_PATH];

/// <summary>
/// A timestep value in ticks/updates per second, to be used for headless mode (due to lack of GPU/refresh rate
/// throttling). If non-zero, sets the timestep override by the given tick rate per second. If zero, removes tick rate
/// throttling.
/// </summary>
extern UINT32 g_headlessTimeStep;

/// <summary>
/// A detour hook for the game's method it uses to build CLI argument definitions.
/// Adds additional definitions to the structure, so that they may be parsed successfully without error.
/// </summary>
/// <param name="game">A pointer to the game instance.</param>
/// <param name="pArgSyntax">A pointer to the CLI argument structure tracking all CLI arguments.</param>
UINT64 BuildCmdLineSyntaxDefinitionsHook(PVOID pGame, PVOID pArgSyntax);

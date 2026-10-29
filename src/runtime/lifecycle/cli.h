#pragma once

#include <string>

#include "core/pch.h"

/// A CLI argument flag indicating whether the game is booting as a dedicated server.
/// When true, headless and noovr patches are applied automatically.
extern BOOL g_isServer;

/// A CLI argument flag indicating whether the game is booting as an offline client.
extern BOOL g_isOffline;

/// A CLI argument flag indicating whether the game is booting in a windowed mode.
extern BOOL g_isWindowed;

/// Custom config path provided via -config. If empty, the default path is used.
extern CHAR g_customConfigPath[MAX_PATH];

/// What the command line said about config.yaml (#1255): `-config-file <file>` names the exact file to read, and
/// `-config-path` is refused (use -config-file). `-config` is the game's JSON file and has nothing to do with config.yaml.
struct ConfigYamlFlags {
  bool config_file_given = false;
  std::string config_file;  ///< the value of `-config-file`, UTF-8; empty when the flag had none
  bool config_path_given = false;
};
extern ConfigYamlFlags g_configYamlFlags;

/// Region override from -region or -serverregion CLI args. Empty = default.
extern CHAR g_regionOverride[64];

/// A detour hook for the game's method it uses to build CLI argument definitions.
/// Adds additional definitions to the structure, so that they may be parsed successfully without error.
UINT64 BuildCmdLineSyntaxDefinitionsHook(PVOID pGame, PVOID pArgSyntax);

#pragma once

// #960 (the PC twin of #758 and #829): the game needs its data folder, <install>\_data, to hold a level manifest.
// Without one the game starts, finds nothing, and closes with no message a player can read. boot.cpp checks the
// folder with the registration's own finder (server/installed_levels.h, FindManifestDirs), writes the log line,
// and shows the dialog this policy allows.
//
// Pure, no I/O: the check and the log line stay in boot.cpp.
//
//   * the dialog is for a client with a screen only: never a dedicated server or a headless run, where a modal box
//     would hold the process; those get the log line;
//   * the text names the folder that was searched and how to fix it, so the player can act without the log;
//   * the dialog is a courtesy: the caller opens it on its own thread and does not wait for it.

#include <string>

namespace nevr_game_data {

inline bool DialogDue(bool isServer, bool isHeadless) { return !isServer && !isHeadless; }

// The player's text. dataDir is the folder that was searched, in full.
inline std::string Text(const std::string& dataDir) {
  return "Echo VR game data is missing or incomplete: no level manifest was found under " + dataDir +
         ". Open the Echo Launcher, use Verify Files for Echo VR, then start the game again.";
}

}  // namespace nevr_game_data

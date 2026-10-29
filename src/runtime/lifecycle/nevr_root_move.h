// nevr_root_move.h: the one-time move of nEVR's own files into the nevr root (ADR 0012 "The one-time move", #857).
//
// Before this change nEVR wrote its run log, crash record and cosmetic packages under %LOCALAPPDATA%\EchoVR and kept the sign-in
// cache in the install's _local folder. The first run that finds those files moves them to %LOCALAPPDATA%\nevr\echovr and
// leaves MOVED-TO-NEVR-ROOT.txt in each folder it took something from (the plugins folder beside the game executable
// excepted: nothing of ours is written there), so a player or support can see why the folder is empty.
// The sign-in cache is the only file taken from _local: config.yaml keeps its resolution (ADR 0012 step 6). The sign-in cache
// is moved through the protected write it is saved with, so the moved file is as private as a saved one. The move
// never overwrites a file already in the root, is safe to run twice, leaves a file where it is (with an event saying why) when it
// cannot move it, and deletes nothing that it did not move. Run() takes every folder as an input and returns what it did, so the
// tests drive it against a temp tree; RunAndLog() is the one call the runtime makes.
#pragma once

#include <string>
#include <vector>

namespace nevr_root_move {

#ifdef NEVR_TEST_HOOKS
// For the tests: run just after the sign-in cache's temp file is written (its path), and just after the cache is in the
// root (its path). They stand in for another process acting in those windows (#1114).
inline void (*g_afterTempWrittenHook)(const std::wstring& temp) = nullptr;
inline void (*g_afterInstallHook)(const std::wstring& installed) = nullptr;
#endif

enum class Outcome {
  kMoved,           // from -> to
  kLeftRootHasIt,   // from stays: `to` already exists in the root, and the move never overwrites
  kLeftMoveFailed,  // from stays: the move failed (error); a later run tries again
  kMarkerWritten,   // `to` is the MOVED-TO-NEVR-ROOT.txt written in the folder something was moved out of
  kMarkerFailed,    // the marker could not be written (error)
  kRootUnavailable  // the root could not be created (error); nothing was moved
};

struct Event {
  Outcome outcome;
  std::wstring from;
  std::wstring to;
  unsigned long error;
};

struct Inputs {
  std::wstring root;                     // %LOCALAPPDATA%\nevr\echovr; empty: the move does nothing
  std::vector<std::wstring> local_dirs;  // the install's _local folders, nearest first (the sign-in cache's search order)
  std::wstring legacy_logs_dir;          // %LOCALAPPDATA%\EchoVR\logs
  std::wstring legacy_cosmetics_dir;     // %LOCALAPPDATA%\EchoVR\cosmetics\v1\packages
  std::wstring legacy_plugins_dir;       // the plugins\ folder next to the game executable (where plugins used to load from)
};

// True for the names nEVR's own writers give a file in the legacy logs folder: the run log (nevr-<timestamp>.jsonl, or .log with
// the plain-text format) and the crash record (nevr-crash-<run id>.txt). Nothing else in that folder is ours.
bool IsNevrLogFileName(const std::wstring& name);

std::vector<Event> Run(const Inputs& inputs);

// Inputs from %LOCALAPPDATA% and the folder of the game executable.
Inputs InputsFromEnvironment();

// Run(InputsFromEnvironment()), one Log() line per event and one summary line naming the root. Boot phase safe (Log() routes
// through the boot tee there).
void RunAndLog();

}  // namespace nevr_root_move

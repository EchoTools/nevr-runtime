// nevr_root_move.h: the one-time move of NEVR's own files into the nevr root (ADR 0012 "The one-time move", #857).
//
// Before this change NEVR read config.yaml from the install's _local folders and wrote its run log, crash record and cosmetic
// packages under %LOCALAPPDATA%\EchoVR. The first run that finds those files moves them to %LOCALAPPDATA%\nevr\echovr and leaves
// MOVED-TO-NEVR-ROOT.txt in each folder it took something from, so a player or support can see why the folder is empty. The move
// never overwrites a file already in the root, is safe to run twice, leaves a file where it is (with an event saying why) when it
// cannot move it, and deletes nothing that it did not move. Run() takes every folder as an input and returns what it did, so the
// tests drive it against a temp tree; RunAndLog() is the one call the runtime makes.
#pragma once

#include <string>
#include <vector>

namespace nevr_root_move {

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
  std::vector<std::wstring> local_dirs;  // the install's _local folders, nearest first (the old config.yaml search order)
  std::wstring legacy_logs_dir;          // %LOCALAPPDATA%\EchoVR\logs
  std::wstring legacy_cosmetics_dir;     // %LOCALAPPDATA%\EchoVR\cosmetics\v1\packages
};

// True for the names NEVR's own writers give a file in the legacy logs folder: the run log (nevr-<timestamp>.jsonl, or .log with
// the plain-text format) and the crash record (nevr-crash-<run id>.txt). Nothing else in that folder is ours.
bool IsNevrLogFileName(const std::wstring& name);

std::vector<Event> Run(const Inputs& inputs);

// Inputs from %LOCALAPPDATA% and the folder of the game executable.
Inputs InputsFromEnvironment();

// Run(InputsFromEnvironment()), one Log() line per event and one summary line naming the root. Boot phase safe (Log() routes
// through the boot tee there).
void RunAndLog();

}  // namespace nevr_root_move

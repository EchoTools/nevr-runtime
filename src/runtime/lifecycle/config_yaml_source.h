// config_yaml_source.h: the file-system half of the config.yaml choice (#1255; the decision is config_yaml_choice.h).
//
// Reads the nevr root and the command-line values, probes the files, copies a legacy `_local\config.yaml` into the root
// when the root has none, and says what it did in the log. It writes only inside the nevr root: the legacy file is never
// modified, renamed or deleted, and nothing is created in the game folder. Paths are UTF-8; every Win32 call is the wide one.
#pragma once

#include <string>

#include "runtime/lifecycle/config_yaml_choice.h"

namespace nevr_config_yaml {

/// The Win32 file probes: GetFileAttributesW, and CreateFileW for reading.
Probe RealProbe();

/// The whole of `path` through CreateFileW (so a non-ASCII path reads). False with the Win32 error when it cannot be read.
bool ReadWholeFile(const std::string& path, std::string* bytes, unsigned long* error);

/// Copies `from` to `to` (both UTF-8) with CopyFileW and fail-if-exists, creating the folder of `to` first. False with the
/// Win32 error when it did not copy (an existing `to` is ERROR_FILE_EXISTS and is left as it is). `from` is only read.
bool CopyIfAbsent(const std::string& from, const std::string& to, unsigned long* error);

/// The decision, the copy it calls for and the log lines for the copy and for an ignored legacy file. On a copy the
/// returned choice names the root's file (unless `-config-file` chose one) as chosen by the nevr root; a failed copy is a
/// Warning and the run continues with no file from the root.
Choice Resolve(const Inputs& inputs, const Probe& probe);

/// Fills the places of this process into `inputs`: the nevr root under %LOCALAPPDATA% and the three legacy paths beside
/// `game_module_dir` (the folder of the game module, ending in a separator). The command-line fields are the caller's.
void AddProcessPaths(const std::string& game_module_dir, Inputs* inputs);

}  // namespace nevr_config_yaml

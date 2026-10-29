// config_yaml_choice.h: which config.yaml a run reads (#1255), as a pure decision.
//
// Nothing of ours lives in the game folder, so config.yaml is read from `-config-file <file>` (the exact file) or else from
// <nevr root>\config.yaml (core/nevr_root.h). Nothing else is searched. The three `_local\config.yaml` paths beside the game
// module (the places an older build looked in) are never read: when the root has none, the first of them that exists is
// copied into the root once (config_yaml_source.h); when the root has one, they are ignored.
//
// The decision takes the file system as two injected callbacks, so a test drives every case without a file. Paths are UTF-8.
// Header-only: the same text builds in the test targets and in nevr-runtime.dll.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/nevr_root.h"

namespace nevr_config_yaml {

/// How the file of a run was chosen. The names are what the "config.yaml loaded from" line prints (`chosen_by=`).
enum class ChosenBy { kNone, kConfigFile, kNevrRoot };

inline const char* ChosenByName(ChosenBy by) {
  switch (by) {
    case ChosenBy::kConfigFile: return "-config-file";
    case ChosenBy::kNevrRoot: return "nevr root";
    case ChosenBy::kNone: break;
  }
  return "none";
}

/// A command line the run is refused for (the process ends with exit code 64, cli_reject.h).
enum class Rejection { kNone, kConfigPathFlag, kConfigFileNoValue, kConfigFileMissing, kConfigFileUnreadable };

struct Inputs {
  bool config_path_flag = false;  ///< `-config-path` was on the command line, with any value
  bool config_file_flag = false;  ///< `-config-file` was on the command line
  std::string config_file;        ///< its value; empty when the flag had none
  std::string root;               ///< the nevr root folder; empty when %LOCALAPPDATA% is unset
  std::vector<std::string> legacy;  ///< the old `_local\config.yaml` candidates, in the order they were searched
};

struct Probe {
  std::function<bool(const std::string&)> exists;    ///< something is at the path
  std::function<bool(const std::string&)> readable;  ///< the file at the path can be opened for reading
};

struct Choice {
  Rejection rejection = Rejection::kNone;
  std::string rejected_path;   ///< the `-config-file` value a rejection is about
  std::string path;            ///< the file to read; empty when there is none
  ChosenBy chosen_by = ChosenBy::kNone;
  std::string root_path;       ///< <root>\config.yaml; empty without a root. The one place searched.
  std::string copy_from;       ///< the legacy file to copy into the root (the root has none); empty when no copy is due
  std::string ignored_legacy;  ///< the legacy file left unread because the root has one; empty when there is none
};

/// The argument and the reason a rejection reports; the stop line's detail is "<argument>: <why>".
struct RejectionText {
  std::string argument;
  std::string why;
};

/// The three places an older build looked for config.yaml, from the game module's folder (ending in a separator): `_local` in
/// that folder, then one and then two folders up. In that order.
inline std::vector<std::string> LegacyCandidates(const std::string& module_dir) {
  const std::string name = nevr_root::Names<char>::kConfigYaml;
  return {module_dir + "_local\\" + name, module_dir + "..\\_local\\" + name, module_dir + "..\\..\\_local\\" + name};
}

/// The decision. `-config-path` and a `-config-file` that names nothing usable are rejections, and nothing is chosen after one.
/// Otherwise the file is the `-config-file` value, else the root's config.yaml when it exists. A legacy file is never the file:
/// the first that exists is the one to copy into a root that has none (`copy_from`), or the one ignored beside a root that has one.
inline Choice Choose(const Inputs& inputs, const Probe& probe) {
  Choice choice;
  if (inputs.config_path_flag) {
    choice.rejection = Rejection::kConfigPathFlag;
    return choice;
  }
  if (inputs.config_file_flag) {
    if (inputs.config_file.empty()) {
      choice.rejection = Rejection::kConfigFileNoValue;
      return choice;
    }
    choice.rejected_path = inputs.config_file;
    if (!probe.exists(inputs.config_file)) {
      choice.rejection = Rejection::kConfigFileMissing;
      return choice;
    }
    if (!probe.readable(inputs.config_file)) {
      choice.rejection = Rejection::kConfigFileUnreadable;
      return choice;
    }
    choice.rejected_path.clear();
    choice.path = inputs.config_file;
    choice.chosen_by = ChosenBy::kConfigFile;
  }

  choice.root_path = nevr_root::ConfigYamlPath(inputs.root);
  const bool root_has = !choice.root_path.empty() && probe.exists(choice.root_path);
  std::string legacy;
  for (const std::string& candidate : inputs.legacy) {
    if (probe.exists(candidate)) {
      legacy = candidate;
      break;
    }
  }
  if (root_has) {
    if (choice.chosen_by == ChosenBy::kNone) {
      choice.path = choice.root_path;
      choice.chosen_by = ChosenBy::kNevrRoot;
    }
    choice.ignored_legacy = legacy;
  } else if (!choice.root_path.empty()) {
    choice.copy_from = legacy;
  }
  return choice;
}

inline RejectionText DescribeRejection(const Choice& choice) {
  switch (choice.rejection) {
    case Rejection::kConfigPathFlag: return {"-config-path", "use -config-file <file>"};
    case Rejection::kConfigFileNoValue: return {"-config-file", "needs a file: -config-file <file>"};
    case Rejection::kConfigFileMissing: return {"-config-file", choice.rejected_path + ": file does not exist"};
    case Rejection::kConfigFileUnreadable:
      return {"-config-file", choice.rejected_path + ": file cannot be opened for reading"};
    case Rejection::kNone: break;
  }
  return RejectionText();
}

}  // namespace nevr_config_yaml

// plugin_tree.h — a plugin and every DLL it names from its own directory must pass the same signature check
// (ADR 0010 section 1, #741). Pure of the loader: it takes the plugins directory, the open plugin file and a
// policy, so a test calls it with signed fixtures and no game.
//
// A plugin's dependencies reach the process three ways that never go through StageOne: the import table, the
// delay-load import table (resolved on first call) and forwarded exports. N75 restricts their search to the
// plugin's own directory and System32, so a name that matches a file in the plugins directory is a DLL the
// loader will map. System32's are the OS's; a name with no file in the directory is not ours to check.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/ext/plugin_signature.h"

namespace nevr_plugin_tree {

// The dependency files opened for the check, held with FILE_SHARE_READ only (no writer, no delete) until this
// object goes away, so the bytes that were verified are the bytes that load.
class HeldFiles {
 public:
  HeldFiles() = default;
  HeldFiles(const HeldFiles&) = delete;
  HeldFiles& operator=(const HeldFiles&) = delete;
  ~HeldFiles();
  void Add(HANDLE handle);
  size_t size() const { return handles_.size(); }

 private:
  std::vector<HANDLE> handles_;
};

struct TreeVerdict {
  bool accepted = true;
  std::string file;  // when refused: the file that failed, a dependency's name or the plugin's own
  std::string via;   // "import", "delay-load", "forwarder" or "" for the plugin itself
  nevr_plugin_signature::Verdict verdict;

  // "<file> (<via>): <reason>" for the refusal line.
  std::string Describe() const;
};

// Checks every file reachable from `root` (the plugin, already open) that exists in `pluginDir` (a directory with a
// trailing backslash), transitively, each with VerifyPluginFile. The first failure ends the walk. At most 64
// files; a longer tree is refused. Dependencies that pass stay open in `held`.
TreeVerdict VerifyPluginTree(const std::string& pluginDir, HANDLE root, const nevr_plugin_signature::Policy& policy,
                             int64_t nowUnix, HeldFiles* held);

}  // namespace nevr_plugin_tree

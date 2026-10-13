// Loads script files in `plugins:` order: reads each manifest without running
// the script, opens its owner, limits it to its declaration, and runs it in the
// script VM. In dev builds it reloads a script whose file changed, keeping its
// place in the order.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scripting/host_registry.h"
#include "scripting/script_manifest.h"
#include "scripting/script_vm.h"

namespace nevr_script {

// What a policy engine (or a launcher) reads before anything runs.
struct ManifestCheck {
  std::string path;
  bool ok = false;
  ScriptManifest manifest;  // valid when ok
  std::string error;        // "<file>:<line>: manifest: <why>" when not ok
};

// Reads every file's manifest; runs nothing. One entry per path, in order.
std::vector<ManifestCheck> ReadManifests(const std::vector<std::string>& paths);

class ScriptHost {
 public:
  ScriptHost(Registry& registry, ScriptVm& vm, bool dev_reload);

  // Loads the scripts in order. A script is refused (one script_refused record
  // with the reason) when its file can't be read, its manifest is invalid, it
  // needs a newer host API, its name repeats an earlier script's, or its top
  // level fails; the others still load. Returns how many loaded.
  int LoadAll(const std::vector<std::string>& paths);

  // Dev builds: reloads each script whose file changed since it was last read.
  // A new version whose manifest is invalid or renames the script is not
  // loaded and the running version stays (reload_failed). A new version whose
  // top level fails leaves the script with no overrides or callbacks until the
  // file changes again (reload_failed). Returns how many reloaded. Does nothing
  // unless constructed with dev_reload.
  int PollReload();

  // The loaded scripts' manifests as a JSON array, in load order.
  std::string LoadedManifestsJson() const;

 private:
  struct Loaded {
    std::string path;
    std::string chunkname;
    NevrOwner* owner;
    ScriptManifest manifest;
    int64_t mtime;
    uintmax_t size;
  };

  bool LoadOne(const std::string& path);
  bool Reload(Loaded& script);

  Registry& registry_;
  ScriptVm& vm_;
  bool dev_reload_;
  std::vector<Loaded> loaded_;
};

}  // namespace nevr_script

#include "scripting/script_host.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

namespace nevr_script {
namespace {

namespace fs = std::filesystem;

bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !in.bad();
}

// The file's stamp; {-1, 0} when it can't be read.
std::pair<int64_t, uintmax_t> Stamp(const std::string& path) {
  std::error_code ec;
  const auto time = fs::last_write_time(path, ec);
  if (ec) return {-1, 0};
  const uintmax_t size = fs::file_size(path, ec);
  if (ec) return {-1, 0};
  return {static_cast<int64_t>(time.time_since_epoch().count()), size};
}

std::string Chunkname(const std::string& path) { return fs::path(path).filename().string(); }

}  // namespace

std::vector<ManifestCheck> ReadManifests(const std::vector<std::string>& paths) {
  std::vector<ManifestCheck> checks;
  for (const std::string& path : paths) {
    ManifestCheck check;
    check.path = path;
    std::string source;
    if (!ReadFile(path, &source)) {
      check.error = Chunkname(path) + ": cannot read " + path;
    } else {
      check.ok = ParseScriptManifest(Chunkname(path), source, &check.manifest, &check.error);
    }
    checks.push_back(std::move(check));
  }
  return checks;
}

ScriptHost::ScriptHost(Registry& registry, ScriptVm& vm, bool dev_reload)
    : registry_(registry), vm_(vm), dev_reload_(dev_reload) {}

int ScriptHost::LoadAll(const std::vector<std::string>& paths) {
  int count = 0;
  for (const std::string& path : paths) count += LoadOne(path) ? 1 : 0;
  return count;
}

bool ScriptHost::LoadOne(const std::string& path) {
  const std::string chunkname = Chunkname(path);
  const auto stamp = Stamp(path);
  std::string source;
  if (!ReadFile(path, &source)) {
    registry_.Record(NEVR_LOG_ERROR, "script_refused", nullptr, chunkname.c_str(), "cannot read " + path);
    return false;
  }
  ScriptManifest manifest;
  std::string error;
  if (!ParseScriptManifest(chunkname, source, &manifest, &error)) {
    registry_.Record(NEVR_LOG_ERROR, "script_refused", nullptr, chunkname.c_str(), error);
    return false;
  }
  if (manifest.api > NEVR_HOST_API_VERSION) {
    registry_.Record(NEVR_LOG_ERROR, "script_refused", nullptr, chunkname.c_str(),
                     manifest.name + " needs host API v" + std::to_string(manifest.api) +
                         "; this host has v" + std::to_string(NEVR_HOST_API_VERSION));
    return false;
  }
  for (const Loaded& other : loaded_) {
    if (other.manifest.name == manifest.name) {
      registry_.Record(NEVR_LOG_ERROR, "script_refused", nullptr, chunkname.c_str(),
                       "name " + manifest.name + " is already used by " + other.chunkname);
      return false;
    }
  }
  NevrOwner* owner = registry_.OpenOwner(manifest.name);
  registry_.Declare(owner, manifest.declaration);
  if (!vm_.Load(owner, chunkname, source, &error)) {
    vm_.Unload(owner);
    registry_.DisableOwner(owner, "its top level failed: " + error);
    registry_.Record(NEVR_LOG_ERROR, "script_refused", owner, chunkname.c_str(), error);
    return false;
  }
  registry_.Record(NEVR_LOG_INFO, "script_loaded", owner, chunkname.c_str(),
                   ScriptManifestJson(manifest));
  loaded_.push_back({path, chunkname, owner, std::move(manifest), stamp.first, stamp.second});
  return true;
}

int ScriptHost::PollReload() {
  if (!dev_reload_) return 0;
  int count = 0;
  for (Loaded& script : loaded_) {
    const auto stamp = Stamp(script.path);
    if (stamp.first == script.mtime && stamp.second == script.size) continue;
    script.mtime = stamp.first;  // tried once per change, whatever the outcome
    script.size = stamp.second;
    count += Reload(script) ? 1 : 0;
  }
  return count;
}

bool ScriptHost::Reload(Loaded& script) {
  std::string source;
  if (!ReadFile(script.path, &source)) {
    registry_.Record(NEVR_LOG_WARNING, "reload_failed", script.owner, script.chunkname.c_str(),
                     "cannot read " + script.path + "; the running version stays");
    return false;
  }
  ScriptManifest manifest;
  std::string error;
  if (!ParseScriptManifest(script.chunkname, source, &manifest, &error)) {
    registry_.Record(NEVR_LOG_WARNING, "reload_failed", script.owner, script.chunkname.c_str(),
                     error + "; the running version stays");
    return false;
  }
  if (manifest.name != script.manifest.name) {
    registry_.Record(NEVR_LOG_WARNING, "reload_failed", script.owner, script.chunkname.c_str(),
                     "the manifest renames " + script.manifest.name + " to " + manifest.name +
                         "; a rename needs a restart; the running version stays");
    return false;
  }
  vm_.Unload(script.owner);
  registry_.ResetOwner(script.owner);
  registry_.Declare(script.owner, manifest.declaration);
  script.manifest = std::move(manifest);
  if (!vm_.Load(script.owner, script.chunkname, source, &error)) {
    vm_.Unload(script.owner);
    registry_.ResetOwner(script.owner);
    registry_.Record(NEVR_LOG_ERROR, "reload_failed", script.owner, script.chunkname.c_str(),
                     error + "; the script is inert until its file changes");
    return false;
  }
  registry_.Record(NEVR_LOG_INFO, "script_reloaded", script.owner, script.chunkname.c_str(),
                   ScriptManifestJson(script.manifest));
  return true;
}

std::string ScriptHost::LoadedManifestsJson() const {
  nlohmann::json list = nlohmann::json::array();
  for (const Loaded& script : loaded_) {
    list.push_back(nlohmann::json::parse(ScriptManifestJson(script.manifest), nullptr, false));
  }
  return list.dump();
}

}  // namespace nevr_script

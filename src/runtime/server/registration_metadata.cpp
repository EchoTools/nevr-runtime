#include "runtime/server/registration_metadata.h"

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "core/logging.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/patch/asset_registry.h"
#include "runtime/server/installed_levels.h"

namespace nevr_game_server {
namespace {

// Long enough for a Windows extended-length path; GetModuleFileNameW reports truncation by returning the size.
constexpr DWORD kMaxModulePathChars = 32768;

std::filesystem::path GameExePath() {
  std::vector<wchar_t> buffer(kMaxModulePathChars);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), kMaxModulePathChars);
  if (length == 0 || length >= kMaxModulePathChars) return {};
  return std::filesystem::path(std::wstring(buffer.data(), length));
}

std::vector<std::string> ConfigCsvList(const char* flatKey) {
  const char* value = NevrCfgGetFlatCsv(flatKey);
  return value != nullptr ? SplitCsvList(value) : std::vector<std::string>{};
}

}  // namespace

void FillRegistrationMetadata(RegistrationParams& params) {
  const std::filesystem::path exe = GameExePath();
  const std::filesystem::path manifestDir = exe.empty() ? std::filesystem::path() : ManifestDirFromExe(exe);
  InstalledLevels levels;
  if (exe.empty()) {
    levels.problems.push_back("the game's exe path is unavailable");
  } else {
    levels = CollectInstalledLevels(manifestDir, BootPackageSets());
  }
  for (const std::string& problem : levels.problems) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] level manifest not read dir=%s %s", manifestDir.string().c_str(),
        problem.c_str());
  }
  std::vector<std::pair<uint64_t, uint64_t>> registered;
  for (const nevr::assets::Asset& asset : nevr::assets::Global().Snapshot()) registered.emplace_back(asset.type, asset.name);
  AddRegisteredLevels(registered, &levels);
  params.installedLevels = levels.levels;
  params.regions = ConfigCsvList("nevr_regions");
  params.guilds = ConfigCsvList("nevr_guilds");
  Log(EchoVR::LogLevel::Info,
      "[NEVR.GAMESERVER] registration metadata levels=%zu manifests_read=%zu plugin_levels=%zu regions=%zu guilds=%zu",
      params.installedLevels.size(), levels.manifestsRead, levels.pluginLevels, params.regions.size(),
      params.guilds.size());
}

}  // namespace nevr_game_server

#include "runtime/server/registration_metadata.h"

#include <windows.h>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>
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

// The config value for a list key, or "" when unset. The ServerDB parse is applied by the caller.
std::string ConfigCsv(const char* flatKey) {
  const char* value = NevrCfgGetFlatCsv(flatKey);
  return value != nullptr ? std::string(value) : std::string();
}

}  // namespace

// Nothing here may stop the registration from going out. The values are computed first and assigned only
// when every step has finished, so a failure leaves the caller's fields empty and the registration still
// goes out with what the caller set.
void FillRegistrationMetadata(RegistrationParams& params) {
  try {
    const std::filesystem::path exe = GameExePath();
    if (exe.empty()) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.GAMESERVER] level manifests: the game's exe path is unavailable; no installed levels reported");
    }
    const std::vector<std::filesystem::path> dirs = exe.empty() ? std::vector<std::filesystem::path>{}
                                                                : FindManifestDirs(exe);
    if (!exe.empty() && dirs.empty()) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] level manifests: no data dir found under %s",
          (InstallRootFromExe(exe) / "_data").string().c_str());
    }

    InstalledLevels levels = CollectInstalledLevelsFromDirs(dirs, BootPackageSets());
    for (const std::string& problem : levels.problems) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] level manifest reader failed: %s", problem.c_str());
    }

    std::vector<std::pair<uint64_t, uint64_t>> registered;
    for (const nevr::assets::Asset& asset : nevr::assets::Global().Snapshot()) {
      registered.emplace_back(asset.type, asset.name);
    }
    AddRegisteredLevels(registered, &levels);
    if (levels.levels.empty()) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] level manifests: 0 levels from %zu manifest(s) read",
          levels.manifestsRead);
    }

    std::size_t rejectedRegions = 0;
    std::size_t rejectedGuilds = 0;
    std::vector<std::string> regions =
        ParseServerDbList(ConfigCsv("nevr_regions"), ListItemKind::kRegion, &rejectedRegions);
    std::vector<std::string> guilds = ParseServerDbList(ConfigCsv("nevr_guilds"), ListItemKind::kGuild, &rejectedGuilds);
    if (rejectedRegions > 0 || rejectedGuilds > 0) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.GAMESERVER] registration list items rejected by the ServerDB pattern regions=%zu guilds=%zu",
          rejectedRegions, rejectedGuilds);
    }

    params.installedLevels = std::move(levels.levels);
    params.regions = std::move(regions);
    params.guilds = std::move(guilds);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.GAMESERVER] registration metadata levels=%zu manifests_read=%zu plugin_levels=%zu regions=%zu guilds=%zu",
        params.installedLevels.size(), levels.manifestsRead, levels.pluginLevels, params.regions.size(),
        params.guilds.size());
  } catch (const std::exception& error) {
    Log(EchoVR::LogLevel::Error,
        "[NEVR.GAMESERVER] registration metadata failed (%s); registering without installed levels, regions or guilds",
        error.what());
  }
}

}  // namespace nevr_game_server

#include <dlfcn.h>

#include <memory>
#include <string>

#include "core/map_derive.h"
#include "quest/cargo/map_service.h"
#include "quest/cargo/posix_env.h"
#include "quest/cargo/posix_map_fs.h"
#include "quest/cargo/stock_source.h"
#include "quest/data/curl_transport.h"
#include "quest/data/pack.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/obb_skip_hook.h"

// The production entry of the custom-map channel, kept apart from map_service.cpp (like cargo_start.cpp) so the host tests of the
// service need no libcurl and no sentinel hooks.
namespace nevr_quest::maps {

namespace {

// The production objects. Never freed: the worker thread and the mounted sets use them for the life of the process. Built only when
// the channel runs, so a run without maps.base_url constructs none of them.
struct Production {
  nevr_quest::data::CurlTransport transport;
  nevr_quest::cargo::PosixEnv env;
  PosixMapFs fs;
  std::unique_ptr<nevr_maps::StockSource> stock;
  std::unique_ptr<nevr_maps::ChannelDeriver> deriver;
  Production() : env(&transport) {}
};

// The path this library was loaded from: `<app>/lib/<abi>/libovrplatformloader.so`, or `<apk>!/lib/<abi>/...` when the
// installer kept it inside the APK.
std::string OwnLibraryPath() {
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&OwnLibraryPath), &info) == 0 || info.dli_fname == nullptr) return "";
  return info.dli_fname;
}

// The core's view of the maps section: the flat keys the core reads (service_map.cpp names them), answered from the Quest config.
nevr_map_channel::ScalarLookup ScalarsOf(const MapsConfig& config) {
  return [config](const char* key) -> std::string {
    if (std::string(key) == "maps_base_url") return config.baseUrl;
    if (std::string(key) == "maps_channel") return config.channel;
    if (std::string(key) == "maps_boot_budget_ms") return config.bootBudgetMs == 0 ? std::string() : std::to_string(config.bootBudgetMs);
    return std::string();
  };
}

nevr_map_channel::ListLookup ListsOf(const MapsConfig& config) {
  return [config](const char* key) -> std::vector<std::string> {
    if (std::string(key) == "maps_official_keys") return config.officialKeys;
    if (std::string(key) == "maps_trusted_keys") return config.trustedKeys;
    return {};
  };
}

}  // namespace

bool StartFromConfig(const MapsConfig& config) noexcept {
  const nevr_map_channel::Settings settings =
      nevr_map_channel::ReadSettings(ScalarsOf(config), ListsOf(config), nevr_map_channel::CompiledOfficialKeys());
  if (settings.reason == "not_configured") {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "map_start", {{"reason", "not_configured"}});
    return false;
  }
  if (nevr_map_channel::DecideStart(settings) == nevr_map_channel::Start::kRefused) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "map_start",
                        {{"reason", settings.reason.c_str()},
                         {"detail", settings.detail.c_str()},
                         {"consequence", "no map is mounted or fetched this run"}});
    return false;
  }
  // Never freed once made: Start may keep a reference to its environment for the life of the process, even when it refuses.
  Production* const production = new Production();
  const std::string library = OwnLibraryPath();
  const std::size_t slash = library.rfind('/');
  const std::string lib_dir =
      library.find("!/") == std::string::npos && slash != std::string::npos ? library.substr(0, slash) : std::string();
  production->stock = MakePosixStockSource(&obb_skip::CurrentRoot, lib_dir,
                                           nevr_quest::data::ApkPathForLibrary(library, &obb_skip::PathExists));
  production->deriver = std::make_unique<nevr_maps::ChannelDeriver>(production->env, production->fs, *production->stock);
  return Start(&production->env, settings, production->deriver.get(), nevr_map_channel::NoSeams());
}

}  // namespace nevr_quest::maps

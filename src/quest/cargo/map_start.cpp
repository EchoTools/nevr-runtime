#include <dlfcn.h>

#include <memory>
#include <string>

#include "core/map_derive.h"
#include "quest/cargo/map_pin_game.h"
#include "quest/cargo/map_service.h"
#include "quest/cargo/posix_env.h"
#include "quest/cargo/posix_map_fs.h"
#include "quest/cargo/stock_source_posix.h"
#include "quest/data/curl_transport.h"
#include "quest/data/pack.h"
#include "quest/sentinel/config_alias_hook.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/obb_skip_hook.h"

// The production entry of the custom-map channel, kept apart from map_service.cpp (like cargo_start.cpp) so the host tests of the
// service need no libcurl and no sentinel hooks.
namespace nevr_quest::maps {

namespace {

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

}  // namespace

bool StartFromConfig(const MapsConfig& config) noexcept {
  if (config.source != Source::kFile) return false;
  MapsChoice choice;
  std::string bad_key;
  if (!ChoiceFromConfig(config, &choice, &bad_key)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "maps_start",
                        {{"result", "off"},
                         {"reason", "bad_key"},
                         {"key", bad_key.c_str()},
                         {"consequence", "no downloaded map is served this run"}});
    return false;
  }
  // Never freed once started: the service's thread and the deriver use it for the life of the process.
  Production* const production = new Production();
  const std::string library = OwnLibraryPath();
  const std::size_t slash = library.rfind('/');
  const std::string lib_dir =
      library.find("!/") == std::string::npos && slash != std::string::npos ? library.substr(0, slash) : std::string();
  production->stock = MakePosixStockSource(&obb_skip::CurrentRoot, lib_dir,
                                           nevr_quest::data::ApkPathForLibrary(library, &obb_skip::PathExists));
  production->deriver = std::make_unique<nevr_maps::ChannelDeriver>(production->env, production->fs, *production->stock);
  if (Start(&production->env, choice, production->deriver.get())) {
    if (pin::InstallTick()) config_alias_hook::SetMainLevelSource(&pin::MainLevelNow);
    return true;
  }
  delete production;  // Start keeps no reference when it refuses
  return false;
}

}  // namespace nevr_quest::maps

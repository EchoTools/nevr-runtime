/* The custom-map channel on the Quest (CDN_DESIGN §10.4 Q2): PC5a's core (core/map_run.h) for `android-arm64`, over the POSIX
 * cache under <internalDataPath>/nevr/cache/maps/, mounting each map's set through map_sets.h (Q1).
 *
 * At start, on the calling thread: BootMount from the cache (no network) registers the cached maps' sets. Then one detached
 * thread waits for the game's manifest (the resource request hook sees the first request), mounts the sets against it, and
 * runs FetchAll, whose downloads mount at the next launch. The Quest is never a server. A map's scripts (route B, Q3) and aliases
 * (Q5) are granted with its set, all or nothing (map_mount.h). Recipes rebuild through the Deriver Start is given (PC10's core over
 * stock_source_posix.h); with none, a map with recipes is `derived_unsupported`. The boot view and the mounted maps feed the session
 * pin check (map_pin.h).
 *
 * With no subscribed map nothing starts and nothing is logged beyond one `maps_start` line: the game runs exactly as before.
 *
 * Built without exceptions, like the cargo service.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/cargo_env.h"
#include "core/map_run.h"
#include "core/resource_set_registry.h"
#include "quest/cargo/map_mount.h"
#include "quest/cargo/map_pin.h"
#include "core/resource_shadow.h"
#include "quest/sentinel/quest_config.h"

namespace nevr_quest::maps {

inline constexpr const char* kDefaultCacheDir = "/data/data/com.readyatdawn.r15/files/nevr/cache/maps";
// How long the mount waits for the game's first resource request before it gives the run up (map_sets::Abandon).
inline constexpr std::uint32_t kManifestWaitMs = 120000;

// The maps settings, as the Quest config gives them (the parse is the integration step's; an empty field is the core's default).
struct MapsChoice {
  std::string base_url;
  std::string channel;
  std::string game_build;
  std::vector<std::string> subscribed;
  std::vector<std::string> stock_manifest_sha256;
  std::vector<nevr_maps::PinnedKey> keys;
  bool signed_only = false;
  std::uint64_t cap_bytes = 0;  // 0: the core's default
};

nevr_maps::ChannelConfig MakeChannelConfig(const MapsChoice& choice, const std::string& cache_dir = kDefaultCacheDir);

// The UTC time the core compares expiries with: "YYYY-MM-DDTHH:MM:SSZ".
std::string NowUtc();

// Waits (polling, at most `wait_ms`, while `keep_going`) until `records` reports a built manifest, then mounts the registered sets
// against `has` and publishes them with `publish`. On a timeout the run's registration is abandoned. True when it mounted.
bool MountWhenManifestReady(std::uint64_t (*records)(), bool (*has)(std::uint64_t type, std::uint64_t name),
                            void (*publish)(const nevr_shadow::Shadow*), std::uint32_t wait_ms,
                            const std::function<bool()>& keep_going);

// The boot mount, then the thread that mounts and fetches. `env` (the POSIX env over the app's transport) and `deriver` (null: no
// recipe rebuild) outlive the process. False when it was started before, or when nothing is subscribed (one `maps_start` line
// either way).
bool Start(nevr_cargo::Env* env, const MapsChoice& choice, nevr_maps::Deriver* deriver = nullptr) noexcept;

// The pins of the staged maps whose sets `report` did not refuse (owners `map:<id>`).
std::vector<pin::Pin> MountedPins(const std::vector<pin::Pin>& staged, const nevr_resource_sets::MountReport& report);

// The choice from the config's maps keys. False, with the config key at fault in *bad_key, when a trusted key does not parse.
bool ChoiceFromConfig(const MapsConfig& config, MapsChoice* out, std::string* bad_key);

// Production (map_start.cpp; production_steps.cpp calls it beside the cargo start): nothing at all when the config gives no
// maps keys; otherwise the libcurl transport, the POSIX env, the stock source over the game's data root and the APK, and Start.
bool StartFromConfig(const MapsConfig& config) noexcept;

}  // namespace nevr_quest::maps

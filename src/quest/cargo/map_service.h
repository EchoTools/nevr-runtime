/* The custom-map channel on the Quest (docs/adr/0048-custom-maps-channel.md; ADR 0045 decision 7): the core's start (core/map_channel.h)
 * over the POSIX cache under <internalDataPath>/nevr/cache/maps/, mounting each cached group's maps at boot with no network, then one
 * detached thread that waits for the game's manifest (the resource request hook sees the first request), mounts the sets against it,
 * and fetches the planned groups in the background. What arrives mounts at the next launch.
 *
 * The map scripts (route B) and aliases are granted with their set, all or nothing (map_mount.h). Recipes rebuild through the Deriver
 * the caller gives (the stock source over the game's data root and the APK, map_start.cpp); with none, a recipe map is refused
 * derived_unsupported. The session pin's wiring and the group recording are not here (a later part of the channel).
 *
 * Built without exceptions, like the cargo service.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "core/cargo_env.h"
#include "core/map_channel.h"
#include "core/map_run.h"
#include "core/resource_shadow.h"
#include "quest/cargo/map_mount.h"
#include "quest/sentinel/quest_config.h"

namespace nevr_quest::maps {

inline constexpr const char* kDefaultCacheDir = "/data/data/com.readyatdawn.r15/files/nevr/cache/maps";
// How long the mount waits for the game's first resource request before it gives the run up (map_sets::Abandon).
inline constexpr std::uint32_t kManifestWaitMs = 120000;

// The channel's config for the Quest: the cache directory and the platform; the rest comes from the core's settings.
nevr_maps::ChannelConfig MakeChannelConfig(const std::string& cache_dir = kDefaultCacheDir);

// The UTC time the core compares expiries with: "YYYY-MM-DDTHH:MM:SSZ".
std::string NowUtc();

// Waits (polling, at most `wait_ms`, while `keep_going`) until `records` reports a built manifest, then mounts the registered sets
// against `has` and publishes them with `publish`. On a timeout the run's registration is abandoned. True when it mounted.
bool MountWhenManifestReady(std::uint64_t (*records)(), bool (*has)(std::uint64_t type, std::uint64_t name),
                            void (*publish)(const nevr_shadow::Shadow*), std::uint32_t wait_ms,
                            const std::function<bool()>& keep_going);

// The boot mount on the calling thread, then the thread that mounts and fetches. `env` (the POSIX env over the app's transport) and
// `deriver` (null: no recipe rebuild) outlive the process. False when the channel does not run (its refusal or not_configured is
// logged by the core) or when it was started before.
bool Start(nevr_cargo::Env* env, const nevr_map_channel::Settings& settings, nevr_maps::Deriver* deriver,
           const nevr_map_channel::Seams& seams) noexcept;

// Production (map_start.cpp; production_steps.cpp calls it beside the cargo start): the core's settings from the Quest config's maps
// section, then, only when the channel runs, the libcurl transport, the POSIX env, the stock source over the game's data root and the
// APK, and Start. With no maps.base_url it writes one Info record (the core's reason) and makes no directory, thread or hook.
bool StartFromConfig(const MapsConfig& config) noexcept;

}  // namespace nevr_quest::maps

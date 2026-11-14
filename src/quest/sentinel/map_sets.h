/* A downloaded map's resource set on the Quest (docs/adr/0048-custom-maps-channel.md decision 5): the shared registration
 * (core/resource_set_registry.h RegisterMapFile) over a held POSIX descriptor, checked by the shared registry against the Android
 * on-disk types, and mounted against the game's manifest. The mounted shadow goes to the resource request hook, which asks it after
 * cargo and before the game's own manifest (resource_request_hook.h PublishMapSets; ADR 0045 decision 2).
 *
 * What is the Quest's own here, and nothing else: the descriptor's size, its positioned reads, and the read-only mapping of the file
 * for the check, which is released before the registration returns. The descriptor is the engine's handle (quest/cargo/posix_env.h):
 * the game's reader reads the set through it, never through a mapping. With no map registered nothing here runs, and the hook serves
 * exactly what it served before.
 *
 * Built without exceptions, like the hook: a mount observer cannot throw.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/resource_set_registry.h"

namespace nevr_quest::map_sets {

using nevr_resource_sets::MapFileRequest;
using nevr_resource_sets::MountReport;
using nevr_resource_sets::Outcome;
using nevr_resource_sets::StockView;

// The whole file open read-only as `held_fd`, checked as RegisterMapFile says. On ok() this module owns the descriptor for the
// process; on a refusal the caller keeps it. After the mount: kTooLate. One `map_set` record either way. Runs off the game's thread
// when `request.rehash` is set: the re-hash reads the whole file, with no lock held.
Outcome RegisterFileResourceSet(const MapFileRequest& request, std::int64_t held_fd);

// Undoes a map's registration before the mount: its sets are dropped and their descriptors closed. Nothing after the mount.
void Withdraw(const std::string& owner);

// The mount observer (core/resource_set_registry.h MountObserver): called once, on the mounting thread, after the registry's mount
// and before the shadow is published, with no lock of this module held (it may call MountedLevelNames). Abandon calls it with an
// empty report. Null: none.
void SetMountObserver(void (*observer)(const MountReport& report));

using PublishFn = void (*)(const nevr_shadow::Shadow* shadow);

// Mounts every registered set against `stock` (stock.records must be the game's live count: refusing stock keys needs the manifest
// built), closes the refused sets' descriptors, calls the observer, then hands the shadow to `publish` when anything mounted. The
// run's one mount: a second call, or one after Abandon, mounts nothing and returns an empty report (a set registered after the mount
// is kTooLate; core/resource_set_registry.h).
MountReport Mount(const StockView& stock, PublishFn publish);

// Ends the run's registration with nothing served (the mount never came): every descriptor is closed, later registrations are
// kTooLate, and the observer sees an empty report.
void Abandon();

// How many sets are registered (before the mount) or mounted (after it).
std::size_t Held();

// The level names of the mounted sets, in registration order.
std::vector<std::uint64_t> MountedLevelNames();

// Each mounted set's owner with its level names, in registration order. Empty before the mount.
std::vector<std::pair<std::string, std::vector<std::uint64_t>>> MountedOwnerLevels();

// True when a registered set (before the mount) or a mounted set (after it) holds (type, name).
bool HoldsKey(std::uint64_t type, std::uint64_t name);

// Test support: forgets every set and the observer, closing what is held.
void ResetForTest();

}  // namespace nevr_quest::map_sets

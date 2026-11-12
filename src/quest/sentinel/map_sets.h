/* A downloaded map's resource set on the Quest (CDN_DESIGN §10.4 Q1): PC3's RegisterFileResourceSet over a held POSIX
 * descriptor, checked by the shared registry (core/resource_set_registry.h) against the Android on-disk types, and mounted
 * once against the game's manifest. The mounted shadow goes to the resource request hook, which asks it after cargo and
 * before the game's own manifest (resource_request_hook.h PublishMapSets).
 *
 * The descriptor is the engine's handle (posix_env.h): the game's reader reads the set through it. A registered set's file
 * is mapped read-only for the process and never copied. With no map registered nothing here runs, and the hook serves
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

using nevr_resource_sets::MountReport;
using nevr_resource_sets::Outcome;
using nevr_resource_sets::StockView;

// The whole file open read-only as `held_fd`. With `rehash` its SHA-256, read through the descriptor, must be `sha256`
// (64 lowercase hex), else kHash. The file is then mapped and checked as the PC checks a map's set (Registry::AddMapFile:
// an override is kOverride, a script record kScript unless the waiver holds it). On ok() this module owns the descriptor
// and the mapping for the process; on a refusal the caller keeps the descriptor. After the mount: kTooLate. `owner` is
// "map:<id>", `label` "map:<id>@<version>", each at most 128 bytes of printable ASCII with no '"' or '\\'. Runs off the
// game's thread: a rehash reads the whole file, with no lock held.
Outcome RegisterFileResourceSet(const std::string& owner, std::int64_t held_fd, const std::string& label,
                                const std::string& sha256, bool rehash);

// Undoes a map's registration before the mount: its sets are dropped, unmapped and their descriptors closed.
void Withdraw(const std::string& owner);

// PC3 d1: called once, on the mounting thread, after the registry's mount and before the shadow is published, with no lock
// of this module held (it may call MountedLevelNames). Abandon calls it with an empty report. Null: none.
void SetMountObserver(void (*observer)(const MountReport& report));

using PublishFn = void (*)(const nevr_shadow::Shadow* shadow);

// Mounts every registered set once against `stock` (stock.records must be the game's live count: refusing stock keys
// needs the manifest built), closes and unmaps the refused sets, calls the observer, then hands the shadow to `publish`
// when anything mounted. A second call, or one after Abandon, mounts nothing and returns an empty report.
MountReport Mount(const StockView& stock, PublishFn publish);

// Ends the run's registration with nothing served (the mount never came): every set is unmapped and closed, later
// registrations are kTooLate, and the observer sees an empty report.
void Abandon();

// How many sets are registered (before the mount) or mounted (after it).
std::size_t Held();

// The level names of the mounted sets, in registration order.
std::vector<std::uint64_t> MountedLevelNames();

// Each mounted set's owner with its level names, in registration order. Empty before the mount.
std::vector<std::pair<std::string, std::vector<std::uint64_t>>> MountedOwnerLevels();

// True when a registered set (before the mount) or a mounted set (after it) holds (type, name).
bool HoldsKey(std::uint64_t type, std::uint64_t name);

// Test support: forgets every set and the observer (unmapping and closing what is held).
void ResetForTest();

}  // namespace nevr_quest::map_sets

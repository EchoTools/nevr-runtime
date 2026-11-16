/* The dedicated server's demand resolve (docs/adr/0045-cdn-texture-delivery.md decision 9; PLAN-v7 R6). When a server learns a session's
 * level and game type, it needs the objects that level and mode read and nothing else: the level's archive closure (the archive, the
 * archives it depends on, and the assets they list) plus the labels the mode takes whole. Each object comes from the store, from the
 * game's installed files, or from the CDN, in that order; a set is published only when every object is had, and a failed resolve
 * publishes nothing and saves no manifest.
 *
 * This file is the pure core (R6-a-1): the set, the order and the publish. The PC edge that calls it, and the place where the game's
 * installed files are read, are later pieces. Built without exceptions, like the other core files.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cdn/cdn_manifest.h"
#include "cdn/cdn_plan.h"
#include "cdn/cdn_run.h"

namespace nevr_cdn {

// The on-disk type of a level's archive (docs/engine/asset-system.md:53, CArchiveResource).
inline constexpr std::uint64_t kArchiveType = 0x2a41cf1c1d9e5d32ULL;
// The most a session's resolve may wait for its objects (ms). A named wait, chosen rather than measured: the same login ceiling the map
// hold cites (src/core/map_server_hold.h, kMapHoldCeilingMs).
inline constexpr std::uint64_t kServerDemandMs = 120000;

// The body of an archive, read exactly (docs/engine/asset-system.md:262-272): the dependent archive names and the {type, name} assets.
struct ArchiveBody {
  std::vector<std::uint64_t> dependents;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> assets;
};
// False when a count would pass the end of the body, or bytes remain after the final u32 0.
bool ParseArchiveBody(const std::vector<std::uint8_t>& body, ArchiveBody* out);

// The levels a game type takes (PLAN-v7 R6-a-2a, docs/adr/0045 decision 9): arena types take the arena levels and the lobbies, combat
// types the four combat maps, the combat lobby and the lobbies, social types the lobbies, and the NPE social type also the tutorial
// lobby. The order of the service's game types is the table in cdn_demand.cpp, one row per game type the service places. A name the
// table does not place has no levels. Sorted, unique.
std::vector<std::uint64_t> LevelsOfMode(std::string_view game_type);

// The labels a game type takes whole. Proposed, not measured: the tree does not say which modes need which label (open question Q-B2).
// The EchoCombat modes take the combat label, which no stock arena archive lists. Every other game type takes none.
struct ModeLabels {
  const char* game_type;
  const char* label;
};
inline constexpr ModeLabels kModeLabels[] = {
    {"echo_combat", "combat_content"},
    {"echo_combat_private", "combat_content"},
    {"echo_combat_tournament", "combat_content"},
};
std::vector<std::string> LabelsOfMode(std::string_view game_type);

// The game's own installed files, the second place a needed object is had from (the PC reads them through the stock source; none is
// supplied in R6-a-1, so a test supplies a fake one).
class InstalledSource {
 public:
  virtual ~InstalledSource() = default;
  // True when the installed bytes of the object are the manifest's: the game reads its own file and the entry is not mounted.
  virtual bool ReadVerified(const Entry& e) = 0;
  // An archive's decompressed body, when the installed files hold it verified.
  virtual bool ReadBody(const Entry& archive, std::vector<std::uint8_t>* body) = 0;
};

struct DemandInput {
  std::uint64_t level = 0;  // the level's archive name; 0 is no level
  std::string game_type;    // the mode's name, e.g. "echo_arena"
};

// The roots of a session's closure: its level, when it names one, and the levels of its mode. Sorted, unique; empty when neither
// names a level, which is a named failure ("no_mode").
std::vector<std::uint64_t> RootLevels(const DemandInput& input);

// What a session needs from the manifest: the closure's entries, the mode labels' entries, records of 0 bytes and scripts, and the labels
// that contribute at least one entry (sorted).
struct Needed {
  std::vector<Entry> entries;
  std::vector<EmptyRecord> empties;
  std::vector<ScriptEntry> scripts;
  std::vector<std::string> labels;
};

// Reads an archive's decompressed body (places 1 to 3). False when it cannot be had.
using ReadArchiveFn = std::function<bool(const Entry& archive, std::vector<std::uint8_t>* body)>;

// The closure and the mode labels over `manifest`, for the labels `policy` serves. The closure starts from every root (RootLevels).
// False, with *reason, on "no_mode" (no root),
// "archive_unavailable" (read_archive refused an archive) or "bad_archive" (a body that does not parse). `failed`, when given, names the
// archive that ended it.
bool ComputeNeeded(const Manifest& manifest, const Platform& platform, const Policy& policy, const DemandInput& input,
                   const ReadArchiveFn& read_archive, Needed* out, const char** reason, const Entry** failed = nullptr);

// The manifest the mount reads: the needed entries, empties and scripts, and the labels that contribute.
Manifest NeededManifest(const Needed& needed);

struct DemandResult {
  bool ok = false;
  const char* reason = "";
  std::size_t needed = 0;
  std::size_t from_cache = 0;
  std::size_t from_installed = 0;
  std::size_t fetched = 0;
  std::uint64_t bytes = 0;
  std::uint64_t ms = 0;
  std::string label;   // the object that ended a failed resolve
  std::string name;    // hex
  std::string sha256;
  bool published = false;
};

// One resolve (the order of R6-a-1 section 2): the manifest is fetched and not saved; the closure is computed with each archive had from
// the store, the installed files or the CDN; the assets are had in the same order, the CDN ones all-or-nothing through Prefetch; the set
// is mounted and published once, when it is complete. `installed` may be null (no place 2). The role of `config.policy` is the server's.
// Writes one cdn_demand record: Info when ok, Warning when failed.
DemandResult ResolveDemand(Env& env, const RunConfig& config, const DemandInput& input, InstalledSource* installed, const Publish& publish,
                           const std::function<bool()>& keep_going);

}  // namespace nevr_cdn

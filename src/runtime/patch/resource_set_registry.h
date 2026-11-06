#pragma once

// The resource sets plugins carry (plugin API v9, #2161; docs/reference/plugin-resources.md): what each set holds, the
// checks it passes before it is served, and the one shadow manifest (core/resource_shadow.h) the game's resolver is
// pointed at for their keys. Pure: no windows.h, no Log(), no game memory. The game's own manifest is reached through
// the StockView the caller hands to Mount, so a test drives every rule with a fake.
//
// The life of a set: Add (parsed and checked, held for its plugin), then Commit when the plugin's init returns 0, or
// Drop when it fails; then one Mount for the run, which refuses a set that names a key the game already has or a level
// whose archives (its own and every archive it depends on, through the sets and down to the game's) list a resource
// nothing serves, and builds the shadow from the rest. A set registered after the mount is refused (kTooLate). A set's
// overrides are held with it and handed to the override registry by the caller for mounted sets only.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/resource_shadow.h"

namespace nevr_resource_sets {

// On-disk types (CSymbol64(<class> + "Win10")), the keys the game's manifests use. docs/engine/asset-system.md, "Names".
inline constexpr std::uint64_t kArchiveType = 0x2a41cf1c1d9e5d32ULL;      // CArchiveResourceWin10
inline constexpr std::uint64_t kLevelRootType = 0xe8e38d7781a338a6ULL;    // CGameLevelResourceWin10
inline constexpr std::uint64_t kScriptType = 0x991421b41de58370ULL;       // CScriptResourceWin10: a DLL found by name on disk
inline constexpr std::uint64_t kTextureType = 0x4a4c32c49300b8a0ULL;      // CGTextureResourceWin10: the 256-byte descriptor
inline constexpr std::uint64_t kTextureGpuType = 0xbeac1969cb7b8861ULL;   // CGTextureResourceWin10GPU: the pixel payload

enum class Refusal {
  kNone = 0,
  kFormat,          // not a resource set (nevr/resource_set.h); `detail` names the rule and the index
  kScript,          // a CScriptResource record: the game loads a script as a DLL by name, which no set can serve
  kTexturePayload,  // a texture descriptor without its payload record (stock pairs all 12,275 of them)
  kLevelIncomplete, // a level root without its archive in the same set
  kArchive,         // a level's archive does not decompress or does not parse
  kCollides,        // a key another set holds; `detail` names the key and the plugin
  kTooLate,         // the run's sets are already mounted
  kStockKey,        // (mount) a key the game's manifests already hold
  kUnresolved,      // (mount) a level's archive lists a resource, or names an archive, that nothing serves
  kIndexSpace,      // the sets held so far and this one need more chunk indices than the shadow has
  kFileOffset,      // a chunk ends past the file offsets the game's u32 chunk record can hold
};

const char* RefusalName(Refusal refusal);

struct Outcome {
  Refusal refusal = Refusal::kNone;
  std::string detail;  // what was refused, for the log line: a rule and an index, or a key "0x<type>:0x<name>"
  bool ok() const { return refusal == Refusal::kNone; }
};

// The game's manifests, as Mount reads them: true when the game holds (type, name). `records` is the count it holds,
// 0 when its manifest is not built yet.
struct StockView {
  bool (*has)(void* ctx, std::uint64_t type, std::uint64_t name) = nullptr;
  void* ctx = nullptr;
  std::uint64_t records = 0;
};

// One archive a set carries: its name and what it lists (docs/engine/asset-system.md, "Archives"). A level is the
// archive that has a level root of the same name.
struct Archive {
  std::uint64_t name = 0;
  std::vector<std::uint64_t> dependencies;                        // dependent archive names
  std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;   // (on-disk type, name)
};

// An override a set carries: replacement bytes for a resource the game has, in the plugin's image.
struct OverrideRef {
  std::uint64_t type = 0;
  std::uint64_t name = 0;
  const unsigned char* data = nullptr;
  std::uint32_t size = 0;
};

// The largest archive record a set may carry: an archive sits in a block-cache chunk (the game's largest is 66,256
// bytes, in a chunk of 524,260), so its chunk decompresses to at most kBlockLimit and reading it allocates no more.
inline constexpr std::uint32_t kMaxArchiveBytes = 0x80000;

// A set as the registry holds it. Chunk offsets are offsets in the plugin's file; `package` is filled at the mount.
struct Set {
  std::string plugin;
  std::string label;
  std::int64_t handle = -1;  // the plugin's file, read-only, held for the process: the game's reader reads it
  std::uint64_t bytes = 0;
  std::vector<nevr_shadow::Record> records;
  std::vector<nevr_shadow::Chunk> chunks;
  std::vector<Archive> archives;        // every archive record of the set, parsed, sorted by name
  std::vector<std::uint64_t> levels;    // the names of the set's level roots, each with its archive in `archives`
  std::vector<OverrideRef> overrides;
  bool committed = false;
};

struct MountReport {
  std::size_t sets = 0;           // sets mounted
  std::size_t refused = 0;        // sets refused at the mount
  std::size_t records = 0;
  std::size_t levels = 0;
  std::uint32_t chunk_base = 0;   // the shadow's first chunk index (0 when nothing was mounted)
  std::uint64_t stock_records = 0;
  nevr_shadow::Status shadow_status = nevr_shadow::Status::kOk;
  struct Refused {
    std::string plugin;
    std::string label;
    Outcome outcome;
  };
  std::vector<Refused> refusals;
  std::vector<std::int64_t> handles_to_close;  // the files of the refused sets, which nothing will read
};

class Registry {
 public:
  // `indexSpace`: how many chunk indices the shadow can give out (from kFirstChunkIndex to kChunkIndexLimit).
  explicit Registry(std::uint64_t indexSpace = nevr_shadow::kChunkIndexLimit - nevr_shadow::kFirstChunkIndex)
      : indexSpace_(indexSpace) {}

  // Parses and checks a set of `size` bytes at `bytes` (the plugin's mapped image) whose first byte is at
  // `fileOffset` in the plugin's file, open as `handle`. On success the set is held for `plugin` until Commit or
  // Drop; on a refusal nothing is held and the caller keeps the handle.
  Outcome Add(const std::string& plugin, const std::string& label, const void* bytes, std::uint64_t size,
              std::int64_t handle, std::uint64_t fileOffset);

  // The plugin's init returned 0: its held sets are kept for the mount.
  void Commit(const std::string& plugin);
  // The plugin's init failed: its held sets are dropped; returns their handles for the caller to close.
  std::vector<std::int64_t> Drop(const std::string& plugin);

  // Mounts every committed set once. A refused set is dropped with its reason. Builds the one shadow from the rest.
  MountReport Mount(const StockView& stock);
  // Ends the run's registration without serving anything (the resolver hook could not be installed): every held set
  // is dropped, later Adds are kTooLate. Returns the sets' handles for the caller to close.
  std::vector<std::int64_t> Abandon();
  // How many sets are held, committed or not.
  std::size_t held() const { return sets_.size(); }

  bool mounted() const { return mounted_; }
  // The shadow the resolver hook uses, or null before the mount or when no set was mounted.
  const nevr_shadow::Shadow* shadow() const { return shadow_.get(); }
  // The level names of the mounted sets, in set order.
  std::vector<std::uint64_t> MountedLevels() const;
  // True when a held set (committed or not, mounted or not) holds (type, name).
  bool Holds(std::uint64_t type, std::uint64_t name) const;
  // The mounted sets, for the log and the login report.
  std::vector<const Set*> MountedSets() const;
  // Every set held, committed or not, in the order registered.
  std::vector<const Set*> HeldSets() const;
  // The set a shadow record's user tag names, or null.
  const Set* SetForUser(std::uint32_t user) const;

 private:
  std::vector<std::unique_ptr<Set>> sets_;
  std::unique_ptr<nevr_shadow::Shadow> shadow_;
  std::vector<const Set*> mountedOrder_;
  std::uint64_t indexSpace_;
  std::uint64_t heldChunks_ = 0;  // the chunks of the held sets, against the shadow's index space
  bool mounted_ = false;
};

// "0x<type>:0x<name>", as the log writes a key.
std::string KeyText(std::uint64_t type, std::uint64_t name);

// Parses an archive's body (docs/engine/asset-system.md, "Archives"): u32 D, D x u64 dependencies, u32 N, N x (u64
// type, u64 name), then the type table and a closing 0, which this does not read. False when it runs past `size`.
bool ParseArchive(const unsigned char* data, std::size_t size, Archive* out);

}  // namespace nevr_resource_sets

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

// The on-disk types a registry checks its sets against: one platform's names for the five classes above.
struct ResourceTypes {
  std::uint64_t archive;
  std::uint64_t level_root;
  std::uint64_t script;
  std::uint64_t texture;
  std::uint64_t texture_gpu;
};
inline constexpr ResourceTypes kWin10Types = {kArchiveType, kLevelRootType, kScriptType, kTextureType, kTextureGpuType};
// The Quest's: CSymbol64(<class> + "Android"). resource_set_registry.cpp checks every field of both against its class name.
inline constexpr ResourceTypes kAndroidTypes = {0x004c5273be3eab58ULL,   // CArchiveResourceAndroid
                                                0x275adc4c85e7ba74ULL,   // CGameLevelResourceAndroid
                                                0x4fa8daee4329a4fcULL,   // CScriptResourceAndroid
                                                0xe2efe7289d5985b8ULL,   // CGTextureResourceAndroid
                                                0x489bb35d53ca50e9ULL};  // CGTextureResourceAndroidGPU

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
  kSizeLimit,       // a chunk that decompresses to more than kMaxResourceBytes (so does any record in it)
  kFrameSize,       // a chunk whose zstd frame header states no content size, or one other than the chunk's
  // A map's file set (RegisterFileResourceSet, resource_sets.h) only:
  kHash,            // the file's SHA-256, read through its handle, is not the one the manifest pins
  kOverride,        // the set carries an override: a map only adds resources (docs/adr/0047, decision 5)
};

// The owner every map's file set is registered under: "map:<id>". Only AddMapFile applies the script waiver, so a
// plugin's set (Add) never gets it, whatever the plugin's id.
inline constexpr char kMapOwnerPrefix[] = "map:";
bool IsMapOwner(const std::string& owner);

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

// The largest chunk a set may carry, decompressed: ADR 0045's size rule (decision 8, 128 MiB), the bound on any buffer
// the game's reader allocates for a resource. A record lies inside its chunk's decompressed bytes (Parse), so it bounds
// every record too.
inline constexpr std::uint32_t kMaxResourceBytes = 128u * 1024u * 1024u;

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
  // `types`: the platform whose on-disk types the checks read (the PC's unless the caller names another).
  explicit Registry(std::uint64_t indexSpace = nevr_shadow::kChunkIndexLimit - nevr_shadow::kFirstChunkIndex,
                    const ResourceTypes& types = kWin10Types)
      : indexSpace_(indexSpace), types_(types) {}

  // Parses and checks a set of `size` bytes at `bytes` (the plugin's mapped image) whose first byte is at
  // `fileOffset` in the plugin's file, open as `handle`. On success the set is held for `plugin` until Commit or
  // Drop; on a refusal nothing is held and the caller keeps the handle.
  Outcome Add(const std::string& plugin, const std::string& label, const void* bytes, std::uint64_t size,
              std::int64_t handle, std::uint64_t fileOffset);
  // A map's set: the whole of the file open as `handle`, mapped at `bytes`, for `owner` ("map:<id>"). Add's checks at
  // file offset 0, except that a script record passes while the script waiver holds it (SetScriptWaiver). Held
  // committed: there is no init to wait for.
  Outcome AddMapFile(const std::string& owner, const std::string& label, const void* bytes, std::uint64_t size,
                     std::int64_t handle);

  // The plugin's init returned 0: its held sets are kept for the mount.
  void Commit(const std::string& plugin);
  // The plugin's init failed: its held sets are dropped; returns their handles for the caller to close.
  std::vector<std::int64_t> Drop(const std::string& plugin);
  // A map's grants are undone before the mount (its owner, IsMapOwner): its sets are dropped, committed or not; returns
  // their handles for the caller to close. Nothing for any other owner, or after the mount.
  std::vector<std::int64_t> DropMap(const std::string& owner);

  // Mounts every committed set once. A refused set, and one not committed, is dropped with its reason, and its handle
  // goes to the report for the caller to close: after the mount the registry holds the mounted sets only, so a key of
  // a refused set is free (Holds). Builds the one shadow from the rest.
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
  // True when a held set holds (type, name): before the mount any set registered (committed or not), after it a
  // mounted set.
  bool Holds(std::uint64_t type, std::uint64_t name) const;
  // The mounted sets, for the log and the login report.
  std::vector<const Set*> MountedSets() const;
  // Every set held, in the order registered: before the mount committed or not, after it the mounted ones.
  std::vector<const Set*> HeldSets() const;
  // The set a shadow record's user tag names, or null.
  const Set* SetForUser(std::uint32_t user) const;

 private:
  Outcome AddChecked(const std::string& owner, const std::string& label, const void* bytes, std::uint64_t size,
                     std::int64_t handle, std::uint64_t fileOffset, bool mapFile);
  // Drops every set the mount did not mount (its handle is already in the report).
  void KeepOnlyMounted();

  std::vector<std::unique_ptr<Set>> sets_;
  std::unique_ptr<nevr_shadow::Shadow> shadow_;
  std::vector<const Set*> mountedOrder_;
  std::uint64_t indexSpace_;
  ResourceTypes types_;
  // The chunks of the held sets, against the shadow's index space; read only before the mount (an Add after it is
  // kTooLate).
  std::uint64_t heldChunks_ = 0;
  bool mounted_ = false;
};

// Map scripts (docs/adr/0047, decision 6): while a waiver is set, a kScriptType record of a map's set (IsMapOwner)
// passes AddMapFile when `holds(owner, name)` (the map script provider holds that script for that owner) and is refused kScript otherwise. A
// plugin's set is refused kScript whatever the waiver says. Null clears it. Process-wide: every Registry reads it.
void SetScriptWaiver(bool (*holds)(const std::string& owner, std::uint64_t name));

// "0x<type>:0x<name>", as the log writes a key.
std::string KeyText(std::uint64_t type, std::uint64_t name);

// Parses an archive's body (docs/engine/asset-system.md, "Archives"): u32 D, D x u64 dependencies, u32 N, N x (u64
// type, u64 name), then the type table and a closing 0, which this does not read. False when it runs past `size`.
bool ParseArchive(const unsigned char* data, std::size_t size, Archive* out);

}  // namespace nevr_resource_sets

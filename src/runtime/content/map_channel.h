/* The custom-map channel on the PC (docs/adr/0047; CDN_DESIGN §10.2 PC5b): reads the maps.* config, mounts the cached maps
 * at boot through nevr's resource sets, map scripts and config aliases, fetches in the background, and keeps the session
 * pin (PC9) and the alias reconcile (PC8) on the per-frame tick.
 *
 * Purely additive: with no maps config (no maps.subscribed and no local dev key) Boot logs one map_start
 * reason=not_configured line and every other call returns at once, so the run is the run without the channel.
 *
 * Sources, in precedence order for one map id (each loser is logged as map_skipped reason=local_map with its source):
 *   1. EchoMapLoader (the editor's Play path): <id>.kfclvl under the loader root, the game's maps folder or .testmap.
 *   2. LOCAL: <nevr root>\maps\local, a bucket the editor publishes and signs with this machine's dev key. Read with no
 *      network on the boot thread, mounted this run, tagged LOCAL in every [NEVR.MAPS] line, never pinned to peers.
 *   3. The CDN channel: maps.base_url, fetched in the background, mounted at the next launch.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/cargo_env.h"
#include "core/config_alias.h"
#include "core/map_run.h"
#include "runtime/content/map_session_pin.h"

namespace nevr_map_channel {

// Boot thread, once, after LoadPlugins and before MountWhenReady.
void Boot(std::uintptr_t game_base, bool is_server);
// Boot thread, after MountWhenReady: the background fetch of the CDN channel.
void StartFetch();
// Sets keep_going false and joins for up to 2 s; on a timeout the worker and everything it uses are left alive. Called
// on both process exits (crash_recovery.cpp: the client's game-loop return and PerformGracefulShutdown); safe to call
// twice and from two threads; a no-op when the channel is not configured.
void StopFetch();
// Any thread: one more fetch pass for a subscribed id, at most once per 10 minutes.
void RequestFetch(const std::string& id);
// Game thread, from the per-frame dispatch.
void Tick();

namespace detail {

constexpr std::uint64_t kAliasRecordType = nevr_config_alias::kAliasRecordTypeWin10;
constexpr std::uint64_t kExtraFetchGapMs = 10ull * 60 * 1000;
constexpr const char* kLocalTag = "[NEVR.MAPS] LOCAL";

using Lookup = std::function<std::string(const char* flat_key)>;  // "" when absent

struct Settings {
  std::string reason;  // "" = the CDN channel runs; not_configured, bad_key, mixed_keys
  std::string detail;
  nevr_maps::ChannelConfig config;  // dir and local_maps are the caller's
  std::vector<nevr_maps::PinnedKey> author_keys;
};
// value: a scalar maps_* key; csv: a list-shaped one as "a,b". `official` is the compiled-in key list.
Settings ReadSettings(const Lookup& value, const Lookup& csv, const std::vector<nevr_maps::PinnedKey>& official);

std::vector<std::string> SplitCsv(const std::string& text);
// A minisign public key file (an "untrusted comment:" line, then the base64 key) or a bare base64 line.
bool ParseKeyText(const std::string& text, nevr_maps::PinnedKey* out);

// The ids EchoMapLoader will load this run, with the .kfclvl that shows it: file presence only, and only when
// <plugins>/EchoMapLoader.dll is present. As the loader indexes (EchoConstruct loader/plugin/map_loader.cpp), that is
// one folder with no recursion: <loader root>/maps/.testmap under ECHOMAPS_TESTMAP=1, else <loader root>/maps.
struct LoaderFolders {
  std::wstring plugin_dll, loader_root;  // an empty loader_root loads nothing, as in the loader
  bool testmap = false;
};
// This run's folders, read as the loader reads them (nevr_entry.cpp, choose_loader_root): ECHOMAPS_ROOT when it is set,
// without trailing separators, else <nevr root>/echomaps; testmap only when ECHOMAPS_TESTMAP is exactly "1".
LoaderFolders LoaderFoldersThisRun(const std::wstring& nevr_root, const std::wstring& plugins_dir);
std::vector<std::pair<std::string, std::string>> FindLoaderMaps(const LoaderFolders& folders,
                                                                 const std::vector<std::string>& ids);

// LOCAL sideload (the editor's test path): <dir>/<id>.windows-x86_64.nevrmap with its .minisig, signed by the key in
// <dir>/dev_key.pub with the trusted comment "nevr-local <id>". That key signs nothing else: it is never a channel key.
constexpr const char* kLocalSuffix = ".windows-x86_64.nevrmap";
constexpr const char* kDevKeyFile = "dev_key.pub";
constexpr const char* kLocalRecordFile = "local.json";
constexpr std::uint64_t kMaxLocalBytes = 1ull << 30;
constexpr std::uint64_t kMaxZipDirectoryBytes = 16ull << 20;

struct LocalMap {
  std::string id, path;  // path: the .nevrmap
  std::string reason;    // "" when verified and extracted; else the refusal token
  std::string dir;       // the extracted copy the mount holds
  bool imported = false;  // this run verified the .nevrmap and hashed the extraction (else the stamps stood for it)
  nevr_maps::Package package;
  std::map<std::string, nevr_maps::FileStamp> stamps;  // member path -> the stamp its verified bytes had
};
struct LocalScan {
  std::string reason;  // "" when dev_key.pub read; absent (no key file), bad_dev_key
  std::vector<LocalMap> maps;  // sorted by id
};
// Every <id>.windows-x86_64.nevrmap in `dir`, extracted under <cache>/<id>/<sha256 of the .nevrmap, first 12>, with
// <cache>/<id>/local.json recording what was verified. The CDN's boot model: a .nevrmap is verified (signature, then
// every member's sha256 against its signed package.json, hashed through a share-read handle) when it is new; when the
// .nevrmap's stamp, its .minisig, the dev key and every member's stamp equal the record, nothing is read or hashed.
// Memory is one 1 MiB buffer plus the zip directory and package.json. Boot thread; no network. Hashing a new or
// changed .nevrmap on the boot thread is the dev path's accepted exception to CDN_DESIGN §6.1 (§14.1 OI-11).
LocalScan ScanLocal(nevr_cargo::Env& env, nevr_maps::MapFs& fs, const std::string& dir, const std::string& cache);
// Grants in BootMount's order (scripts, rekeys, the set, then the aliases), each member held share-read and its stamp
// equal to the verified one before anything is granted. "" when everything was granted, else the refusal token; on a
// refusal every grant of the owner is withdrawn and no handle is left open.
std::string MountLocal(nevr_cargo::Env& env, nevr_maps::MapFs& fs, const LocalMap& map, nevr_maps::Mounter& mounter);

// A .nevrmap is a zip (stored or deflated members, no zip64, no encryption), read in place through `read`, which reads
// exactly `length` bytes at `offset` or fails. Names are checked as relative paths and duplicates are refused.
using ReadAt = std::function<bool(std::uint64_t offset, void* buffer, std::size_t length)>;
struct ZipEntry {
  std::string name;
  std::uint32_t method = 0, crc = 0, packed = 0, size = 0;
  std::uint64_t data_at = 0;
};
// The central directory (at most kMaxZipDirectoryBytes) and each entry's local header; no entry data is read.
bool ReadZipDirectory(const ReadAt& read, std::uint64_t file_size, std::vector<ZipEntry>* entries, std::string* problem);
// One entry's bytes to `sink` in pieces of at most 1 MiB; its size and crc are checked as it streams.
bool InflateZipEntry(const ReadAt& read, const ZipEntry& entry, const std::function<bool(const char*, std::size_t)>& sink,
                     std::string* problem);

// The resource-set half of the registry, as function pointers so a test can stand in for the game's mount.
struct Registry {
  nevr_maps::Grant (*register_set)(const std::string& owner, std::int64_t held, const std::string& label,
                                   const std::string& sha256) = nullptr;
  std::string (*report_json)() = nullptr;
  bool (*holds_key)(std::uint64_t type, std::uint64_t name) = nullptr;
  void (*withdraw_set)(const std::string& owner) = nullptr;  // drops the owner's set before the mount, closing its file
};

// Grants go to nevr_map_scripts (scripts, rekeys), the registry (the set) and nevr_config_alias (staged aliases).
// Withdraw undoes all three.
class ChannelMounter : public nevr_maps::Mounter {
 public:
  explicit ChannelMounter(const Registry& registry) : registry_(registry) {}
  nevr_maps::Grant Script(const std::string& owner, std::uint64_t hash, std::int64_t held,
                          const std::string& sha256) override;
  nevr_maps::Grant Rekey(const std::string& owner, std::uint64_t hash, std::uint64_t stock_source,
                         const std::string& stock_sha256) override;
  nevr_maps::Grant Set(const std::string& owner, std::int64_t held, const std::string& label, const std::string& sha256,
                       bool verified_stamp) override;
  nevr_maps::Grant StageAliases(const std::string& owner, const std::string& id, std::uint64_t main_level,
                                const std::vector<nevr_maps::ConfigAlias>& aliases) override;
  nevr_maps::Grant DerivedScript(const std::string& owner, std::uint64_t hash, std::int64_t held,
                                 const std::string& sha256, const std::string& stock_sha256) override;
  void Withdraw(const std::string& owner) override;

  bool AnyScript() const { return any_script_; }

 private:
  Registry registry_;
  bool any_script_ = false;
};

// The owners whose set mounted, with their levels: the ReportJson entries whose plugin is "map:<id>".
std::vector<std::pair<std::string, std::vector<std::uint64_t>>> MountedMapOwners(const std::string& report_json);

// What the mount observer acts on. `expected` is every owner a BootMount granted; `pins` the CDN maps (LOCAL ones are
// never pinned).
struct MountPlan {
  std::vector<std::string> expected;
  std::vector<nevr_map_session_pin::MapPin> pins;
  Registry registry;
};
struct MountOutcome {
  std::vector<std::string> mounted, withdrawn;
  std::size_t armed = 0;
};
// Withdraws every expected owner whose set did not mount (an empty or unparsable report withdraws them all), arms the
// aliases of the rest and hands PC9 the pins of the CDN maps that mounted.
MountOutcome ApplyMount(const MountPlan& plan, const std::string& report_json);
bool HoldsAlias(void* ctx, const std::string& owner, std::uint64_t alias);

// Local\nevr-maps-<first 16 hex of sha256(root)>: the name's existence is the lock, not mutex ownership (§14.1 OI-7),
// held by one process per cache root for its life. A crashed or killed run leaves no stale lock: the kernel closes its
// handle at exit, and the name goes with the last handle.
bool TryLockRoot(nevr_cargo::Env& env, const std::string& root);

// The background fetch: one pass, then it parks until a request (one more pass) or a stop. Heap-owned: on a stop
// timeout it is left alive with whatever its pass references.
class FetchWorker {
 public:
  using Pass = std::function<nevr_maps::FetchReport(const std::function<bool()>& keep_going)>;
  using Clock = std::function<std::uint64_t()>;
  FetchWorker(Pass pass, Clock now_ms) : pass_(std::move(pass)), now_ms_(std::move(now_ms)) {}
  FetchWorker(const FetchWorker&) = delete;
  FetchWorker& operator=(const FetchWorker&) = delete;

  void Start();
  // False when the last extra pass started under kExtraFetchGapMs ago or the worker is stopping.
  bool Request();
  // False on a timeout: the thread is detached and this object must stay alive. A repeated or concurrent call only
  // waits up to `wait` for the worker to finish.
  bool Stop(std::chrono::milliseconds wait);
  std::size_t Passes() const { return passes_.load(); }

 private:
  void Run();

  Pass pass_;
  Clock now_ms_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool stop_ = false, requested_ = false, finished_ = false, started_ = false;
  bool any_extra_ = false;
  std::uint64_t last_extra_ms_ = 0;
  std::atomic<bool> keep_going_{true};
  std::atomic<std::size_t> passes_{0};
  std::thread thread_;
};

std::string NowUtc();
std::string Utf8(const std::wstring& wide);
std::wstring Wide(const std::string& utf8);

}  // namespace detail
}  // namespace nevr_map_channel

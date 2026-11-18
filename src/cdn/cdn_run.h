/* The CDN worker (docs/adr/0045-cdn-texture-delivery.md D3 and D4): mount what the store holds, then fetch what the manifest
 * names, verify it, append it to the store and mount what arrived. It runs on its own thread, never in the game's frame. The
 * mount at boot reads the saved manifest and the blob index and hashes nothing: an object is hashed once, when it is downloaded.
 *
 * Every outcome is an event: cdn_limits, cdn_store, cdn_start, cdn_manifest, cdn_refused, cdn_plan, cdn_blob,
 * cdn_progress, cdn_mounted, cdn_done. A refusal names its rule (cdn_validate.h) and a failure its reason. A failure
 * leaves the store and the mounted set as they were: the bytes of an object that failed are cut off again.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cdn/cdn_env.h"
#include "cdn/cdn_manifest.h"
#include "cdn/cdn_plan.h"
#include "cdn/cdn_prefetch.h"
#include "cdn/cdn_script_gate.h"
#include "cdn/cdn_store.h"

namespace nevr_cdn {

// Where a fetch stands, for the notice the first-run step shows. Called after the plan and after every object, on the worker's thread.
struct FetchProgress {
  std::uint64_t total_bytes = 0;  // stored bytes the plan asks for
  std::uint64_t done_bytes = 0;
  std::size_t total_blobs = 0;
  std::size_t done_blobs = 0;
  std::size_t failed_blobs = 0;
};
using ProgressFn = std::function<void(const FetchProgress&)>;

// What the boot mount built and what it was built from (PLAN-v7 5.4 item 4): the feed the PC's cargo pin reads (R11). `mounted` carries the
// counts and per_label of the set the boot mount built, never its shadow, which went to the publisher; it is empty when nothing was built.
struct BootMount {
  Manifest boot;                    // the manifest the boot mount mounted from; empty when there is none or it was refused
  Mounted mounted;                  // the counts and per_label of the set; no shadow
  std::vector<GatedLabel> gated;    // the boot gate's refusals, each with the reason token of cdn_script_gate.h
  bool published = false;           // a set reached the publisher
  // The fetch state the pin decision reads (PLAN-v7 5.5 G6): "none", "undeliverable" or "no_space". `settled` reads as "none".
  std::string boot_fetch = "none";
  // The manifest path the boot fetch asked the CDN for (ResolveManifestPath); empty when no boot fetch ran. Start hands it to the fetch
  // thread, so that thread asks again only when the boot half did not.
  std::string resolved_manifest_path;
};

// The client's boot-only fetch (PLAN-v7 5.4 "Client start" item 2): a stall (no byte for this long) ends it, the wait's ceiling ends it.
inline constexpr std::uint64_t kClientBootFetchStallMs = 15000;
inline constexpr std::uint64_t kClientBootFetchCeilingMs = 120000;
// The dedicated server's boot fetch (PLAN-v7 5.4 "Server"): the one wait of a server's start, from the manifest request to the last object.
// A named wait, chosen rather than measured. The fetch ends there with `deadline`, and the boot set is what the cache holds.
inline constexpr std::uint64_t kServerBootFetchMs = 60000;

struct RunConfig {
  std::string dir;            // the store directory (absolute)
  std::string base_url;       // e.g. "https://cargo.echo.taxi/", ending in '/'
  std::string manifest_path;  // relative to base_url, e.g. "v2/manifest.json.zst"
  Platform platform = kAndroidArm64;
  Policy policy;
  // "default" or "config": whether the policy or the URL came from config. cdn_start logs it and the host, not the configured values;
  // cdn_manifest names the one URL it requested, so a failed request says what it asked for.
  std::string source = "default";
  ProgressFn progress;             // optional
  // The first-run hold (docs/adr/0045 decision 11): the fetch stores every object of every served label through the first-run prefetch
  // (cdn/cdn_prefetch.h), one after another, and ends at the first object that cannot be had, so a caller that waits for it knows the
  // store is whole when RunResult::stopped is empty and `failed` is 0. Off: the background fetch, which counts a failed object and goes on.
  bool first_run = false;
  // The game's script folder with its trailing separator (`<exe dir>\scripts\` on the PC): the script gate (cdn_script_gate.h) asks it
  // whether a script a label names is on disk, before both mounts. Empty (the Quest's): the gate is not applied and one
  // `cdn_scripts_gate result=off reason=no_script_dir` is written for each half that mounts.
  std::string script_dir;
  // Runs before the gate, for the manifest of each mount: it registers the `scripts` copies of every label and reports what it
  // held and refused (cdn_script_gate.h). Unset: nothing is held and nothing is refused.
  ScriptRegistrar registrar;
  // The labels fixed at boot (docs/adr/0045 decision 7): a fetch mounts each of them only as the boot set had it. BootOnlyLabels adds
  // every label with a `scripts` entry of this platform. Empty: none, and the fetch mounts what it fetched (the Quest's).
  std::vector<std::string> boot_only;
  // The manifest of the set the boot mount published; empty when it published none (MountFromCache fills it).
  Manifest boot;
  // The no-byte window of the first-run prefetch (PrefetchConfig::stall_ms); the default keeps the first-run hold's own window.
  std::uint64_t stall_ms = kPrefetchStallMs;
  // The boot fetch (ClientBoot): the run saves the manifest and stores its objects, and returns before Mount, so it publishes nothing.
  bool store_only = false;
  // This start is the last start of a self-update trial (PLAN-v7 G9): ClientBoot makes no boot fetch for it. The PC edge sets it (R6b-3).
  bool last_update_trial = false;
  // The boot-state writes (PLAN-v7 5.4 "The end of a fetch", 5.5 "The refetch marker"): the refetch marker and the boot-fetch stamp are
  // written and removed only by a run that sets this. The PC edge sets it in R6b-3, so until then no run writes either file.
  bool boot_state = false;
  // Seams the PC binds (PLAN-v7 5.5 "Feed", R11). Unset: nothing is called. ClientBoot calls on_boot once, after the boot mount, whether or
  // not a set was published. FetchAndMount calls on_manifest right after each manifest parse, and on_end at each end of a fetch that
  // parsed one, with the cause the run's boot-state writes settled on: "none", "settled", "undeliverable" or "no_space".
  std::function<void(const BootMount&)> on_boot;
  std::function<void(const Manifest&)> on_manifest;
  std::function<void(const char* cause)> on_end;
};

// The boot-only labels, the fixed part of RunConfig::boot_only: the EchoCombat data a combat session simulates. MakeRunConfig gives them to
// the windows-x86_64 client only; ServerBoot gives them to the server (its boot fetch takes these labels and no other).
inline constexpr const char* kBootOnlyLabels[] = {"combat_content"};

// Receives a mounted set. The set is never freed by the worker: a request being filled from its shadow must stay valid.
using Publish = std::function<void(std::unique_ptr<Mounted>)>;

// RunResult::stopped when the manifest could not be had (a failed request, or a body over the size limit): what the store holds stays
// mounted. A body that was had but is no manifest (a zstd frame with a broken header or no content size, text that is not the manifest's
// JSON) is "manifest_refused".
inline constexpr const char* kStoppedManifestFailed = "manifest_failed";

struct RunResult {
  std::size_t fetched = 0;   // blobs stored this run
  std::size_t cached = 0;    // blobs already held
  std::size_t refused = 0;   // manifest entries the validator refused
  const char* stopped = "";  // why the run did nothing or ended early: "limits", "manifest_failed", "manifest_refused", "storage", "cancelled", "" when it ran through
  std::size_t failed = 0;    // blobs that could not be stored
  std::uint64_t bytes = 0;   // bytes stored this run
  bool manifest_loaded = false;
  bool published = false;    // a set was published by the fetch half
  // The first-run prefetch, when it ran: its status, the HTTP status of a refused object (0 when none) and the labels the plan skipped,
  // each with its reason, so the caller reads the cause without parsing events.
  bool prefetched = false;
  PrefetchStatus prefetch_status = PrefetchStatus::kComplete;
  long prefetch_http_status = 0;
  std::vector<std::pair<std::string, SkipReason>> skipped;
  // The end cause the run's boot-state writes settled on, as RunConfig::on_end names it: "none", "settled", "undeliverable" or "no_space".
  std::string end_cause = "none";
};

// Boot half: no network. Reads the index and mounts it. Event cdn_mounted source=boot when anything was mounted or refused. `out`, when
// not null, is filled with what the mount built (BootMount): its manifest, its counts and gate, and whether a set was published.
bool MountFromCache(Env& env, const RunConfig& config, const Publish& publish, BootMount* out = nullptr);

// The client's boot half (PLAN-v7 5.4 "Client start" items 1-4), on the calling thread: decides whether the boot-only labels need a fetch
// before the boot mount (NeedsBootFetch), makes it when they do (store only, under the ceiling and stall window), mounts once, and calls
// RunConfig::on_boot once. Events: cdn_start role=client (fetch=boot_only, the result; fetch=none, the trial's reason).
BootMount ClientBoot(Env& env, const RunConfig& config, const Publish& give);

// The dedicated server's boot half (PLAN-v7 5.4 "Server"), on the calling thread: fetches the boot-only labels (store only, under
// kServerBootFetchMs), mounts once from the cache, and calls RunConfig::on_boot once, also when nothing was published. A server fetches
// its labels before its boot set so that its pin and its features are known when it registers, and its set never changes in a run. It
// takes no other label (a session's game data is the demand resolve's, cdn_demand.h), makes no background fetch and writes neither the
// refetch marker nor the boot-fetch stamp (the copy of `config` it runs with has boot_state and last_update_trial off). Events: one
// cdn_start role=server (result ok, deadline or failed), written after the mount so its consequence says what the server holds.
BootMount ServerBoot(Env& env, const RunConfig& config, const Publish& give);

// The result text of the client's boot fetch from its run: "ok", "stalled", "ceiling", "undeliverable", "no_space" or "failed" (PLAN-v7
// 5.4 item 2). Undeliverable and no_space also name a pin label the plan skipped for its files or its storage.
std::string BootFetchResultOf(const RunResult& result);

// The refetch marker and the boot-fetch stamp, beside the saved manifest (ManifestCopyPath).
std::string BootRefetchPath(const std::string& dir);
std::string BootFetchStampPath(const std::string& dir);

// The manifest a fetch mounts for the boot-only labels: a copy of `fetched` in which every entry, record of 0 bytes, script and refusal
// of a label in `boot_only` is replaced by the boot set's items of that label. A label of `boot_only` the boot set lacks keeps no record.
Manifest KeepBootOnly(const Manifest& fetched, const Manifest& boot, const std::vector<std::string>& boot_only);

// The boot-only labels of a run: `fixed` (sorted, no duplicates), plus every label with a `scripts` entry of `platform` in `boot` or
// `fetched`. Empty when `fixed` is empty.
std::vector<std::string> BootOnlyLabels(const std::vector<std::string>& fixed, const Manifest& boot, const Manifest& fetched,
                                        const Platform& platform);

// Whether two manifests give Mount the same records for `platform`: the same entries of this platform (label, type, name, sha256,
// size, bytes, align), the same records of 0 bytes of this platform or of none (label, type, name, align) and the same `scripts`
// entries of this platform (label, name, stock_source, stock_sha256), each compared as a multiset. Document order, `labels`, `refused`,
// `unknown_fields` and `stock_size` are not compared: they do not change what Mount builds.
bool SameMountRecords(const Manifest& a, const Manifest& b, const Platform& platform);

namespace detail {
// The helpers the demand resolve (cdn_demand.h) shares with the worker above. Their behaviour is the worker's: nothing here is a second
// implementation.

// The policy a mount uses: the config's, with every label whose scripts are not delivered off (cdn_script_gate.h). `source` is "boot",
// "fetch" or "demand". `gated`, when not null, receives the gate's refusals.
Policy GatedPolicy(Env& env, const RunConfig& config, const Manifest& manifest, const char* source, std::vector<GatedLabel>* gated = nullptr);

// The gate's refusals for `manifest` under `config`, with no event: what GatedPolicy applies and reports.
std::vector<GatedLabel> GatedLabelsOf(Env& env, const RunConfig& config, const Manifest& manifest);

// The limits of this run, from the device: one line with every number, or the missing fact by name.
Limits LoadLimits(Env& env, const std::string& dir);

std::unique_ptr<Store> OpenStoreLogged(Env& env, const std::string& dir, const Limits& limits, const char* source);

struct Downloaded {
  bool ok = false;
  bool cancelled = false;
  bool stop_run = false;  // the failure is the store's, so it will be the next object's as well
  const char* reason = "";
  long status = 0;
  int code = 0;
};

// Streams one object into the store while hashing it, and names it in the index when its size, its frame header and its SHA-256 are
// the manifest's. Any failure gives the bytes back.
Downloaded Download(Env& env, const RunConfig& config, Store& store, const Entry& e, const std::function<bool()>& keep_going);

// The store as the first-run prefetch's store (cdn_prefetch.h): each object's decompressed size is kept from the entry it came from.
class StorePrefetch : public PrefetchStore {
 public:
  StorePrefetch(Store& store, const std::vector<Entry>& objects);
  bool Has(const std::string& sha256) const override;
  bool HoldsSized(const std::string& sha256, std::uint64_t size, std::uint64_t bytes) const override;
  std::unique_ptr<ObjectSink> Open(const std::string& sha256, std::uint64_t size, StoreStatus* status) override;

 private:
  Store& store_;
  std::map<std::string, std::uint64_t> bytes_;
};

// The manifest of one fetch, read by its bytes: the parsed manifest, the bytes as had (a frame or JSON text) and the HTTP status. `stopped`
// says why it was not had (kStoppedManifestFailed, "manifest_refused" or "cancelled"); its events are written.
struct ManifestLoad {
  bool ok = false;
  const char* stopped = "";
  Manifest manifest;
  std::string body;
  long http_status = 0;
  std::string url;        // the manifest's URL, as requested
  bool plain = false;     // the body is JSON text, not a zstd frame
  std::uint64_t declared = 0;  // a frame's declared text size
};
ManifestLoad FetchManifest(Env& env, const RunConfig& config, const Limits& limits, const std::function<bool()>& keep_going);

}  // namespace detail

// Worker half. `keep_going` returning false ends it at the next boundary (shutdown).
RunResult FetchAndMount(Env& env, const RunConfig& config, const Publish& publish, const std::function<bool()>& keep_going);

}  // namespace nevr_cdn

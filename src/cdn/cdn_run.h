/* The CDN worker (docs/adr/0045-cdn-texture-delivery.md D3 and D4): mount what the store holds, then fetch what the manifest
 * names, verify it, append it to the store and mount what arrived. It runs on its own thread, never in the game's frame. The
 * mount at boot reads the saved manifest and the blob index and hashes nothing: an object is hashed once, when it is downloaded.
 *
 * Every outcome is an event: cdn_limits, cdn_store, cdn_start, cdn_manifest, cdn_refused, cdn_plan, cdn_blob,
 * cdn_progress, cdn_mounted, cdn_done. A refusal names its rule (cdn_validate.h) and a failure its reason. A failure
 * leaves the store and the mounted set as they were: the bytes of an object that failed are cut off again.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cdn/cdn_env.h"
#include "cdn/cdn_manifest.h"
#include "cdn/cdn_plan.h"
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
};

// The client's boot-only labels, the fixed part of RunConfig::boot_only: the EchoCombat data a combat session simulates. MakeRunConfig
// gives them to the windows-x86_64 client only.
inline constexpr const char* kClientBootOnlyLabels[] = {"combat_content"};

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
};

// Boot half: no network. Reads the index and mounts it. Event cdn_mounted source=boot when anything was mounted or refused. `boot`, when
// not null, receives the manifest of the set it published, and is left empty on every return that publishes nothing.
bool MountFromCache(Env& env, const RunConfig& config, const Publish& publish, Manifest* boot = nullptr);

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

// Worker half. `keep_going` returning false ends it at the next boundary (shutdown).
RunResult FetchAndMount(Env& env, const RunConfig& config, const Publish& publish, const std::function<bool()>& keep_going);

}  // namespace nevr_cdn

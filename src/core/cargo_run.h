/* The cargo worker (docs/adr/0045-cdn-texture-delivery.md D3 and D4): mount what the store holds, then fetch what the manifest
 * names, verify it, append it to the store and mount what arrived. It runs on its own thread, never in the game's frame. The
 * mount at boot reads the saved manifest and the blob index and hashes nothing: an object is hashed once, when it is downloaded.
 *
 * Every outcome is an event: cargo_limits, cargo_store, cargo_start, cargo_manifest, cargo_refused, cargo_plan, cargo_blob,
 * cargo_progress, cargo_mounted, cargo_done. A refusal names its rule (cargo_validate.h) and a failure its reason. A failure
 * leaves the store and the mounted set as they were: the bytes of an object that failed are cut off again.
 */
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "core/cargo_env.h"
#include "core/cargo_manifest.h"
#include "core/cargo_plan.h"
#include "core/cargo_store.h"

namespace nevr_cargo {

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
  std::string manifest_path;  // relative to base_url, e.g. "v1/manifest.json"
  Platform platform = kAndroidArm64;
  Policy policy;
  std::string source = "default";  // "default" or "config": whether the policy or the URL came from config (logged, never the values)
  ProgressFn progress;             // optional
};

// Receives a mounted set. The set is never freed by the worker: a request being filled from its shadow must stay valid.
using Publish = std::function<void(std::unique_ptr<Mounted>)>;

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

// Boot half: no network. Reads the index and mounts it. Event cargo_mounted source=boot when anything was mounted or refused.
bool MountFromCache(Env& env, const RunConfig& config, const Publish& publish);

// Worker half. `keep_going` returning false ends it at the next boundary (shutdown).
RunResult FetchAndMount(Env& env, const RunConfig& config, const Publish& publish, const std::function<bool()>& keep_going);

}  // namespace nevr_cargo

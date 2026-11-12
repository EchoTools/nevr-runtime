/* The cargo worker (docs/adr/0045-cdn-texture-delivery.md D3 and D4): mount what the cache holds, then fetch what the manifest
 * names, verify it, store it, write the index and mount what arrived. It runs on its own thread on a client, never in the game's
 * frame, and never blocks the boot: the mount at boot reads files only, and the fetch runs after.
 *
 * Every outcome is an event (docs/adr/0045 "Log vocabulary"): cargo_start, cargo_manifest, cargo_plan, cargo_blob, cargo_index,
 * cargo_mounted, cargo_done. A failure leaves the cache and the mounted set as they were.
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

struct RunConfig {
  std::string dir;            // the cache directory (absolute)
  std::string base_url;       // e.g. "https://cargo.echo.taxi/", ending in '/'
  std::string manifest_path;  // relative to base_url, e.g. "v1/manifest.json"
  Platform platform = kAndroidArm64;
  Policy policy;
  std::uint64_t cap_bytes = kDefaultCapBytes;  // the blobs the directory keeps; Evict removes unreferenced ones above it
  std::string source = "default";  // "default" or "config": whether the policy or the URL came from config (logged, never the values)
};

// Receives a mounted set. The set is never freed by the worker: a request being filled from its shadow must stay valid.
using Publish = std::function<void(std::unique_ptr<Mounted>)>;

struct RunResult {
  std::size_t fetched = 0;   // blobs stored this run
  std::size_t cached = 0;    // blobs already held
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

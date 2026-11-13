/* The first-run prefetch (docs/adr/0045-cdn-texture-delivery.md, the first-run hold of docs/engine/asset-system.md): before the engine
 * starts, fetch every object a cargo manifest lists into the store, one after another on the caller's thread, verify each against
 * its SHA-256 and store it. A run that is killed midway is resumed by the next one: an object the store already holds is not
 * requested again.
 *
 * The run ends at the first object that cannot be had, with one status naming why: a non-200 answer (404 included), a transfer that
 * stops returning bytes for stall_ms, a body whose SHA-256 or size is not the manifest's, or a store that refuses a write (no space
 * or an I/O error). Nothing here exits, throws or shows anything: the caller prints the failure line from the result and the events.
 *
 * The store is the seam: PrefetchStore is the adapter a store implements (a lookup, and a sink that is committed or aborted). The
 * platform is the Env (cargo_env.h) for the transfer, the clock and the events. Built without exceptions.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/cargo_env.h"
#include "core/cargo_manifest.h"

namespace nevr_cargo {

// What a store answers for a write. kNoSpace is the disk-full case; it ends the run as no_space, not as a generic failure.
enum class StoreStatus { kOk, kNoSpace, kIoError };
const char* StoreStatusName(StoreStatus status) noexcept;

// One object being written. Write takes its bytes in order; Commit makes it durable under its name, and only then is it held;
// Abort leaves nothing behind. A sink destroyed without Commit is aborted.
class ObjectSink {
 public:
  virtual ~ObjectSink() = default;
  virtual StoreStatus Write(const void* data, std::size_t length) = 0;
  virtual StoreStatus Commit() = 0;
  virtual void Abort() = 0;
};

// The store adapter. Has is the index lookup: true when the object is held, with its stored size. Open starts an object of `size`
// bytes; a null sink means the store refused it, and `*status` says why.
class PrefetchStore {
 public:
  virtual ~PrefetchStore() = default;
  // Has: the name is held. HoldsSized: it is held at the manifest's frame size and decompressed size (the store's own check,
  // as src/core/cargo_store.h's Store::HoldsSized). A name held at another size is refused, never skipped.
  virtual bool Has(const std::string& sha256) const = 0;
  virtual bool HoldsSized(const std::string& sha256, std::uint64_t size, std::uint64_t bytes) const = 0;
  virtual std::unique_ptr<ObjectSink> Open(const std::string& sha256, std::uint64_t size, StoreStatus* status) = 0;
};

// How long the first-run hold waits on a transfer that delivers no body byte before it ends the run as stalled. A policy number, not a
// format or a device fact: the player is held at the first-run screen meanwhile.
inline constexpr std::uint64_t kPrefetchStallMs = 30000;

// The object URL is `base_url` + "blobs/sha256/" + sha256 (the bucket's immutable blob path, docs/adr/0045 decision 5).
struct PrefetchConfig {
  std::string base_url;             // ends in '/', e.g. "https://cargo.echo.taxi/"
  std::vector<Entry> objects;       // every entry of the manifest the run serves; duplicates by sha256 are fetched once
  std::uint64_t stall_ms = kPrefetchStallMs;  // no body byte for this long ends the run as stalled
  // false cancels at the next boundary; null: never. The stall rule needs the transport to call it while no bytes arrive: an Env::Get
  // that blocks in a read without calling keep_going is never detected as stalled (the P6 transport must poll it, e.g. from curl's
  // progress callback).
  std::function<bool()> keep_going;
  std::function<void(std::uint64_t done_bytes, std::uint64_t total_bytes)> progress;  // null: none
};

enum class PrefetchStatus {
  kComplete,      // every object is held
  kHttpFailed,    // an object's answer was not 200 or 206 (detail names the status)
  kTransport,     // the transfer ended without a complete answer (detail names the transport code)
  kStalled,       // no byte for stall_ms
  kHeldWrongSize, // the store holds the name at another size than the manifest's: refused by name, not skipped
  kHashMismatch,  // the body's SHA-256 is not the manifest's: refused by name, not stored
  kSizeMismatch,  // the body's length is not the manifest's size
  kNoSpace,       // the store ran out of room
  kStoreFailed,   // the store could not write or open an object for another reason
  kCancelled,     // keep_going said stop
};
const char* PrefetchStatusName(PrefetchStatus status) noexcept;

struct PrefetchResult {
  PrefetchStatus status = PrefetchStatus::kComplete;
  std::size_t objects = 0;          // distinct objects in the set
  std::size_t fetched = 0;          // stored by this run
  std::size_t held = 0;             // already stored, not requested
  std::uint64_t bytes_fetched = 0;  // bytes stored by this run
  std::string failed_sha256;        // the object that ended the run, when one did
  std::string detail;               // the reason token, or the status code, for the log and the failure line
};

// Fetches the set. Events: cargo_prefetch_start, cargo_prefetch_object (Debug, per object stored), cargo_prefetch_refused (Warning,
// per object refused), cargo_prefetch_failed (Error, once, the run's end when it failed) and cargo_prefetch_done (once: Info when the run is complete, Error otherwise).
// The events never carry a URL; they carry the label, the name (hex), the sha256 and the reason.
PrefetchResult Prefetch(Env& env, PrefetchStore& store, const PrefetchConfig& config);

}  // namespace nevr_cargo

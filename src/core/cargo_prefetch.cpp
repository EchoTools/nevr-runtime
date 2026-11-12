// The first-run prefetch: one object at a time, each through one GET, verified and committed to the store (cargo_prefetch.h).
#include "core/cargo_prefetch.h"

#include <set>
#include <utility>

#include "core/cargo_store.h"

namespace nevr_cargo {

namespace {

const char* ReasonOf(PrefetchStatus status) noexcept {
  switch (status) {
    case PrefetchStatus::kComplete: return "complete";
    case PrefetchStatus::kHttpFailed: return "http_status";
    case PrefetchStatus::kTransport: return "transport";
    case PrefetchStatus::kStalled: return "stalled";
    case PrefetchStatus::kHeldWrongSize: return "held_size_mismatch";
    case PrefetchStatus::kHashMismatch: return "sha256_mismatch";
    case PrefetchStatus::kSizeMismatch: return "size_mismatch";
    case PrefetchStatus::kNoSpace: return "no_space";
    case PrefetchStatus::kStoreFailed: return "store_failed";
    case PrefetchStatus::kCancelled: return "cancelled";
  }
  return "unknown";
}

bool IsRefusal(PrefetchStatus status) noexcept {
  return status == PrefetchStatus::kHashMismatch || status == PrefetchStatus::kSizeMismatch ||
         status == PrefetchStatus::kHeldWrongSize;
}

// The outcome of one object's transfer.
struct Attempt {
  PrefetchStatus status = PrefetchStatus::kComplete;
  std::string detail;        // the failing step's own words: a status code, a store status, a transport code
  long http_status = 0;      // the answer's status, when one was received
  std::string actual_sha256; // the body's digest, for a hash mismatch
};

// Streams one object from the transport into a sink, verifies it and commits it. `on_bytes` is called with each chunk stored.
Attempt Transfer(Env& env, PrefetchStore& store, const PrefetchConfig& config, const Entry& entry,
                 const std::function<void(std::uint64_t)>& on_bytes) {
  Attempt out;
  StoreStatus open_status = StoreStatus::kOk;
  std::unique_ptr<ObjectSink> sink = store.Open(entry.sha256, entry.size, &open_status);
  if (!sink) {
    out.status = open_status == StoreStatus::kNoSpace ? PrefetchStatus::kNoSpace : PrefetchStatus::kStoreFailed;
    out.detail = StoreStatusName(open_status);
    return out;
  }

  std::unique_ptr<Hasher> hasher = env.NewHasher();
  std::uint64_t delivered = 0;
  long answer = 0;  // the first status the transport handed over
  StoreStatus write_status = StoreStatus::kOk;
  bool too_long = false;
  bool stalled = false;
  bool cancelled = false;
  std::uint64_t last_byte_ms = env.NowMs();

  const std::string url = config.base_url + "blobs/sha256/" + entry.sha256;
  const HttpBody on_body = [&](long status, const void* data, std::size_t length) -> bool {
    if (answer == 0) answer = status;
    if (status != 200 && status != 206) return false;  // a refused answer delivers nothing
    if (length == 0) return true;
    if (delivered + length > entry.size) {
      too_long = true;
      return false;
    }
    hasher->Update(data, length);
    write_status = sink->Write(data, length);
    if (write_status != StoreStatus::kOk) return false;
    delivered += length;
    last_byte_ms = env.NowMs();
    on_bytes(length);
    return true;
  };
  const std::function<bool()> keep_going = [&]() -> bool {
    if (env.NowMs() - last_byte_ms >= config.stall_ms) {
      stalled = true;
      return false;
    }
    if (config.keep_going && !config.keep_going()) {
      cancelled = true;
      return false;
    }
    return true;
  };

  const HttpResult http = env.Get(url, on_body, keep_going);

  // Each ending is checked in turn; the sink is aborted on every one that is not a commit.
  if (write_status != StoreStatus::kOk) {
    sink->Abort();
    out.status = write_status == StoreStatus::kNoSpace ? PrefetchStatus::kNoSpace : PrefetchStatus::kStoreFailed;
    out.detail = StoreStatusName(write_status);
    return out;
  }
  if (answer != 0 && answer != 200 && answer != 206) {
    sink->Abort();
    out.status = PrefetchStatus::kHttpFailed;
    out.http_status = answer;
    out.detail = std::to_string(answer);
    return out;
  }
  if (stalled) {
    sink->Abort();
    out.status = PrefetchStatus::kStalled;
    out.detail = "no bytes for " + std::to_string(config.stall_ms) + " ms";
    return out;
  }
  if (cancelled) {
    sink->Abort();
    out.status = PrefetchStatus::kCancelled;
    out.detail = "cancelled";
    return out;
  }
  if (too_long) {
    sink->Abort();
    out.status = PrefetchStatus::kSizeMismatch;
    out.detail = "longer than the manifest's size";
    return out;
  }
  if (!http.completed) {
    sink->Abort();
    out.status = PrefetchStatus::kTransport;
    out.http_status = http.status;
    out.detail = "code " + std::to_string(http.code);
    return out;
  }
  if (delivered != entry.size) {
    sink->Abort();
    out.status = PrefetchStatus::kSizeMismatch;
    out.detail = "shorter than the manifest's size";
    return out;
  }
  const std::string digest = hasher->Hex();
  if (digest != entry.sha256) {
    sink->Abort();
    out.status = PrefetchStatus::kHashMismatch;
    out.actual_sha256 = digest;
    out.detail = "sha256 differs from the manifest";
    return out;
  }
  const StoreStatus commit = sink->Commit();
  if (commit != StoreStatus::kOk) {
    out.status = commit == StoreStatus::kNoSpace ? PrefetchStatus::kNoSpace : PrefetchStatus::kStoreFailed;
    out.detail = StoreStatusName(commit);
    return out;
  }
  out.status = PrefetchStatus::kComplete;
  return out;
}

}  // namespace

PrefetchResult Prefetch(Env& env, PrefetchStore& store, const PrefetchConfig& config) {
  PrefetchResult result;

  std::vector<Entry> objects;
  std::set<std::string> seen;
  std::uint64_t total = 0;
  for (const Entry& entry : config.objects) {
    if (!seen.insert(entry.sha256).second) continue;
    objects.push_back(entry);
    total += entry.size;
  }
  result.objects = objects.size();
  env.Event(EventLevel::kInfo, "cargo_prefetch_start",
            {{"objects", static_cast<std::uint64_t>(objects.size())}, {"bytes", total}, {"stall_ms", config.stall_ms}});

  std::uint64_t done = 0;
  const auto report = [&]() {
    if (config.progress) config.progress(done, total);
  };

  for (const Entry& entry : objects) {
    Attempt attempt;
    std::uint64_t stored_now = 0;
    if (store.Has(entry.sha256)) {
      if (store.HoldsSized(entry.sha256, entry.size, entry.bytes)) {
        ++result.held;
        done += entry.size;
        report();
        continue;
      }
      // Held at another size: the object is not skipped, and the run ends by name, as any refused object does.
      attempt.status = PrefetchStatus::kHeldWrongSize;
      attempt.detail = "held at another size than the manifest's";
    } else {
      attempt = Transfer(env, store, config, entry, [&](std::uint64_t n) {
        stored_now += n;
        done += n;
        report();
      });
    }

    if (attempt.status == PrefetchStatus::kComplete) {
      ++result.fetched;
      result.bytes_fetched += stored_now;
      env.Event(EventLevel::kDebug, "cargo_prefetch_object",
                {{"label", entry.label}, {"name", FormatHex64(entry.name)}, {"sha256", entry.sha256}, {"bytes", entry.size}});
      continue;
    }

    // The run ends here. The object's bytes, if any, were aborted by Transfer; the objects stored before it stay, so the next
    // launch resumes from them.
    result.status = attempt.status;
    result.failed_sha256 = entry.sha256;
    result.detail = attempt.detail;
    if (IsRefusal(attempt.status)) {
      std::vector<Field> fields = {{"label", entry.label},
                                   {"name", FormatHex64(entry.name)},
                                   {"sha256", entry.sha256},
                                   {"reason", ReasonOf(attempt.status)}};
      if (!attempt.actual_sha256.empty()) fields.emplace_back("actual_sha256", attempt.actual_sha256);
      env.Event(EventLevel::kWarn, "cargo_prefetch_refused", fields);
    }
    std::vector<Field> fields = {{"label", entry.label},
                                 {"name", FormatHex64(entry.name)},
                                 {"sha256", entry.sha256},
                                 {"reason", ReasonOf(attempt.status)},
                                 {"detail", attempt.detail}};
    if (attempt.http_status != 0) fields.emplace_back("http_status", static_cast<std::uint64_t>(attempt.http_status));
    env.Event(EventLevel::kError, "cargo_prefetch_failed", fields);
    break;
  }

  env.Event(result.status == PrefetchStatus::kComplete ? EventLevel::kInfo : EventLevel::kError, "cargo_prefetch_done",
            {{"result", PrefetchStatusName(result.status)},
             {"objects", static_cast<std::uint64_t>(result.objects)},
             {"fetched", static_cast<std::uint64_t>(result.fetched)},
             {"held", static_cast<std::uint64_t>(result.held)},
             {"bytes", result.bytes_fetched}});
  return result;
}

const char* StoreStatusName(StoreStatus status) noexcept {
  switch (status) {
    case StoreStatus::kOk: return "ok";
    case StoreStatus::kNoSpace: return "no_space";
    case StoreStatus::kIoError: return "io_error";
  }
  return "unknown";
}

const char* PrefetchStatusName(PrefetchStatus status) noexcept {
  switch (status) {
    case PrefetchStatus::kComplete: return "complete";
    case PrefetchStatus::kHttpFailed: return "http_failed";
    case PrefetchStatus::kTransport: return "transport";
    case PrefetchStatus::kStalled: return "stalled";
    case PrefetchStatus::kHeldWrongSize: return "held_size_mismatch";
    case PrefetchStatus::kHashMismatch: return "hash_mismatch";
    case PrefetchStatus::kSizeMismatch: return "size_mismatch";
    case PrefetchStatus::kNoSpace: return "no_space";
    case PrefetchStatus::kStoreFailed: return "store_failed";
    case PrefetchStatus::kCancelled: return "cancelled";
  }
  return "unknown";
}

}  // namespace nevr_cargo

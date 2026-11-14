#include "cdn/cdn_run.h"

#include "cdn/cdn_prefetch.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <utility>

namespace nevr_cdn {

namespace {

constexpr const char* kBlobPrefix = "blobs/sha256/";
// How many failures of one kind a run says one by one before it only counts them (a dead network must not write 36,000 lines).
constexpr std::size_t kSingleLines = 16;
// The event that says how far a fetch has come is written at each tenth of the bytes: ten lines a run, however many objects.
constexpr std::uint64_t kProgressSteps = 10;
constexpr char kSchemeSeparator[] = "://";
constexpr std::size_t kSchemeSeparatorChars = sizeof(kSchemeSeparator) - 1;  // the "://" of a URL
// "0x" and eight hex digits of a 32-bit chunk index, and the terminator.
constexpr std::size_t kChunkBaseTextBytes = kHexPrefixChars + sizeof(std::uint32_t) * kHexCharsPerByte + 1;
constexpr int kChunkBaseDigits = static_cast<int>(sizeof(std::uint32_t) * kHexCharsPerByte);

// The transport asks every GET with `Range: bytes=0-` (quest/data/transport.h), which a CDN answers 206 with the whole object, and a
// server that ignores a range answers 200. Both carry the object from its first byte.
bool IsWholeBody(long status) noexcept { return status == kHttpOk || status == kHttpPartialContent; }

// The host part of a URL, for the log: never the path or a query.
std::string HostOf(const std::string& url) {
  const std::size_t scheme = url.find(kSchemeSeparator);
  const std::size_t begin = scheme == std::string::npos ? 0 : scheme + kSchemeSeparatorChars;
  const std::size_t end = url.find_first_of("/?#", begin);
  return url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

std::string StatesText(const Policy& policy, const Manifest* manifest) {
  std::map<std::string, LabelState> states;
  for (const LabelDefault& d : kLabelDefaults) states[d.label] = d.state;
  for (const auto& kv : policy.overrides) states[kv.first] = kv.second;
  if (manifest != nullptr) {
    for (const std::string& label : manifest->labels) states[label] = StateOf(label, policy);
  }
  std::string text;
  for (const auto& kv : states) {
    if (!text.empty()) text += ",";
    text += kv.first + "=" + LabelStateName(kv.second);
  }
  return text;
}

// The first chunk index of the mounted shadow, or "none" when nothing mounted (every shadow of the process owns a range of its own).
std::string ChunkBaseText(const Mounted& m) {
  if (!m.shadow) return "none";
  char buf[kChunkBaseTextBytes];
  std::snprintf(buf, sizeof(buf), "0x%0*x", kChunkBaseDigits, static_cast<unsigned>(m.shadow->chunk_base()));
  return buf;
}

// `ms` is the mount's own time. It reads no object: the boot mount is the saved manifest and the blob index.
void SayMounted(Env& env, const char* source, const Mounted& m, std::uint64_t ms) {
  std::vector<Field> none;
  if (!m.shadow) {
    none.push_back({"result", "none"});
    none.push_back({"reason", m.refused != 0 || m.missing != 0 ? "all_refused_or_missing" : "nothing_mounted"});
    none.push_back({"consequence", "the game's own data is read for every name"});
  }
  std::vector<Field> fields = {{"source", source},
             {"labels", m.labels.size()},
             {"names", m.names},
             {"records", m.records},
             {"empty_records", m.empty_records},
             {"files", m.files},
             {"chunk_base", ChunkBaseText(m)},
             {"missing", m.missing},
             {"refused", m.refused},
             {"ms", ms}};
  fields.insert(fields.end(), none.begin(), none.end());
  env.Event(m.refused != 0 ? EventLevel::kWarn : EventLevel::kInfo, "cdn_mounted", fields);
}

// The limits of this run, from the device: one line with every number, or the missing fact by name.
Limits LoadLimits(Env& env, const std::string& dir) {
  DeviceFacts facts;
  env.QueryDevice(dir, &facts);
  const Limits limits = ComputeLimits(facts, ManifestEntryMemoryBytes());
  if (limits.ok) {
    env.Event(EventLevel::kInfo, "cdn_limits",
              {{"result", "ok"},
               {"manifest_text_bytes", limits.manifest_text_bytes},
               {"max_entries", limits.max_entries},
               {"max_object_bytes", limits.max_object_bytes},
               {"store_file_bytes", limits.store_file_bytes},
               {"max_store_files", limits.max_store_files},
               {"storage_budget", limits.storage_budget},
               {"free_bytes", facts.free_bytes},
               {"memory_available", facts.memory_available},
               {"fd_soft_limit", facts.fd_soft_limit},
               {"fds_open", facts.fds_open}});
  } else {
    env.Event(EventLevel::kWarn, "cdn_limits",
              {{"result", "refused"}, {"reason", limits.reason},
               {"consequence", "no CDN asset is fetched or served this run; the game's own data shows"}});
  }
  return limits;
}

std::unique_ptr<Store> OpenStoreLogged(Env& env, const std::string& dir, const Limits& limits, const char* source) {
  StoreOpenInfo info;
  std::unique_ptr<Store> store = Store::Open(env, dir, limits, &info);
  const bool damaged = std::string(info.index) == "invalid" || std::string(info.index) == "tail_cut" || info.dropped != 0;
  std::vector<Field> fields = {{"source", source}, {"index", info.index}, {"blobs", info.blobs},
                               {"files", info.files}, {"dropped", info.dropped}, {"bytes", info.bytes}};
  if (damaged) fields.push_back({"consequence", "the objects the index no longer names are fetched again"});
  env.Event(damaged ? EventLevel::kWarn : EventLevel::kInfo, "cdn_store", fields);
  return store;
}

// Says what the validator left out of the manifest: the first few one by one, then one line a rule with its count.
void ReportRefusals(Env& env, const Manifest& manifest, const char* source) {
  std::map<Rule, std::size_t> counts;
  std::size_t said = 0;
  for (const Refusal& r : manifest.refused) {
    ++counts[r.rule];
    if (said >= kSingleLines) continue;
    ++said;
    env.Event(EventLevel::kWarn, "cdn_refused",
              {{"source", source}, {"label", r.label}, {"index", r.index}, {"type", FormatHex64(r.type)}, {"name", FormatHex64(r.name)},
               {"rule", RuleName(r.rule)}, {"consequence", "this record is neither fetched nor served"}});
  }
  if (manifest.refused.size() > kSingleLines) {
    for (const auto& kv : counts) {
      env.Event(EventLevel::kWarn, "cdn_refused",
                {{"source", source}, {"rule", RuleName(kv.first)}, {"count", kv.second},
                 {"consequence", "these records are neither fetched nor served"}});
    }
  }
}

struct Downloaded {
  bool ok = false;
  bool cancelled = false;
  bool stop_run = false;  // the failure is the store's, so it will be the next object's as well
  const char* reason = "";
  long status = 0;
  int code = 0;
};

// Streams one object into the store while hashing it, and names it in the index when its size, its frame header and its SHA-256 are
// the manifest's. A transfer that gives more than `size` bytes is ended. Any failure gives the bytes back. This is the one place an
// object is hashed.
Downloaded Download(Env& env, const RunConfig& config, Store& store, const Entry& e, const std::function<bool()>& keep_going) {
  Downloaded out;
  // The store holds an object under this name at another size than the manifest states: it is refused by name, and nothing is requested.
  if (store.Has(e.sha256) && !store.HoldsSized(e.sha256, e.size, e.bytes)) {
    out.reason = "held_size_mismatch";
    return out;
  }
  const char* why = "";
  std::unique_ptr<Store::Writer> writer = store.Begin(e.size, &why);
  std::unique_ptr<Hasher> hasher = env.NewHasher();
  if (!writer || !hasher) {
    out.reason = !writer ? why : "hasher";
    out.stop_run = !writer && std::string(why) != "too_large";
    return out;
  }
  std::uint64_t got = 0;
  long status = 0;
  bool tooMuch = false, writeFailed = false;
  Rule rule = Rule::kNone;
  unsigned char head[kMaxFrameHeaderBytes];
  const std::size_t headNeeded = static_cast<std::size_t>(std::min<std::uint64_t>(kMaxFrameHeaderBytes, e.size));
  std::size_t headHave = 0;
  bool headChecked = false;
  const HttpResult http = env.Get(config.base_url + kBlobPrefix + e.sha256,
                                  [&](long st, const void* data, std::size_t length) {
                                    status = st;
                                    if (!IsWholeBody(st)) return false;
                                    if (got + length > e.size) {
                                      tooMuch = true;
                                      return false;
                                    }
                                    if (headHave < headNeeded) {
                                      const std::size_t take = std::min(length, headNeeded - headHave);
                                      std::copy_n(static_cast<const unsigned char*>(data), take, head + headHave);
                                      headHave += take;
                                    }
                                    if (!headChecked && headHave == headNeeded) {
                                      headChecked = true;
                                      rule = CheckFrame(head, headHave, e.size, e.bytes);
                                      if (rule != Rule::kNone) return false;
                                    }
                                    hasher->Update(data, length);
                                    if (!writer->Write(data, length)) {
                                      writeFailed = true;
                                      return false;
                                    }
                                    got += length;
                                    return true;
                                  },
                                  keep_going);
  out.status = status != 0 ? status : http.status;
  out.code = http.code;
  if (http.cancelled) {
    writer->Abort();
    out.cancelled = true;
    out.reason = "cancelled";
    return out;
  }
  if (rule != Rule::kNone) {
    writer->Abort();
    out.reason = RuleName(rule);
    return out;
  }
  if (tooMuch) {
    writer->Abort();
    out.reason = "too_large";
    return out;
  }
  if (writeFailed) {
    writer->Abort();
    out.reason = "write_failed";
    out.stop_run = true;
    return out;
  }
  if (!IsWholeBody(out.status)) {
    writer->Abort();
    out.reason = out.status == 0 ? "transport" : "http_status";
    return out;
  }
  if (!http.completed) {
    writer->Abort();
    out.reason = "transport";
    return out;
  }
  if (got != e.size) {
    writer->Abort();
    out.reason = "size_mismatch";
    return out;
  }
  if (!headChecked) {  // an object shorter than any header never reached the check above
    rule = CheckFrame(head, headHave, e.size, e.bytes);
    if (rule != Rule::kNone) {
      writer->Abort();
      out.reason = RuleName(rule);
      return out;
    }
  }
  if (hasher->Hex() != e.sha256) {
    writer->Abort();
    out.reason = "sha256_mismatch";
    return out;
  }
  if (!writer->Commit(e.sha256, e.bytes)) {
    out.reason = "commit_failed";
    out.stop_run = true;
    return out;
  }
  out.ok = true;
  return out;
}

// The CDN store as the first-run prefetch's store (cdn_prefetch.h). The store's writer wants the decompressed size at Commit, which
// the prefetch's Open does not carry, so the adapter keeps each object's size from the plan it was built from.
class PrefetchSink : public ObjectSink {
 public:
  PrefetchSink(std::unique_ptr<Store::Writer> writer, std::string sha256, std::uint64_t size, std::uint64_t bytes)
      : writer_(std::move(writer)), sha256_(std::move(sha256)), size_(size), bytes_(bytes),
        head_needed_(static_cast<std::size_t>(std::min<std::uint64_t>(kMaxFrameHeaderBytes, size))) {}
  StoreStatus Write(const void* data, std::size_t length) override {
    // The frame's own header is held to the manifest as the background fetch holds it (Download): the SHA-256 covers the frame's bytes,
    // not the decompressed size its header states.
    if (head_have_ < head_needed_) {
      const std::size_t take = std::min(length, head_needed_ - head_have_);
      std::copy_n(static_cast<const unsigned char*>(data), take, head_ + head_have_);
      head_have_ += take;
    }
    if (!head_checked_ && head_have_ == head_needed_ && CheckFrame(head_, head_have_, size_, bytes_) != Rule::kNone) return StoreStatus::kIoError;
    head_checked_ = head_checked_ || head_have_ == head_needed_;
    return writer_->Write(data, length) ? StoreStatus::kOk : StoreStatus::kIoError;
  }
  StoreStatus Commit() override {
    if (!head_checked_ && CheckFrame(head_, head_have_, size_, bytes_) != Rule::kNone) return StoreStatus::kIoError;  // shorter than any header
    committed_ = true;
    return writer_->Commit(sha256_, bytes_) ? StoreStatus::kOk : StoreStatus::kIoError;
  }
  void Abort() override {
    if (!committed_) writer_->Abort();
  }

 private:
  std::unique_ptr<Store::Writer> writer_;
  std::string sha256_;
  std::uint64_t size_;
  std::uint64_t bytes_;
  std::size_t head_needed_;
  unsigned char head_[kMaxFrameHeaderBytes] = {};
  std::size_t head_have_ = 0;
  bool head_checked_ = false;
  bool committed_ = false;
};

class StorePrefetch : public PrefetchStore {
 public:
  StorePrefetch(Store& store, const std::vector<Entry>& objects) : store_(store) {
    for (const Entry& e : objects) bytes_[e.sha256] = e.bytes;
  }
  bool Has(const std::string& sha256) const override { return store_.Has(sha256); }
  bool HoldsSized(const std::string& sha256, std::uint64_t size, std::uint64_t bytes) const override {
    return store_.HoldsSized(sha256, size, bytes);
  }
  std::unique_ptr<ObjectSink> Open(const std::string& sha256, std::uint64_t size, StoreStatus* status) override {
    const char* why = "";
    std::unique_ptr<Store::Writer> writer = store_.Begin(size, &why);
    if (!writer) {
      // no_store_file: the store has no room left for another file of objects; too_large and open_failed are the store's other refusals.
      *status = std::string(why) == "no_store_file" ? StoreStatus::kNoSpace : StoreStatus::kIoError;
      return nullptr;
    }
    return std::unique_ptr<ObjectSink>(new PrefetchSink(std::move(writer), sha256, size, bytes_[sha256]));
  }

 private:
  Store& store_;
  std::map<std::string, std::uint64_t> bytes_;
};

bool ReadSavedManifest(Env& env, const std::string& dir, const Limits& limits, std::string* frame) {
  return env.ReadSmallFile(ManifestCopyPath(dir), frame, static_cast<std::size_t>(MaxManifestFrameBytes(limits.manifest_text_bytes)));
}

}  // namespace

bool MountFromCache(Env& env, const RunConfig& config, const Publish& publish) {
  const Limits limits = LoadLimits(env, config.dir);
  if (!limits.ok) return false;
  std::unique_ptr<Store> store = OpenStoreLogged(env, config.dir, limits, "boot");
  if (!store) return false;  // only when the limits are not ok, which LoadLimits has said
  if (store->blob_count() == 0) {
    // The normal first launch: nothing was ever fetched.
    env.Event(EventLevel::kInfo, "cdn_mounted",
              {{"source", "boot"}, {"result", "none"}, {"reason", "store_empty"},
               {"consequence", "the game's own data is read until a fetch stores objects"}});
    return false;
  }
  std::string frame;
  if (!ReadSavedManifest(env, config.dir, limits, &frame)) {
    // Objects are stored but no fetch ever finished far enough to save the manifest that names them.
    env.Event(EventLevel::kWarn, "cdn_manifest",
              {{"source", "saved"}, {"result", "absent"}, {"reason", "no_saved_manifest"}, {"blobs", store->blob_count()},
               {"consequence", "no CDN asset is served until a fetch saves a manifest"}});
    return false;
  }
  Manifest manifest;
  const ParseResult parsed = ParseManifestFrame(frame, limits, &manifest);
  if (parsed.status != ParseStatus::kOk) {
    env.Event(EventLevel::kWarn, "cdn_manifest",
              {{"source", "saved"}, {"result", "refused"}, {"reason", ParseStatusName(parsed.status)},
               {"consequence", "no CDN asset is served until a fetch saves a manifest again"}});
    return false;
  }
  ReportRefusals(env, manifest, "boot");
  const std::uint64_t mountStarted = env.NowMs();
  std::unique_ptr<Mounted> mounted(new Mounted(Mount(env, *store, manifest, config.platform, config.policy)));
  const std::uint64_t mountMs = env.NowMs() - mountStarted;
  SayMounted(env, "boot", *mounted, mountMs);
  if (!mounted->shadow) return false;
  publish(std::move(mounted));
  return true;
}

RunResult FetchAndMount(Env& env, const RunConfig& config, const Publish& publish, const std::function<bool()>& keep_going) {
  RunResult result;
  const std::uint64_t started = env.NowMs();
  const auto done = [&](const char* resultText) {
    env.Event(result.failed != 0 || std::string(result.stopped) == "write_failed" ? EventLevel::kWarn : EventLevel::kInfo, "cdn_done",
              {{"result", resultText},
               {"fetched", result.fetched},
               {"cached", result.cached},
               {"failed", result.failed},
               {"refused", result.refused},
               {"bytes", result.bytes},
               {"ms", env.NowMs() - started}});
  };
  if (!env.MakeDirs(config.dir)) {
    env.Event(EventLevel::kError, "cdn_done", {{"result", "failed"}, {"reason", "cache_dir"}});
    return result;
  }
  const Limits limits = LoadLimits(env, config.dir);
  if (!limits.ok) {
    result.stopped = "limits";
    done("limits");
    return result;
  }
  std::unique_ptr<Store> store = OpenStoreLogged(env, config.dir, limits, "fetch");
  env.Event(EventLevel::kInfo, "cdn_start",
            {{"source", config.source},
             {"host", HostOf(config.base_url)},
             {"platform", config.platform.name},
             {"labels", StatesText(config.policy, nullptr)},
             {"required_only", config.policy.required_only ? 1 : 0}});

  // Step 1: the manifest, one zstd frame. Its header states the size of the text; that size is held to the device's memory as soon as the
  // header has arrived, and the rest of the body is not downloaded when it does not fit.
  std::string body;
  long status = 0;
  const char* early = nullptr;  // a refusal made from the header alone
  std::uint64_t declared = 0;
  bool haveDeclared = false;
  const std::uint64_t maxFrame = MaxManifestFrameBytes(limits.manifest_text_bytes);
  const std::string manifestUrl = config.base_url + config.manifest_path;  // every cdn_manifest record names what it asked for
  const HttpResult http = env.Get(manifestUrl,
                                  [&](long st, const void* data, std::size_t length) {
                                    status = st;
                                    if (!IsWholeBody(st)) return false;
                                    const std::uint64_t cap = haveDeclared ? MaxManifestFrameBytes(declared) : maxFrame;
                                    if (body.size() + length > cap) {
                                      early = "too_large";
                                      return false;
                                    }
                                    body.append(static_cast<const char*>(data), length);
                                    if (!haveDeclared) {
                                      const unsigned char* b = reinterpret_cast<const unsigned char*>(body.data());
                                      const std::size_t n = std::min<std::size_t>(body.size(), kMaxFrameHeaderBytes);
                                      switch (FrameContentSize(b, n, &declared)) {
                                        case Rule::kNone:
                                          haveDeclared = true;
                                          if (declared > limits.manifest_text_bytes) {
                                            early = "too_large";
                                            return false;
                                          }
                                          break;
                                        case Rule::kContentSizeMissing:
                                          early = "no_content_size";
                                          return false;
                                        default:  // not a frame start, or a header not all here yet
                                          if (n >= kZstdProbeBytes && FrameHeaderBytes(b, n) == 0) {
                                            early = "not_zstd";
                                            return false;
                                          }
                                          break;
                                      }
                                    }
                                    return true;
                                  },
                                  keep_going);
  if (http.cancelled) {
    result.stopped = "cancelled";
    done("cancelled");
    return result;
  }
  const long httpStatus = status != 0 ? status : http.status;
  const char* manifestFailure = early;
  if (manifestFailure == nullptr) {
    if (!IsWholeBody(httpStatus)) {
      manifestFailure = httpStatus == 0 ? "transport" : "http_status";
    } else if (!http.completed) {
      manifestFailure = "transport";
    }
  }
  if (manifestFailure != nullptr) {
    std::vector<Field> fields = {{"result", "failed"}, {"reason", manifestFailure}, {"url", manifestUrl}, {"http_status", httpStatus},
                                 {"code", http.code}};
    if (haveDeclared) fields.push_back({"declared_bytes", declared});
    if (haveDeclared && std::string(manifestFailure) == "too_large") fields.push_back({"limit_bytes", limits.manifest_text_bytes});
    fields.push_back({"consequence", "no CDN asset is fetched this run; what the store holds stays mounted"});
    env.Event(EventLevel::kWarn, "cdn_manifest", fields);
    const bool refused = std::string(manifestFailure) == "not_zstd" || std::string(manifestFailure) == "no_content_size";
    result.stopped = refused ? "manifest_refused" : kStoppedManifestFailed;
    done(result.stopped);
    return result;
  }
  Manifest manifest;
  const ParseResult parsed = ParseManifestFrame(body, limits, &manifest);
  if (parsed.status != ParseStatus::kOk) {
    std::vector<Field> fields = {{"result", "refused"}, {"reason", ParseStatusName(parsed.status)}, {"url", manifestUrl},
                                 {"http_status", httpStatus}};
    if (!parsed.label.empty()) {
      fields.push_back({"label", parsed.label});
      fields.push_back({"index", parsed.index});
    }
    fields.push_back({"consequence", "no CDN asset is fetched this run; what the store holds stays mounted"});
    env.Event(EventLevel::kWarn, "cdn_manifest", fields);
    result.stopped = parsed.status == ParseStatus::kTooLarge ? kStoppedManifestFailed : "manifest_refused";
    done(result.stopped);
    return result;
  }
  result.manifest_loaded = true;
  result.refused = manifest.refused.size();
  std::string manifestSha;
  if (std::unique_ptr<Hasher> hasher = env.NewHasher()) {
    hasher->Update(body.data(), body.size());
    manifestSha = hasher->Hex();
  }

  // Step 2: the plan, before any request. What the store holds is a lookup in the index: nothing is read or hashed.
  Plan plan = MakePlan(manifest, config.platform, config.policy, limits, store->held_bytes(),
                       [&](const Entry& e) { return store->HoldsSized(e.sha256, e.size, e.bytes); });
  env.Event(EventLevel::kInfo, "cdn_manifest",
            {{"result", "loaded"},
             {"url", manifestUrl},
             {"http_status", httpStatus},
             {"frame_bytes", body.size()},
             {"bytes", declared},
             {"sha256", manifestSha},
             {"labels", manifest.labels.size()},
             {"entries", manifest.entries.size()},
             {"empty_records", manifest.empties.size()},
             {"refused", manifest.refused.size()},
             {"platform_entries", plan.platform_entries},
             {"unknown_fields", manifest.unknown_fields}});
  ReportRefusals(env, manifest, "fetch");
  for (const PlanLabel& pl : plan.labels) {
    std::vector<Field> fields = {{"label", pl.label},   {"state", LabelStateName(pl.state)}, {"entries", pl.entries},
                                 {"cached", pl.cached}, {"to_fetch", pl.to_fetch},           {"fetch_bytes", pl.fetch_bytes}};
    if (pl.skipped != SkipReason::kNone) {
      fields.push_back({"skipped", SkipReasonName(pl.skipped)});
      fields.push_back({"consequence", "this label serves nothing from the CDN"});
    }
    const bool loud = pl.skipped == SkipReason::kOverStorage || pl.skipped == SkipReason::kOverFiles;
    env.Event(loud ? EventLevel::kWarn : EventLevel::kInfo, "cdn_plan", fields);
    for (const PairRefusal& r : pl.refused) {
      env.Event(EventLevel::kWarn, "cdn_plan",
                {{"label", pl.label}, {"name", FormatHex64(r.name)}, {"result", "refused"}, {"reason", r.reason},
                 {"consequence", "the stock asset shows for this name"}});
    }
  }

  // Step 3: fetch what the store does not hold, an object once however many entries name it.
  FetchProgress progress;
  progress.total_bytes = plan.fetch_bytes;
  progress.total_blobs = plan.to_fetch;
  const auto report = [&]() {
    if (config.progress) config.progress(progress);
  };
  report();
  if (config.first_run) {
    // The first-run hold: every object of every served label, one after another, ending at the first one that cannot be had.
    PrefetchConfig prefetch;
    prefetch.base_url = config.base_url;
    for (const PlanLabel& pl : plan.labels) {
      if (pl.skipped != SkipReason::kNone) continue;
      for (const PlanItem& item : pl.items) prefetch.objects.push_back(item.entry);
    }
    prefetch.keep_going = keep_going;
    prefetch.progress = [&](std::uint64_t done, std::uint64_t) {
      progress.done_bytes = done;
      report();
    };
    StorePrefetch adapter(*store, prefetch.objects);
    const PrefetchResult fetched = Prefetch(env, adapter, prefetch);
    result.fetched = fetched.fetched;
    result.cached = fetched.held;
    result.bytes = fetched.bytes_fetched;
    if (fetched.status != PrefetchStatus::kComplete) {
      // What was stored stays and the next launch resumes from it; the manifest is not saved and nothing new is mounted.
      result.failed = 1;
      result.stopped = fetched.status == PrefetchStatus::kCancelled ? "cancelled" : PrefetchStatusName(fetched.status);
      done(result.stopped);
      return result;
    }
  }
  std::size_t lastDecile = 0;
  std::set<std::string> fetchedShas, failedShas;
  std::size_t failureLines = 0;
  bool stoppedRun = false;
  for (const PlanLabel& pl : plan.labels) {
    if (pl.skipped != SkipReason::kNone || stoppedRun || config.first_run) continue;
    for (const PlanItem& item : pl.items) {
      if (item.cached) continue;
      const Entry& e = item.entry;
      if (fetchedShas.count(e.sha256) != 0 || failedShas.count(e.sha256) != 0) continue;
      if (!keep_going()) {
        result.stopped = "cancelled";
        done("cancelled");
        return result;
      }
      const std::uint64_t blobStarted = env.NowMs();
      const Downloaded d = Download(env, config, *store, e, keep_going);
      if (d.cancelled) {
        result.stopped = "cancelled";
        done("cancelled");
        return result;
      }
      if (d.ok) {
        fetchedShas.insert(e.sha256);
        ++result.fetched;
        result.bytes += e.size;
        progress.done_bytes += e.size;
        ++progress.done_blobs;
        env.Event(EventLevel::kDebug, "cdn_blob",
                  {{"result", "stored"}, {"label", e.label}, {"type", FormatHex64(e.type)}, {"name", FormatHex64(e.name)},
                   {"sha256", e.sha256}, {"size", e.size}, {"ms", env.NowMs() - blobStarted}});
      } else {
        failedShas.insert(e.sha256);
        ++result.failed;
        ++progress.failed_blobs;
        // The first few failures are said one by one at the label's severity; the rest are Debug, and cdn_done counts them.
        const EventLevel level = failureLines < kSingleLines ? (pl.state == LabelState::kRequired ? EventLevel::kError : EventLevel::kWarn)
                                                              : EventLevel::kDebug;
        ++failureLines;
        env.Event(level, "cdn_blob",
                  {{"result", "failed"}, {"label", e.label}, {"type", FormatHex64(e.type)}, {"name", FormatHex64(e.name)},
                   {"sha256", e.sha256}, {"reason", d.reason}, {"http_status", d.status}, {"code", d.code},
                   {"consequence", "the stock asset shows for this label until a later launch fetches it"}});
        if (d.stop_run) {
          result.stopped = d.reason;
          stoppedRun = true;
        }
      }
      report();
      if (progress.total_bytes != 0) {
        const std::size_t decile = static_cast<std::size_t>(progress.done_bytes * kProgressSteps / progress.total_bytes);
        if (decile > lastDecile) {
          lastDecile = decile;
          env.Event(EventLevel::kInfo, "cdn_progress",
                    {{"done_bytes", progress.done_bytes}, {"total_bytes", progress.total_bytes}, {"done_blobs", progress.done_blobs},
                     {"total_blobs", progress.total_blobs}, {"failed_blobs", progress.failed_blobs}});
        }
      }
      if (stoppedRun) break;
    }
  }
  std::set<std::string> cachedShas;
  for (const PlanLabel& pl : plan.labels) {
    for (const PlanItem& item : pl.items) {
      if (item.cached) cachedShas.insert(item.entry.sha256);
    }
  }
  if (!config.first_run) result.cached = cachedShas.size();
  if (stoppedRun) {
    // The store failed (a full disk, a file that would not open): what was stored stays, the manifest is not saved and nothing new is
    // mounted, so the next launch plans again from the same store.
    done(result.stopped);
    return result;
  }

  // Step 4: save the manifest as it was published, so a launch with no network mounts what the store holds, and mount.
  std::string previous;
  const bool hadSaved = ReadSavedManifest(env, config.dir, limits, &previous);
  const bool changed = !hadSaved || previous != body;
  if (changed && !env.WriteFileAtomically(ManifestCopyPath(config.dir), body)) {
    env.Event(EventLevel::kError, "cdn_manifest",
              {{"result", "save_failed"}, {"consequence", "the next launch mounts the manifest saved before this one, or none"}});
  }
  if (changed || result.fetched != 0) {
    const std::uint64_t mountStarted = env.NowMs();
    std::unique_ptr<Mounted> mounted(new Mounted(Mount(env, *store, manifest, config.platform, config.policy)));
    const std::uint64_t mountMs = env.NowMs() - mountStarted;
    if (mounted->shadow || mounted->refused != 0 || mounted->missing != 0) SayMounted(env, "fetch", *mounted, mountMs);
    if (mounted->shadow) {
      publish(std::move(mounted));
      result.published = true;
    }
  }
  done("ok");
  return result;
}

}  // namespace nevr_cdn

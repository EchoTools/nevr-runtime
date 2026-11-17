#include "cdn/cdn_run.h"

#include "cdn/cdn_boot_fetch.h"
// ResolveManifestPath (the v2-or-v1 manifest choice) is the service's; the boot fetch asks it under its own ceiling (ClientBoot).
#include "cdn/cdn_service.h"
#include "core/cargo_session_pin.h"

#include "cdn/cdn_prefetch.h"
#include "cdn/cdn_scripts.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
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
  for (const LabelDefault& d : kLabelDefaults) states[d.label] = StateOf(d.label, policy, policy.role);
  for (const LabelDefault& d : kServerLabelDefaults) states[d.label] = StateOf(d.label, policy, policy.role);
  for (const auto& kv : policy.overrides) states[kv.first] = kv.second;
  if (manifest != nullptr) {
    for (const std::string& label : manifest->labels) states[label] = StateOf(label, policy, policy.role);
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

// `ms` is the mount's own time. It reads no object: the boot mount is the saved manifest and the blob index. `bootOnlyKept` is set on a
// fetch whose run has boot-only labels (RunConfig::boot_only); unset, the line has no such field.
void SayMounted(Env& env, const char* source, const Mounted& m, std::uint64_t ms,
                const std::optional<std::size_t>& bootOnlyKept = std::nullopt) {
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
  if (bootOnlyKept) fields.push_back({"boot_only_kept", *bootOnlyKept});
  fields.insert(fields.end(), none.begin(), none.end());
  env.Event(m.refused != 0 ? EventLevel::kWarn : EventLevel::kInfo, "cdn_mounted", fields);
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
               {"rule", RuleName(r.rule)},
               {"consequence", IsScriptRule(r.rule) ? "this label is not mounted; its levels run on the game's own data"
                                                    : "this record is neither fetched nor served"}});
  }
  if (manifest.refused.size() > kSingleLines) {
    for (const auto& kv : counts) {
      env.Event(EventLevel::kWarn, "cdn_refused",
                {{"source", source}, {"rule", RuleName(kv.first)}, {"count", kv.second},
                 {"consequence", "these records are neither fetched nor served"}});
    }
  }
}

}  // namespace

namespace detail {

// The gate's refusals for a manifest, with no event: what GatedPolicy applies and reports. The registrar runs first, so the gate knows
// what it held.
std::vector<GatedLabel> GatedLabelsOf(Env& env, const RunConfig& config, const Manifest& manifest) {
  if (config.script_dir.empty()) return {};
  ScriptRegistration registration;
  if (config.registrar) registration = config.registrar(manifest, config.platform);
  const auto present = [&env, &config](std::uint64_t hash) {
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    if (!env.OpenBlob(config.script_dir + ScriptHash16(hash) + ".dll", &handle, &size)) return false;
    env.CloseBlob(handle);
    return true;
  };
  const auto held = [&registration](const std::string& label, std::uint64_t hash) {
    return std::find(registration.held.begin(), registration.held.end(), std::make_pair(label, hash)) != registration.held.end();
  };
  // A name in the label's own `scripts` list must be held, a file on disk under it does not count (cdn_script_gate.h, X8).
  const auto listed = [&manifest, &config](const std::string& label, std::uint64_t name) {
    return ListsScript(manifest, config.platform, label, name);
  };
  std::vector<GatedLabel> gated = GateRefusals(registration.refused);
  for (GatedLabel& g : GateScripts(manifest, config.platform, present, held, listed)) {
    const bool refusedAlready = std::any_of(gated.begin(), gated.end(), [&g](const GatedLabel& r) { return r.label == g.label; });
    if (!refusedAlready) gated.push_back(std::move(g));
  }
  return gated;
}

// The policy a mount uses: the config's, with every label whose scripts are not delivered off (cdn_script_gate.h). `source` is "boot" or
// "fetch"; `gated`, when not null, receives the refusals the gate made.
Policy GatedPolicy(Env& env, const RunConfig& config, const Manifest& manifest, const char* source, std::vector<GatedLabel>* gated) {
  if (config.script_dir.empty()) {
    env.Event(EventLevel::kInfo, "cdn_scripts_gate",
              {{"result", "off"}, {"reason", "no_script_dir"}, {"source", source},
               {"consequence", "no label is checked for the scripts it names before it is mounted"}});
    if (gated != nullptr) gated->clear();
    return config.policy;
  }
  std::vector<GatedLabel> refusals = GatedLabelsOf(env, config, manifest);
  SayGated(env, refusals, config.policy, config.script_dir, source);
  Policy applied = ApplyGate(refusals, config.policy);
  if (gated != nullptr) *gated = std::move(refusals);
  return applied;
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

StorePrefetch::StorePrefetch(Store& store, const std::vector<Entry>& objects) : store_(store) {
  for (const Entry& e : objects) bytes_[e.sha256] = e.bytes;
}

bool StorePrefetch::Has(const std::string& sha256) const { return store_.Has(sha256); }

bool StorePrefetch::HoldsSized(const std::string& sha256, std::uint64_t size, std::uint64_t bytes) const {
  return store_.HoldsSized(sha256, size, bytes);
}

std::unique_ptr<ObjectSink> StorePrefetch::Open(const std::string& sha256, std::uint64_t size, StoreStatus* status) {
  const char* why = "";
  std::unique_ptr<Store::Writer> writer = store_.Begin(size, &why);
  if (!writer) {
    // no_store_file: the store has no room left for another file of objects; too_large and open_failed are the store's other refusals.
    *status = std::string(why) == "no_store_file" ? StoreStatus::kNoSpace : StoreStatus::kIoError;
    return nullptr;
  }
  return std::unique_ptr<ObjectSink>(new PrefetchSink(std::move(writer), sha256, size, bytes_[sha256]));
}

}  // namespace detail

namespace {

bool ReadSavedManifest(Env& env, const std::string& dir, const Limits& limits, std::string* frame) {
  return env.ReadSmallFile(ManifestCopyPath(dir), frame, static_cast<std::size_t>(MaxManifestFrameBytes(limits.manifest_text_bytes)));
}

// The records Mount reads for `platform` and the labels `wanted` accepts, one text key each, sorted (SameMountRecords). A record of 0
// bytes of no platform (a top-level record) belongs to the platform the plan is made for, so it is kept for every platform.
std::vector<std::string> MountKeys(const Manifest& m, const Platform& platform, const std::function<bool(const std::string&)>& wanted) {
  std::vector<std::string> keys;
  for (const Entry& e : m.entries) {
    if (e.platform != platform.name || !wanted(e.label)) continue;
    keys.push_back("e|" + e.label + "|" + FormatHex64(e.type) + "|" + FormatHex64(e.name) + "|" + e.sha256 + "|" +
                   std::to_string(e.size) + "|" + std::to_string(e.bytes) + "|" + std::to_string(e.align));
  }
  for (const EmptyRecord& z : m.empties) {
    if ((z.platform != platform.name && !z.platform.empty()) || !wanted(z.label)) continue;
    keys.push_back("z|" + z.label + "|" + FormatHex64(z.type) + "|" + FormatHex64(z.name) + "|" + std::to_string(z.align));
  }
  for (const ScriptEntry& s : m.scripts) {
    if (s.platform != platform.name || !wanted(s.label)) continue;
    keys.push_back("s|" + s.label + "|" + FormatHex64(s.name) + "|" + FormatHex64(s.stock_source) + "|" + s.stock_sha256);
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

// Whether the records of one label differ between two manifests, for `platform`.
bool LabelRecordsDiffer(const Manifest& a, const Manifest& b, const Platform& platform, const std::string& label) {
  const auto only = [&label](const std::string& l) { return l == label; };
  return MountKeys(a, platform, only) != MountKeys(b, platform, only);
}

}  // namespace

Manifest KeepBootOnly(const Manifest& fetched, const Manifest& boot, const std::vector<std::string>& boot_only) {
  const std::set<std::string> held(boot_only.begin(), boot_only.end());
  const auto isHeld = [&held](const std::string& label) { return held.count(label) != 0; };
  Manifest out = fetched;
  out.entries.erase(std::remove_if(out.entries.begin(), out.entries.end(), [&isHeld](const Entry& e) { return isHeld(e.label); }),
                    out.entries.end());
  out.empties.erase(std::remove_if(out.empties.begin(), out.empties.end(), [&isHeld](const EmptyRecord& z) { return isHeld(z.label); }),
                    out.empties.end());
  out.scripts.erase(std::remove_if(out.scripts.begin(), out.scripts.end(), [&isHeld](const ScriptEntry& s) { return isHeld(s.label); }),
                    out.scripts.end());
  out.refused.erase(std::remove_if(out.refused.begin(), out.refused.end(), [&isHeld](const Refusal& r) { return isHeld(r.label); }),
                    out.refused.end());
  for (const Entry& e : boot.entries) {
    if (isHeld(e.label)) out.entries.push_back(e);
  }
  for (const EmptyRecord& z : boot.empties) {
    if (isHeld(z.label)) out.empties.push_back(z);
  }
  for (const ScriptEntry& s : boot.scripts) {
    if (isHeld(s.label)) out.scripts.push_back(s);
  }
  for (const Refusal& r : boot.refused) {
    if (isHeld(r.label)) out.refused.push_back(r);
  }
  for (const std::string& label : boot.labels) {
    if (isHeld(label) && std::find(out.labels.begin(), out.labels.end(), label) == out.labels.end()) out.labels.push_back(label);
  }
  return out;
}

std::vector<std::string> BootOnlyLabels(const std::vector<std::string>& fixed, const Manifest& boot, const Manifest& fetched,
                                        const Platform& platform) {
  std::vector<std::string> out;
  if (fixed.empty()) return out;
  std::set<std::string> labels(fixed.begin(), fixed.end());
  const Manifest* const sets[] = {&boot, &fetched};
  for (const Manifest* m : sets) {
    for (const ScriptEntry& s : m->scripts) {
      if (s.platform == platform.name) labels.insert(s.label);
    }
  }
  out.assign(labels.begin(), labels.end());
  return out;
}

bool SameMountRecords(const Manifest& a, const Manifest& b, const Platform& platform) {
  const auto all = [](const std::string&) { return true; };
  return MountKeys(a, platform, all) == MountKeys(b, platform, all);
}

namespace {

// What the boot mount built, without its shadow: the publisher owns the shadow, and BootMount keeps the counts and per_label.
Mounted MetadataOf(const Mounted& m) {
  Mounted out;
  out.labels = m.labels;
  out.names = m.names;
  out.records = m.records;
  out.empty_records = m.empty_records;
  out.missing = m.missing;
  out.refused = m.refused;
  out.files = m.files;
  out.per_label = m.per_label;
  return out;
}

}  // namespace

namespace {

// The boot mount of a store the caller has open: MountFromCache opens it and logs the open, and the client's boot half (ClientBoot) mounts
// through the store its decision read, so each boot half logs its limits and its store once. `store` null: nothing is mounted.
bool MountStore(Env& env, const RunConfig& config, const Limits& limits, Store* store, const Publish& publish, BootMount* out) {
  BootMount filled;
  // Every return fills `out` with what this mount built, even when it built nothing (PLAN-v7 5.4 item 4).
  const auto finish = [&filled, out](bool published) {
    filled.published = published;
    if (out != nullptr) *out = std::move(filled);
    return published;
  };
  if (store == nullptr) return finish(false);  // the limits did not hold, which detail::LoadLimits has said
  if (store->blob_count() == 0) {
    // The normal first launch: nothing was ever fetched.
    env.Event(EventLevel::kInfo, "cdn_mounted",
              {{"source", "boot"}, {"result", "none"}, {"reason", "store_empty"},
               {"consequence", "the game's own data is read until a fetch stores objects"}});
    return finish(false);
  }
  std::string frame;
  if (!ReadSavedManifest(env, config.dir, limits, &frame)) {
    // Objects are stored but no fetch ever finished far enough to save the manifest that names them.
    env.Event(EventLevel::kWarn, "cdn_manifest",
              {{"source", "saved"}, {"result", "absent"}, {"reason", "no_saved_manifest"}, {"blobs", store->blob_count()},
               {"consequence", "no CDN asset is served until a fetch saves a manifest"}});
    return finish(false);
  }
  Manifest manifest;
  const ParseResult parsed = ParseManifestBody(frame, limits, &manifest);
  if (parsed.status != ParseStatus::kOk) {
    env.Event(EventLevel::kWarn, "cdn_manifest",
              {{"source", "saved"}, {"result", "refused"}, {"reason", ParseStatusName(parsed.status)},
               {"consequence", "no CDN asset is served until a fetch saves a manifest again"}});
    return finish(false);
  }
  ReportRefusals(env, manifest, "boot");
  const std::uint64_t mountStarted = env.NowMs();
  std::vector<GatedLabel> gated;
  const Policy policy = detail::GatedPolicy(env, config, manifest, "boot", &gated);
  std::unique_ptr<Mounted> mounted(new Mounted(Mount(env, *store, manifest, config.platform, policy)));
  const std::uint64_t mountMs = env.NowMs() - mountStarted;
  SayMounted(env, "boot", *mounted, mountMs);
  filled.mounted = MetadataOf(*mounted);
  filled.gated = std::move(gated);
  if (!mounted->shadow) return finish(false);
  // The manifest of the set published on this thread: the fetch's boot-only labels keep its records (F1, PLAN-v7 5.4 item 4).
  filled.boot = std::move(manifest);
  publish(std::move(mounted));
  return finish(true);
}

}  // namespace

bool MountFromCache(Env& env, const RunConfig& config, const Publish& publish, BootMount* out) {
  const Limits limits = detail::LoadLimits(env, config.dir);
  std::unique_ptr<Store> store = limits.ok ? detail::OpenStoreLogged(env, config.dir, limits, "boot") : nullptr;
  return MountStore(env, config, limits, store.get(), publish, out);
}

namespace detail {

ManifestLoad FetchManifest(Env& env, const RunConfig& config, const Limits& limits, const std::function<bool()>& keep_going) {
  ManifestLoad out;
  // Step 1: the manifest, read by its bytes. A body that begins with the zstd magic is one frame: its header states the size of the
  // text, that size is held to the device's memory as soon as the header has arrived, and the rest of the body is not downloaded when
  // it does not fit or the header is one no frame may have. Any other body is the JSON text, held to the same text limit as it arrives.
  std::string body;
  long status = 0;
  const char* early = nullptr;  // a refusal made from the header alone
  std::uint64_t declared = 0;
  bool haveDeclared = false;
  bool plain = false;  // the body is JSON text, not a zstd frame
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
                                      // The body says what it is, never the path: one that does not begin with the zstd magic is
                                      // the JSON text itself, held to the text limit as it arrives.
                                      if (!plain && body.size() >= kZstdMagicBytes && !StartsWithZstdMagic(body)) plain = true;
                                      if (plain) {
                                        if (body.size() > limits.manifest_text_bytes) {
                                          early = "too_large";
                                          return false;
                                        }
                                        return true;
                                      }
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
                                        default:  // a frame header not all here yet, or one that breaks the format
                                          // The magic says this is a frame, so a header no frame may have (a reserved bit set)
                                          // is refused here, before the rest of the body is downloaded.
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
    out.stopped = "cancelled";
    return out;
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
    if ((haveDeclared || plain) && std::string(manifestFailure) == "too_large") fields.push_back({"limit_bytes", limits.manifest_text_bytes});
    fields.push_back({"consequence", "no CDN asset is fetched this run; what the store holds stays mounted"});
    env.Event(EventLevel::kWarn, "cdn_manifest", fields);
    const bool refused = std::string(manifestFailure) == "not_zstd" || std::string(manifestFailure) == "no_content_size";
    out.stopped = refused ? "manifest_refused" : kStoppedManifestFailed;
    return out;
  }
  Manifest& manifest = out.manifest;
  const ParseResult parsed = ParseManifestBody(body, limits, &manifest);
  if (parsed.status != ParseStatus::kOk) {
    std::vector<Field> fields = {{"result", "refused"}, {"reason", ParseStatusName(parsed.status)}, {"url", manifestUrl},
                                 {"http_status", httpStatus}};
    if (!parsed.label.empty()) {
      fields.push_back({"label", parsed.label});
      fields.push_back({"index", parsed.index});
    }
    fields.push_back({"consequence", "no CDN asset is fetched this run; what the store holds stays mounted"});
    env.Event(EventLevel::kWarn, "cdn_manifest", fields);
    out.stopped = parsed.status == ParseStatus::kTooLarge ? kStoppedManifestFailed : "manifest_refused";
    return out;
  }
  out.body = std::move(body);
  out.http_status = httpStatus;
  out.url = manifestUrl;
  out.plain = plain;
  out.declared = declared;
  out.ok = true;
  return out;
}

}  // namespace detail

// The refetch marker and the boot-fetch stamp, beside the saved manifest (PLAN-v7 5.4, 5.5).
std::string BootRefetchPath(const std::string& dir) { return dir + "/boot_refetch"; }
std::string BootFetchStampPath(const std::string& dir) { return dir + "/boot_fetch_stamp"; }

namespace {

// The HTTP statuses of a refusal a retry does not cure (PLAN-v7 5.4 item 2): a 4xx answer. Anything from 500 up is a retry.
constexpr long kHttpClientErrorFirst = 400;
constexpr long kHttpServerErrorFirst = 500;

bool IsPinLabel(const std::string& label) {
  for (std::size_t i = 0; i < nevr_cargo_pin::kCargoPinLabelCount; ++i) {
    if (label == nevr_cargo_pin::kCargoPinLabels[i]) return true;
  }
  return false;
}

// Whether a pin label is served under `policy` (the same rule NeedsBootFetch applies).
bool PinWanted(const Policy& policy) {
  for (std::size_t i = 0; i < nevr_cargo_pin::kCargoPinLabelCount; ++i) {
    const LabelState state = StateOf(nevr_cargo_pin::kCargoPinLabels[i], policy);
    if (state == LabelState::kRequired || (state == LabelState::kOn && !policy.required_only)) return true;
  }
  return false;
}

std::string Sha256Of(Env& env, const std::string& bytes) {
  std::string hex;
  if (std::unique_ptr<Hasher> hasher = env.NewHasher()) {
    hasher->Update(bytes.data(), bytes.size());
    hex = hasher->Hex();
  }
  return hex;
}

// The read cap of the boot-fetch stamp: the longest stamp this build writes, measured from the format itself (AGENTS.md "Limit source":
// computed at run time, since the stamp is our own format and the game fixes no value for it). Its manifest hash is as long as this
// build's hasher writes one, its cause the longest, and both byte counts the largest a 64-bit count can print as.
std::size_t BootFetchStampMaxBytes(Env& env) {
  BootFetchStamp widest;
  widest.manifest_sha256 = std::string(Sha256Of(env, std::string()).size(), 'f');
  widest.cause = BootFetchCause::kNoSpace;
  widest.free_bytes = std::numeric_limits<std::uint64_t>::max();
  widest.need_bytes = std::numeric_limits<std::uint64_t>::max();
  return FormatBootFetchStamp(widest).size();
}

// The cargo pin of a manifest's records for this platform, hashed through the Env (cargo_session_pin.h).
std::uint64_t PinOf(Env& env, const RunConfig& config, const Manifest& manifest) {
  return nevr_cargo_pin::CargoPinOf(manifest, config.platform, config.policy,
                                    [&env](const std::string& bytes) { return Sha256Of(env, bytes); });
}

bool MarkerPresent(Env& env, const std::string& dir) {
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  if (!env.OpenBlob(BootRefetchPath(dir), &handle, &size)) return false;
  env.CloseBlob(handle);
  return true;
}

// A boot-state write that failed is a Warning: the next launch may fetch before boot once more (PLAN-v7 5.4 "The end of a fetch").
void BootStateFailed(Env& env, const char* file) {
  env.Event(EventLevel::kWarn, "cdn_boot_state",
            {{"result", "write_failed"}, {"file", file}, {"consequence", "the next launch may fetch before boot once more"}});
}

// Writes the refetch marker (present) or removes it (absent).
void SetMarker(Env& env, const std::string& dir, bool present) {
  const bool ok = present ? env.WriteFileAtomically(BootRefetchPath(dir), std::string()) : env.RemoveFile(BootRefetchPath(dir));
  if (!ok) BootStateFailed(env, "boot_refetch");
}

void WriteStamp(Env& env, const std::string& dir, const BootFetchStamp& stamp) {
  if (!env.WriteFileAtomically(BootFetchStampPath(dir), FormatBootFetchStamp(stamp))) BootStateFailed(env, "boot_fetch_stamp");
}

// Every object the pin labels name for this platform is held (Store::HoldsSized, a lookup). A pin label the plan skipped for its files or
// its storage is not held.
bool PinObjectsHeld(const Plan& plan, Store& store) {
  for (const PlanLabel& pl : plan.labels) {
    if (!IsPinLabel(pl.label)) continue;
    if (pl.skipped != SkipReason::kNone) {
      if (pl.skipped == SkipReason::kOverStorage || pl.skipped == SkipReason::kOverFiles) return false;
      continue;
    }
    for (const PlanItem& item : pl.items) {
      if (!store.HoldsSized(item.entry.sha256, item.entry.size, item.entry.bytes)) return false;
    }
  }
  return true;
}

// The outcome a prefetch's own status names (PLAN-v7 5.4 item 2): "ok", "stalled", "ceiling", "undeliverable", "no_space" or "failed".
std::string ClassifyPrefetch(PrefetchStatus status, long httpStatus) {
  switch (status) {
    case PrefetchStatus::kComplete:
      return "ok";
    case PrefetchStatus::kStalled:
      return "stalled";
    case PrefetchStatus::kCancelled:
      return "ceiling";
    case PrefetchStatus::kHttpFailed:
      return httpStatus >= kHttpClientErrorFirst && httpStatus < kHttpServerErrorFirst ? "undeliverable" : "failed";
    case PrefetchStatus::kHashMismatch:
    case PrefetchStatus::kSizeMismatch:
    case PrefetchStatus::kHeldWrongSize:
      return "undeliverable";
    case PrefetchStatus::kNoSpace:
      return "no_space";
    case PrefetchStatus::kTransport:
    case PrefetchStatus::kStoreFailed:
      return "failed";
  }
  return "failed";
}

// The cause a pin label's skip names: over_storage is no_space, over_files is undeliverable, "" for none.
std::string SkipCauseOf(const std::vector<std::pair<std::string, SkipReason>>& skipped) {
  std::string cause;
  for (const auto& s : skipped) {
    if (!IsPinLabel(s.first)) continue;
    if (s.second == SkipReason::kOverStorage) return "no_space";
    if (s.second == SkipReason::kOverFiles) cause = "undeliverable";
  }
  return cause;
}

// What a fetch that parsed its manifest leaves behind (PLAN-v7 5.4 "The end of a fetch", 5.5): when `learned` names a cause a retry cannot
// cure, the marker is removed and the stamp written with it; when the fetched body is the saved manifest and every pin object is held,
// the stamp settles it. `body` is the fetched manifest's bytes; `saved_is_fetched` says the manifest on disk now is that body.
struct FetchEnd {
  const std::string* body = nullptr;
  bool saved_is_fetched = false;
  std::string learned;
  const Plan* plan = nullptr;
  Store* store = nullptr;
};

std::string EndOfFetch(Env& env, const RunConfig& config, const FetchEnd& end) {
  std::string cause = "none";
  if (!end.learned.empty()) {
    cause = end.learned;
  } else if (end.saved_is_fetched && PinObjectsHeld(*end.plan, *end.store)) {
    cause = "settled";
  }
  if (!config.boot_state || cause == "none") return cause;
  BootFetchStamp stamp;
  stamp.manifest_sha256 = Sha256Of(env, *end.body);
  ParseBootFetchCause(cause, &stamp.cause);
  if (stamp.cause == BootFetchCause::kNoSpace) {
    DeviceFacts facts;
    if (env.QueryDevice(config.dir, &facts) && facts.have_free) stamp.free_bytes = facts.free_bytes;
    for (const PlanLabel& pl : end.plan->labels) {
      if (IsPinLabel(pl.label)) stamp.need_bytes += pl.skipped == SkipReason::kNone ? pl.fetch_bytes : pl.needed_bytes;
    }
  }
  SetMarker(env, config.dir, false);
  WriteStamp(env, config.dir, stamp);
  return cause;
}

}  // namespace

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
  const Limits limits = detail::LoadLimits(env, config.dir);
  if (!limits.ok) {
    result.stopped = "limits";
    done("limits");
    return result;
  }
  std::unique_ptr<Store> store = detail::OpenStoreLogged(env, config.dir, limits, "fetch");
  env.Event(EventLevel::kInfo, "cdn_start",
            {{"source", config.source},
             {"host", HostOf(config.base_url)},
             {"platform", config.platform.name},
             {"labels", StatesText(config.policy, nullptr)},
             {"required_only", config.policy.required_only ? 1 : 0}});

  // Step 1: the manifest, read by its bytes (detail::FetchManifest). The run ends with done(stopped) when it is not had.
  detail::ManifestLoad loaded = detail::FetchManifest(env, config, limits, keep_going);
  if (!loaded.ok) {
    result.stopped = loaded.stopped;
    done(loaded.stopped);
    return result;
  }
  Manifest& manifest = loaded.manifest;
  const std::string& body = loaded.body;
  const long httpStatus = loaded.http_status;
  const std::string& manifestUrl = loaded.url;
  const bool plain = loaded.plain;
  const std::uint64_t declared = loaded.declared;
  result.manifest_loaded = true;
  result.refused = manifest.refused.size();
  std::string manifestSha;
  if (std::unique_ptr<Hasher> hasher = env.NewHasher()) {
    hasher->Update(body.data(), body.size());
    manifestSha = hasher->Hex();
  }

  // The refetch marker (PLAN-v7 5.5): written when this manifest's pin differs from the saved manifest's and is not kCargoPinNone. Only a
  // run that sets boot_state writes it. on_manifest is the PC's feed (R11), called right after the parse.
  if (config.boot_state) {
    std::string savedFrame;
    Manifest savedManifest;
    std::uint64_t savedPin = nevr_cargo_pin::kCargoPinNone;
    if (ReadSavedManifest(env, config.dir, limits, &savedFrame) &&
        ParseManifestBody(savedFrame, limits, &savedManifest).status == ParseStatus::kOk) {
      savedPin = PinOf(env, config, savedManifest);
    }
    const std::uint64_t fetchedPin = PinOf(env, config, manifest);
    if (fetchedPin != nevr_cargo_pin::kCargoPinNone && fetchedPin != savedPin) SetMarker(env, config.dir, true);
  }
  if (config.on_manifest) config.on_manifest(manifest);

  // Step 2: the plan, before any request. What the store holds is a lookup in the index: nothing is read or hashed.
  Plan plan = MakePlan(manifest, config.platform, config.policy, limits, store->held_bytes(),
                       [&](const Entry& e) { return store->HoldsSized(e.sha256, e.size, e.bytes); });
  env.Event(EventLevel::kInfo, "cdn_manifest",
            {{"result", "loaded"},
             {"url", manifestUrl},
             {"http_status", httpStatus},
             {plain ? "body_bytes" : "frame_bytes", body.size()},
             {"format", plain ? "json" : "zstd"},
             {"bytes", plain ? static_cast<std::uint64_t>(body.size()) : declared},
             {"sha256", manifestSha},
             {"labels", manifest.labels.size()},
             {"entries", manifest.entries.size()},
             {"empty_records", manifest.empties.size()},
             {"refused", manifest.refused.size()},
             {"platform_entries", plan.platform_entries},
             {"unknown_fields", manifest.unknown_fields}});
  // The end of this fetch (PLAN-v7 5.4 "The end of a fetch"): the boot-state writes, then on_end. `learned` is a cause this run met that a
  // retry cannot cure; `savedIsFetched` is whether the manifest on disk is this fetch's body (step 4 sets it).
  bool savedIsFetched = false;
  std::string learned;
  const auto finish = [&](const char* resultText) {
    FetchEnd end;
    end.body = &body;
    end.saved_is_fetched = savedIsFetched;
    end.learned = learned;
    end.plan = &plan;
    end.store = store.get();
    result.end_cause = EndOfFetch(env, config, end);
    if (config.on_end) config.on_end(result.end_cause.c_str());
    done(resultText);
  };
  for (const PlanLabel& pl : plan.labels) {
    if (pl.skipped != SkipReason::kNone) result.skipped.emplace_back(pl.label, pl.skipped);
  }
  ReportRefusals(env, manifest, "fetch");
  for (const PlanLabel& pl : plan.labels) {
    std::vector<Field> fields = {{"label", pl.label},   {"state", LabelStateName(pl.state)}, {"entries", pl.entries},
                                 {"empty_records", pl.empty_records}, {"cached", pl.cached}, {"to_fetch", pl.to_fetch},
                                 {"fetch_bytes", pl.fetch_bytes}};
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

  // The boot-only labels of this run (docs/adr/0045 decision 7): the fixed list and every label with scripts of this platform. With an
  // empty list the fetch mounts what it fetched, as it always has.
  const std::vector<std::string> bootOnly = BootOnlyLabels(config.boot_only, config.boot, manifest, config.platform);
  std::size_t fetchedOutside = 0;  // objects stored this run under a label that is not boot-only

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
    prefetch.stall_ms = config.stall_ms;
    prefetch.keep_going = keep_going;
    prefetch.progress = [&](std::uint64_t done, std::uint64_t) {
      progress.done_bytes = done;
      report();
    };
    detail::StorePrefetch adapter(*store, prefetch.objects);
    const PrefetchResult fetched = Prefetch(env, adapter, prefetch);
    result.fetched = fetched.fetched;
    result.cached = fetched.held;
    result.bytes = fetched.bytes_fetched;
    result.prefetched = true;
    result.prefetch_status = fetched.status;
    result.prefetch_http_status = fetched.http_status;
    fetchedOutside = result.fetched;  // the labels are not told apart here: the publish rule below then publishes (the safe side)
    if (fetched.status != PrefetchStatus::kComplete) {
      // What was stored stays and the next launch resumes from it. The manifest is not saved and nothing new is mounted, except that a
      // boot fetch saves it for a cause a retry cannot cure (undeliverable, no_space), so the next start's stamp settles it (PLAN-v7 5.4 item 2).
      result.failed = 1;
      result.stopped = fetched.status == PrefetchStatus::kCancelled ? "cancelled" : PrefetchStatusName(fetched.status);
      const std::string outcome = ClassifyPrefetch(fetched.status, fetched.http_status);
      if (!config.store_only || (outcome != "undeliverable" && outcome != "no_space")) {
        finish(result.stopped);
        return result;
      }
      learned = outcome;
    }
    if (config.store_only && learned.empty()) learned = SkipCauseOf(result.skipped);
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
        finish("cancelled");
        return result;
      }
      const std::uint64_t blobStarted = env.NowMs();
      const detail::Downloaded d = detail::Download(env, config, *store, e, keep_going);
      if (d.cancelled) {
        result.stopped = "cancelled";
        finish("cancelled");
        return result;
      }
      if (d.ok) {
        fetchedShas.insert(e.sha256);
        ++result.fetched;
        if (std::find(bootOnly.begin(), bootOnly.end(), e.label) == bootOnly.end()) ++fetchedOutside;
        result.bytes += e.size;
        progress.done_bytes += e.size;
        ++progress.done_blobs;
        env.Event(EventLevel::kDebug, "cdn_blob",
                  {{"result", "stored"}, {"label", e.label}, {"type", FormatHex64(e.type)}, {"name", FormatHex64(e.name)},
                   {"sha256", e.sha256}, {"size", e.size}, {"ms", env.NowMs() - blobStarted}});
      } else {
        failedShas.insert(e.sha256);
        // A pin object the CDN refuses for good (a 4xx, or a body that is not the manifest's) makes the set undeliverable (PLAN-v7 5.4).
        const std::string reason = d.reason;
        if (IsPinLabel(e.label) && ((d.status >= kHttpClientErrorFirst && d.status < kHttpServerErrorFirst) || reason == "sha256_mismatch" ||
                                    reason == "size_mismatch" || reason == "held_size_mismatch")) {
          learned = "undeliverable";
        }
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
    finish(result.stopped);
    return result;
  }

  // Step 4: save the manifest as it was published, so a launch with no network mounts what the store holds, and mount.
  std::string previous;
  const bool hadSaved = ReadSavedManifest(env, config.dir, limits, &previous);
  const bool changed = !hadSaved || previous != body;
  bool wrote = false;
  if (changed) {
    wrote = env.WriteFileAtomically(ManifestCopyPath(config.dir), body);
    if (!wrote) {
      env.Event(EventLevel::kError, "cdn_manifest",
                {{"result", "save_failed"}, {"consequence", "the next launch mounts the manifest saved before this one, or none"}});
    }
  }
  savedIsFetched = !changed || wrote;
  // The boot fetch stores and saves; it mounts nothing (RunConfig::store_only, PLAN-v7 5.4 item 2).
  if (config.store_only) {
    finish(*result.stopped != '\0' ? result.stopped : "ok");
    return result;
  }
  // With boot-only labels the set mounted gives each of them the boot set's records (KeepBootOnly), so the set changes only for what is not
  // boot-only. It is published when it differs from the boot set, or when the fetch stored an object of another label: a new object of
  // a label that is not boot-only must show. What arrived for a boot-only label waits for the next start (`boot_only_kept`).
  const Manifest keptSet = bootOnly.empty() ? Manifest() : KeepBootOnly(manifest, config.boot, bootOnly);
  const Manifest& mountSet = bootOnly.empty() ? manifest : keptSet;
  bool publishSet = changed || result.fetched != 0;
  std::optional<std::size_t> bootOnlyKept;
  if (!bootOnly.empty()) {
    std::size_t kept = 0;
    for (const std::string& label : bootOnly) {
      if (LabelRecordsDiffer(manifest, config.boot, config.platform, label)) ++kept;
    }
    bootOnlyKept = kept;
    publishSet = publishSet && (!SameMountRecords(mountSet, config.boot, config.platform) || fetchedOutside != 0);
  }
  if (publishSet) {
    store->EndWrites();  // the mount opens the store files this run appended to; on the PC an open append handle refuses that open
    const std::uint64_t mountStarted = env.NowMs();
    std::unique_ptr<Mounted> mounted(new Mounted(Mount(env, *store, mountSet, config.platform, detail::GatedPolicy(env, config, mountSet, "fetch"))));
    const std::uint64_t mountMs = env.NowMs() - mountStarted;
    if (mounted->shadow || mounted->refused != 0 || mounted->missing != 0) SayMounted(env, "fetch", *mounted, mountMs, bootOnlyKept);
    if (mounted->shadow) {
      publish(std::move(mounted));
      result.published = true;
    }
  } else if (bootOnlyKept && *bootOnlyKept != 0) {
    env.Event(EventLevel::kInfo, "cdn_mounted",
              {{"source", "fetch"},
               {"result", "unchanged"},
               {"reason", "boot_only"},
               {"boot_only_kept", *bootOnlyKept},
               {"consequence", "the labels fixed at boot keep the boot set's records this run; what arrived is mounted at the next start"}});
  }
  finish("ok");
  return result;
}

namespace {

// The policy of the boot fetch: every label of the client's table off, the boot-only labels served (PLAN-v7 5.4 item 2).
Policy BootOnlyPolicy(const Policy& policy, const std::vector<std::string>& bootOnly) {
  Policy out = policy;
  out.required_only = false;
  for (auto& entry : out.overrides) entry.second = LabelState::kOff;
  for (const LabelDefault& d : kLabelDefaults) out.overrides[d.label] = LabelState::kOff;
  for (const std::string& label : bootOnly) out.overrides[label] = LabelState::kRequired;
  return out;
}

// The consequence a boot fetch's result names (PLAN-v7 5.4 item 2).
std::string BootFetchConsequence(const std::string& outcome) {
  if (outcome == "undeliverable") return "combat sessions are left until the CDN publishes another set; no launch fetches this set before boot again";
  if (outcome == "no_space") return "combat sessions are left until there is room for this set; the next launch with that room fetches before boot";
  return "combat sessions are left this run; the next launch fetches before boot again";
}

}  // namespace

std::string BootFetchResultOf(const RunResult& result) {
  if (!result.prefetched) return std::string(result.stopped) == "cancelled" ? "ceiling" : "failed";
  const std::string outcome = ClassifyPrefetch(result.prefetch_status, result.prefetch_http_status);
  if (outcome != "ok") return outcome;
  std::vector<std::pair<std::string, SkipReason>> skipped = result.skipped;
  const std::string skip = SkipCauseOf(skipped);
  return skip.empty() ? "ok" : skip;
}

BootMount ClientBoot(Env& env, const RunConfig& config, const Publish& give) {
  // One open of the store for the whole boot half: the limits and the store are logged once (cdn_limits, cdn_store source=boot), the
  // decision reads through that open, and the mount below uses it again when no fetch ran. A fetch stores through its own open, so this
  // one is closed before it runs (a PC append handle refuses a second open).
  const Limits limits = detail::LoadLimits(env, config.dir);
  std::unique_ptr<Store> store = limits.ok ? detail::OpenStoreLogged(env, config.dir, limits, "boot") : nullptr;
  BootFetchDecision decision;
  std::string savedFrame;
  Manifest saved;
  bool haveSaved = false;
  std::string savedSha;
  std::string stampText;
  BootFetchStamp stamp;
  bool haveStamp = false;
  // The decision reads the saved manifest, the marker, the stamp and the store only when there are boot-only labels: a run with none (the
  // Quest) makes no decision and reads nothing beyond its mount.
  if (!config.boot_only.empty()) {
    BootFetchInput input;
    input.platform = config.platform;
    input.policy = config.policy;
    input.boot_only = config.boot_only;
    input.last_update_trial = config.last_update_trial;
    if (store && ReadSavedManifest(env, config.dir, limits, &savedFrame)) {
      haveSaved = ParseManifestBody(savedFrame, limits, &saved).status == ParseStatus::kOk;
    }
    if (haveSaved) savedSha = Sha256Of(env, savedFrame);
    input.saved = haveSaved ? &saved : nullptr;
    input.saved_sha256 = savedSha;
    input.marker = MarkerPresent(env, config.dir);
    if (env.ReadSmallFile(BootFetchStampPath(config.dir), &stampText, BootFetchStampMaxBytes(env)) && ParseBootFetchStamp(stampText, &stamp)) {
      haveStamp = true;
      input.stamp = &stamp;
    }
    input.held = [&store](const Entry& e) { return store != nullptr && store->HoldsSized(e.sha256, e.size, e.bytes); };
    input.free_space = [&env, &config](std::uint64_t* freeBytes) {
      DeviceFacts facts;
      if (!env.QueryDevice(config.dir, &facts) || !facts.have_free) return false;
      *freeBytes = facts.free_bytes;
      return true;
    };
    decision = NeedsBootFetch(input);
  }

  std::string bootFetch = "none";
  std::string resolvedPath;  // the manifest path the boot fetch asked for; empty when no fetch ran
  if (decision.fetch) {
    env.Event(EventLevel::kInfo, "cdn_start",
              {{"role", "client"}, {"fetch", "boot_only"}, {"result", "started"},
               {"reason", BootFetchReasonName(decision.reason)}, {"label", decision.label}});
    store.reset();
    // The ceiling is the only bound on the wait besides the stall: keep_going is false once it has run that long (PLAN-v7 5.4 item 2). The
    // manifest path is asked for under the same clock, and only here, because the fetch is what needs it (ResolveManifestPath).
    const std::uint64_t waitStarted = env.NowMs();
    const std::function<bool()> keepGoing = [&env, waitStarted]() { return env.NowMs() - waitStarted < kClientBootFetchCeilingMs; };
    const RunConfig resolved = ResolveManifestPath(env, config, keepGoing);
    resolvedPath = resolved.manifest_path;
    RunConfig fetchConfig = resolved;
    fetchConfig.policy = BootOnlyPolicy(config.policy, config.boot_only);
    fetchConfig.first_run = true;
    fetchConfig.store_only = true;
    fetchConfig.stall_ms = kClientBootFetchStallMs;
    const RunResult fetched = FetchAndMount(env, fetchConfig, [](std::unique_ptr<Mounted>) {}, keepGoing);
    const std::string outcome = BootFetchResultOf(fetched);
    const std::uint64_t fetchMs = env.NowMs() - waitStarted;
    if (outcome == "ok") {
      env.Event(EventLevel::kInfo, "cdn_start",
                {{"role", "client"}, {"fetch", "boot_only"}, {"result", outcome}, {"fetch_ms", fetchMs}, {"bytes", fetched.bytes}});
    } else {
      env.Event(EventLevel::kWarn, "cdn_start",
                {{"role", "client"}, {"fetch", "boot_only"}, {"result", outcome}, {"fetch_ms", fetchMs}, {"bytes", fetched.bytes},
                 {"consequence", BootFetchConsequence(outcome)}});
    }
    bootFetch = fetched.end_cause == "settled" ? "none" : fetched.end_cause;
  } else if (config.last_update_trial && !config.boot_only.empty() && PinWanted(config.policy)) {
    env.Event(EventLevel::kInfo, "cdn_start",
              {{"role", "client"}, {"fetch", "none"}, {"reason", "update_last_trial"},
               {"consequence", "combat sessions are left this run if this client holds no current set; the next start fetches before boot"}});
  }
  // A stamp that settles the saved manifest is the boot fetch's state for this start, even when no fetch ran (PLAN-v7 5.4 item 4).
  if (!decision.fetch && haveStamp && haveSaved && stamp.manifest_sha256 == savedSha) {
    bootFetch = stamp.cause == BootFetchCause::kSettled ? "none" : BootFetchCauseName(stamp.cause);
  }

  // One mount with the client's full policy (item 3). After a fetch the store is opened again, quietly: the open above was logged, and the
  // fetch stored objects the mount must see. MountStore overwrites `bm`, so the boot fetch's state is set after it.
  if (decision.fetch && limits.ok) {
    StoreOpenInfo reopened;
    store = Store::Open(env, config.dir, limits, &reopened);
  }
  BootMount bm;
  MountStore(env, config, limits, store.get(), give, &bm);
  bm.boot_fetch = bootFetch;
  bm.resolved_manifest_path = resolvedPath;
  if (config.on_boot) config.on_boot(bm);
  return bm;
}

}  // namespace nevr_cdn

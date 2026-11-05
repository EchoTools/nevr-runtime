#include "core/cargo_run.h"

#include <cstdio>
#include <map>
#include <set>

namespace nevr_cargo {

namespace {

constexpr const char* kBlobPrefix = "blobs/sha256/";

// The host part of a URL, for the log: never the path or a query.
std::string HostOf(const std::string& url) {
  const std::size_t scheme = url.find("://");
  const std::size_t begin = scheme == std::string::npos ? 0 : scheme + 3;
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

std::string ChunkBaseText() {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08x", static_cast<unsigned>(nevr_shadow::kCargoChunkBase));
  return buf;
}

void SayMounted(Env& env, const char* source, const Mounted& m) {
  env.Event(EventLevel::kInfo, "cargo_mounted",
            {{"source", source},
             {"labels", m.labels.size()},
             {"names", m.names},
             {"records", m.records},
             {"chunk_base", ChunkBaseText()},
             {"refused", m.refused}});
}

struct Downloaded {
  bool ok = false;
  bool cancelled = false;
  const char* reason = "";
  long status = 0;
  int code = 0;
};

// Streams one blob into a temp file in the cache directory while hashing it, and renames it to its name when the size and the
// SHA-256 are the manifest's. A transfer that gives more than `size` bytes is ended. A mismatch deletes the temp file.
Downloaded Download(Env& env, const RunConfig& config, const Entry& e, const std::function<bool()>& keep_going) {
  Downloaded out;
  std::unique_ptr<TempFile> temp = env.CreateTemp(config.dir);
  std::unique_ptr<Hasher> hasher = env.NewHasher();
  if (!temp || !hasher) {
    out.reason = "temp";
    return out;
  }
  std::uint64_t got = 0;
  long status = 0;
  bool tooMuch = false, writeFailed = false;
  const HttpResult http = env.Get(config.base_url + kBlobPrefix + e.sha256,
                                  [&](long st, const void* data, std::size_t length) {
                                    status = st;
                                    if (st != 200) return false;
                                    if (got + length > e.size) {
                                      tooMuch = true;
                                      return false;
                                    }
                                    hasher->Update(data, length);
                                    if (!temp->Write(data, length)) {
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
    temp->Abort();
    out.cancelled = true;
    out.reason = "cancelled";
    return out;
  }
  if (tooMuch) {
    temp->Abort();
    out.reason = "too_large";
    return out;
  }
  if (writeFailed) {
    temp->Abort();
    out.reason = "write_failed";
    return out;
  }
  if (out.status != 200) {
    temp->Abort();
    out.reason = out.status == 0 ? "transport" : "http_status";
    return out;
  }
  if (!http.completed) {
    temp->Abort();
    out.reason = "transport";
    return out;
  }
  if (got != e.size) {
    temp->Abort();
    out.reason = "size_mismatch";
    return out;
  }
  if (hasher->Hex() != e.sha256) {
    temp->Abort();
    out.reason = "sha256_mismatch";
    return out;
  }
  if (!temp->Commit(BlobPath(config.dir, e.sha256))) {
    out.reason = "commit_failed";
    return out;
  }
  out.ok = true;
  return out;
}

IndexEntry ToIndexEntry(const Entry& e) {
  IndexEntry i;
  i.type = e.type;
  i.name = e.name;
  i.sha256 = e.sha256;
  i.label = e.label;
  i.format = kFormatZstdFrame;
  i.bytes = e.bytes;
  i.align = e.align;
  return i;
}

}  // namespace

bool MountFromCache(Env& env, const RunConfig& config, const Publish& publish) {
  Index index;
  const IndexStatus status = ReadIndex(env, config.dir, &index);
  if (status == IndexStatus::kAbsent) return false;
  if (status != IndexStatus::kOk) {
    env.Event(EventLevel::kWarn, "cargo_index", {{"result", IndexStatusName(status)}, {"consequence", "no cargo texture is served until a fetch rewrites it"}});
    return false;
  }
  std::unique_ptr<Mounted> mounted(new Mounted(Mount(env, config.dir, index, config.policy)));
  if (mounted->shadow || mounted->refused != 0) SayMounted(env, "boot", *mounted);
  if (!mounted->shadow) return false;
  publish(std::move(mounted));
  return true;
}

RunResult FetchAndMount(Env& env, const RunConfig& config, const Publish& publish, const std::function<bool()>& keep_going) {
  RunResult result;
  const std::uint64_t started = env.NowMs();
  env.Event(EventLevel::kInfo, "cargo_start",
            {{"source", config.source},
             {"host", HostOf(config.base_url)},
             {"platform", config.platform.name},
             {"labels", StatesText(config.policy, nullptr)},
             {"required_only", config.policy.required_only ? 1 : 0}});
  const auto done = [&](const char* resultText) {
    env.Event(result.failed != 0 ? EventLevel::kWarn : EventLevel::kInfo, "cargo_done",
              {{"result", resultText},
               {"fetched", result.fetched},
               {"cached", result.cached},
               {"failed", result.failed},
               {"bytes", result.bytes},
               {"ms", env.NowMs() - started}});
  };
  if (!env.MakeDirs(config.dir)) {
    env.Event(EventLevel::kError, "cargo_done", {{"result", "failed"}, {"reason", "cache_dir"}});
    return result;
  }

  // Step 1: the manifest, capped.
  std::string body;
  bool tooBig = false;
  long status = 0;
  const HttpResult http = env.Get(config.base_url + config.manifest_path,
                                  [&](long st, const void* data, std::size_t length) {
                                    status = st;
                                    if (st != 200) return false;
                                    if (body.size() + length > kMaxManifestBytes) {
                                      tooBig = true;
                                      return false;
                                    }
                                    body.append(static_cast<const char*>(data), length);
                                    return true;
                                  },
                                  keep_going);
  if (http.cancelled) {
    done("cancelled");
    return result;
  }
  const long httpStatus = status != 0 ? status : http.status;
  const char* manifestFailure = nullptr;
  if (tooBig) {
    manifestFailure = "too_large";
  } else if (httpStatus != 200) {
    manifestFailure = httpStatus == 0 ? "transport" : "http_status";
  } else if (!http.completed) {
    manifestFailure = "transport";
  }
  if (manifestFailure != nullptr) {
    env.Event(EventLevel::kWarn, "cargo_manifest",
              {{"result", "failed"}, {"reason", manifestFailure}, {"http_status", httpStatus}, {"code", http.code},
               {"consequence", "no cargo texture is fetched this run; what the cache holds stays mounted"}});
    done("manifest_failed");
    return result;
  }
  Manifest manifest;
  const ParseResult parsed = ParseManifest(body, &manifest);
  if (parsed.status != ParseStatus::kOk) {
    std::vector<Field> fields = {{"result", "refused"}, {"reason", ParseStatusName(parsed.status)}, {"http_status", httpStatus}};
    if (!parsed.label.empty()) {
      fields.push_back({"label", parsed.label});
      fields.push_back({"index", parsed.index});
    }
    fields.push_back({"consequence", "no cargo texture is fetched this run; what the cache holds stays mounted"});
    env.Event(EventLevel::kWarn, "cargo_manifest", fields);
    done("manifest_refused");
    return result;
  }
  result.manifest_loaded = true;
  std::string manifestSha;
  if (std::unique_ptr<Hasher> hasher = env.NewHasher()) {
    hasher->Update(body.data(), body.size());
    manifestSha = hasher->Hex();
  }

  // Step 2: the plan, before any request.
  std::map<std::string, bool> heldMemo;
  const auto held = [&](const std::string& sha) {
    const auto it = heldMemo.find(sha);
    if (it != heldMemo.end()) return it->second;
    const bool ok = HashFile(env, BlobPath(config.dir, sha)) == sha;
    heldMemo[sha] = ok;
    return ok;
  };
  Plan plan = MakePlan(manifest, config.platform, config.policy, held);
  env.Event(EventLevel::kInfo, "cargo_manifest",
            {{"result", "loaded"},
             {"http_status", httpStatus},
             {"bytes", body.size()},
             {"sha256", manifestSha},
             {"labels", manifest.labels.size()},
             {"entries", manifest.entries.size()},
             {"platform_entries", plan.platform_entries},
             {"unknown_fields", manifest.unknown_fields}});
  for (const PlanLabel& pl : plan.labels) {
    std::vector<Field> fields = {{"label", pl.label},     {"state", LabelStateName(pl.state)}, {"entries", pl.entries},
                                 {"cached", pl.cached},   {"to_fetch", pl.to_fetch},           {"fetch_bytes", pl.fetch_bytes}};
    if (pl.skipped != SkipReason::kNone) {
      fields.push_back({"skipped", SkipReasonName(pl.skipped)});
      fields.push_back({"consequence", "this label serves nothing from the CDN"});
    }
    env.Event(pl.skipped == SkipReason::kOverCap ? EventLevel::kWarn : EventLevel::kInfo, "cargo_plan", fields);
    for (const Refusal& r : pl.refused) {
      env.Event(EventLevel::kWarn, "cargo_plan",
                {{"label", pl.label}, {"name", FormatHex64(r.name)}, {"result", "refused"}, {"reason", r.reason},
                 {"consequence", "the stock texture shows for this name"}});
    }
  }

  // Step 3 and 4: fetch what is not held, a blob once however many entries name it.
  std::set<std::string> fetchedShas, failedShas;
  for (const PlanLabel& pl : plan.labels) {
    if (pl.skipped != SkipReason::kNone) continue;
    for (const PlanItem& item : pl.items) {
      if (item.cached) {
        // counted once per blob below
        continue;
      }
      const Entry& e = item.entry;
      if (fetchedShas.count(e.sha256) != 0 || failedShas.count(e.sha256) != 0) continue;
      if (!keep_going()) {
        done("cancelled");
        return result;
      }
      const std::uint64_t blobStarted = env.NowMs();
      const Downloaded d = Download(env, config, e, keep_going);
      if (d.cancelled) {
        done("cancelled");
        return result;
      }
      if (d.ok) {
        fetchedShas.insert(e.sha256);
        ++result.fetched;
        result.bytes += e.size;
        env.Event(EventLevel::kInfo, "cargo_blob",
                  {{"result", "stored"}, {"label", e.label}, {"type", FormatHex64(e.type)}, {"name", FormatHex64(e.name)},
                   {"sha256", e.sha256}, {"size", e.size}, {"ms", env.NowMs() - blobStarted}});
      } else {
        failedShas.insert(e.sha256);
        ++result.failed;
        env.Event(pl.state == LabelState::kRequired ? EventLevel::kError : EventLevel::kWarn, "cargo_blob",
                  {{"result", "failed"}, {"label", e.label}, {"type", FormatHex64(e.type)}, {"name", FormatHex64(e.name)},
                   {"sha256", e.sha256}, {"reason", d.reason}, {"http_status", d.status}, {"code", d.code},
                   {"consequence", "the stock texture shows for this label until a later launch fetches it"}});
      }
    }
  }
  std::set<std::string> cachedShas;
  for (const PlanLabel& pl : plan.labels) {
    for (const PlanItem& item : pl.items) {
      if (item.cached) cachedShas.insert(item.entry.sha256);
    }
  }
  result.cached = cachedShas.size();

  // Step 5: the index, whole, last. A label whose blobs all arrived is written fresh; a label that failed keeps what the old
  // index said about it (those blobs are still on disk and are hashed again at mount); a label the manifest no longer serves goes.
  Index old;
  const IndexStatus oldStatus = ReadIndex(env, config.dir, &old);
  Index next;
  std::set<std::string> replaced;
  for (const PlanLabel& pl : plan.labels) {
    if (pl.skipped != SkipReason::kNone) {
      replaced.insert(pl.label);  // off, over cap or empty: not served, so not in the index
      continue;
    }
    bool complete = true;
    for (const PlanItem& item : pl.items) {
      if (failedShas.count(item.entry.sha256) != 0) complete = false;
    }
    if (!complete) continue;  // keep the old entries of this label
    replaced.insert(pl.label);
    for (const PlanItem& item : pl.items) next.entries.push_back(ToIndexEntry(item.entry));
  }
  if (oldStatus == IndexStatus::kOk) {
    for (const IndexEntry& e : old.entries) {
      bool inManifest = false;
      for (const std::string& l : manifest.labels) {
        if (l == e.label) inManifest = true;
      }
      if (replaced.count(e.label) == 0 && inManifest) next.entries.push_back(e);
    }
  }
  Normalize(&next);
  Index oldNormal = old;
  Normalize(&oldNormal);
  // Nothing to say and nothing said before: no index is written (an empty one would name nothing and mean nothing).
  const bool same = oldStatus == IndexStatus::kOk ? next == oldNormal : next.entries.empty();
  bool indexOk = true;
  if (!same) {
    indexOk = WriteIndex(env, config.dir, next);
    env.Event(indexOk ? EventLevel::kInfo : EventLevel::kError, "cargo_index",
              {{"result", indexOk ? "written" : "failed"}, {"entries", next.entries.size()}});
  }

  // Step 6: mount what the index names, when it changed or a blob was stored.
  if (indexOk && (!same || result.fetched != 0)) {
    std::unique_ptr<Mounted> mounted(new Mounted(Mount(env, config.dir, next, config.policy)));
    if (mounted->shadow || mounted->refused != 0) SayMounted(env, "fetch", *mounted);
    if (mounted->shadow) {
      publish(std::move(mounted));
      result.published = true;
    }
  }
  done("ok");
  return result;
}

}  // namespace nevr_cargo

#include "quest/obb/stock_data.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>

#include "core/owned_remove_rules.h"
#include "quest/data/sha256.h"
#include "quest/data/staging.h"
#include "quest/obb/byte_source.h"
#include "quest/obb/fat16.h"
#include "quest/obb/obb_cipher.h"
#include "quest/obb/obb_footer.h"

namespace nevr_quest::obb {

namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr std::size_t kHashChunk = 4u << 20;

double MillisSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// Removes, by its manifest, the item an extraction made when it goes out of scope unless it was released: a refused or failed
// extraction leaves nothing behind that it recorded. Anything the manifest does not name stays, and is logged.
class RemoveOnExit {
 public:
  RemoveOnExit(fs::path path, BuildLog& log, std::vector<std::string> pinned)
      : path_(std::move(path)), log_(log), pinned_(std::move(pinned)) {}
  ~RemoveOnExit() {
    if (armed_) {
      const nevr_quest::data::ItemRemoval r = nevr_quest::data::RemoveUnionRecord(path_.string(), pinned_);
      if (!r.removed) {
        log_.Error("cache_left", {{"directory", path_.generic_string()}, {"outcome", r.outcome}, {"entry", r.entry}});
      }
    }
  }
  RemoveOnExit(const RemoveOnExit&) = delete;
  RemoveOnExit& operator=(const RemoveOnExit&) = delete;
  void Arm() { armed_ = true; }
  void Release() { armed_ = false; }

 private:
  fs::path path_;
  BuildLog& log_;
  std::vector<std::string> pinned_;
  bool armed_ = false;
};

// Every name the stock data writes for these pins: each pinned file, the folders above it, and the cache's manifest. The build before
// manifests wrote no marker for its cache or its partial, so these pins are the record of what it made.
std::vector<std::string> PinnedMembers(const StockPins& pins) {
  std::set<std::string> names;
  for (const PinnedFile& p : pins.files) {
    for (std::size_t slash = p.path.find('/'); slash != std::string::npos; slash = p.path.find('/', slash + 1)) {
      names.insert(p.path.substr(0, slash));
    }
    names.insert(p.path);
  }
  names.insert(std::string(kStockDataManifestName));
  return std::vector<std::string>(names.begin(), names.end());
}

// Removes an item this extraction made. With a marker, by its manifest; without one (the build before manifests), by the pinned names
// alone, and the folder only when it is then empty. Anything that stops the removal is a failure, and the item stays.
Status RemoveMade(const fs::path& item, BuildLog& log, const std::vector<std::string>& pinned) {
  std::error_code ec;
  const bool marked = fs::exists(fs::symlink_status(nevr_quest::data::OwnerMarkerPath(item.string()), ec));
  const nevr_quest::data::ItemRemoval r = nevr_quest::data::RemoveUnionRecord(item.string(), pinned);
  if (r.removed) {
    if (!marked && r.had_item) log.Info("legacy_removed", {{"directory", item.generic_string()}, {"names", pinned.size()}});
    return Status::Ok();
  }
  log.Error("cache_refused", {{"directory", item.generic_string()}, {"outcome", r.outcome}, {"entry", r.entry}});
  return Status::Fail("cache_unwritable", "cannot remove '" + item.string() + "': " + r.outcome + " at '" + r.entry + "'");
}

Status ReadText(const std::string& path, std::string* text, const char* code) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return Status::Fail(code, "cannot read '" + path + "'");
  text->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (in.bad()) return Status::Fail(code, "reading '" + path + "' failed");
  return Status::Ok();
}

Json FilesJson(const std::vector<MeasuredFile>& files) {
  Json a = Json::array();
  for (const MeasuredFile& f : files) a.push_back({{"path", f.path}, {"sha256", f.sha256}, {"bytes", f.size}});
  return a;
}

// Is the directory a finished extraction for these pins? The manifest it carries must agree with the pins, the tree must hold
// exactly the pinned files at the pinned sizes and nothing else, and with `rehash` each file must hash to its pinned digest.
// Without it the digests the manifest recorded at extraction stand for the files.
bool CacheIsValid(const fs::path& dir, const StockPins& pins, bool rehash, std::vector<MeasuredFile>* files, std::string* why) {
  std::string text;
  if (!ReadText((dir / kStockDataManifestName).string(), &text, "cache_manifest_unreadable").ok) {
    *why = "the cache has no readable " + std::string(kStockDataManifestName);
    return false;
  }
  std::map<std::string, std::string> recorded;
  try {
    const Json manifest = Json::parse(text);
    if (manifest.at("obb_sha256").get<std::string>() != pins.obb_sha256) {
      *why = "the manifest names another OBB";
      return false;
    }
    for (const Json& f : manifest.at("files")) recorded[f.at("path").get<std::string>()] = f.at("sha256").get<std::string>();
  } catch (const std::exception& e) {
    *why = std::string("the manifest does not parse: ") + e.what();
    return false;
  }
  std::set<std::string> pinned;
  for (const PinnedFile& p : pins.files) pinned.insert(p.path);
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const fs::file_status st = it->symlink_status(ec);  // a link at a pinned name is refused below, where each pinned file is read
    if (ec) break;
    if (fs::is_directory(st)) continue;
    const std::string rel = fs::relative(it->path(), dir, ec).generic_string();
    if (ec) break;
    if (rel == kStockDataManifestName) continue;
    if (!pinned.count(rel)) {
      *why = "the cache holds '" + rel + "', which is not a pinned file";
      return false;
    }
  }
  if (ec) {
    *why = "the cache cannot be listed: " + ec.message();
    return false;
  }
  files->clear();
  for (const PinnedFile& p : pins.files) {
    const fs::path path = dir / p.path;
    const std::uintmax_t size = fs::is_regular_file(fs::symlink_status(path, ec)) ? fs::file_size(path, ec) : 0;
    if (ec || !fs::is_regular_file(fs::symlink_status(path, ec))) {
      *why = "'" + p.path + "' is missing from the cache";
      return false;
    }
    MeasuredFile m;
    m.path = p.path;
    m.size = size;
    if (rehash) {
      std::uint64_t hashed = 0;
      if (!HashFile(path.string(), &m.sha256, &hashed).ok) {
        *why = "'" + p.path + "' cannot be read";
        return false;
      }
      m.size = hashed;
    } else {
      const auto it = recorded.find(p.path);
      if (it == recorded.end()) {
        *why = "the manifest does not record '" + p.path + "'";
        return false;
      }
      m.sha256 = it->second;
    }
    files->push_back(std::move(m));
  }
  const Status check = CheckTreeAgainstPins(pins, *files);
  if (!check.ok) {
    *why = check.code + ": " + check.detail;
    return false;
  }
  return true;
}

Status Extract(const StockDataOptions& options, const fs::path& partial, BuildLog& log, std::vector<MeasuredFile>* files) {
  FileSource obb;
  if (Status s = obb.Open(options.obb_path); !s.ok) return s;
  ObbFooter footer;
  if (Status s = ParseObbFooter(obb, &footer); !s.ok) return s;
  log.Info("obb_footer", {{"package", footer.package_name}, {"footer_version", footer.footer_version}, {"package_version", footer.package_version},
                          {"flags", footer.flags}, {"footer_bytes", footer.footer_bytes}, {"image_bytes", footer.image_bytes},
                          {"image_sectors", footer.image_bytes / kObbSectorBytes}});
  // The key stays in this function: it is derived, used by the source and never written to the log.
  const VolumeKey key = DeriveVolumeKey(options.key_string, footer.salt);
  DecryptingSource image(obb, key, footer.image_bytes);
  std::unique_ptr<Fat16Volume> volume;
  if (Status s = Fat16Volume::Open(image, &volume); !s.ok) return s;
  log.Info("image_opened", {{"filesystem", "FAT16"}, {"clusters", volume->cluster_count()}, {"cluster_bytes", volume->cluster_bytes()}});

  std::error_code ec;
  fs::create_directories(partial, ec);
  if (ec) return Status::Fail("cache_unwritable", "cannot create '" + partial.string() + "': " + ec.message());

  std::set<std::string> written;
  // Every member is in the partial's manifest before it is made: its folders above it, as create_directories makes them, and
  // the entry itself. A folder is recorded once.
  std::set<std::string> recorded;
  return volume->Walk([&](const std::string& path, const FatEntry& entry) -> Status {
    if (!written.insert(path).second) return Status::Fail("fat_duplicate_name", "the image names '" + path + "' twice");
    if (!nevr_owned::ValidEntry(path)) return Status::Fail("fat_unsafe_name", "the image names '" + path + "', which is not a plain path");
    std::vector<std::string> members;
    for (std::size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1)) {
      const std::string folder = path.substr(0, slash);
      if (recorded.insert(folder).second) members.push_back(folder);
    }
    if (recorded.insert(path).second) members.push_back(path);
    if (!nevr_quest::data::RecordOwned(partial.string(), members)) {
      return Status::Fail("cache_write_failed", "recording '" + path + "' in the manifest of '" + partial.string() + "' failed");
    }
    const fs::path target = partial / path;
    std::error_code e;
    if (entry.is_directory) {
      fs::create_directories(target, e);
      if (e) return Status::Fail("cache_unwritable", "cannot create '" + target.string() + "': " + e.message());
      return Status::Ok();
    }
    const auto start = std::chrono::steady_clock::now();
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) return Status::Fail("cache_unwritable", "cannot create '" + target.string() + "'");
    nevr_quest::data::Sha256 hash;
    std::uint64_t bytes = 0;
    Status read = volume->ReadFile(entry, [&](const std::uint8_t* data, std::size_t n) -> Status {
      out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
      if (!out) return Status::Fail("cache_write_failed", "writing '" + target.string() + "' failed");
      hash.Update(data, n);
      bytes += n;
      return Status::Ok();
    });
    out.close();
    if (!read.ok) return read;
    if (!out) return Status::Fail("cache_write_failed", "closing '" + target.string() + "' failed");
    MeasuredFile m{path, hash.Hex(), bytes};
    log.Info("file_extracted", {{"path", m.path}, {"bytes", m.size}, {"sha256", m.sha256}, {"ms", MillisSince(start)}});
    files->push_back(std::move(m));
    return Status::Ok();
  });
}

Status Run(const StockDataOptions& options, BuildLog& log, StockDataResult* result) {
  std::string pins_text;
  if (Status s = ReadText(options.pins_path, &pins_text, "pins_unreadable"); !s.ok) return s;
  StockPins pins;
  if (Status s = ParseStockPins(pins_text, &pins); !s.ok) return s;
  const std::vector<std::string> pinned_names = PinnedMembers(pins);
  log.Info("pins_loaded", {{"pins", options.pins_path}, {"obb_sha256", pins.obb_sha256}, {"obb_bytes", pins.obb_size}, {"files", pins.files.size()}});

  const auto hash_start = std::chrono::steady_clock::now();
  std::string obb_sha;
  std::uint64_t obb_size = 0;
  if (Status s = HashFile(options.obb_path, &obb_sha, &obb_size); !s.ok) return s;
  log.Info("obb_hashed", {{"obb", options.obb_path}, {"sha256", obb_sha}, {"bytes", obb_size}, {"ms", MillisSince(hash_start)}});
  if (Status s = CheckObbAgainstPins(pins, obb_sha, obb_size); !s.ok) return s;

  result->obb_sha256 = obb_sha;
  const fs::path dir = fs::path(options.cache_root) / obb_sha;
  result->directory = (fs::path(options.cache_root) / obb_sha).generic_string();
  std::error_code ec;
  if (fs::exists(dir, ec)) {
    std::string why;
    if (CacheIsValid(dir, pins, options.rehash_cache, &result->files, &why)) {
      result->cache_hit = true;
      log.Info("cache_hit", {{"directory", result->directory}, {"files", result->files.size()}, {"rehashed", options.rehash_cache}});
      log.Info("stock_data_ready", {{"directory", result->directory}, {"files", FilesJson(result->files)}, {"cache_hit", true}});
      return Status::Ok();
    }
    log.Warning("cache_invalid", {{"directory", result->directory}, {"reason", why}});
    if (Status s = RemoveMade(dir, log, pinned_names); !s.ok) return s;
  } else {
    log.Info("cache_miss", {{"directory", result->directory}});
    if (Status s = RemoveMade(dir, log, pinned_names); !s.ok) return s;  // a marker whose folder is gone: nothing else is there
  }

  fs::create_directories(options.cache_root, ec);
  if (ec) return Status::Fail("cache_unwritable", "cannot create '" + options.cache_root + "': " + ec.message());
  // The cache root's ledger names every cache and partial this run made; the names that are gone are pruned here, so it cannot grow
  // past its cap.
  nevr_quest::data::CleanLeftovers(options.cache_root, nullptr);
  const fs::path partial = fs::path(options.cache_root) / (obb_sha + ".partial");
  if (fs::exists(partial, ec)) log.Warning("partial_removed", {{"directory", partial.generic_string()}});
  if (Status s = RemoveMade(partial, log, pinned_names); !s.ok) return s;
  // The marker is made before the folder: every member is then recorded in it before the member is made.
  if (!nevr_quest::data::MarkOwned(partial.string())) {
    return Status::Fail("cache_unwritable", "cannot mark '" + partial.string() + "'");
  }
  RemoveOnExit cleanup(partial, log, pinned_names);
  cleanup.Arm();
  result->files.clear();
  const auto extract_start = std::chrono::steady_clock::now();
  if (Status s = Extract(options, partial, log, &result->files); !s.ok) return s;
  if (Status s = CheckTreeAgainstPins(pins, result->files); !s.ok) return s;

  const Json manifest = {{"schema", 1}, {"obb_sha256", obb_sha}, {"obb_bytes", obb_size}, {"files", FilesJson(result->files)}};
  if (!nevr_quest::data::RecordOwned(partial.string(), {kStockDataManifestName})) {
    return Status::Fail("cache_write_failed", "recording the manifest of '" + partial.string() + "' failed");
  }
  {
    std::ofstream out(partial / kStockDataManifestName, std::ios::binary | std::ios::trunc);
    out << manifest.dump(2) << "\n";
    out.close();
    if (!out) return Status::Fail("cache_write_failed", "writing the manifest of '" + partial.string() + "' failed");
  }
  // The cache's own marker carries the same members, and is made before the folder moves: a crash between the two leaves a
  // marked partial (removed by its manifest) or a marked cache with no folder (its marker removed), never an unmarked cache.
  std::vector<std::string> members;
  if (!nevr_quest::data::ReadOwnerManifest(partial.string(), &members)) {
    return Status::Fail("cache_write_failed", "the manifest of '" + partial.string() + "' does not read back");
  }
  if (!nevr_quest::data::MarkOwned(dir.string()) || !nevr_quest::data::RecordOwned(dir.string(), members)) {
    return Status::Fail("cache_unwritable", "cannot mark '" + dir.string() + "'");
  }
  fs::rename(partial, dir, ec);
  if (ec) {
    if (Status s = RemoveMade(dir, log, pinned_names); !s.ok) return s;
    return Status::Fail("cache_unwritable", "cannot move '" + partial.string() + "' to '" + dir.string() + "': " + ec.message());
  }
  cleanup.Release();
  // The folder moved, so the cache is complete: only the partial's marker is left, and a refusal there is logged, not fatal.
  const nevr_quest::data::ItemRemoval left = nevr_quest::data::RemoveUnionRecord(partial.string(), pinned_names);
  if (!left.removed) log.Warning("partial_marker_left", {{"directory", partial.generic_string()}, {"outcome", left.outcome}, {"entry", left.entry}});
  std::uint64_t total = 0;
  for (const MeasuredFile& f : result->files) total += f.size;
  log.Info("stock_data_ready", {{"directory", result->directory}, {"files", FilesJson(result->files)}, {"cache_hit", false}, {"bytes", total},
                                {"ms", MillisSince(extract_start)}});
  return Status::Ok();
}

}  // namespace

Status HashFile(const std::string& path, std::string* sha256, std::uint64_t* size) {
  FileSource file;
  if (Status s = file.Open(path); !s.ok) return s;
  nevr_quest::data::Sha256 hash;
  std::vector<std::uint8_t> buffer(kHashChunk);
  std::uint64_t offset = 0;
  while (offset < file.Size()) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), file.Size() - offset));
    if (Status s = file.ReadAt(offset, buffer.data(), n); !s.ok) return s;
    hash.Update(buffer.data(), n);
    offset += n;
  }
  *sha256 = hash.Hex();
  *size = file.Size();
  return Status::Ok();
}

Status BuildStockData(const StockDataOptions& options, BuildLog& log, StockDataResult* result) {
  Status status;
  try {
    status = Run(options, log, result);
  } catch (const std::exception& e) {
    status = Status::Fail("internal_error", e.what());
  }
  if (!status.ok) log.Error("refused", {{"code", status.code}, {"detail", status.detail}});
  return status;
}

}  // namespace nevr_quest::obb

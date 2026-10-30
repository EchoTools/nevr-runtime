#include "quest/data/pack.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>

#include <nlohmann/json.hpp>

#include "quest/data/sha256.h"
#include "quest/data/staging.h"

namespace nevr_quest::data {

namespace {

constexpr std::size_t kIoBuffer = 256 * 1024;

std::string Mib(std::uint64_t bytes) { return std::to_string((bytes + (1024u * 1024u) - 1) / (1024u * 1024u)); }

bool IsHex64(const std::string& text) {
  if (text.size() != 64) return false;
  return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool IsPlainChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-';
}

bool IsId(const std::string& text) {
  if (text.empty() || text.size() > 64) return false;
  const char first = text.front();
  if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9'))) return false;
  return std::all_of(text.begin(), text.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}

// X.Y.Z, each part 1 to 9 digits.
bool IsVersion(const std::string& text) {
  int parts = 0;
  std::size_t at = 0;
  while (at <= text.size()) {
    const std::size_t dot = text.find('.', at);
    const std::string part = text.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
    if (part.empty() || part.size() > 9 || !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; })) {
      return false;
    }
    ++parts;
    if (dot == std::string::npos) break;
    at = dot + 1;
  }
  return parts == 3;
}

bool EndsWith(const std::string& text, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return text.size() >= n && text.compare(text.size() - n, n, suffix) == 0;
}

std::string Lower(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

// The pins of the data step this pack must agree with: kManifestPath is `_data/<version>/rad15/android/manifests/<id>`.
std::string PinnedVersionFolder() {
  const std::string path = kManifestPath;
  const std::size_t first = path.find('/');
  const std::size_t second = path.find('/', first + 1);
  return path.substr(first + 1, second - first - 1);
}
std::string PinnedManifestId() {
  const std::string path = kManifestPath;
  return path.substr(path.rfind('/') + 1);
}

bool Refuse(std::string* reason, const char* token) {
  *reason = token;
  return false;
}

const std::string* StringField(const nlohmann::json& object, const char* key) {
  const auto it = object.find(key);
  return it != object.end() && it->is_string() ? &it->get_ref<const std::string&>() : nullptr;
}

bool UnsignedField(const nlohmann::json& object, const char* key, std::uint64_t* out) {
  const auto it = object.find(key);
  if (it == object.end() || !it->is_number_unsigned()) return false;
  *out = it->get<std::uint64_t>();
  return true;
}

// `manifests/<name>` or `packages/<name>`: one plain file name below one of the pack's two directories.
bool IsPackPath(const std::string& path) {
  const std::size_t slash = path.find('/');
  if (slash == std::string::npos) return false;
  const std::string dir = path.substr(0, slash);
  const std::string name = path.substr(slash + 1);
  if (dir != kManifestsDir && dir != kPackagesDir) return false;
  if (name.empty() || name.size() > 64 || name.front() == '.') return false;
  if (!std::all_of(name.begin(), name.end(), IsPlainChar)) return false;
  // Names the step writes for itself are not names a pack may carry.
  return !EndsWith(name, kPackTmpSuffix) && !EndsWith(name, kStockBackupSuffix) && !EndsWith(name, kOwnerSuffix);
}

std::string DirOf(const std::string& path) { return path.substr(0, path.rfind('/')); }

bool Lexists(const std::string& path) {
  struct stat st;
  return ::lstat(path.c_str(), &st) == 0;
}

bool WriteAll(int fd, const unsigned char* bytes, std::size_t length) {
  while (length != 0) {
    const ssize_t n = ::write(fd, bytes, length);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    bytes += n;
    length -= static_cast<std::size_t>(n);
  }
  return true;
}

// The sha-256 and size of the regular file at `path` (never through a link). False when it is not one or cannot be read.
bool HashRegularFile(const std::string& path, std::string* hex, std::uint64_t* size) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return false;
  }
  Sha256 sha;
  std::vector<unsigned char> buffer(kIoBuffer);
  std::uint64_t total = 0;
  bool ok = true;
  for (;;) {
    const ssize_t n = ::read(fd, buffer.data(), buffer.size());
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      ok = false;
      break;
    }
    if (n == 0) break;
    sha.Update(buffer.data(), static_cast<std::size_t>(n));
    total += static_cast<std::uint64_t>(n);
  }
  ::close(fd);
  if (!ok) return false;
  *hex = sha.Hex();
  *size = total;
  return true;
}

// What the root's stock manifest hashes to, as a log token: the digest, `none` (no file) or `unreadable`.
std::string ManifestDigest(const std::string& path) {
  std::string hex;
  std::uint64_t size = 0;
  if (HashRegularFile(path, &hex, &size)) return hex;
  return Lexists(path) ? "unreadable" : "none";
}

struct Step {
  const PackConfig& config;
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();

  void Emit(EventLevel level, const char* name, std::vector<std::pair<std::string, std::string>> fields) const {
    if (config.on_event) config.on_event(level, Event{name, std::move(fields)});
  }
  long long ElapsedMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
  }
};

// Writes the pack's `file` to `<dest>.nevr-pack.tmp`: a marker first, the bytes from the APK entry, fsync, the size and sha-256
// pack.json names. Leaves the verified temporary in place; on a failure removes what it made (except for an interruption).
PackStatus WriteTemporary(const std::string& apk_path, const ZipEntry& entry, const PackFile& file, const std::string& tmp,
                          std::string* detail) {
  // The name must be free for O_EXCL. A leftover of ours was cleaned already; a file here with no marker is not ours to remove.
  if (Lexists(tmp)) {
    if (!IsOwned(tmp)) {
      *detail = "temporary name taken";
      return PackStatus::kWriteFailed;
    }
    RemoveTree(tmp);
  }
  if (!MarkOwned(tmp)) {
    *detail = "marker";
    return PackStatus::kWriteFailed;
  }
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) {
    *detail = std::string("open errno=") + std::to_string(errno);
    RemoveTree(OwnerMarkerPath(tmp));
    return PackStatus::kWriteFailed;
  }
  Sha256 sha;
  std::uint64_t written = 0;
  bool write_failed = false;
  const ZipStatus read = ReadZipEntry(apk_path, entry,
                                      [&](const unsigned char* bytes, std::size_t length) {
                                        if (!WriteAll(fd, bytes, length)) {
                                          write_failed = true;
                                          return false;
                                        }
                                        sha.Update(bytes, length);
                                        written += length;
                                        return true;
                                      },
                                      detail);
  const bool synced = read == ZipStatus::kOk && ::fsync(fd) == 0;
  ::close(fd);
  PackStatus status = PackStatus::kOk;
  if (read != ZipStatus::kOk) {
    *detail = std::string(ZipStatusName(read)) + " " + *detail;
    status = write_failed ? PackStatus::kWriteFailed : PackStatus::kExtractFailed;
  } else if (!synced) {
    *detail = "fsync";
    status = PackStatus::kWriteFailed;
  } else if (written != file.size) {
    *detail = "size " + std::to_string(written) + " expected " + std::to_string(file.size);
    status = PackStatus::kHashMismatch;
  } else if (sha.Hex() != file.sha256) {
    *detail = "sha256";
    status = PackStatus::kHashMismatch;
  }
  if (status != PackStatus::kOk) {
    RemoveTree(tmp);
    RemoveTree(OwnerMarkerPath(tmp));
  }
  return status;
}

// Keeps the stock manifest at `dest` as `<dest>.stock` unless a backup is already there: a marker first, a verified copy in a
// temporary, one rename. A failure leaves `dest` as it was.
PackStatus KeepStockManifest(const std::string& dest, const std::string& stock_sha256, std::string* detail, std::string* kept) {
  const std::string backup = dest + kStockBackupSuffix;
  if (Lexists(backup)) {
    *kept = "existing";
    return PackStatus::kOk;
  }
  const std::string tmp = backup + kPackTmpSuffix;
  RemoveTree(tmp);
  if (!MarkOwned(backup) || !MarkOwned(tmp)) {
    *detail = "marker";
    return PackStatus::kBackupFailed;
  }
  const int in = ::open(dest.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  const int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
  bool ok = in >= 0 && out >= 0;
  Sha256 sha;
  if (ok) {
    std::vector<unsigned char> buffer(kIoBuffer);
    for (;;) {
      const ssize_t n = ::read(in, buffer.data(), buffer.size());
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) {
        ok = false;
        break;
      }
      if (n == 0) break;
      sha.Update(buffer.data(), static_cast<std::size_t>(n));
      if (!WriteAll(out, buffer.data(), static_cast<std::size_t>(n))) {
        ok = false;
        break;
      }
    }
  }
  if (in >= 0) ::close(in);
  if (out >= 0) {
    ok = ok && ::fsync(out) == 0;
    ::close(out);
  }
  if (ok && sha.Hex() != stock_sha256) {  // the file changed between the check and the copy
    *detail = "stock manifest changed";
    ok = false;
  }
  if (ok && ::rename(tmp.c_str(), backup.c_str()) != 0) ok = false;
  if (!ok) {
    if (detail->empty()) *detail = "copy errno=" + std::to_string(errno);
    RemoveTree(tmp);
    RemoveTree(OwnerMarkerPath(tmp));
    RemoveTree(OwnerMarkerPath(backup));
    return PackStatus::kBackupFailed;
  }
  SyncDir(DirOf(backup));
  ::unlink(OwnerMarkerPath(tmp).c_str());
  SyncDir(DirOf(backup));
  *kept = "kept";
  return PackStatus::kOk;
}

}  // namespace

ZipLimits PackZipLimits() noexcept {
  ZipLimits limits;
  limits.max_entries = 16384;
  limits.max_entry_bytes = kMaxPackFileBytes;
  limits.max_total_bytes = 0xFFFFFFFFull;
  return limits;
}

const char* PackOutcomeName(PackOutcome outcome) noexcept {
  switch (outcome) {
    case PackOutcome::kNone: return "none";
    case PackOutcome::kInvalid: return "invalid";
    case PackOutcome::kStockMismatch: return "stock_mismatch";
    case PackOutcome::kCurrent: return "current";
    case PackOutcome::kPlaced: return "placed";
    case PackOutcome::kFailed: return "failed";
  }
  return "unknown";
}

const char* PackStatusName(PackStatus status) noexcept {
  switch (status) {
    case PackStatus::kOk: return "ok";
    case PackStatus::kBadRoot: return "bad_root";
    case PackStatus::kNoSpace: return "no_space";
    case PackStatus::kExtractFailed: return "extract_failed";
    case PackStatus::kHashMismatch: return "hash_mismatch";
    case PackStatus::kWriteFailed: return "write_failed";
    case PackStatus::kBackupFailed: return "backup_failed";
    case PackStatus::kInterrupted: return "interrupted";
  }
  return "unknown";
}

bool ParsePackJson(const std::string& text, PackInfo* out, std::string* reason) {
  *out = PackInfo{};
  reason->clear();
  if (text.size() > kMaxPackJsonBytes) return Refuse(reason, "too_large");
  const nlohmann::json doc = nlohmann::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object()) return Refuse(reason, "not_json");
  const auto schema = doc.find("schema");
  if (schema == doc.end() || !schema->is_number_integer() || schema->get<long long>() != 1) return Refuse(reason, "bad_schema");
  PackInfo info;
  const std::string* id = StringField(doc, "id");
  if (id == nullptr || !IsId(*id)) return Refuse(reason, "bad_id");
  const std::string* version = StringField(doc, "version");
  if (version == nullptr || !IsVersion(*version)) return Refuse(reason, "bad_version");
  const std::string* stock = StringField(doc, "stock_data_version");
  if (stock == nullptr || *stock != PinnedVersionFolder()) return Refuse(reason, "bad_stock_version");
  info.id = *id;
  info.version = *version;
  info.stock_data_version = *stock;

  const auto requires_it = doc.find("requires_stock_manifest_sha256");
  if (requires_it == doc.end() || !requires_it->is_array() || requires_it->empty() || requires_it->size() > 2) {
    return Refuse(reason, "bad_requires");
  }
  for (const nlohmann::json& hash : *requires_it) {
    if (!hash.is_string() || !IsHex64(hash.get_ref<const std::string&>())) return Refuse(reason, "bad_requires");
    const std::string& value = hash.get_ref<const std::string&>();
    if (!info.requires_stock_sha256.empty() && !(info.requires_stock_sha256.back() < value)) {
      return Refuse(reason, "bad_requires");  // sorted, no duplicates
    }
    info.requires_stock_sha256.push_back(value);
  }

  const auto files_it = doc.find("files");
  if (files_it == doc.end() || !files_it->is_array() || files_it->empty()) return Refuse(reason, "bad_file");
  if (files_it->size() > kMaxPackFiles) return Refuse(reason, "too_many_files");
  std::set<std::string> seen;
  const std::string wanted_manifest = std::string(kManifestsDir) + "/" + PinnedManifestId();
  for (const nlohmann::json& entry : *files_it) {
    if (!entry.is_object()) return Refuse(reason, "bad_file");
    const std::string* path = StringField(entry, "path");
    const std::string* sha = StringField(entry, "sha256");
    PackFile file;
    if (path == nullptr || !IsPackPath(*path)) return Refuse(reason, "bad_file");
    if (sha == nullptr || !IsHex64(*sha)) return Refuse(reason, "bad_sha256");
    if (!UnsignedField(entry, "size", &file.size) || file.size == 0 || file.size > kMaxPackFileBytes) {
      return Refuse(reason, "bad_size");
    }
    if (!seen.insert(Lower(*path)).second) return Refuse(reason, "duplicate_file");
    file.path = *path;
    file.sha256 = *sha;
    if (file.path.compare(0, std::strlen(kManifestsDir), kManifestsDir) == 0) {
      if (file.path != wanted_manifest) return Refuse(reason, "wrong_manifest");
      info.manifest_path = file.path;
      info.manifest_sha256 = file.sha256;
    }
    info.files.push_back(std::move(file));
  }
  if (info.manifest_path.empty()) return Refuse(reason, "no_manifest");

  const std::string* source_sha = StringField(doc, "source_zip_sha256");
  if (source_sha == nullptr || !IsHex64(*source_sha) || !UnsignedField(doc, "source_zip_size", &info.source_size) ||
      info.source_size == 0) {
    return Refuse(reason, "bad_source");
  }
  info.source_sha256 = *source_sha;
  *out = std::move(info);
  return true;
}

std::string BuildPackFloorMessage(const PackFloorInfo& info) {
  std::string text = "Echo VR could not set up its bundled game-data pack (" + info.id + "). ";
  switch (info.status) {
    case PackStatus::kNoSpace:
      text += "Not enough free space: " + Mib(info.available_bytes) + " MiB free, " + Mib(info.needed_bytes) +
              " MiB needed. Free some space on the headset and start the game again.";
      break;
    case PackStatus::kHashMismatch:
      text += "The pack did not verify (" + info.detail + "). Reinstall the game and start it again.";
      break;
    case PackStatus::kExtractFailed:
      text += "It could not be read from the game's own package (" + info.detail + "). Reinstall the game and start it again.";
      break;
    case PackStatus::kBadRoot:
    case PackStatus::kWriteFailed:
    case PackStatus::kBackupFailed:
      text += "It could not be written to the headset storage (" + info.detail +
              "). Check the storage and start the game again.";
      break;
    case PackStatus::kInterrupted:
    case PackStatus::kOk:
      text += "Start the game again to retry.";
      break;
  }
  return text;
}

std::string ApkPathForLibrary(const std::string& library_path, bool (*exists)(const char* path) noexcept) {
  const std::size_t bang = library_path.find("!/");
  if (bang != std::string::npos) {
    const std::string apk = library_path.substr(0, bang);
    return apk.empty() ? std::string() : apk;
  }
  // `<dir>/lib/<abi>/<name>`: the app's install directory holds `lib/` and `base.apk`.
  const std::size_t name_at = library_path.rfind('/');
  if (name_at == std::string::npos || name_at == 0) return "";
  const std::size_t abi_at = library_path.rfind('/', name_at - 1);
  if (abi_at == std::string::npos || abi_at == 0) return "";
  const std::size_t lib_at = library_path.rfind('/', abi_at - 1);
  if (lib_at == std::string::npos) return "";
  if (library_path.compare(lib_at, abi_at - lib_at, "/lib") != 0) return "";
  const std::string apk = library_path.substr(0, lib_at) + "/base.apk";
  return exists != nullptr && exists(apk.c_str()) ? apk : std::string();
}

PackResult RunPackStep(const PackConfig& config) {
  Step step{config};
  PackResult result;
  const auto finish = [&](PackOutcome outcome, PackStatus status) {
    result.outcome = outcome;
    result.status = status;
    if (outcome == PackOutcome::kNone) return result;
    const bool bad = outcome == PackOutcome::kInvalid || outcome == PackOutcome::kFailed;
    step.Emit(bad ? EventLevel::kError : (outcome == PackOutcome::kStockMismatch ? EventLevel::kWarn : EventLevel::kInfo),
              "pack_step",
              {{"outcome", PackOutcomeName(outcome)},
               {"id", result.id},
               {"version", result.version},
               {"root", config.root},
               {"duration_ms", std::to_string(step.ElapsedMs())},
               {"status", PackStatusName(status)}});
    return result;
  };
  const auto none = [&](const char* reason, const std::string& detail) {
    step.Emit(EventLevel::kInfo, "pack_check",
              {{"outcome", "none"}, {"reason", reason}, {"detail", detail}, {"apk", config.apk_path}});
    return finish(PackOutcome::kNone, PackStatus::kOk);
  };

  if (config.apk_path.empty()) return none("apk_path_unknown", "");
  std::vector<ZipEntry> entries;
  std::string detail;
  const ZipStatus listed = ReadZipListing(config.apk_path, PackZipLimits(), &entries, &detail);
  if (listed != ZipStatus::kOk) return none("apk_unreadable", std::string(ZipStatusName(listed)) + " " + detail);
  const ZipEntry* json_entry = FindZipEntry(entries, kPackJsonEntry);
  if (json_entry == nullptr) return none("no_pack", "");

  // The pack describes itself.
  std::string json_text;
  bool json_too_big = json_entry->size > kMaxPackJsonBytes;
  if (!json_too_big) {
    const ZipStatus read = ReadZipEntry(config.apk_path, *json_entry,
                                        [&](const unsigned char* bytes, std::size_t length) {
                                          json_text.append(reinterpret_cast<const char*>(bytes), length);
                                          return true;
                                        },
                                        &detail);
    if (read != ZipStatus::kOk) {
      step.Emit(EventLevel::kError, "pack_check",
                {{"outcome", "invalid"}, {"reason", "pack_json_unreadable"}, {"detail", ZipStatusName(read)}});
      return finish(PackOutcome::kInvalid, PackStatus::kExtractFailed);
    }
  }
  PackInfo pack;
  std::string reason;
  if (json_too_big || !ParsePackJson(json_text, &pack, &reason)) {
    step.Emit(EventLevel::kError, "pack_check",
              {{"outcome", "invalid"}, {"reason", json_too_big ? "too_large" : reason}, {"detail", "pack.json"}});
    return finish(PackOutcome::kInvalid, PackStatus::kOk);
  }
  result.id = pack.id;
  result.version = pack.version;

  // Every file is in the APK at the size pack.json says; a pack that names a file the APK lacks is not one to place.
  std::vector<const ZipEntry*> apk_entries;
  for (const PackFile& file : pack.files) {
    const ZipEntry* entry = FindZipEntry(entries, std::string(kPackFilesPrefix) + file.path);
    if (entry == nullptr || entry->IsDirectory() || entry->size != file.size) {
      step.Emit(EventLevel::kError, "pack_check",
                {{"outcome", "invalid"}, {"reason", entry == nullptr ? "file_missing" : "file_size"}, {"id", pack.id},
                 {"version", pack.version}, {"detail", file.path}});
      return finish(PackOutcome::kInvalid, PackStatus::kOk);
    }
    apk_entries.push_back(entry);
  }

  // The root is written below only after the leftovers of an interrupted run are gone.
  int clean_failed = 0;
  const int cleaned = CleanPackLeftovers(config.root, &clean_failed);
  if (cleaned != 0 || clean_failed != 0) {
    step.Emit(EventLevel::kInfo, "data_recover",
              {{"root", config.root}, {"cleaned", std::to_string(cleaned)}, {"failed", std::to_string(clean_failed)}});
  }

  const std::string android_rel = std::string(data_root::kMarker) + "/" + pack.stock_data_version + "/rad15/android";
  const std::string android = config.root + "/" + android_rel;
  const std::string manifest_path = android + "/" + pack.manifest_path;
  const std::string stock_sha256 = ManifestDigest(manifest_path);
  const bool stock_ok = std::find(pack.requires_stock_sha256.begin(), pack.requires_stock_sha256.end(), stock_sha256) !=
                        pack.requires_stock_sha256.end();
  const bool ours = stock_sha256 == pack.manifest_sha256;
  std::string required;
  for (const std::string& hash : pack.requires_stock_sha256) required += (required.empty() ? "" : ",") + hash;
  std::vector<std::pair<std::string, std::string>> check_fields = {
      {"id", pack.id},
      {"version", pack.version},
      {"root", config.root},
      {"apk", config.apk_path},
      {"manifest_sha256", stock_sha256},
      {"requires_stock_manifest_sha256", required},
      {"pack_manifest_sha256", pack.manifest_sha256}};
  // `pack_check` leads with its outcome; the rest of the fields are the same whichever it is.
  const auto check_with = [&check_fields](const char* outcome) {
    std::vector<std::pair<std::string, std::string>> fields = {{"outcome", outcome}};
    fields.insert(fields.end(), check_fields.begin(), check_fields.end());
    return fields;
  };
  if (!stock_ok && !ours) {
    step.Emit(EventLevel::kWarn, "pack_check", check_with("stock_mismatch"));
    return finish(PackOutcome::kStockMismatch, PackStatus::kOk);
  }

  // Order of placement: every package, then the manifest that names them.
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < pack.files.size(); ++i) {
    if (pack.files[i].path != pack.manifest_path) order.push_back(i);
  }
  for (std::size_t i = 0; i < pack.files.size(); ++i) {
    if (pack.files[i].path == pack.manifest_path) order.push_back(i);
  }
  std::vector<bool> current(pack.files.size(), false);
  std::uint64_t needed = 0;
  for (const std::size_t i : order) {
    const PackFile& file = pack.files[i];
    std::string hex;
    std::uint64_t size = 0;
    if (file.path == pack.manifest_path) {
      current[i] = ours;
    } else {
      current[i] = HashRegularFile(android + "/" + file.path, &hex, &size) && size == file.size && hex == file.sha256;
    }
    if (!current[i]) needed += file.size;
  }
  const bool all_current = std::all_of(current.begin(), current.end(), [](bool c) { return c; });
  step.Emit(EventLevel::kInfo, "pack_check", check_with(all_current ? "current" : "place"));
  if (all_current) {
    for (const std::size_t i : order) {
      step.Emit(EventLevel::kInfo, "pack_place",
                {{"path", pack.files[i].path}, {"bytes", std::to_string(pack.files[i].size)},
                 {"sha256", pack.files[i].sha256}, {"result", "current"}});
    }
    return finish(PackOutcome::kCurrent, PackStatus::kOk);
  }

  const auto fail = [&](PackStatus status, const std::string& why, std::uint64_t need, std::uint64_t avail) {
    PackFloorInfo info;
    info.status = status;
    info.detail = why;
    info.id = pack.id;
    info.needed_bytes = need;
    info.available_bytes = avail;
    result.message = BuildPackFloorMessage(info);
    if (config.present) config.present(result.message);
    if (config.hold) config.hold();
    return finish(PackOutcome::kFailed, status);
  };

  // Space: what is written, and the headroom the temporary and the manifest's backup need beside it.
  needed += kPackHeadroomBytes;
  std::uint64_t available = 0;
  if (config.available_bytes) {
    available = config.available_bytes(config.root);
  } else if (CheckSpace(config.root, needed, &available) == StageStatus::kStatFailed) {
    step.Emit(EventLevel::kError, "pack_place", {{"path", config.root}, {"result", "failed"}, {"detail", "statvfs"}});
    return fail(PackStatus::kBadRoot, "statvfs", needed, 0);
  }
  if (available < needed) {
    step.Emit(EventLevel::kError, "pack_place",
              {{"path", config.root}, {"result", "failed"}, {"detail", "no_space"},
               {"free_bytes", std::to_string(available)}, {"needed_bytes", std::to_string(needed)}});
    return fail(PackStatus::kNoSpace, "no_space", needed, available);
  }
  if (!MakeDirsBelow(config.root, android_rel + "/" + kManifestsDir) ||
      !MakeDirsBelow(config.root, android_rel + "/" + kPackagesDir)) {
    step.Emit(EventLevel::kError, "pack_place", {{"path", android}, {"result", "failed"}, {"detail", "mkdir"}});
    return fail(PackStatus::kBadRoot, "mkdir " + android, needed, available);
  }

  bool placed_any = false;
  for (const std::size_t i : order) {
    const PackFile& file = pack.files[i];
    const std::string dest = android + "/" + file.path;
    std::vector<std::pair<std::string, std::string>> fields = {
        {"path", file.path}, {"bytes", std::to_string(file.size)}, {"sha256", file.sha256}};
    if (current[i]) {
      fields.push_back({"result", "current"});
      step.Emit(EventLevel::kInfo, "pack_place", fields);
      continue;
    }
    const std::string tmp = dest + kPackTmpSuffix;
    std::string why;
    PackStatus status = WriteTemporary(config.apk_path, *apk_entries[i], file, tmp, &why);
    if (status == PackStatus::kOk && config.probe && !config.probe("tmp_written", dest)) {
      fields.push_back({"result", "interrupted"});
      step.Emit(EventLevel::kError, "pack_place", fields);
      return finish(PackOutcome::kFailed, PackStatus::kInterrupted);
    }
    std::string backup = "none";
    if (status == PackStatus::kOk && file.path == pack.manifest_path) {
      status = KeepStockManifest(dest, stock_sha256, &why, &backup);
      if (status != PackStatus::kOk) {
        RemoveTree(tmp);
        RemoveTree(OwnerMarkerPath(tmp));
      }
    }
    if (status == PackStatus::kOk) {
      if (::rename(tmp.c_str(), dest.c_str()) != 0) {
        why = "rename errno=" + std::to_string(errno);
        RemoveTree(tmp);
        RemoveTree(OwnerMarkerPath(tmp));
        status = PackStatus::kWriteFailed;
      } else {
        SyncDir(DirOf(dest));
        ::unlink(OwnerMarkerPath(tmp).c_str());
        SyncDir(DirOf(dest));
        placed_any = true;
      }
    }
    if (status != PackStatus::kOk) {
      fields.push_back({"result", "failed"});
      fields.push_back({"status", PackStatusName(status)});
      fields.push_back({"detail", why});
      step.Emit(EventLevel::kError, "pack_place", fields);
      return fail(status, why, needed, available);
    }
    fields.push_back({"result", "placed"});
    if (file.path == pack.manifest_path) fields.push_back({"stock_backup", backup});
    step.Emit(EventLevel::kInfo, "pack_place", fields);
    if (config.probe && !config.probe("renamed", dest)) return finish(PackOutcome::kFailed, PackStatus::kInterrupted);
  }

  // The data now in place, read back from the root: a report says which data the player has.
  step.Emit(EventLevel::kInfo, "data_version",
            {{"root", config.root},
             {"data_version", pack.stock_data_version},
             {"manifest_sha256", ManifestDigest(manifest_path)},
             {"pack", pack.id},
             {"pack_version", pack.version}});
  return finish(placed_any ? PackOutcome::kPlaced : PackOutcome::kCurrent, PackStatus::kOk);
}

}  // namespace nevr_quest::data

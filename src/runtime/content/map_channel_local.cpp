// LOCAL sideload for the editor's test path (map_channel.h): a signed <id>.windows-x86_64.nevrmap in <nevr root>\maps\local
// is verified against the machine's dev key when it is new, extracted beside it, and at later boots trusted by stamps.
#include <winsock2.h>
#include <windows.h>

#include "runtime/content/map_channel.h"

#include <algorithm>
#include <array>
#include <set>

#include <nlohmann/json.hpp>
#include <zlib.h>

#include "core/map_manifest.h"
#include "core/minisign_verify.h"

namespace nevr_map_channel {
namespace detail {
namespace {

using json = nlohmann::json;
using nevr_maps::FileStamp;
using nevr_maps::Member;

constexpr std::size_t kChunk = 1u << 20;

std::uint32_t U16(const std::string& b, std::size_t at) {
  return static_cast<std::uint8_t>(b[at]) | (static_cast<std::uint32_t>(static_cast<std::uint8_t>(b[at + 1])) << 8);
}
std::uint32_t U32(const std::string& b, std::size_t at) { return U16(b, at) | (U16(b, at + 2) << 16); }

bool SafeName(const std::string& name) {
  if (name.empty() || name.front() == '/' || name.find('\\') != std::string::npos ||
      name.find(':') != std::string::npos || name.find("..") != std::string::npos) {
    return false;
  }
  return std::all_of(name.begin(), name.end(), [](char c) { return c >= 0x20 && c < 0x7f; });
}

std::string HexOf(const void* data, std::size_t length) {
  static const char kDigits[] = "0123456789abcdef";
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::string out;
  out.reserve(2 * length);
  for (std::size_t i = 0; i < length; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 0xF]);
  }
  return out;
}

bool IsLeafName(const std::string& leaf) {
  return leaf.size() == 12 &&
         std::all_of(leaf.begin(), leaf.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string Sha(nevr_cargo::Env& env, const std::string& bytes) {
  std::unique_ptr<nevr_cargo::Hasher> h = env.NewHasher();
  h->Update(bytes.data(), bytes.size());
  return h->Hex();
}

bool ReadExact(nevr_cargo::Env& env, std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length) {
  std::size_t done = 0;
  while (done < length) {
    std::size_t got = 0;
    if (!env.ReadBlob(handle, offset + done, static_cast<char*>(buffer) + done, length - done, &got) || got == 0) {
      return false;
    }
    done += got;
  }
  return true;
}

// [0, size) of a held file through `each`, one 1 MiB piece at a time.
bool StreamHeld(nevr_cargo::Env& env, std::int64_t handle, std::uint64_t size, std::vector<char>* buffer,
                const std::function<void(const char*, std::size_t)>& each) {
  buffer->resize(kChunk);
  for (std::uint64_t at = 0; at < size;) {
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size - at, kChunk));
    if (!ReadExact(env, handle, at, buffer->data(), want)) return false;
    each(buffer->data(), want);
    at += want;
  }
  return true;
}

std::uint64_t HexValue(const json& doc, const char* key) {
  const auto it = doc.find(key);
  if (it == doc.end() || !it->is_string()) return 0;
  return std::strtoull(it->get<std::string>().c_str(), nullptr, 16);
}

std::string Text(const json& doc, const char* key) {
  const auto it = doc.find(key);
  return it != doc.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string ParentOf(const std::string& path) {
  const std::size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// The package from its package.json, pinned to `sha256` as a channel entry pins it.
std::string ParseIndex(const std::string& text, const std::string& sha256, const std::string& id,
                       nevr_maps::Package* out) {
  const json doc = json::parse(text, nullptr, false);
  if (!doc.is_object()) return "index_json";
  nevr_maps::PlatformEntry entry;
  entry.version = Text(doc, "version");
  entry.level = HexValue(doc, "level");
  entry.level_name = Text(doc, "level_name");
  entry.game_build = Text(doc, "game_build");
  entry.index.sha256 = sha256;
  entry.index.size = text.size();
  nevr_maps::ParsedPackage parsed = nevr_maps::ParsePackageIndex(text, id, nevr_maps::kPlatformPc, entry);
  if (!parsed.reason.empty()) return "index_" + parsed.reason;
  *out = std::move(parsed.package);
  return std::string();
}

// <cache>/<id>/local.json: what the last import verified. Unsigned, like the CDN cache's index.json: equal stamps say
// the bytes on disk are the bytes that were hashed.
struct Record {
  std::string leaf, dev_key, minisig, index, index_sha256;
  FileStamp nevrmap;
  std::map<std::string, FileStamp> files;  // member path -> stamp
};

json StampJson(const FileStamp& s) { return json::array({s.volume, s.id_hi, s.id_lo, s.size, s.last_write}); }

bool StampFrom(const json& j, FileStamp* s) {
  if (!j.is_array() || j.size() != 5) return false;
  for (const json& v : j) {
    if (!v.is_number_unsigned()) return false;
  }
  s->volume = j[0].get<std::uint64_t>();
  s->id_hi = j[1].get<std::uint64_t>();
  s->id_lo = j[2].get<std::uint64_t>();
  s->size = j[3].get<std::uint64_t>();
  s->last_write = j[4].get<std::uint64_t>();
  return true;
}

bool ReadRecord(nevr_cargo::Env& env, const std::string& path, Record* r) {
  std::string text;
  if (!env.ReadSmallFile(path, &text, 2 * nevr_maps::kMaxIndexBytes)) return false;
  const json doc = json::parse(text, nullptr, false);
  if (!doc.is_object()) return false;
  const auto schema = doc.find("schema");
  if (schema == doc.end() || !schema->is_number_unsigned() || schema->get<std::uint64_t>() != 1) return false;
  r->leaf = Text(doc, "leaf");
  r->dev_key = Text(doc, "dev_key");
  r->minisig = Text(doc, "minisig");
  r->index = Text(doc, "index");
  r->index_sha256 = Text(doc, "index_sha256");
  const auto nevrmap = doc.find("nevrmap");
  const auto files = doc.find("files");
  if (nevrmap == doc.end() || !StampFrom(*nevrmap, &r->nevrmap) || files == doc.end() || !files->is_object()) return false;
  for (auto it = files->begin(); it != files->end(); ++it) {
    if (!StampFrom(it.value(), &r->files[it.key()])) return false;
  }
  return IsLeafName(r->leaf) && !r->index.empty();
}

void WriteRecord(nevr_cargo::Env& env, const std::string& path, const Record& r) {
  json files = json::object();
  for (const auto& [member, stamp] : r.files) files[member] = StampJson(stamp);
  const json doc = {{"schema", 1},          {"leaf", r.leaf},   {"dev_key", r.dev_key},
                    {"minisig", r.minisig}, {"index", r.index}, {"index_sha256", r.index_sha256},
                    {"nevrmap", StampJson(r.nevrmap)}, {"files", files}};
  // A write that fails costs the next boot an import, nothing else.
  env.WriteFileAtomically(path, doc.dump(-1, ' ', false, json::error_handler_t::replace));
}

// The extraction the record describes, when every member is still the file whose stamp the import took after hashing
// it. Nothing is read or hashed but the record's package.json.
bool Reuse(nevr_cargo::Env& env, nevr_maps::MapFs& fs, const Record& rec, const std::string& map_dir, LocalMap* map) {
  nevr_maps::Package package;
  if (!ParseIndex(rec.index, rec.index_sha256, map->id, &package).empty()) return false;
  const std::string dir = map_dir + "/" + rec.leaf;
  std::map<std::string, FileStamp> stamps;
  for (const Member& m : package.members) {
    const auto verified = rec.files.find(m.path);
    if (verified == rec.files.end()) return false;
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    if (!fs.OpenHeld(dir + "/" + m.path, &handle, &size)) return false;
    FileStamp stamp;
    const bool same = fs.StampOf(handle, &stamp) && stamp == verified->second && size == m.size;
    env.CloseBlob(handle);
    if (!same) return false;
    stamps[m.path] = stamp;
  }
  map->dir = dir;
  map->package = std::move(package);
  map->stamps = std::move(stamps);
  return true;
}

// Every member into <dir>.tmp<pid>, then renamed to <dir>. Nothing here is trusted yet: VerifyHeld hashes it after.
std::string ExtractTo(nevr_cargo::Env& env, nevr_maps::MapFs& fs, const ReadAt& read, const nevr_maps::Package& package,
                      const std::map<std::string, const ZipEntry*>& by_name, const std::string& dir) {
  const std::string tmp = dir + ".tmp" + std::to_string(GetCurrentProcessId());
  fs.RemoveTree(tmp);
  auto abandon = [&](const std::string& reason) {
    fs.RemoveTree(tmp);
    return reason;
  };
  for (const Member& m : package.members) {
    const std::string path = tmp + "/" + m.path;
    if (!env.MakeDirs(ParentOf(path))) return abandon("extract_io");
    std::unique_ptr<nevr_cargo::TempFile> out = env.CreateTemp(ParentOf(path));
    if (out == nullptr) return abandon("extract_io");
    std::string problem;
    const auto write = [&](const char* data, std::size_t length) { return out->Write(data, length); };
    if (!InflateZipEntry(read, *by_name.at(m.path), write, &problem)) {
      out->Abort();
      return abandon(problem == "sink" ? "extract_io" : "zip_" + problem);
    }
    if (!out->Commit(path)) return abandon("extract_io");
  }
  if (!fs.RenameDir(tmp, dir)) return abandon("extract_io");
  return std::string();
}

// The member hashed through a share-read handle, and its stamp taken through the same handle: the stamp a later boot
// compares is the stamp of exactly these bytes.
std::string VerifyHeld(nevr_cargo::Env& env, nevr_maps::MapFs& fs, const std::string& path, const Member& m,
                       std::vector<char>* buffer, FileStamp* stamp) {
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  if (!fs.OpenHeld(path, &handle, &size)) return "member_unreadable";
  std::unique_ptr<nevr_cargo::Hasher> h = env.NewHasher();
  const bool sized = size == m.size;
  const bool read = sized && StreamHeld(env, handle, size, buffer, [&](const char* d, std::size_t n) { h->Update(d, n); });
  const bool stamped = read && fs.StampOf(handle, stamp) && *stamp != FileStamp();
  env.CloseBlob(handle);
  if (!sized) return "member_hash";
  if (!read) return "member_unreadable";
  if (h->Hex() != m.sha256) return "member_hash";
  if (!stamped) return "stamp_unsupported";
  return std::string();
}

// A new or changed .nevrmap, held share-read by the caller: the signature over a streamed BLAKE2b, then the zip in place,
// then each member's sha256 against the signed package.json.
std::string Import(nevr_cargo::Env& env, nevr_maps::MapFs& fs, std::int64_t handle, std::uint64_t size,
                   const std::string& signature, const nevr_minisign::PublicKey& key, const std::string& map_dir,
                   LocalMap* map, Record* rec) {
  std::vector<char> buffer;
  nevr_minisign::PrehashStream prehash;
  std::unique_ptr<nevr_cargo::Hasher> whole = env.NewHasher();
  const auto both = [&](const char* d, std::size_t n) {
    prehash.Update(d, n);
    whole->Update(d, n);
  };
  if (!StreamHeld(env, handle, size, &buffer, both)) return "unreadable";
  std::string comment;
  const nevr_minisign::Verdict v = nevr_minisign::VerifyPrehashed(prehash.Final(), signature, {key}, nullptr, &comment);
  if (v != nevr_minisign::Verdict::kOk) return std::string("signature_") + nevr_minisign::VerdictName(v);
  if (comment != "nevr-local " + map->id) return "signature_comment";
  const std::string leaf = whole->Hex().substr(0, 12);

  const ReadAt read = [&](std::uint64_t offset, void* out, std::size_t length) {
    return ReadExact(env, handle, offset, out, length);
  };
  std::vector<ZipEntry> entries;
  std::string problem;
  if (!ReadZipDirectory(read, size, &entries, &problem)) return "zip_" + problem;
  std::map<std::string, const ZipEntry*> by_name;
  for (const ZipEntry& e : entries) by_name[e.name] = &e;
  const auto index = by_name.find("package.json");
  if (index == by_name.end()) return "no_index";
  if (index->second->size > nevr_maps::kMaxIndexBytes) return "index_too_large";
  std::string index_text;
  const auto append = [&](const char* d, std::size_t n) {
    index_text.append(d, n);
    return true;
  };
  if (!InflateZipEntry(read, *index->second, append, &problem)) return "zip_" + problem;
  const std::string index_sha256 = Sha(env, index_text);
  nevr_maps::Package package;
  std::string reason = ParseIndex(index_text, index_sha256, map->id, &package);
  if (!reason.empty()) return reason;
  for (const Member& m : package.members) {
    const auto found = by_name.find(m.path);
    if (found == by_name.end()) return "member_missing";
    if (found->second->size != m.size) return "member_hash";
  }

  const std::string dir = map_dir + "/" + leaf;
  // A copy another running game holds cannot be replaced; its bytes are hashed below like a fresh extraction's.
  if (fs.RemoveTree(dir)) {
    reason = ExtractTo(env, fs, read, package, by_name, dir);
    if (!reason.empty()) return reason;
  }
  std::map<std::string, FileStamp> stamps;
  for (const Member& m : package.members) {
    reason = VerifyHeld(env, fs, dir + "/" + m.path, m, &buffer, &stamps[m.path]);
    if (!reason.empty()) return reason;
  }

  std::vector<std::string> old;
  if (fs.ListDir(map_dir, &old)) {
    for (const std::string& name : old) {
      if (name != leaf && name != kLocalRecordFile) fs.RemoveTree(map_dir + "/" + name);
    }
  }
  rec->leaf = leaf;
  rec->index = std::move(index_text);
  rec->index_sha256 = index_sha256;
  rec->files = stamps;
  map->dir = dir;
  map->package = std::move(package);
  map->stamps = std::move(stamps);
  map->imported = true;
  return std::string();
}

std::string ScanOne(nevr_cargo::Env& env, nevr_maps::MapFs& fs, const nevr_minisign::PublicKey& key,
                    const std::string& signature, const std::string& map_dir, LocalMap* map) {
  std::int64_t handle = 0;
  std::uint64_t size = 0;
  if (!env.OpenBlob(map->path, &handle, &size)) return "unreadable";
  // Held share-read to the end of the scan, so its stamp, its hash and its extraction are of one file.
  FileStamp stamp;
  if (!fs.StampOf(handle, &stamp)) stamp = FileStamp();
  const bool stamped = stamp != FileStamp();
  Record rec;
  rec.dev_key = HexOf(key.key_id.data(), key.key_id.size()) + HexOf(key.pk.data(), key.pk.size());
  rec.minisig = HexOf(signature.data(), signature.size());
  rec.nevrmap = stamp;
  const std::string record_path = map_dir + "/" + kLocalRecordFile;
  Record last;
  std::string reason;
  if (size > kMaxLocalBytes) {
    reason = "too_large";
  } else if (!(stamped && ReadRecord(env, record_path, &last) && last.nevrmap == stamp && last.dev_key == rec.dev_key &&
               last.minisig == rec.minisig && Reuse(env, fs, last, map_dir, map))) {
    reason = Import(env, fs, handle, size, signature, key, map_dir, map, &rec);
    if (reason.empty() && stamped) WriteRecord(env, record_path, rec);
  }
  env.CloseBlob(handle);
  return reason;
}

}  // namespace

bool ReadZipDirectory(const ReadAt& read, std::uint64_t file_size, std::vector<ZipEntry>* entries, std::string* problem) {
  constexpr std::size_t kEocd = 22;
  if (file_size < kEocd) return *problem = "short", false;
  std::string tail(static_cast<std::size_t>(std::min<std::uint64_t>(file_size, kEocd + 0xffff)), '\0');
  const std::uint64_t tail_at = file_size - tail.size();
  if (!read(tail_at, &tail[0], tail.size())) return *problem = "unreadable", false;
  std::size_t eocd = std::string::npos;
  for (std::size_t at = tail.size() - kEocd + 1; at-- > 0;) {
    if (U32(tail, at) == 0x06054b50) {
      eocd = at;
      break;
    }
  }
  if (eocd == std::string::npos) return *problem = "no_directory", false;
  const std::uint32_t count = U16(tail, eocd + 10), cd_size = U32(tail, eocd + 12), cd_at = U32(tail, eocd + 16);
  if (U16(tail, eocd + 4) != 0 || U16(tail, eocd + 6) != 0 || count == 0xffff || cd_at == 0xffffffffu) {
    return *problem = "zip64", false;
  }
  if (static_cast<std::uint64_t>(cd_at) + cd_size > tail_at + eocd || cd_size > kMaxZipDirectoryBytes) {
    return *problem = "directory_bounds", false;
  }
  std::string directory(cd_size, '\0');
  if (cd_size > 0 && !read(cd_at, &directory[0], cd_size)) return *problem = "unreadable", false;

  std::set<std::string> names;
  std::uint64_t total = 0;
  std::string header(30, '\0');
  std::size_t at = 0;
  for (std::uint32_t i = 0; i < count; ++i) {
    if (at + 46 > directory.size() || U32(directory, at) != 0x02014b50) return *problem = "directory_entry", false;
    const std::uint32_t flags = U16(directory, at + 8), method = U16(directory, at + 10), crc = U32(directory, at + 16);
    const std::uint32_t packed = U32(directory, at + 20), size = U32(directory, at + 24);
    const std::uint32_t name_len = U16(directory, at + 28), extra_len = U16(directory, at + 30);
    const std::uint32_t comment_len = U16(directory, at + 32), local_at = U32(directory, at + 42);
    if (at + 46 + name_len > directory.size()) return *problem = "directory_entry", false;
    std::string name = directory.substr(at + 46, name_len);
    at += 46 + name_len + extra_len + comment_len;
    if ((flags & 1) != 0) return *problem = "encrypted", false;
    if (!name.empty() && name.back() == '/' && size == 0) continue;  // a directory entry
    if (!SafeName(name)) return *problem = "name", false;
    if (packed == 0xffffffffu || size == 0xffffffffu || local_at == 0xffffffffu) return *problem = "zip64", false;
    if (method != 0 && method != 8) return *problem = "method", false;
    if (method == 0 && packed != size) return *problem = "stored_size", false;
    total += size;
    if (total > 2 * kMaxLocalBytes) return *problem = "too_large", false;
    if (static_cast<std::uint64_t>(local_at) + 30 > cd_at || !read(local_at, &header[0], header.size()) ||
        U32(header, 0) != 0x04034b50) {
      return *problem = "local_header", false;
    }
    const std::uint64_t data_at = static_cast<std::uint64_t>(local_at) + 30 + U16(header, 26) + U16(header, 28);
    if (data_at + packed > cd_at) return *problem = "data_bounds", false;
    if (!names.insert(name).second) return *problem = "duplicate", false;
    entries->push_back(ZipEntry{std::move(name), method, crc, packed, size, data_at});
  }
  return true;
}

bool InflateZipEntry(const ReadAt& read, const ZipEntry& entry, const std::function<bool(const char*, std::size_t)>& sink,
                     std::string* problem) {
  uLong crc = crc32(0L, Z_NULL, 0);
  std::uint64_t written = 0;
  const auto emit = [&](const char* data, std::size_t length) {
    crc = crc32(crc, reinterpret_cast<const Bytef*>(data), static_cast<uInt>(length));
    written += length;
    if (sink(data, length)) return true;
    *problem = "sink";
    return false;
  };
  std::vector<char> in(static_cast<std::size_t>(std::min<std::uint64_t>(entry.packed, kChunk)));
  if (entry.method == 0) {
    for (std::uint64_t at = 0; at < entry.packed;) {
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(entry.packed - at, kChunk));
      if (!read(entry.data_at + at, in.data(), n)) return *problem = "unreadable", false;
      if (!emit(in.data(), n)) return false;
      at += n;
    }
  } else {
    std::vector<char> out(kChunk);
    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return *problem = "inflate", false;
    std::uint64_t at = 0;
    bool ok = true, done = false;
    while (ok && !done) {
      if (zs.avail_in == 0 && at < entry.packed) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(entry.packed - at, kChunk));
        if (!read(entry.data_at + at, in.data(), n)) {
          *problem = "unreadable";
          ok = false;
          break;
        }
        at += n;
        zs.next_in = reinterpret_cast<Bytef*>(in.data());
        zs.avail_in = static_cast<uInt>(n);
      }
      zs.next_out = reinterpret_cast<Bytef*>(out.data());
      zs.avail_out = static_cast<uInt>(out.size());
      // With no input left and nothing pending, inflate makes no progress and says Z_BUF_ERROR: a truncated stream.
      const int rc = inflate(&zs, Z_NO_FLUSH);
      done = rc == Z_STREAM_END;
      const std::size_t produced = out.size() - zs.avail_out;
      if ((rc != Z_OK && !done) || written + produced > entry.size) {
        *problem = "inflate";
        ok = false;
      } else if (produced > 0 && !emit(out.data(), produced)) {
        ok = false;
      }
    }
    inflateEnd(&zs);
    if (!ok) return false;
  }
  if (written != entry.size) return *problem = "inflate", false;
  if (crc != entry.crc) return *problem = "crc", false;
  return true;
}

LocalScan ScanLocal(nevr_cargo::Env& env, nevr_maps::MapFs& fs, const std::string& dir, const std::string& cache) {
  LocalScan scan;
  std::string key_text;
  if (!env.ReadSmallFile(dir + "/" + kDevKeyFile, &key_text, 4096)) {
    scan.reason = "absent";
    return scan;
  }
  nevr_maps::PinnedKey key;
  if (!ParseKeyText(key_text, &key)) {
    scan.reason = "bad_dev_key";
    return scan;
  }
  std::vector<std::string> names;
  fs.ListDir(dir, &names);
  std::sort(names.begin(), names.end());
  const std::string suffix = kLocalSuffix;
  for (const std::string& name : names) {
    if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
    LocalMap map;
    map.id = name.substr(0, name.size() - suffix.size());
    map.path = dir + "/" + name;
    std::string signature;
    if (!nevr_maps::IsValidMapId(map.id)) {
      map.reason = "bad_id";
    } else if (!env.ReadSmallFile(map.path + ".minisig", &signature, nevr_maps::kMaxMinisigBytes)) {
      map.reason = "no_signature";
    } else {
      map.reason = ScanOne(env, fs, key.key, signature, cache + "/" + map.id, &map);
    }
    if (!map.reason.empty()) {
      map.package = nevr_maps::Package();
      map.stamps.clear();
    }
    scan.maps.push_back(std::move(map));
  }
  return scan;
}

std::string MountLocal(nevr_cargo::Env& env, nevr_maps::MapFs& fs, const LocalMap& map, nevr_maps::Mounter& mounter) {
  const nevr_maps::Package& p = map.package;
  const std::string owner = "map:" + map.id;
  if (!p.derived.empty()) return "derived_unsupported";
  for (const nevr_maps::RekeyPin& r : p.rekeyed) {
    if (!r.edits.empty()) return "derived_unsupported";
  }
  const Member* set = nullptr;
  std::vector<const Member*> wanted;
  for (const Member& m : p.members) {
    if (m.role == nevr_maps::Role::kSet && set == nullptr) set = &m;
    if (m.role == nevr_maps::Role::kScript) wanted.push_back(&m);
  }
  if (set == nullptr) return "no_set";
  wanted.insert(wanted.begin(), set);

  // Every file is held and its stamp compared with the verified one before anything is granted, as BootMount does.
  struct Held {
    const Member* member;
    std::int64_t handle;
    bool granted;
  };
  std::vector<Held> held;
  const auto close_ungranted = [&] {
    for (const Held& h : held) {
      if (!h.granted) env.CloseBlob(h.handle);
    }
  };
  for (const Member* m : wanted) {
    const auto verified = map.stamps.find(m->path);
    std::int64_t handle = 0;
    std::uint64_t size = 0;
    if (verified == map.stamps.end() || !fs.OpenHeld(map.dir + "/" + m->path, &handle, &size)) {
      close_ungranted();
      return "missing";
    }
    held.push_back(Held{m, handle, false});
    FileStamp stamp;
    if (!fs.StampOf(handle, &stamp) || stamp != verified->second) {
      close_ungranted();
      return "stamp";
    }
  }

  const auto refuse = [&](const std::string& token) {
    mounter.Withdraw(owner);
    close_ungranted();
    return token;
  };
  for (std::size_t i = 1; i < held.size(); ++i) {
    const nevr_maps::Grant g = mounter.Script(owner, held[i].member->script_hash, held[i].handle, held[i].member->sha256);
    if (!g.ok) return refuse("script:" + g.reason);
    held[i].granted = true;
  }
  for (const nevr_maps::RekeyPin& r : p.rekeyed) {
    const nevr_maps::Grant g = mounter.Rekey(owner, r.hash, r.stock_source, r.stock_sha256);
    if (!g.ok) return refuse("rekey:" + g.reason);
  }
  const nevr_maps::Grant g = mounter.Set(owner, held[0].handle, owner + "@" + p.version, set->sha256, false);
  if (!g.ok) return refuse("set:" + g.reason);
  held[0].granted = true;
  return nevr_maps::StageAliasesOrWithdraw(mounter, owner, map.id, p.level, p.config_aliases);
}

}  // namespace detail
}  // namespace nevr_map_channel

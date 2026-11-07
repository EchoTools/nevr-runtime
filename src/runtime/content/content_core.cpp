#include "runtime/content/content_core.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <utility>

#include "runtime/content/content_json.h"

namespace nevr_content {

namespace {

// CFile::Open's and CSysFile::Open's flags: bit 0 reads; 0x2, 0x4, 0x8 and 0x10 write, create, truncate or append.
constexpr std::uint32_t kOpenRead = 0x1;
constexpr std::uint32_t kOpenWriteBits = 0x2 | 0x4 | 0x8 | 0x10;

// The ADR 0029 step keeps the stock manifest it replaced as `<manifest>.stock` (quest/data/plan.h, kStockBackupSuffix).
constexpr const char* kBundledStockSuffix = ".stock";

Decision Refused(Decision d, const char* reason, std::string detail) {
  d.outcome = Outcome::kRefused;
  d.reason = reason;
  d.detail = std::move(detail);
  return d;
}

std::unique_ptr<Served> MakeServed(Kind kind, const std::string& name, std::string key, const HeldFile& held,
                                   const std::string& pack_file) {
  auto s = std::make_unique<Served>();
  s->kind = kind;
  s->name = name;
  s->key = std::move(key);
  s->serve_path = held.serve_path;
  s->pack_file = pack_file;
  return s;
}

}  // namespace

const char* OutcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::kNone: return "none";
    case Outcome::kServed: return "served";
    case Outcome::kRefused: return "refused";
  }
  return "?";
}

const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::kManifest: return "manifest";
    case Kind::kPackage: return "package";
    case Kind::kScript: return "script";
  }
  return "?";
}

Decision Decide(const CoreInputs& in, PackFiles& files, std::unique_ptr<Pack>* out) {
  Decision d;
  const std::vector<std::string> folders = files.Folders(in.content_dir);
  if (folders.empty()) return d;
  if (folders.size() > 1) {
    std::string names;
    for (const std::string& f : folders) names += (names.empty() ? "" : ",") + f;
    return Refused(d, "several_packs", names);
  }
  const std::string& folder = folders.front();
  d.id = folder;
  if (in.data_read_already) return Refused(d, "loaded_too_late", "the game has read its manifests already");

  const std::string dir = files.Join(in.content_dir, folder);
  const std::string json_path = files.Join(dir, "content.json");
  std::string text;
  if (!files.ReadText(json_path, kMaxContentJsonBytes, &text)) return Refused(d, "no_content_json", json_path);
  Descriptor desc;
  std::string reason;
  std::string detail;
  if (!ParseContentJson(text, folder, &desc, &reason, &detail)) return Refused(d, reason.c_str(), detail);
  d.version = desc.version;

  if (!desc.scripts.empty() && in.signed_only) {
    return Refused(d, "signed_only", "the pack carries " + std::to_string(desc.scripts.size()) +
                                         " script DLLs and this run loads signed code only (-require-signed-plugins)");
  }
  if (!desc.scripts.empty() && !in.game_supported) {
    return Refused(d, "unsupported_game", "the pack carries scripts, which fit echovr.exe 631547 only");
  }
  if (!desc.scripts.empty() && !in.scripts_supported) {
    return Refused(d, "scripts_unsupported", "the pack carries " + std::to_string(desc.scripts.size()) +
                                                 " scripts and this platform loads no script from a pack folder");
  }

  auto pack = std::make_unique<Pack>();
  pack->id = desc.id;
  // Every pack file, hashed through the handle that is then held.
  std::map<std::string, HeldFile> held;
  std::map<std::string, std::string> held_path;
  for (const auto& [rel, want] : desc.files) {
    const std::string path = files.Join(dir, rel);
    HeldFile file = files.Hold(path);
    if (!file.opened) {
      files.ReleaseAll();
      return Refused(d, "file_missing", rel + " error=" + std::to_string(file.error));
    }
    if (file.size == 0) {
      files.ReleaseAll();
      return Refused(d, "file_empty", rel);  // a pack file always has bytes; an empty one is a broken copy
    }
    if (file.sha256 != want) {
      files.ReleaseAll();
      return Refused(d, "file_changed", rel + " sha256=" + (file.sha256.empty() ? "unreadable" : file.sha256) + " want=" + want);
    }
    held_path[rel] = path;
    held[rel] = std::move(file);
    ++d.files_checked;
  }

  const std::string data = files.Join(in.game_dir, desc.data_dir);
  for (const ManifestEntry& m : desc.manifests) {
    const std::string stock = files.Join(data, "manifests/" + m.package);
    if (in.refuse_placed_bundle && files.Exists(stock + kBundledStockSuffix)) {
      files.ReleaseAll();
      return Refused(d, "bundled_pack_placed", m.package + " (a bundled pack has replaced this manifest and kept the stock one as " +
                                                   m.package + kBundledStockSuffix + ")");
    }
    const std::string got = files.Sha256Of(stock);
    if (got != m.stock_sha256) {
      files.ReleaseAll();
      return Refused(d, "stock_mismatch", m.package + " sha256=" + (got.empty() ? "missing" : got) + " built_from=" + m.stock_sha256);
    }
    pack->opens.push_back(MakeServed(Kind::kManifest, m.package, files.Key(stock), held[m.file], held_path[m.file]));
    ++d.manifests;
    for (const std::string& rel : m.packages) {
      const std::string name = LeafOf(rel);
      pack->opens.push_back(
          MakeServed(Kind::kPackage, name, files.Key(files.Join(data, "packages/" + name)), held[rel], held_path[rel]));
      ++d.packages;
    }
  }
  for (const std::string& rel : desc.scripts) {
    const std::string name = LeafOf(rel);
    pack->scripts.push_back(
        MakeServed(Kind::kScript, name, files.Key(files.Join(in.exe_dir, "scripts/" + name)), held[rel], held_path[rel]));
    ++d.scripts;
  }

  *out = std::move(pack);
  d.outcome = Outcome::kServed;
  return d;
}

bool IsServedRead(std::uint32_t flags) noexcept { return (flags & kOpenRead) != 0 && (flags & kOpenWriteBits) == 0; }

const Served* FindKey(const std::vector<std::unique_ptr<Served>>& table, const char* key) noexcept {
  if (key == nullptr) return nullptr;
  for (const std::unique_ptr<Served>& s : table) {
    if (std::strcmp(s->key.c_str(), key) == 0) return s.get();
  }
  return nullptr;
}

std::string Printable(const std::string& text) {
  std::string out;
  for (const char c : text.substr(0, 300)) out += (c >= 0x20 && c <= 0x7e && c != '"') ? c : '?';
  return out;
}

std::string FormatDecision(const Decision& d, const std::string& dir, LineLevel* level) {
  char buf[2048];
  switch (d.outcome) {
    case Outcome::kNone:
      *level = LineLevel::kInfo;
      std::snprintf(buf, sizeof(buf), "[NEVR.CONTENT] no content pack installed dir=%s (the game runs on its stock data)",
                    dir.c_str());
      break;
    case Outcome::kServed:
      *level = LineLevel::kInfo;
      std::snprintf(buf, sizeof(buf),
                    "[NEVR.CONTENT] content pack served id=%s version=%s manifests=%zu packages=%zu scripts=%zu "
                    "files_checked=%zu dir=%s (the game reads these files in place of its stock ones; removing the pack's "
                    "folder runs the game stock)",
                    d.id.c_str(), d.version.c_str(), d.manifests, d.packages, d.scripts, d.files_checked, dir.c_str());
      break;
    case Outcome::kRefused:
    default:
      *level = LineLevel::kWarning;
      std::snprintf(buf, sizeof(buf),
                    "[NEVR.CONTENT] content pack refused id=%s reason=%s detail=\"%s\" dir=%s (nothing of the pack is served; "
                    "the game runs on its stock data)",
                    d.id.empty() ? "none" : Printable(d.id).c_str(), d.reason.c_str(), Printable(d.detail).c_str(),
                    dir.c_str());
      break;
  }
  return buf;
}

LineLevel FormatServed(const Served& served, const char* id, const char* game_path, std::uint64_t result, char* buf,
                       std::size_t cap) noexcept {
  const unsigned n = served.hits.fetch_add(1, std::memory_order_relaxed) + 1;
  if (cap == 0) return LineLevel::kDebug;
  if (result != 0) {
    std::snprintf(buf, cap,
                  "[NEVR.CONTENT] %s open failed id=%s name=%s game_path=%s pack_file=%s result=0x%llx (the game gets the open's failure)",
                  KindName(served.kind), id, served.name.c_str(), game_path, served.pack_file.c_str(),
                  static_cast<unsigned long long>(result));
    return LineLevel::kWarning;
  }
  std::snprintf(buf, cap, "[NEVR.CONTENT] %s served id=%s name=%s game_path=%s pack_file=%s count=%u", KindName(served.kind), id,
                served.name.c_str(), game_path, served.pack_file.c_str(), n);
  return n == 1 ? LineLevel::kInfo : LineLevel::kDebug;
}

}  // namespace nevr_content

#include "runtime/content/content_pack.h"

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/logging.h"
#include "runtime/content/content_json.h"
#include "runtime/hook/dll_load_hook.h"
#include "runtime/host/file_digest.h"

namespace nevr_content {

namespace {

// CFile::Open's flags: bit 0 reads; 0x2, 0x4, 0x8 and 0x10 write, create or truncate. A read is bit 0 alone among those.
constexpr std::uint32_t kOpenRead = 0x1;
constexpr std::uint32_t kOpenWriteBits = 0x2 | 0x4 | 0x8 | 0x10;

enum class Kind { kManifest, kPackage, kScript };

const char* KindName(Kind kind) {
  switch (kind) {
    case Kind::kManifest: return "manifest";
    case Kind::kPackage: return "package";
    case Kind::kScript: return "script";
  }
  return "?";
}

struct Served {
  Kind kind;
  std::string name;          // the stock file's name (the package set, the package, the script)
  std::wstring pack_path;    // the pack's file, as held
  std::string pack_path_utf8;
  std::shared_ptr<std::atomic<unsigned>> hits = std::make_shared<std::atomic<unsigned>>(0);
};

struct Pack {
  std::string id;
  std::map<std::wstring, Served> opens;    // canonical stock path -> the pack's file
  std::map<std::wstring, Served> scripts;  // canonical <exe dir>\scripts\<name>.dll -> the pack's file
  std::vector<HANDLE> held;
};

// Written once, before any hook is installed, and only read after.
Pack* g_pack = nullptr;

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
  return out;
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

// One spelling of a full path: no `\\?\` prefix, backslashes and no "." or ".." parts (GetFullPathNameW makes both), ASCII
// lower case (the fold the LoadLibrary hooks hand their filters, so both sides of a comparison are folded the same way).
std::wstring Canonical(std::wstring path) {
  if (path.rfind(L"\\\\?\\", 0) == 0 && path.rfind(L"\\\\?\\UNC\\", 0) != 0) path.erase(0, 4);
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetFullPathNameW(path.c_str(), MAX_PATH * 2, buf, nullptr);
  if (n > 0 && n < MAX_PATH * 2) path.assign(buf, n);
  for (wchar_t& c : path) {
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  }
  return path;
}

std::wstring Under(const std::wstring& dir, const std::string& relative) {
  std::wstring out = dir;
  if (!out.empty() && out.back() != L'\\') out += L'\\';
  out += Widen(relative);
  std::replace(out.begin(), out.end(), L'/', L'\\');
  return out;
}

// Opens `path` for reading with read sharing only, so nothing can write, rename or delete it while the handle is held.
HANDLE OpenHeld(const std::wstring& path) {
  return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// The file's sha-256 through `file`, or "" when it does not read.
std::string DigestOf(HANDLE file) {
  std::vector<unsigned char> bytes;
  if (!nevr_host::ReadWholeFile(file, &bytes)) return std::string();
  return nevr_host::Sha256Hex(bytes);
}

std::string DigestOfPath(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::string();
  const std::string digest = DigestOf(file);
  CloseHandle(file);
  return digest;
}

// The folders directly under `dir`, by name (hidden ones and "." and ".." left out).
std::vector<std::wstring> Folders(const std::wstring& dir) {
  std::vector<std::wstring> out;
  WIN32_FIND_DATAW found;
  HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &found);
  if (find == INVALID_HANDLE_VALUE) return out;
  do {
    const std::wstring name = found.cFileName;
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || name == L"." || name == L"..") continue;
    out.push_back(name);
  } while (FindNextFileW(find, &found));
  FindClose(find);
  std::sort(out.begin(), out.end());
  return out;
}

bool ReadText(const std::wstring& path, std::string* text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size;
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
            static_cast<std::uint64_t>(size.QuadPart) <= kMaxContentJsonBytes;
  if (ok) {
    text->assign(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    ok = text->empty() || (ReadFile(file, text->data(), static_cast<DWORD>(text->size()), &got, nullptr) && got == text->size());
  }
  CloseHandle(file);
  return ok;
}

Decision Refused(Decision d, const char* reason, std::string detail) {
  d.outcome = Outcome::kRefused;
  d.reason = reason;
  d.detail = std::move(detail);
  return d;
}

void Release(Pack* pack) {
  for (HANDLE h : pack->held) CloseHandle(h);
  pack->held.clear();
}

// One record per served file: the first serve of each file is Info, a repeat is Debug (AddPackage reopens a set's files when a
// set is added again). `result` is CFile::Open's own return (0: opened); a script has none (the load comes after the rewrite).
void LogServed(const Served& served, const std::string& game_path, bool has_result, std::uint64_t result) {
  const unsigned n = served.hits->fetch_add(1, std::memory_order_relaxed) + 1;
  if (has_result && result != 0) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.CONTENT] %s open failed id=%s name=%s game_path=%s pack_file=%s result=0x%llx (the game gets the open's failure)",
        KindName(served.kind), g_pack->id.c_str(), served.name.c_str(), game_path.c_str(), served.pack_path_utf8.c_str(),
        static_cast<unsigned long long>(result));
    return;
  }
  Log(n == 1 ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Debug,
      "[NEVR.CONTENT] %s served id=%s name=%s game_path=%s pack_file=%s count=%u", KindName(served.kind), g_pack->id.c_str(),
      served.name.c_str(), game_path.c_str(), served.pack_path_utf8.c_str(), n);
}

const Served* FindOpen(const char* game_path, std::uint32_t flags) {
  const Pack* pack = g_pack;
  if (pack == nullptr || game_path == nullptr || pack->opens.empty()) return nullptr;
  if ((flags & kOpenRead) == 0 || (flags & kOpenWriteBits) != 0) return nullptr;
  const auto it = pack->opens.find(Canonical(Widen(game_path)));
  return it == pack->opens.end() ? nullptr : &it->second;
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

Decision Decide(const Inputs& in) {
  Decision d;
  const std::vector<std::wstring> folders = Folders(in.content_dir);
  if (folders.empty()) return d;
  if (folders.size() > 1) {
    std::string names;
    for (const std::wstring& f : folders) names += (names.empty() ? "" : ",") + Narrow(f);
    return Refused(d, "several_packs", names);
  }
  const std::string folder = Narrow(folders.front());
  d.id = folder;
  if (in.data_read_already) return Refused(d, "loaded_too_late", "the game has read its manifests already");

  const std::wstring dir = in.content_dir + L"\\" + folders.front();
  std::string text;
  if (!ReadText(dir + L"\\content.json", &text)) return Refused(d, "no_content_json", Narrow(dir) + "\\content.json");
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

  auto pack = std::make_unique<Pack>();
  pack->id = desc.id;
  // Every pack file, hashed through the handle that is then held.
  std::map<std::string, std::wstring> held_path;
  for (const auto& [rel, want] : desc.files) {
    const std::wstring path = Under(dir, rel);
    HANDLE file = OpenHeld(path);
    if (file == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      Release(pack.get());
      return Refused(d, "file_missing", rel + " error=" + std::to_string(error));
    }
    pack->held.push_back(file);
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size) && size.QuadPart == 0) {
      Release(pack.get());
      return Refused(d, "file_empty", rel);  // a pack file always has bytes; an empty one is a broken copy
    }
    const std::string got = DigestOf(file);
    if (got != want) {
      Release(pack.get());
      return Refused(d, "file_changed", rel + " sha256=" + (got.empty() ? "unreadable" : got) + " want=" + want);
    }
    held_path[rel] = path;
    ++d.files_checked;
  }

  const std::wstring data = Under(in.game_dir, desc.data_dir);
  for (const ManifestEntry& m : desc.manifests) {
    const std::wstring stock = data + L"\\manifests\\" + Widen(m.package);
    const std::string got = DigestOfPath(stock);
    if (got != m.stock_sha256) {
      Release(pack.get());
      return Refused(d, "stock_mismatch", m.package + " sha256=" + (got.empty() ? "missing" : got) + " built_from=" + m.stock_sha256);
    }
    const std::wstring& file = held_path[m.file];
    pack->opens[Canonical(stock)] = Served{Kind::kManifest, m.package, file, Narrow(file)};
    ++d.manifests;
    for (const std::string& rel : m.packages) {
      const std::string name = LeafOf(rel);
      const std::wstring& pfile = held_path[rel];
      pack->opens[Canonical(data + L"\\packages\\" + Widen(name))] = Served{Kind::kPackage, name, pfile, Narrow(pfile)};
      ++d.packages;
    }
  }
  for (const std::string& rel : desc.scripts) {
    const std::string name = LeafOf(rel);
    const std::wstring& sfile = held_path[rel];
    pack->scripts[Canonical(in.exe_dir + L"\\scripts\\" + Widen(name))] = Served{Kind::kScript, name, sfile, Narrow(sfile)};
    ++d.scripts;
  }

  if (g_pack != nullptr) {
    Release(g_pack);
    delete g_pack;
  }
  g_pack = pack.release();
  d.outcome = Outcome::kServed;
  return d;
}

bool ServeOpen(const char* game_path, std::uint32_t flags, std::string* pack_path) {
  const Served* served = FindOpen(game_path, flags);
  if (served == nullptr) return false;
  *pack_path = served->pack_path_utf8;
  return true;
}

bool RewriteScript(const wchar_t* lower_path, std::wstring* load_path) {
  const Pack* pack = g_pack;
  if (pack == nullptr || lower_path == nullptr || pack->scripts.empty()) return false;
  const auto it = pack->scripts.find(Canonical(lower_path));
  if (it == pack->scripts.end()) return false;
  *load_path = it->second.pack_path;
  // A line here is safe outside the loader lock only; a load from a DllMain is served and counted, not logged.
  if (nevr_dll_load_hook::LoaderLockHeldByThisThread()) {
    it->second.hits->fetch_add(1, std::memory_order_relaxed);
  } else {
    LogServed(it->second, Narrow(lower_path), false, 0);
  }
  return true;
}

std::uint64_t OpenThrough(CFileOpenFn original, const char* path, std::uint32_t flags, HANDLE* out) {
  const Served* served = FindOpen(path, flags);
  if (served == nullptr) return original(path, flags, out);
  const std::uint64_t result = original(served->pack_path_utf8.c_str(), flags, out);
  LogServed(*served, path, true, result);
  return result;
}

void Forget() {
  if (g_pack != nullptr) {
    Release(g_pack);
    delete g_pack;
    g_pack = nullptr;
  }
}

}  // namespace nevr_content

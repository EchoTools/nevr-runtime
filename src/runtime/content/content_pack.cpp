#include "runtime/content/content_pack.h"

#include <winsock2.h>
#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/logging.h"
#include "runtime/content/content_core.h"
#include "runtime/hook/dll_load_hook.h"

namespace nevr_content {

namespace {

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

// Opens `path` for reading with read sharing only, so nothing can write, rename or delete it while the handle is held.
HANDLE OpenHeld(const std::wstring& path) {
  return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// The shared decision's file access on Windows (content_core.h, PackFiles). Paths are UTF-8 outside, wide inside.
class Win32PackFiles final : public PackFiles {
 public:
  ~Win32PackFiles() override { ReleaseAll(); }

  std::vector<std::string> Folders(const std::string& dir) override {
    std::vector<std::wstring> wide;
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW((Widen(dir) + L"\\*").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = found.cFileName;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || name == L"." || name == L"..") continue;
        wide.push_back(name);
      } while (FindNextFileW(find, &found));
      FindClose(find);
    }
    std::sort(wide.begin(), wide.end());
    std::vector<std::string> out;
    for (const std::wstring& w : wide) out.push_back(Narrow(w));
    return out;
  }

  bool ReadText(const std::string& path, std::size_t max_bytes, std::string* text) override {
    HANDLE file = CreateFileW(Widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && static_cast<std::uint64_t>(size.QuadPart) <= max_bytes;
    if (ok) {
      text->assign(static_cast<std::size_t>(size.QuadPart), '\0');
      DWORD got = 0;
      ok = text->empty() || (ReadFile(file, text->data(), static_cast<DWORD>(text->size()), &got, nullptr) && got == text->size());
    }
    CloseHandle(file);
    return ok;
  }

  HeldFile Hold(const std::string& path) override {
    HeldFile held;
    HANDLE file = OpenHeld(Widen(path));
    if (file == INVALID_HANDLE_VALUE) {
      held.error = GetLastError();
      return held;
    }
    held_.push_back(file);
    held.opened = true;
    LARGE_INTEGER size;
    if (GetFileSizeEx(file, &size)) held.size = size.QuadPart;
    if (held.size != 0) held.sha256 = StreamSha256(file, kDigestChunkBytes);
    held.serve_path = path;  // the handle keeps the file as it was checked: no write, rename or delete while it is held
    return held;
  }

  void ReleaseAll() override {
    for (HANDLE h : held_) CloseHandle(h);
    held_.clear();
  }

  std::string Sha256Of(const std::string& path) override {
    HANDLE file = CreateFileW(Widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::string();
    const std::string digest = StreamSha256(file, kDigestChunkBytes);
    CloseHandle(file);
    return digest;
  }

  bool Exists(const std::string& path) override { return GetFileAttributesW(Widen(path).c_str()) != INVALID_FILE_ATTRIBUTES; }

  std::string Join(const std::string& dir, const std::string& relative) override {
    std::string out = dir;
    if (!out.empty() && out.back() != '\\') out += '\\';
    out += relative;
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
  }

  std::string Key(const std::string& path) override { return Narrow(Canonical(Widen(path))); }

 private:
  std::vector<HANDLE> held_;
};

// The pack this process serves: the shared table and the files that hold its files open. Written once, before any hook is
// installed, and only read after.
struct State {
  std::unique_ptr<Pack> pack;
  std::unique_ptr<Win32PackFiles> files;
};
State* g_state = nullptr;

EchoVR::LogLevel LevelOf(LineLevel level) {
  switch (level) {
    case LineLevel::kDebug: return EchoVR::LogLevel::Debug;
    case LineLevel::kInfo: return EchoVR::LogLevel::Info;
    case LineLevel::kWarning: return EchoVR::LogLevel::Warning;
  }
  return EchoVR::LogLevel::Warning;
}

// One record per served file (content_core.h, FormatServed): the first serve of each file is Info, a repeat is Debug (AddPackage
// reopens a set's files when a set is added again). `result` is CFile::Open's own return (0: opened); a script passes 0 (the
// load comes after the rewrite).
void LogServed(const Served& served, const std::string& game_path, std::uint64_t result) {
  char line[4096];
  const LineLevel level = FormatServed(served, g_state->pack->id.c_str(), game_path.c_str(), result, line, sizeof(line));
  Log(LevelOf(level), "%s", line);
}

const Served* FindOpen(const char* game_path, std::uint32_t flags) {
  const State* state = g_state;
  if (state == nullptr || game_path == nullptr || state->pack->opens.empty()) return nullptr;
  if (!IsServedRead(flags)) return nullptr;
  return FindKey(state->pack->opens, Narrow(Canonical(Widen(game_path))).c_str());
}

}  // namespace

std::string StreamSha256(HANDLE file, std::size_t chunk_bytes) {
  if (file == INVALID_HANDLE_VALUE || chunk_bytes == 0 || chunk_bytes > 0x7fffffff) return std::string();
  LARGE_INTEGER start{};
  if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return std::string();
  HCRYPTPROV provider = 0;
  if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return std::string();
  std::string hex;
  HCRYPTHASH hash = 0;
  if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
    std::vector<unsigned char> buffer(chunk_bytes);  // one chunk, whatever the file's size
    bool ok = true;
    for (;;) {
      DWORD got = 0;
      if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
        ok = false;
        break;
      }
      if (got == 0) break;  // end of file
      if (!CryptHashData(hash, buffer.data(), got, 0)) {
        ok = false;
        break;
      }
    }
    BYTE digest[32];
    DWORD length = sizeof(digest);
    if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) && length == sizeof(digest)) {
      static const char kHex[] = "0123456789abcdef";
      for (const BYTE b : digest) {
        hex += kHex[b >> 4];
        hex += kHex[b & 15];
      }
    }
    CryptDestroyHash(hash);
  }
  CryptReleaseContext(provider, 0);
  return hex;
}

Decision Decide(const Inputs& in) {
  CoreInputs core;
  core.content_dir = Narrow(in.content_dir);
  core.game_dir = Narrow(in.game_dir);
  core.exe_dir = Narrow(in.exe_dir);
  core.data_read_already = in.data_read_already;
  core.signed_only = in.signed_only;
  core.game_supported = in.game_supported;
  auto files = std::make_unique<Win32PackFiles>();
  std::unique_ptr<Pack> pack;
  const Decision d = Decide(core, *files, &pack);
  if (d.outcome != Outcome::kServed) return d;
  Forget();
  g_state = new State{std::move(pack), std::move(files)};
  return d;
}

bool ServeOpen(const char* game_path, std::uint32_t flags, std::string* pack_path) {
  const Served* served = FindOpen(game_path, flags);
  if (served == nullptr) return false;
  *pack_path = served->serve_path;
  return true;
}

bool RewriteScript(const wchar_t* lower_path, std::wstring* load_path) {
  const State* state = g_state;
  if (state == nullptr || lower_path == nullptr || state->pack->scripts.empty()) return false;
  const Served* served = FindKey(state->pack->scripts, Narrow(Canonical(lower_path)).c_str());
  if (served == nullptr) return false;
  *load_path = Widen(served->serve_path);
  // A line here is safe outside the loader lock only; a load from a DllMain is served and counted, not logged.
  if (nevr_dll_load_hook::LoaderLockHeldByThisThread()) {
    served->hits.fetch_add(1, std::memory_order_relaxed);
  } else {
    LogServed(*served, Narrow(lower_path), 0);
  }
  return true;
}

std::uint64_t OpenThrough(CFileOpenFn original, const char* path, std::uint32_t flags, HANDLE* out) {
  const Served* served = FindOpen(path, flags);
  if (served == nullptr) return original(path, flags, out);
  const std::uint64_t result = original(served->serve_path.c_str(), flags, out);
  LogServed(*served, path, result);
  return result;
}

void Forget() {
  delete g_state;  // its files close their handles
  g_state = nullptr;
}

}  // namespace nevr_content

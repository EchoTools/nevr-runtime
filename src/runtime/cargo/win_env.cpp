#include "runtime/cargo/win_env.h"

#include "core/nevr_root.h"

#include <bcrypt.h>
#include <curl/curl.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include "core/curl_global.h"
#include "core/logging.h"

namespace {

using nevr_cargo::EventLevel;
using nevr_cargo::Field;
using nevr_cargo::HttpBody;
using nevr_cargo::HttpResult;

constexpr std::size_t kIoChunk = std::size_t{1} << 30;  // ReadFile, WriteFile and BCryptHashData take 32-bit lengths

HANDLE ToHandle(std::int64_t handle) { return reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(handle)); }
std::int64_t FromHandle(HANDLE handle) { return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(handle)); }

bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

// A UTF-8 absolute path ("X:\..." or "X:/...") as UTF-16. Relative paths would resolve against the game's working directory, and an
// embedded NUL would cut the path short, so both are refused.
bool WinPath(const std::string& path, std::wstring* out) {
  if (path.empty() || path.find('\0') != std::string::npos) return false;
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
  if (n <= 0) return false;
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), &wide[0], n);
  const bool absolute = wide.size() >= 3 && ((wide[0] >= L'A' && wide[0] <= L'Z') || (wide[0] >= L'a' && wide[0] <= L'z')) &&
                        wide[1] == L':' && IsSeparator(wide[2]);
  if (!absolute) return false;
  *out = std::move(wide);
  return true;
}

std::string Utf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &out[0], n, nullptr, nullptr);
  return out;
}

// The handle's file is a plain file: not a directory and not a link (opened with FILE_FLAG_OPEN_REPARSE_POINT, so a link is the
// link itself, refused here).
bool PlainFileInfo(HANDLE file, BY_HANDLE_FILE_INFORMATION* info) {
  if (!GetFileInformationByHandle(file, info)) return false;
  return (info->dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

std::uint64_t SizeOf(const BY_HANDLE_FILE_INFORMATION& info) {
  return (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

// Read-only, shared with readers only: no writer, no delete and no rename over it while the handle is open.
bool OpenHeldFile(const std::string& path, std::int64_t* handle, std::uint64_t* size) {
  std::wstring wide;
  if (!WinPath(path, &wide)) return false;
  const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info;
  if (!PlainFileInfo(file, &info)) {
    CloseHandle(file);
    return false;
  }
  *handle = FromHandle(file);
  *size = SizeOf(info);
  return true;
}

bool WriteAll(HANDLE file, const void* data, std::size_t length) {
  const char* p = static_cast<const char*>(data);
  while (length > 0) {
    const DWORD want = static_cast<DWORD>(std::min(length, kIoChunk));
    DWORD wrote = 0;
    if (!WriteFile(file, p, want, &wrote, nullptr) || wrote == 0) return false;
    p += wrote;
    length -= wrote;
  }
  return true;
}

std::string HexOf(const unsigned char* bytes, std::size_t n) {
  static const char kDigits[] = "0123456789abcdef";
  std::string hex(2 * n, '0');
  for (std::size_t i = 0; i < n; ++i) {
    hex[2 * i] = kDigits[bytes[i] >> 4];
    hex[2 * i + 1] = kDigits[bytes[i] & 0xF];
  }
  return hex;
}

BCRYPT_ALG_HANDLE Sha256Provider() {
  static const BCRYPT_ALG_HANDLE provider = [] {
    BCRYPT_ALG_HANDLE h = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&h, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) h = nullptr;
    return h;
  }();
  return provider;
}

// A hasher that cannot be created or fed answers "" from Hex, which matches no 64-hex pin, so a broken provider fails closed.
class BcryptSha256 : public nevr_cargo::Hasher {
 public:
  BcryptSha256() { Begin(); }
  ~BcryptSha256() override {
    if (hash_ != nullptr) BCryptDestroyHash(hash_);
  }
  BcryptSha256(const BcryptSha256&) = delete;
  BcryptSha256& operator=(const BcryptSha256&) = delete;

  void Update(const void* data, std::size_t length) override {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    while (!failed_ && length > 0) {
      const ULONG chunk = static_cast<ULONG>(std::min(length, kIoChunk));
      failed_ = !BCRYPT_SUCCESS(BCryptHashData(hash_, const_cast<PUCHAR>(p), chunk, 0));
      p += chunk;
      length -= chunk;
    }
  }

  std::string Hex() override {
    unsigned char digest[32];
    const bool ok = !failed_ && BCRYPT_SUCCESS(BCryptFinishHash(hash_, digest, sizeof(digest), 0));
    if (hash_ != nullptr) BCryptDestroyHash(hash_);
    Begin();
    return ok ? HexOf(digest, sizeof(digest)) : std::string();
  }

 private:
  void Begin() {
    hash_ = nullptr;
    const BCRYPT_ALG_HANDLE provider = Sha256Provider();
    failed_ = provider == nullptr || !BCRYPT_SUCCESS(BCryptCreateHash(provider, &hash_, nullptr, 0, nullptr, 0, 0));
    if (failed_) hash_ = nullptr;
  }

  BCRYPT_HASH_HANDLE hash_ = nullptr;
  bool failed_ = false;
};

// A file written beside its final name, opened exclusively, flushed and renamed over the final name in one step. A writer killed
// before Commit leaves the final file as it was and this .part behind for the cache's GC.
class WinTemp : public nevr_cargo::TempFile {
 public:
  WinTemp(HANDLE file, std::wstring path) : file_(file), path_(std::move(path)) {}
  ~WinTemp() override { Abort(); }
  WinTemp(const WinTemp&) = delete;
  WinTemp& operator=(const WinTemp&) = delete;

  bool Write(const void* data, std::size_t length) override { return file_ != INVALID_HANDLE_VALUE && WriteAll(file_, data, length); }

  bool Commit(const std::string& final_path) override {
    if (file_ == INVALID_HANDLE_VALUE) return false;
    std::wstring target;
    const bool named = WinPath(final_path, &target);
    const bool flushed = FlushFileBuffers(file_) != 0;
    CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
    if (!named || !flushed ||
        !MoveFileExW(path_.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      DeleteFileW(path_.c_str());
      path_.clear();
      return false;
    }
    path_.clear();
    return true;
  }

  void Abort() override {
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    file_ = INVALID_HANDLE_VALUE;
    if (!path_.empty()) DeleteFileW(path_.c_str());
    path_.clear();
  }

 private:
  HANDLE file_;
  std::wstring path_;
};

struct Transfer {
  const HttpBody* on_body;
  const std::function<bool()>* keep_going;
  CURL* curl;
  bool stopped = false;
};

// Returning less than was given ends the transfer (CURLE_WRITE_ERROR).
std::size_t OnBody(char* data, std::size_t size, std::size_t count, void* user) {
  Transfer* t = static_cast<Transfer*>(user);
  const std::size_t n = size * count;
  long status = 0;
  curl_easy_getinfo(t->curl, CURLINFO_RESPONSE_CODE, &status);
  return (*t->on_body)(status, data, n) ? n : 0;
}

// A nonzero return ends the transfer (CURLE_ABORTED_BY_CALLBACK); libcurl calls this at least once a second.
int OnProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  Transfer* t = static_cast<Transfer*>(user);
  if (*t->keep_going && !(*t->keep_going)()) {
    t->stopped = true;
    return 1;
  }
  return 0;
}

// The value of key=value, quoted when it is empty or holds a space, a quote, an '=', a backslash or a control character.
void AppendValue(std::string* line, const std::string& text) {
  const bool plain = !text.empty() && std::none_of(text.begin(), text.end(), [](char c) {
    return c == ' ' || c == '"' || c == '=' || c == '\\' || static_cast<unsigned char>(c) < 0x20;
  });
  if (plain) {
    *line += text;
    return;
  }
  *line += '"';
  for (char c : text) {
    if (c == '"' || c == '\\') *line += '\\';
    *line += static_cast<unsigned char>(c) < 0x20 ? ' ' : c;
  }
  *line += '"';
}

class WinEnv : public nevr_cargo::Env {
 public:
  explicit WinEnv(const char* event_prefix) : prefix_(event_prefix != nullptr ? event_prefix : "") {}

  bool ReadSmallFile(const std::string& path, std::string* out, std::size_t max_bytes) override {
    std::wstring wide;
    if (!WinPath(path, &wide)) return false;
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info;
    if (!PlainFileInfo(file, &info) || SizeOf(info) > max_bytes) {
      CloseHandle(file);
      return false;
    }
    std::string text(static_cast<std::size_t>(SizeOf(info)), '\0');
    std::size_t got = 0;
    while (got < text.size()) {
      DWORD n = 0;
      const DWORD want = static_cast<DWORD>(std::min(text.size() - got, kIoChunk));
      if (!ReadFile(file, &text[got], want, &n, nullptr) || n == 0) break;
      got += n;
    }
    CloseHandle(file);
    if (got != text.size()) return false;
    *out = std::move(text);
    return true;
  }

  bool MakeDirs(const std::string& dir) override {
    std::wstring wide;
    return WinPath(dir, &wide) && nevr_root::EnsureDirectoryTree(wide);
  }

  std::unique_ptr<nevr_cargo::TempFile> CreateTemp(const std::string& dir) override {
    std::wstring base;
    if (!WinPath(dir, &base)) return nullptr;
    if (!IsSeparator(base.back())) base += L'\\';
    for (int attempt = 0; attempt < 8; ++attempt) {
      unsigned char random[8];
      if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return nullptr;
      const std::string name = HexOf(random, sizeof(random));
      const std::wstring path = base + L"~" + std::wstring(name.begin(), name.end()) + L".part";
      const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file != INVALID_HANDLE_VALUE) return std::unique_ptr<nevr_cargo::TempFile>(new WinTemp(file, path));
      if (GetLastError() != ERROR_FILE_EXISTS) return nullptr;
    }
    return nullptr;
  }

  bool WriteFileAtomically(const std::string& path, const std::string& data) override {
    const std::size_t slash = path.find_last_of("\\/");
    if (slash == std::string::npos) return false;
    std::unique_ptr<nevr_cargo::TempFile> temp = CreateTemp(path.substr(0, slash));
    return temp != nullptr && temp->Write(data.data(), data.size()) && temp->Commit(path);
  }

  bool OpenBlob(const std::string& path, std::int64_t* handle, std::uint64_t* size) override {
    return OpenHeldFile(path, handle, size);
  }

  bool ReadBlob(std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length, std::size_t* got) override {
    char* out = static_cast<char*>(buffer);
    std::size_t total = 0;
    while (total < length) {
      const std::uint64_t at = offset + total;
      OVERLAPPED where = {};
      where.Offset = static_cast<DWORD>(at);
      where.OffsetHigh = static_cast<DWORD>(at >> 32);
      DWORD n = 0;
      const DWORD want = static_cast<DWORD>(std::min(length - total, kIoChunk));
      if (!ReadFile(ToHandle(handle), out + total, want, &n, &where)) {
        if (GetLastError() == ERROR_HANDLE_EOF) break;
        return false;
      }
      if (n == 0) break;
      total += n;
    }
    *got = total;
    return true;
  }

  void CloseBlob(std::int64_t handle) override { CloseHandle(ToHandle(handle)); }

  std::unique_ptr<nevr_cargo::Hasher> NewHasher() override { return std::unique_ptr<nevr_cargo::Hasher>(new BcryptSha256()); }

  HttpResult Get(const std::string& url, const HttpBody& on_body, const std::function<bool()>& keep_going) override {
    HttpResult out;
    nevr::EnsureCurlGlobalInit();
    CURL* const curl = curl_easy_init();
    if (curl == nullptr) {
      out.code = static_cast<int>(CURLE_FAILED_INIT);
      return out;
    }
    Transfer t{&on_body, &keep_going, curl};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &OnBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &OnProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, nevr_cargo::kConnectTimeoutSeconds);
    // No whole-transfer timeout, a piece can be a large frame; one under the low-speed floor is dead (CDN_DESIGN PC1).
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, nevr_cargo::kLowSpeedBytesPerSecond);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, nevr_cargo::kLowSpeedSeconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "nevr-runtime/1.0");
    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    out.status = status;
    out.completed = res == CURLE_OK;
    out.cancelled = t.stopped;
    out.code = static_cast<int>(res);
    return out;
  }

  std::uint64_t NowMs() override { return GetTickCount64(); }

  void Event(EventLevel level, const char* name, const std::vector<Field>& fields) override {
    std::string line = prefix_;
    if (!line.empty()) line += ' ';
    line += name != nullptr ? name : "";
    for (const Field& f : fields) {
      line += ' ';
      line += f.key;
      line += '=';
      if (f.is_number) {
        line += std::to_string(f.number);
      } else {
        AppendValue(&line, f.text);
      }
    }
    const EchoVR::LogLevel mapped = level == EventLevel::kInfo   ? EchoVR::LogLevel::Info
                                    : level == EventLevel::kWarn ? EchoVR::LogLevel::Warning
                                                                 : EchoVR::LogLevel::Error;
    Log(mapped, "%s", line.c_str());
  }

 private:
  std::string prefix_;
};

// Removes `dir` and everything under it, as far as it can. A link inside (a junction, a symlink) is removed itself, never entered.
bool RemoveTreeW(const std::wstring& dir) {
  bool all = true;
  WIN32_FIND_DATAW found;
  const HANDLE search = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = found.cFileName;
      if (name == L"." || name == L"..") continue;
      const std::wstring path = dir + L"\\" + name;
      const DWORD attributes = found.dwFileAttributes;
      if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
      bool removed;
      if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        removed = RemoveTreeW(path);
      } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        removed = RemoveDirectoryW(path.c_str()) != 0;
      } else {
        removed = DeleteFileW(path.c_str()) != 0;
      }
      all = all && removed;
    } while (FindNextFileW(search, &found));
    FindClose(search);
  }
  return all && RemoveDirectoryW(dir.c_str()) != 0;
}

class WinMapFs : public nevr_maps::MapFs {
 public:
  bool StampOf(std::int64_t handle, nevr_maps::FileStamp* out) override {
    const HANDLE file = ToHandle(handle);
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info)) return false;
    nevr_maps::FileStamp stamp;
    stamp.volume = info.dwVolumeSerialNumber;
    stamp.id_lo = (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    stamp.size = SizeOf(info);
    stamp.last_write =
        (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    FILE_ID_INFO id;
    if (GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id))) {
      stamp.volume = id.VolumeSerialNumber;
      std::memcpy(&stamp.id_lo, id.FileId.Identifier, 8);
      std::memcpy(&stamp.id_hi, id.FileId.Identifier + 8, 8);
    }
    *out = stamp;
    return true;
  }

  std::string FinalPathOf(std::int64_t handle) override {
    const HANDLE file = ToHandle(handle);
    constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    std::wstring path(MAX_PATH, L'\0');
    DWORD n = GetFinalPathNameByHandleW(file, &path[0], static_cast<DWORD>(path.size()), kFlags);
    if (n >= path.size()) {  // too small: n is the length the path needs, its terminator included
      path.assign(n, L'\0');
      n = GetFinalPathNameByHandleW(file, &path[0], static_cast<DWORD>(path.size()), kFlags);
    }
    if (n == 0 || n >= path.size()) return std::string();
    path.resize(n);
    if (path.size() >= 6 && path.compare(0, 4, L"\\\\?\\") == 0 && path[5] == L':') path.erase(0, 4);
    return Utf8(path);
  }

  bool OpenHeld(const std::string& path, std::int64_t* handle, std::uint64_t* size) override {
    return OpenHeldFile(path, handle, size);
  }

  bool ListDir(const std::string& dir, std::vector<std::string>* names) override {
    std::wstring wide;
    if (!WinPath(dir, &wide)) return false;
    if (!IsSeparator(wide.back())) wide += L'\\';
    WIN32_FIND_DATAW found;
    const HANDLE search = FindFirstFileExW((wide + L"*").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE) return false;
    std::vector<std::string> listed;
    do {
      const std::wstring name = found.cFileName;
      if (name != L"." && name != L"..") listed.push_back(Utf8(name));
    } while (FindNextFileW(search, &found));
    const bool ended = GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(search);
    if (!ended) return false;
    std::sort(listed.begin(), listed.end());
    *names = std::move(listed);
    return true;
  }

  bool RemoveTree(const std::string& dir) override {
    std::wstring wide;
    if (!WinPath(dir, &wide)) return false;
    while (wide.size() > 3 && IsSeparator(wide.back())) wide.pop_back();
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(wide.c_str()) != 0;
    return RemoveTreeW(wide);
  }

  bool RenameDir(const std::string& from, const std::string& to) override {
    std::wstring source, target;
    return WinPath(from, &source) && WinPath(to, &target) && MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
  }
};

}  // namespace

namespace nevr_cargo {

std::unique_ptr<Env> MakeWinEnv(const char* event_prefix) { return std::unique_ptr<Env>(new WinEnv(event_prefix)); }

}  // namespace nevr_cargo

namespace nevr_maps {

std::unique_ptr<MapFs> MakeWinMapFs() { return std::unique_ptr<MapFs>(new WinMapFs()); }

}  // namespace nevr_maps

#include "runtime/cargo/win_env.h"

#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/curl_global.h"
#include "core/device_memory.h"
#include "core/logging.h"
#include "core/nevr_root.h"
#include "quest/data/sha256.h"

namespace {

using nevr_cargo::EventLevel;
using nevr_cargo::Field;
using nevr_cargo::HttpBody;
using nevr_cargo::HttpResult;

// ReadFile and WriteFile take their length as a DWORD (fileapi.h), so one call moves at most this many bytes.
constexpr std::size_t kMaxIoBytes = std::numeric_limits<DWORD>::max();
// A temp file's name is the target's, then the process id and a process-wide count, then this suffix; a name a killed writer left
// is skipped, and after this many names that exist the write gives up.
constexpr wchar_t kTempSuffix[] = L".part";
constexpr int kTempNameAttempts = 16;
// The width of a FILETIME's high word in a 64-bit stamp, and of a 64-bit file index's.
constexpr unsigned kHighWordShift = 32;
constexpr std::size_t kFileIdHalfBytes = sizeof(std::uint64_t);
// A "X:\" absolute path: drive letter, colon, separator.
constexpr std::size_t kDrivePrefixChars = 3;
// The \\?\ prefix GetFinalPathNameByHandleW puts before a drive path, and where that path's colon sits after it.
constexpr wchar_t kLongPathPrefix[] = L"\\\\?\\";
constexpr std::size_t kLongPathPrefixChars = sizeof(kLongPathPrefix) / sizeof(wchar_t) - 1;
constexpr std::size_t kLongPathColonAt = kLongPathPrefixChars + 1;
// What Get tells the server it is.
constexpr char kUserAgent[] = "nevr-runtime";

HANDLE ToHandle(std::int64_t handle) { return reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(handle)); }
std::int64_t FromHandle(HANDLE handle) { return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(handle)); }

bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

// A UTF-8 absolute path ("X:\..." or "X:/...") as UTF-16. A relative path would resolve against the game's working directory, and
// an embedded NUL would cut the path short, so both are refused.
bool WinPath(const std::string& path, std::wstring* out) {
  if (path.empty() || path.find('\0') != std::string::npos) return false;
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
  if (n <= 0) return false;
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), &wide[0], n);
  const bool absolute = wide.size() >= kDrivePrefixChars &&
                        ((wide[0] >= L'A' && wide[0] <= L'Z') || (wide[0] >= L'a' && wide[0] <= L'z')) && wide[1] == L':' &&
                        IsSeparator(wide[2]);
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
  return (static_cast<std::uint64_t>(info.nFileSizeHigh) << kHighWordShift) | info.nFileSizeLow;
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

// Every byte written, or false: a WriteFile that writes fewer bytes than asked is called again for the rest, and one that writes
// none, or fails, ends the write.
bool WriteAll(HANDLE file, const void* data, std::size_t length) {
  const char* p = static_cast<const char*>(data);
  while (length > 0) {
    const DWORD want = static_cast<DWORD>(std::min(length, kMaxIoBytes));
    DWORD wrote = 0;
    if (!WriteFile(file, p, want, &wrote, nullptr) || wrote == 0) return false;
    p += wrote;
    length -= wrote;
  }
  return true;
}

class Sha256Hasher : public nevr_cargo::Hasher {
 public:
  void Update(const void* data, std::size_t length) override { sha_.Update(data, length); }
  std::string Hex() override { return sha_.Hex(); }

 private:
  nevr_quest::data::Sha256 sha_;
};

// A file that only grows: GENERIC_WRITE (and FILE_READ_ATTRIBUTES, for its size), shared with readers only, not through a link.
// Each Append goes to the end as the file is now, so a Truncate before it is where the next bytes land.
class WinAppend : public nevr_cargo::AppendFile {
 public:
  explicit WinAppend(HANDLE file) : file_(file) {}
  ~WinAppend() override { CloseHandle(file_); }
  WinAppend(const WinAppend&) = delete;
  WinAppend& operator=(const WinAppend&) = delete;

  std::uint64_t Size() const override {
    LARGE_INTEGER size = {};
    return GetFileSizeEx(file_, &size) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
  }
  bool Append(const void* data, std::size_t length) override {
    const LARGE_INTEGER none = {};
    return SetFilePointerEx(file_, none, nullptr, FILE_END) && WriteAll(file_, data, length);
  }
  bool Truncate(std::uint64_t size) override {
    if (size > Size()) return false;
    FILE_END_OF_FILE_INFO end = {};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    return SetFileInformationByHandle(file_, FileEndOfFileInfo, &end, sizeof(end)) != 0;
  }
  bool Sync() override { return FlushFileBuffers(file_) != 0; }

 private:
  HANDLE file_;
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

// The value of key=value, quoted when it is empty or holds a space, a quote, an '=', a backslash or a control character; a control
// character becomes a space, so one event is one line.
void AppendValue(std::string* line, const std::string& text) {
  const bool plain = !text.empty() && std::none_of(text.begin(), text.end(), [](char c) {
    return c == ' ' || c == '"' || c == '=' || c == '\\' || static_cast<unsigned char>(c) < ' ';
  });
  if (plain) {
    *line += text;
    return;
  }
  *line += '"';
  for (char c : text) {
    if (c == '"' || c == '\\') *line += '\\';
    *line += static_cast<unsigned char>(c) < ' ' ? ' ' : c;
  }
  *line += '"';
}

EchoVR::LogLevel LevelOf(EventLevel level) {
  switch (level) {
    case EventLevel::kDebug: return EchoVR::LogLevel::Debug;
    case EventLevel::kInfo: return EchoVR::LogLevel::Info;
    case EventLevel::kWarn: return EchoVR::LogLevel::Warning;
    case EventLevel::kError: return EchoVR::LogLevel::Error;
  }
  return EchoVR::LogLevel::Error;
}

class WinEnv : public nevr_cargo::Env {
 public:
  explicit WinEnv(const char* event_tag) : tag_(event_tag != nullptr ? event_tag : "") {}

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
      const DWORD want = static_cast<DWORD>(std::min(text.size() - got, kMaxIoBytes));
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

  // A temp file beside `path`, written whole, flushed and renamed over `path` with MOVEFILE_WRITE_THROUGH, which returns only once
  // the rename is on the disk (the directory's own flush on Windows). A failure at any step removes the temp file; a writer killed
  // before the rename leaves `path` as it was and the temp file behind.
  bool WriteFileAtomically(const std::string& path, const std::string& data) override {
    std::wstring target;
    if (!WinPath(path, &target) || IsSeparator(target.back())) return false;
    std::wstring temp;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < kTempNameAttempts && file == INVALID_HANDLE_VALUE; ++attempt) {
      temp = target + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(NextTempNumber()) + kTempSuffix;
      file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool written = WriteAll(file, data.data(), data.size()) && FlushFileBuffers(file) != 0;
    CloseHandle(file);
    if (!written || !MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      DeleteFileW(temp.c_str());
      return false;
    }
    return true;
  }

  std::unique_ptr<nevr_cargo::AppendFile> OpenAppend(const std::string& path) override {
    std::wstring wide;
    if (!WinPath(path, &wide)) return nullptr;
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return nullptr;
    BY_HANDLE_FILE_INFORMATION info;
    if (!PlainFileInfo(file, &info)) {
      CloseHandle(file);
      return nullptr;
    }
    return std::unique_ptr<nevr_cargo::AppendFile>(new WinAppend(file));
  }

  bool QueryDevice(const std::string& dir, nevr_cargo::DeviceFacts* out) override {
    *out = nevr_cargo::DeviceFacts();
    std::wstring wide;
    ULARGE_INTEGER free_to_caller = {};
    if (WinPath(dir, &wide) && GetDiskFreeSpaceExW(wide.c_str(), &free_to_caller, nullptr, nullptr)) {
      out->have_free = true;
      out->free_bytes = free_to_caller.QuadPart;
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles)) {
      out->have_fds = true;
      out->fd_soft_limit = nevr_cargo::kProcessHandleLimit;
      out->fds_open = handles;
    }
    std::uint64_t memory = 0;
    if (nevr_device::MemoryAvailable(&memory)) {
      out->have_memory = true;
      out->memory_available = memory;
    }
    return out->have_free || out->have_fds || out->have_memory;
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
      where.OffsetHigh = static_cast<DWORD>(at >> kHighWordShift);
      DWORD n = 0;
      const DWORD want = static_cast<DWORD>(std::min(length - total, kMaxIoBytes));
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

  std::unique_ptr<nevr_cargo::Hasher> NewHasher() override { return std::unique_ptr<nevr_cargo::Hasher>(new Sha256Hasher()); }

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
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, nevr_cargo::kLowSpeedBytesPerSecond);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, nevr_cargo::kLowSpeedSeconds);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, nevr_cargo::kMaxRedirects);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
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
    std::string line = tag_;
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
    Log(LevelOf(level), "%s", line.c_str());
  }

 private:
  static std::uint64_t NextTempNumber() {
    static std::atomic<std::uint64_t> next{0};
    return next.fetch_add(1, std::memory_order_relaxed);
  }

  std::string tag_;
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
    stamp.id_lo = (static_cast<std::uint64_t>(info.nFileIndexHigh) << kHighWordShift) | info.nFileIndexLow;
    stamp.size = SizeOf(info);
    stamp.last_write = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << kHighWordShift) |
                       info.ftLastWriteTime.dwLowDateTime;
    FILE_ID_INFO id;
    if (GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id))) {
      stamp.volume = id.VolumeSerialNumber;
      std::memcpy(&stamp.id_lo, id.FileId.Identifier, kFileIdHalfBytes);
      std::memcpy(&stamp.id_hi, id.FileId.Identifier + kFileIdHalfBytes, kFileIdHalfBytes);
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
    if (path.size() > kLongPathColonAt && path.compare(0, kLongPathPrefixChars, kLongPathPrefix) == 0 &&
        path[kLongPathColonAt] == L':') {
      path.erase(0, kLongPathPrefixChars);
    }
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
    while (wide.size() > kDrivePrefixChars && IsSeparator(wide.back())) wide.pop_back();
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = GetLastError();
      return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(wide.c_str()) != 0;
    return RemoveTreeW(wide);
  }

  bool RenameDir(const std::string& from, const std::string& to) override {
    std::wstring source, target;
    return WinPath(from, &source) && WinPath(to, &target) &&
           MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
  }
};

}  // namespace

namespace nevr_cargo {

std::unique_ptr<Env> MakeWinEnv(const char* event_tag) { return std::unique_ptr<Env>(new WinEnv(event_tag)); }

}  // namespace nevr_cargo

namespace nevr_maps {

std::unique_ptr<MapFs> MakeWinMapFs() { return std::unique_ptr<MapFs>(new WinMapFs()); }

}  // namespace nevr_maps

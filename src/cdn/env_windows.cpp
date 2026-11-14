// The PC's nevr_cdn::Env (env_windows.h): Win32 files, SHA-256 from BCrypt, libcurl for the network, the runtime's Log() for events.
// Built without exceptions: nothing here throws and nothing catches.
//
// A link is never followed. A file is opened with FILE_FLAG_OPEN_REPARSE_POINT, which opens a reparse point as itself, and is refused
// when it is one or is not a plain disk file (the POSIX edge refuses the same with O_NOFOLLOW).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <curl/curl.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cdn/env_windows.h"
#include "core/curl_global.h"
#include "core/logging.h"

namespace nevr_cdn {

namespace {

// Windows fixes the number of handles one process may hold at 2^24 (Microsoft, "Pushing the Limits of Windows: Handles"). No call reads
// it back, so it is the one device fact here that is a constant; the store takes its descriptor share from it.
constexpr std::uint64_t kWindowsProcessHandleLimit = 16777216;

constexpr const char* kTempSuffix = ".tmp";
constexpr const char* kLogTag = "[NEVR.CDN] ";
constexpr const char* kAllowedProtocols = "https";

// The offset of a ReadFile is a 64-bit number split across two DWORDs.
constexpr int kBitsPerDword = 32;
constexpr std::uint64_t kLowDwordMask = 0xFFFFFFFFull;

// One ReadFile or WriteFile call moves at most this many bytes (a DWORD count, kept well below its limit).
constexpr std::size_t kMaxIoChunkBytes = 0x40000000;
// One BCryptHashData call takes at most this many bytes.
constexpr std::size_t kMaxHashChunkBytes = 0x40000000;

constexpr std::size_t kSha256DigestBytes = 32;
constexpr std::size_t kHexCharsPerByte = 2;
constexpr int kNibbleBits = 4;
constexpr unsigned char kNibbleMask = 0x0F;
constexpr const char kHexDigits[] = "0123456789abcdef";

// The drive root "X:\" and the characters of it.
constexpr std::size_t kDriveRootLength = 3;
constexpr std::size_t kDriveLetterIndex = 0;
constexpr std::size_t kDriveColonIndex = 1;
constexpr std::size_t kDriveSeparatorIndex = 2;

// The first printable character and the delete character: control characters are written as spaces in an event line.
constexpr unsigned char kFirstPrintableCharacter = 0x20;
constexpr unsigned char kDeleteCharacter = 0x7F;

// libcurl's progress callback returns this to end the transfer (non-zero: CURLE_ABORTED_BY_CALLBACK).
constexpr int kCurlAbortTransfer = 1;
// The Quest transport's numbers (quest/data/curl_transport.h): a connect may take 15 s, and a transfer slower than 1024 bytes a
// second for 30 s is given up, so a stalled object fails and the next one is tried.
constexpr long kConnectTimeoutSeconds = 15;
constexpr long kStallSeconds = 30;
constexpr long kMinBytesPerSecond = 1024;

bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

bool IsAsciiLetter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

bool IsControl(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return u < kFirstPrintableCharacter || u == kDeleteCharacter;
}

// UTF-8 to UTF-16 for one Win32 call. False when the text does not convert or is too long for the API.
bool ToWide(const std::string& utf8, std::wstring* out) {
  if (utf8.empty()) {
    out->clear();
    return true;
  }
  if (utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
  const int size = static_cast<int>(utf8.size());
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, nullptr, 0);
  if (needed <= 0) return false;
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, wide.data(), needed) != needed) return false;
  *out = std::move(wide);
  return true;
}

// A drive-absolute path: "X:\" or "X:/".
bool IsDriveAbsolute(const std::string& path) {
  return path.size() > kDriveSeparatorIndex && IsAsciiLetter(path[kDriveLetterIndex]) && path[kDriveColonIndex] == ':' &&
         (path[kDriveSeparatorIndex] == '\\' || path[kDriveSeparatorIndex] == '/');
}

// Closes its handle when it goes out of scope. An invalid handle (INVALID_HANDLE_VALUE) is not closed.
class ScopedHandle {
 public:
  explicit ScopedHandle(HANDLE h) : h_(h) {}
  ~ScopedHandle() { Close(); }
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;

  HANDLE get() const { return h_; }
  bool valid() const { return h_ != INVALID_HANDLE_VALUE; }
  HANDLE release() {
    const HANDLE h = h_;
    h_ = INVALID_HANDLE_VALUE;
    return h;
  }
  void Close() {
    if (valid()) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE h_;
};

// A handle the CDN holds for a blob: the handle the engine is given is the HANDLE widened to 64 bits.
HANDLE BlobHandle(std::int64_t handle) {
  return reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(handle));
}

// True for a plain disk file: not a directory, not a reparse point (a link or a mount point), not a pipe or a device.
bool IsPlainFile(HANDLE h) {
  if (GetFileType(h) != FILE_TYPE_DISK) return false;
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(h, &info)) return false;
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return false;
  return (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Writes all of `data`. False when the system refuses a write or accepts none.
bool WriteAll(HANDLE h, const void* data, std::size_t length) {
  const char* p = static_cast<const char*>(data);
  while (length > 0) {
    const std::size_t left = length;
    const DWORD chunk = left < kMaxIoChunkBytes ? static_cast<DWORD>(left) : static_cast<DWORD>(kMaxIoChunkBytes);
    DWORD written = 0;
    if (!WriteFile(h, p, chunk, &written, nullptr) || written == 0) return false;
    p += written;
    length -= written;
  }
  return true;
}

// A file that only grows. Each Append seeks to the end first, so the handle needs no FILE_APPEND_DATA and Truncate can cut the file.
class WindowsAppend : public AppendFile {
 public:
  explicit WindowsAppend(HANDLE h) : file_(h) {}

  std::uint64_t Size() const override {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_.get(), &size) || size.QuadPart < 0) return 0;
    return static_cast<std::uint64_t>(size.QuadPart);
  }
  bool Append(const void* data, std::size_t length) override {
    LARGE_INTEGER end{};
    if (SetFilePointerEx(file_.get(), end, nullptr, FILE_END) == FALSE) return false;
    return WriteAll(file_.get(), data, length);
  }
  bool Truncate(std::uint64_t size) override {
    if (size > Size()) return false;
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(size);
    return SetFilePointerEx(file_.get(), at, nullptr, FILE_BEGIN) != FALSE && SetEndOfFile(file_.get()) != FALSE;
  }
  bool Sync() override { return FlushFileBuffers(file_.get()) != FALSE; }

 private:
  ScopedHandle file_;
};

// SHA-256 through BCrypt. The algorithm handle is opened once; the hash object is recreated after each Hex() (the hasher's reset).
// When any call fails the hasher stops taking data and Hex() returns an empty string, which no caller accepts as a digest.
class BcryptHasher : public Hasher {
 public:
  BcryptHasher() {
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
      alg_ = nullptr;
      return;
    }
    ULONG object_length = 0;
    ULONG got = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(alg_, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length),
                                          static_cast<ULONG>(sizeof(object_length)), &got, 0))) {
      return;
    }
    object_.resize(object_length);
    Restart();
  }
  ~BcryptHasher() override {
    DestroyHash();
    if (alg_ != nullptr) BCryptCloseAlgorithmProvider(alg_, 0);
  }
  BcryptHasher(const BcryptHasher&) = delete;
  BcryptHasher& operator=(const BcryptHasher&) = delete;

  void Update(const void* data, std::size_t length) override {
    if (hash_ == nullptr) return;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    while (length > 0) {
      const std::size_t chunk = length < kMaxHashChunkBytes ? length : kMaxHashChunkBytes;
      if (!BCRYPT_SUCCESS(BCryptHashData(hash_, const_cast<PUCHAR>(p), static_cast<ULONG>(chunk), 0))) {
        failed_ = true;
        return;
      }
      p += chunk;
      length -= chunk;
    }
  }

  std::string Hex() override {
    std::string out;
    if (hash_ != nullptr && !failed_) {
      unsigned char digest[kSha256DigestBytes] = {};
      if (BCRYPT_SUCCESS(BCryptFinishHash(hash_, digest, static_cast<ULONG>(sizeof(digest)), 0))) {
        out.reserve(kSha256DigestBytes * kHexCharsPerByte);
        for (unsigned char byte : digest) {
          out.push_back(kHexDigits[byte >> kNibbleBits]);
          out.push_back(kHexDigits[byte & kNibbleMask]);
        }
      }
    }
    Restart();
    return out;
  }

 private:
  // A fresh hash object over the same buffer; a failure leaves hash_ null, so Update does nothing and Hex returns "".
  void Restart() {
    DestroyHash();
    failed_ = false;
    if (alg_ == nullptr || object_.empty()) return;
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg_, &hash_, object_.data(), static_cast<ULONG>(object_.size()), nullptr, 0, 0))) {
      hash_ = nullptr;
    }
  }
  void DestroyHash() {
    if (hash_ != nullptr) BCryptDestroyHash(hash_);
    hash_ = nullptr;
  }

  BCRYPT_ALG_HANDLE alg_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
  std::vector<UCHAR> object_;
  bool failed_ = false;
};

// The state of one libcurl transfer, seen from its callbacks.
struct TransferState {
  CURL* curl = nullptr;
  const HttpBody* on_body = nullptr;
  const std::function<bool()>* keep_going = nullptr;
  bool cancelled = false;
};

std::size_t WriteBody(char* data, std::size_t size, std::size_t count, void* userdata) {
  TransferState* state = static_cast<TransferState*>(userdata);
  const std::size_t length = size * count;
  long status = 0;
  curl_easy_getinfo(state->curl, CURLINFO_RESPONSE_CODE, &status);
  // A body the core refuses (over its limit, a status it does not take) ends the transfer as a failure, not as a stop: only
  // keep_going says stop (the Quest transport does the same, quest/data/curl_transport.cpp).
  if (!(*state->on_body)(status, data, length)) return 0;
  return length;
}

int CheckKeepGoing(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  TransferState* state = static_cast<TransferState*>(userdata);
  if (state->keep_going != nullptr && *state->keep_going && !(*state->keep_going)()) {
    state->cancelled = true;
    return kCurlAbortTransfer;
  }
  return 0;
}

// Makes one event value fit on one line: control characters become spaces, and a value that is empty or holds a space, '=', a quote or
// a backslash is written in double quotes with each quote and backslash escaped.
std::string FormatValue(const std::string& text) {
  std::string sanitized;
  sanitized.reserve(text.size());
  for (char c : text) sanitized.push_back(IsControl(c) ? ' ' : c);
  if (sanitized.empty() || sanitized.find_first_of(" =\"\\") != std::string::npos) {
    std::string quoted = "\"";
    for (char c : sanitized) {
      if (c == '"' || c == '\\') quoted.push_back('\\');
      quoted.push_back(c);
    }
    quoted.push_back('"');
    return quoted;
  }
  return sanitized;
}

EchoVR::LogLevel LevelFor(EventLevel level) {
  switch (level) {
    case EventLevel::kDebug:
      return EchoVR::LogLevel::Debug;
    case EventLevel::kInfo:
      return EchoVR::LogLevel::Info;
    case EventLevel::kWarn:
      return EchoVR::LogLevel::Warning;
    case EventLevel::kError:
      return EchoVR::LogLevel::Error;
  }
  return EchoVR::LogLevel::Error;
}

}  // namespace

bool WindowsEnv::ReadSmallFile(const std::string& path, std::string* out, std::size_t max_bytes) {
  std::wstring wide;
  if (!ToWide(path, &wide)) return false;
  ScopedHandle file(CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!file.valid() || !IsPlainFile(file.get())) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0) return false;
  if (static_cast<std::uint64_t>(size.QuadPart) > max_bytes) return false;
  std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t got = 0;
  while (got < text.size()) {
    const std::size_t left = text.size() - got;
    const DWORD chunk = left < kMaxIoChunkBytes ? static_cast<DWORD>(left) : static_cast<DWORD>(kMaxIoChunkBytes);
    DWORD read = 0;
    if (!ReadFile(file.get(), &text[got], chunk, &read, nullptr) || read == 0) return false;
    got += read;
  }
  *out = std::move(text);
  return true;
}

bool WindowsEnv::MakeDirs(const std::string& dir) {
  if (!IsDriveAbsolute(dir)) return false;
  std::wstring wide;
  if (!ToWide(dir, &wide)) return false;
  // Each prefix that ends before a separator is created in turn; the drive root itself is never created.
  for (std::size_t i = kDriveRootLength + 1; i <= wide.size(); ++i) {
    if (i != wide.size() && !IsSeparator(wide[i])) continue;
    const std::wstring prefix = wide.substr(0, i);
    if (IsSeparator(prefix.back())) continue;
    if (!CreateDirectoryW(prefix.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
  }
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool WindowsEnv::WriteFileAtomically(const std::string& path, const std::string& data) {
  std::wstring wide;
  std::wstring temp;
  if (!ToWide(path, &wide) || !ToWide(path + kTempSuffix, &temp)) return false;
  bool ok = false;
  {
    ScopedHandle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    ok = file.valid() && WriteAll(file.get(), data.data(), data.size()) && FlushFileBuffers(file.get()) != FALSE;
  }
  if (ok) {
    ok = MoveFileExW(temp.c_str(), wide.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
  }
  if (!ok) DeleteFileW(temp.c_str());
  return ok;
}

std::unique_ptr<AppendFile> WindowsEnv::OpenAppend(const std::string& path) {
  std::wstring wide;
  if (!ToWide(path, &wide)) return nullptr;
  ScopedHandle file(CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!file.valid() || !IsPlainFile(file.get())) return nullptr;
  return std::make_unique<WindowsAppend>(file.release());
}

bool WindowsEnv::QueryDevice(const std::string& dir, DeviceFacts* out) {
  *out = DeviceFacts();
  std::wstring wide;
  ULARGE_INTEGER free_to_caller{};
  if (ToWide(dir, &wide) && GetDiskFreeSpaceExW(wide.c_str(), &free_to_caller, nullptr, nullptr) != FALSE) {
    out->have_free = true;
    out->free_bytes = free_to_caller.QuadPart;
  }
  MEMORYSTATUSEX memory{};
  memory.dwLength = static_cast<DWORD>(sizeof(memory));
  if (GlobalMemoryStatusEx(&memory) != FALSE) {
    out->have_memory = true;
    out->memory_available = memory.ullAvailPhys;
  }
  DWORD handles = 0;
  if (GetProcessHandleCount(GetCurrentProcess(), &handles) != FALSE) {
    out->have_fds = true;
    out->fd_soft_limit = kWindowsProcessHandleLimit;
    out->fds_open = handles;
  }
  return out->have_free || out->have_memory || out->have_fds;
}

bool WindowsEnv::OpenBlob(const std::string& path, std::int64_t* handle, std::uint64_t* size) {
  std::wstring wide;
  if (!ToWide(path, &wide)) return false;
  ScopedHandle file(CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!file.valid() || !IsPlainFile(file.get())) return false;
  LARGE_INTEGER length{};
  if (!GetFileSizeEx(file.get(), &length) || length.QuadPart < 0) return false;
  *handle = static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(file.release()));
  *size = static_cast<std::uint64_t>(length.QuadPart);
  return true;
}

bool WindowsEnv::ReadBlob(std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length, std::size_t* got) {
  const HANDLE file = BlobHandle(handle);
  char* out = static_cast<char*>(buffer);
  std::size_t total = 0;
  while (total < length) {
    const std::uint64_t at = offset + total;
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(at & kLowDwordMask);
    position.OffsetHigh = static_cast<DWORD>(at >> kBitsPerDword);
    const std::size_t left = length - total;
    const DWORD chunk = left < kMaxIoChunkBytes ? static_cast<DWORD>(left) : static_cast<DWORD>(kMaxIoChunkBytes);
    DWORD read = 0;
    if (!ReadFile(file, out + total, chunk, &read, &position)) {
      if (GetLastError() == ERROR_HANDLE_EOF) break;
      return false;
    }
    if (read == 0) break;
    total += read;
  }
  *got = total;
  return true;
}

void WindowsEnv::CloseBlob(std::int64_t handle) { CloseHandle(BlobHandle(handle)); }

std::unique_ptr<Hasher> WindowsEnv::NewHasher() { return std::make_unique<BcryptHasher>(); }

HttpResult WindowsEnv::Get(const std::string& url, const HttpBody& on_body, const std::function<bool()>& keep_going) {
  nevr::EnsureCurlGlobalInit();
  HttpResult out;
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    out.code = static_cast<int>(CURLE_FAILED_INIT);
    return out;
  }
  TransferState state;
  state.curl = curl;
  state.on_body = &on_body;
  state.keep_going = &keep_going;
  // The CDN serves each object at its own URL: a redirect is not followed, and it is a failure. The TLS settings are libcurl's
  // defaults, as every other PC libcurl caller in the runtime has them (no CAINFO is set).
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, kAllowedProtocols);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, kMinBytesPerSecond);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kStallSeconds);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &WriteBody);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &CheckKeepGoing);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
  const CURLcode res = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  out.completed = res == CURLE_OK;
  out.status = status;
  out.code = static_cast<int>(res);
  out.cancelled = state.cancelled;
  return out;
}

std::uint64_t WindowsEnv::NowMs() { return static_cast<std::uint64_t>(GetTickCount64()); }

void WindowsEnv::Event(EventLevel level, const char* name, const std::vector<Field>& fields) {
  std::string line = kLogTag;
  line += name;
  for (const Field& field : fields) {
    line += ' ';
    line += field.key;
    line += '=';
    line += field.is_number ? std::to_string(field.number) : FormatValue(field.text);
  }
  Log(LevelFor(level), "%s", line.c_str());
}

}  // namespace nevr_cdn

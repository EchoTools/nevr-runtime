#include "runtime/patch/asset_cache.h"

#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <nlohmann/json.hpp>

namespace nevr_asset_cache {

namespace {

constexpr wchar_t kBlobSuffix[] = L".blob";
constexpr wchar_t kTempSuffix[] = L".tmp";

std::string HexOf(const std::uint8_t* bytes, std::size_t len) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(len * 2);
  for (std::size_t i = 0; i < len; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 0x0F]);
  }
  return out;
}

std::wstring Widen(const std::string& ascii) { return std::wstring(ascii.begin(), ascii.end()); }

std::string FormatHash(std::uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
  return buf;
}

bool ParseHash(const nlohmann::json& j, std::uint64_t* out) {
  if (!j.is_string()) return false;
  const std::string s = j.get<std::string>();
  if (s.size() < 3 || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
  for (std::size_t i = 2; i < s.size(); ++i) {
    if (std::isxdigit(static_cast<unsigned char>(s[i])) == 0) return false;
  }
  if (s.size() > 18) return false;
  errno = 0;
  *out = std::strtoull(s.c_str() + 2, nullptr, 16);
  return errno == 0;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& leaf) {
  if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') return dir + L"\\" + leaf;
  return dir + leaf;
}

/// Creates every missing directory of `dir`.
bool EnsureDirectory(const std::wstring& dir, DWORD* error) {
  if (dir.empty()) {
    *error = ERROR_INVALID_NAME;
    return false;
  }
  const DWORD attrs = GetFileAttributesW(dir.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES) return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
  const std::size_t slash = dir.find_last_of(L"\\/");
  if (slash != std::wstring::npos && slash > 2) {
    if (!EnsureDirectory(dir.substr(0, slash), error)) return false;
  }
  if (CreateDirectoryW(dir.c_str(), nullptr) == 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
    *error = GetLastError();
    return false;
  }
  return true;
}

/// Reads a whole file. false with *error set on failure; *tooLarge when it exceeds `limit`.
bool ReadWholeFile(const std::wstring& path, std::uint64_t limit, std::vector<std::uint8_t>* out, bool* tooLarge,
                   DWORD* error) {
  *tooLarge = false;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    *error = GetLastError();
    return false;
  }
  LARGE_INTEGER size;
  if (GetFileSizeEx(file, &size) == 0) {
    *error = GetLastError();
    CloseHandle(file);
    return false;
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > limit) {
    *tooLarge = true;
    CloseHandle(file);
    return false;
  }
  out->assign(static_cast<std::size_t>(size.QuadPart), 0);
  std::size_t done = 0;
  while (done < out->size()) {
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out->size() - done, 1u << 20));
    if (ReadFile(file, out->data() + done, want, &got, nullptr) == 0 || got == 0) {
      *error = GetLastError();
      CloseHandle(file);
      return false;
    }
    done += got;
  }
  CloseHandle(file);
  return true;
}

/// temp file in `dir` -> flush -> rename over `finalPath`. *step names the failing step.
bool WriteAtomically(const std::wstring& dir, const std::wstring& finalPath, const std::wstring& tempStem,
                     const void* data, std::size_t len, const char** step, DWORD* error) {
  const std::wstring tempPath = JoinPath(
      dir, tempStem + L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) +
               kTempSuffix);
  *step = "create temp";
  HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    *error = GetLastError();
    return false;
  }
  *step = "write temp";
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t done = 0;
  while (done < len) {
    DWORD put = 0;
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(len - done, 1u << 20));
    if (WriteFile(file, bytes + done, want, &put, nullptr) == 0 || put == 0) {
      *error = GetLastError();
      CloseHandle(file);
      DeleteFileW(tempPath.c_str());
      return false;
    }
    done += put;
  }
  *step = "flush temp";
  if (FlushFileBuffers(file) == 0) {
    *error = GetLastError();
    CloseHandle(file);
    DeleteFileW(tempPath.c_str());
    return false;
  }
  CloseHandle(file);
  *step = "rename into place";
  if (MoveFileExW(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    *error = GetLastError();
    DeleteFileW(tempPath.c_str());
    return false;
  }
  return true;
}

/// Sets the file's last-write time to now: the "last used" stamp Evict() orders by.
void Touch(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  SetFileTime(file, nullptr, nullptr, &now);
  CloseHandle(file);
}

std::uint64_t FileTimeValue(const FILETIME& ft) {
  return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

}  // namespace

std::wstring DefaultDir() {
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::wstring();
  return JoinPath(std::wstring(buf, n), L"nevr\\echovr\\cache");
}

std::string Sha256Hex(const void* data, std::size_t len) {
  HCRYPTPROV prov = 0;
  HCRYPTHASH hash = 0;
  if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) == 0) return std::string();
  if (CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash) == 0) {
    CryptReleaseContext(prov, 0);
    return std::string();
  }
  const auto* bytes = static_cast<const BYTE*>(data);
  std::size_t done = 0;
  bool ok = true;
  while (ok && done < len) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(len - done, 1u << 20));
    ok = CryptHashData(hash, bytes + done, chunk, 0) != 0;
    done += chunk;
  }
  std::uint8_t digest[32];
  DWORD digestLen = sizeof(digest);
  if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &digestLen, 0) != 0 && digestLen == sizeof(digest);
  CryptDestroyHash(hash);
  CryptReleaseContext(prov, 0);
  return ok ? HexOf(digest, sizeof(digest)) : std::string();
}

bool IsSha256Hex(const std::string& s) {
  if (s.size() != 64) return false;
  return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::wstring BlobPath(const std::wstring& dir, const std::string& sha256) {
  return JoinPath(dir, Widen(sha256) + kBlobSuffix);
}

LoadResult Load(const std::wstring& dir, const std::string& sha256) {
  LoadResult r;
  r.expectedSha256 = sha256;
  if (!IsSha256Hex(sha256)) {
    r.status = LoadStatus::BadKey;
    return r;
  }
  const std::wstring path = BlobPath(dir, sha256);
  bool tooLarge = false;
  if (!ReadWholeFile(path, kMaxBlobBytes, &r.bytes, &tooLarge, &r.error)) {
    r.bytes.clear();
    if (tooLarge) r.status = LoadStatus::TooLarge;
    else if (r.error == ERROR_FILE_NOT_FOUND || r.error == ERROR_PATH_NOT_FOUND) r.status = LoadStatus::Missing;
    else r.status = LoadStatus::ReadFailed;
    return r;
  }
  r.actualSha256 = Sha256Hex(r.bytes.data(), r.bytes.size());
  if (r.actualSha256.empty()) {
    r.bytes.clear();
    r.status = LoadStatus::ReadFailed;
    r.error = GetLastError();
    return r;
  }
  if (r.actualSha256 != sha256) {
    r.bytes.clear();
    r.status = LoadStatus::HashMismatch;
    return r;
  }
  Touch(path);
  r.status = LoadStatus::Hit;
  return r;
}

StoreResult Store(const std::wstring& dir, const void* data, std::size_t len) {
  StoreResult r;
  if (len > kMaxBlobBytes) {
    r.error = ERROR_FILE_TOO_LARGE;
    r.failedStep = "size check";
    return r;
  }
  r.sha256 = Sha256Hex(data, len);
  if (r.sha256.empty()) {
    r.error = GetLastError();
    r.failedStep = "hash";
    return r;
  }
  if (!EnsureDirectory(dir, &r.error)) {
    r.failedStep = "create directory";
    return r;
  }
  if (Load(dir, r.sha256).status == LoadStatus::Hit) {
    r.status = StoreStatus::AlreadyPresent;
    return r;
  }
  if (!WriteAtomically(dir, BlobPath(dir, r.sha256), Widen(r.sha256), data, len, &r.failedStep, &r.error)) return r;
  r.status = StoreStatus::Stored;
  r.failedStep = "";
  return r;
}

IndexResult ReadIndex(const std::wstring& dir) {
  IndexResult r;
  std::vector<std::uint8_t> raw;
  bool tooLarge = false;
  DWORD err = 0;
  if (!ReadWholeFile(JoinPath(dir, kIndexName), 1u << 20, &raw, &tooLarge, &err)) {
    if (!tooLarge && (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)) return r;  // no index: nothing cached
    r.present = true;
    r.ok = false;
    r.error = tooLarge ? "index.json is larger than 1 MiB" : "index.json unreadable, error " + std::to_string(err);
    return r;
  }
  r.present = true;
  const nlohmann::json doc = nlohmann::json::parse(raw.begin(), raw.end(), nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    r.ok = false;
    r.error = "index.json is not a JSON object";
    return r;
  }
  const auto version = doc.find("version");
  if (version == doc.end() || !version->is_number_integer() || version->get<int>() != 1) {
    r.ok = false;
    r.error = "index.json version is not 1";
    return r;
  }
  const auto list = doc.find("overrides");
  if (list == doc.end() || !list->is_array()) {
    r.ok = false;
    r.error = "index.json has no overrides array";
    return r;
  }
  std::size_t position = 0;
  for (const nlohmann::json& item : *list) {
    ++position;
    IndexEntry e;
    const std::string where = "overrides[" + std::to_string(position - 1) + "]";
    if (!item.is_object()) {
      r.skipped.push_back(where + " is not an object");
      continue;
    }
    const auto type = item.find("type");
    const auto name = item.find("name");
    const auto sha = item.find("sha256");
    if (type == item.end() || !ParseHash(*type, &e.typeHash)) {
      r.skipped.push_back(where + " type is not a 0x hash");
      continue;
    }
    if (name == item.end() || !ParseHash(*name, &e.nameHash) || e.nameHash == 0) {
      r.skipped.push_back(where + " name is not a nonzero 0x hash");
      continue;
    }
    if (sha == item.end() || !sha->is_string() || !IsSha256Hex(sha->get<std::string>())) {
      r.skipped.push_back(where + " sha256 is not 64 lowercase hex characters");
      continue;
    }
    e.sha256 = sha->get<std::string>();
    const auto label = item.find("label");
    if (label != item.end() && label->is_string()) e.label = label->get<std::string>();
    r.entries.push_back(std::move(e));
  }
  return r;
}

bool SetIndexEntry(const std::wstring& dir, const IndexEntry& entry, std::string* error) {
  if (!IsSha256Hex(entry.sha256) || entry.nameHash == 0) {
    *error = "entry needs a 64-hex sha256 and a nonzero name hash";
    return false;
  }
  IndexResult current = ReadIndex(dir);
  if (!current.ok) {
    *error = current.error;  // never overwrite an index we could not read
    return false;
  }
  nlohmann::json overrides = nlohmann::json::array();
  bool replaced = false;
  for (const IndexEntry& e : current.entries) {
    const IndexEntry& use = (e.typeHash == entry.typeHash && e.nameHash == entry.nameHash) ? entry : e;
    replaced = replaced || &use == &entry;
    overrides.push_back({{"type", FormatHash(use.typeHash)},
                         {"name", FormatHash(use.nameHash)},
                         {"sha256", use.sha256},
                         {"label", use.label}});
  }
  if (!replaced) {
    overrides.push_back({{"type", FormatHash(entry.typeHash)},
                         {"name", FormatHash(entry.nameHash)},
                         {"sha256", entry.sha256},
                         {"label", entry.label}});
  }
  const nlohmann::json doc = {{"version", 1}, {"overrides", overrides}};
  const std::string text = doc.dump(2) + "\n";
  DWORD err = 0;
  if (!EnsureDirectory(dir, &err)) {
    *error = "create directory failed, error " + std::to_string(err);
    return false;
  }
  const char* step = "";
  if (!WriteAtomically(dir, JoinPath(dir, kIndexName), L"index", text.data(), text.size(), &step, &err)) {
    *error = std::string(step) + " failed, error " + std::to_string(err);
    return false;
  }
  return true;
}

EvictReport Evict(const std::wstring& dir, std::uint64_t capBytes, const std::set<std::string>& referenced) {
  EvictReport report;
  struct Blob {
    std::string sha;
    std::wstring path;
    std::uint64_t bytes;
    std::uint64_t lastUsed;
  };
  std::vector<Blob> blobs;
  FILETIME nowFt;
  GetSystemTimeAsFileTime(&nowFt);
  const std::uint64_t now = FileTimeValue(nowFt);
  const std::uint64_t staleTicks = kStaleTempSeconds * 10000000ull;

  WIN32_FIND_DATAW fd;
  HANDLE find = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) return report;
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
    const std::wstring name = fd.cFileName;
    const std::uint64_t size = (static_cast<std::uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    const std::uint64_t stamp = FileTimeValue(fd.ftLastWriteTime);
    const auto endsWith = [&name](const wchar_t* suffix) {
      const std::size_t n = std::wcslen(suffix);
      return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
    };
    if (endsWith(kTempSuffix)) {
      if (now > stamp && now - stamp > staleTicks && DeleteFileW(JoinPath(dir, name).c_str()) != 0) {
        ++report.staleTempsRemoved;
      }
      continue;
    }
    if (!endsWith(kBlobSuffix) || name.size() != 64 + std::wcslen(kBlobSuffix)) continue;
    std::string sha;
    for (std::size_t i = 0; i < 64; ++i) sha.push_back(static_cast<char>(name[i] < 128 ? name[i] : '?'));
    if (!IsSha256Hex(sha)) continue;
    blobs.push_back({sha, JoinPath(dir, name), size, stamp});
    report.blobBytesBefore += size;
  } while (FindNextFileW(find, &fd) != 0);
  FindClose(find);

  report.blobBytesAfter = report.blobBytesBefore;
  std::vector<const Blob*> evictable;
  for (const Blob& b : blobs) {
    if (referenced.count(b.sha) == 0) evictable.push_back(&b);
  }
  std::sort(evictable.begin(), evictable.end(), [](const Blob* a, const Blob* b) { return a->lastUsed < b->lastUsed; });
  for (const Blob* b : evictable) {
    if (report.blobBytesAfter <= capBytes) break;
    if (DeleteFileW(b->path.c_str()) != 0) {
      report.blobBytesAfter -= b->bytes;
      ++report.blobsEvicted;
    }
  }
  report.overCapAllReferenced = report.blobBytesAfter > capBytes;
  return report;
}

}  // namespace nevr_asset_cache

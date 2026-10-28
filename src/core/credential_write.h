#pragma once

// How the refresh token reaches disk (#563, #562). The token is written to a temp file beside the cache
// with its protection applied BEFORE any byte of it is written, flushed, and only then renamed over the
// cache: a write that fails at any point leaves the previous token exactly as it was, and a reader opening
// the cache never meets a half-written file or a share-mode lock.
//
//   native Windows  the temp file is created with a protected current-user-only DACL, which is set again
//                   and read back (one ACE, the user's SID, SE_DACL_PROTECTED) before the token is written.
//   Wine / Proton   the Windows DACL calls do not change the host file's Unix mode (measured, wine-11.18:
//                   SetSecurityInfo, SetNamedSecurityInfo and _chmod all left the host mode at 775), so the
//                   file is protected the Unix way: the host's own chmod 600, run through Wine's Unix
//                   process launch, and the host mode is read back with stat. Not 600 => nothing written.
//   POSIX           open(2) with 0600 at creation, written, fsync'd, closed, renamed.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/logging.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <fstream>
#include <vector>
#else
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nevr_credential_write {

#ifdef NEVR_TEST_HOOKS
// Fault injection for the tests: fail the temp-file write after this many bytes (SIZE_MAX = never), and run
// a hook once the first chunk is on disk (a reader opening the cache from inside it proves readers are not
// blocked while a write is in flight).
inline std::size_t g_failAfterBytes = SIZE_MAX;
inline void (*g_midWriteHook)() = nullptr;
// Wine: use this (Windows) path for the helper's result file, and report this mode from the host helper in
// place of what stat printed (empty = off). They stand in for a leftover result file and a chmod that
// "succeeds" without producing 600.
inline std::string g_forceResultPath;
inline std::string g_forcedHostMode;
#endif

// Bytes written per call: small enough that the injection point above lands inside a short token file.
constexpr std::size_t kChunkBytes = 16;

inline std::string TempPathFor(const std::string& finalPath) { return finalPath + ".tmp"; }

// A temp name only this call uses: the process id and a counter between the cache's path and ".tmp". TempPathFor is one
// name per cache, right for the cache's own save; a one-time move into the same cache, possibly in a second process, must
// not meet, remove or reuse the temp file a save has in flight.
inline std::string UniqueTempPathFor(const std::string& finalPath) {
  static std::atomic<unsigned> sequence{0};
#ifdef _WIN32
  const unsigned long pid = GetCurrentProcessId();
#else
  const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
  return finalPath + "." + std::to_string(pid) + "." + std::to_string(sequence.fetch_add(1)) + ".tmp";
}

// What InstallCacheIfAbsent found.
enum class InstallResult {
  kInstalled,      // the temp file is now the cache
  kAlreadyExists,  // a cache was there: it is untouched and the temp file is removed
  kFailed          // the temp file is removed and `why` says what failed; the cache, if any, is untouched
};

#ifdef _WIN32

// Characters a host path may carry into the helper command line. Anything else is refused, not escaped.
inline bool SafeHostPath(const std::string& path) {
  if (path.empty() || path[0] != '/') return false;
  for (const char c : path) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '/' ||
                    c == '.' || c == '_' || c == '-' || c == '+' || c == '@' || c == ',' || c == '=' || c == '~' ||
                    c == ' ';
    if (!ok) return false;
  }
  return true;
}

// The host (Unix) path of a Windows path, through Wine's kernel32 export. False off Wine.
inline bool WineHostPath(const std::string& windowsPath, std::string* hostPath) {
  using HostNameFn = char*(CDECL*)(LPCWSTR);
  const HMODULE kernel = GetModuleHandleA("kernel32.dll");
  const auto toHost = kernel != nullptr
                          ? reinterpret_cast<HostNameFn>(
                                reinterpret_cast<void*>(GetProcAddress(kernel, "wine_get_unix_file_name")))
                          : nullptr;
  if (toHost == nullptr) return false;
  wchar_t wide[2 * MAX_PATH] = {};
  if (MultiByteToWideChar(CP_ACP, 0, windowsPath.c_str(), -1, wide, 2 * MAX_PATH) == 0) return false;
  char* host = toHost(wide);
  if (host == nullptr) return false;
  *hostPath = host;
  HeapFree(GetProcessHeap(), 0, host);
  return SafeHostPath(*hostPath);
}

// Runs `/bin/sh -c <script> sh <hostPath>` on the host and returns what it printed. A Unix process started
// through Wine cannot be waited on (WaitForSingleObject fails on its handle, measured), so the output goes to
// a file the caller polls; the script prints only after the work it reports has succeeded.
inline bool WineHostScript(const std::string& windowsPath, const char* script, std::string* output,
                           std::string* why) {
  std::string hostPath;
  if (!WineHostPath(windowsPath, &hostPath)) {
    *why = "no safe host path for the file (wine_get_unix_file_name missing or the path has unusual characters)";
    return false;
  }
  // The script redirects its own stdout into the result file: Wine does not always hand a redirected std
  // handle to a Unix child (the child got /dev/null under the test binary, measured), so none is relied on.
  // The result path is unique to this call and must not exist: a leftover "600" must never be readable as
  // this call's answer, so an existing file (or one that cannot be removed afterwards) fails closed.
  static std::atomic<unsigned> sequence{0};
  std::string resultPath = windowsPath + "." + std::to_string(GetCurrentProcessId()) + "." +
                           std::to_string(sequence.fetch_add(1)) + ".mode";
#ifdef NEVR_TEST_HOOKS
  if (!g_forceResultPath.empty()) resultPath = g_forceResultPath;
#endif
  if (GetFileAttributesA(resultPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
    *why = "the helper result file already exists";
    return false;
  }
  std::string hostResult;
  if (!WineHostPath(resultPath, &hostResult)) {
    *why = "no safe host path for the helper result file";
    return false;
  }

  std::string command = "sh -c \"{ ";
  command += script;
  command += "; } > \\\"$2\\\"\" sh \"" + hostPath + "\" \"" + hostResult + "\"";
  STARTUPINFOA startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  std::vector<char> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back('\0');
  const BOOL started = CreateProcessA("/bin/sh", mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                      nullptr, &startup, &process);
  if (!started) {
    *why = "cannot start /bin/sh through Wine: " + std::to_string(GetLastError());
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);

  bool got = false;
  for (int waited = 0; waited < 3000 && !got; waited += 10) {
    std::ifstream result(resultPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(result)), std::istreambuf_iterator<char>());
    if (!text.empty() && text.back() == '\n') {
      text.pop_back();
      *output = text;
      got = true;
    } else {
      Sleep(10);
    }
  }
  DeleteFileA(resultPath.c_str());
  if (GetFileAttributesA(resultPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
    *why = "the helper result file could not be removed";
    return false;
  }
  if (!got) *why = "the host helper printed nothing within 3 s";
  return got;
}

// The host mode of a file as octal text ("600"), read with the host's stat. Empty on failure.
inline std::string WineHostMode(const std::string& windowsPath) {
  std::string mode, why;
  return WineHostScript(windowsPath, "stat -c %a -- \\\"$1\\\"", &mode, &why) ? mode : std::string();
}

// The mode a helper reported, after the test seam (a forced value stands in for what stat printed).
inline std::string ReportedMode(std::string mode) {
#ifdef NEVR_TEST_HOOKS
  if (!g_forcedHostMode.empty()) mode = g_forcedHostMode;
#endif
  return mode;
}
inline bool ModeIs600(const std::string& mode) { return ReportedMode(mode) == "600"; }

// Creates the temp file ON THE HOST, born mode 600: the helper's umask is 077 and noclobber makes it fail
// if the name already exists, so there is no moment at which the inode has a wider mode for another local
// user to open and keep. The mode is read back and must be exactly 600. The caller removes any stale temp
// first and opens the result with OPEN_EXISTING.
inline bool WineCreateProtectedTemp(const std::string& windowsPath, std::string* why) {
  std::string mode;
  std::string detail;
  if (!WineHostScript(windowsPath,
                      "umask 077 && set -C && : > \\\"$1\\\" && chmod 600 -- \\\"$1\\\" && stat -c %a -- \\\"$1\\\"",
                      &mode, &detail)) {
    *why = "the host helper could not create the temp file: " + detail;
    return false;
  }
  if (!ModeIs600(mode)) {
    *why = "host mode is " + ReportedMode(mode) + ", not 600";
    return false;
  }
  return true;
}

// Native: a protected DACL naming only the current user, applied to the open handle and read back.
// `attributes` is filled for CreateFile so the file is born with it.
struct NativeProtection {
  std::vector<BYTE> tokenInfo;
  PACL acl = nullptr;
  SECURITY_DESCRIPTOR descriptor = {};
  SECURITY_ATTRIBUTES attributes = {};
  ~NativeProtection() {
    if (acl != nullptr) LocalFree(acl);
  }
  const TOKEN_USER* user() const { return reinterpret_cast<const TOKEN_USER*>(tokenInfo.data()); }
};

inline bool PrepareNativeProtection(NativeProtection* p, std::string* why) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    *why = "cannot inspect the current user token: " + std::to_string(GetLastError());
    return false;
  }
  DWORD bytes = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
  p->tokenInfo.assign(bytes, 0);
  const bool haveUser =
      bytes != 0 && GetTokenInformation(token, TokenUser, p->tokenInfo.data(), bytes, &bytes);
  const DWORD userError = haveUser ? ERROR_SUCCESS : GetLastError();
  CloseHandle(token);
  if (!haveUser) {
    *why = "cannot read the current user SID: " + std::to_string(userError);
    return false;
  }
  EXPLICIT_ACCESSA access = {};
  access.grfAccessPermissions = GENERIC_ALL;
  access.grfAccessMode = SET_ACCESS;
  access.grfInheritance = NO_INHERITANCE;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.ptstrName = reinterpret_cast<LPSTR>(p->user()->User.Sid);
  const DWORD aclError = SetEntriesInAclA(1, &access, nullptr, &p->acl);
  if (aclError != ERROR_SUCCESS) {
    *why = "cannot build the credentials DACL: " + std::to_string(aclError);
    return false;
  }
  if (!InitializeSecurityDescriptor(&p->descriptor, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorDacl(&p->descriptor, TRUE, p->acl, FALSE) ||
      !SetSecurityDescriptorControl(&p->descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
    *why = "cannot prepare the credentials security descriptor: " + std::to_string(GetLastError());
    return false;
  }
  p->attributes = {sizeof(p->attributes), &p->descriptor, FALSE};
  return true;
}

// Set the protected DACL on the open file and require it to read back as one allow-ACE for the user.
inline bool ApplyAndVerifyNativeDacl(HANDLE file, NativeProtection* p, std::string* why) {
  const DWORD setError = SetSecurityInfo(file, SE_FILE_OBJECT,
                                         DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr,
                                         nullptr, p->acl, nullptr);
  if (setError != ERROR_SUCCESS) {
    *why = "cannot set the DACL: " + std::to_string(setError);
    return false;
  }
  PSECURITY_DESCRIPTOR observed = nullptr;
  PACL observedAcl = nullptr;
  const DWORD readError = GetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                          &observedAcl, nullptr, &observed);
  if (readError != ERROR_SUCCESS) {
    *why = "cannot read the DACL back: " + std::to_string(readError);
    return false;
  }
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  void* ace = nullptr;
  const bool ok = observed != nullptr && observedAcl != nullptr &&
                  GetSecurityDescriptorControl(observed, &control, &revision) && (control & SE_DACL_PROTECTED) != 0 &&
                  observedAcl->AceCount == 1 && GetAce(observedAcl, 0, &ace) &&
                  static_cast<ACE_HEADER*>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE &&
                  EqualSid(p->user()->User.Sid, &static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart);
  if (observed != nullptr) LocalFree(observed);
  if (!ok) *why = "the user-only DACL was not preserved";
  return ok;
}

// Writes `data` to a fresh, protected temp file and flushes it. On any failure the temp file is removed and
// `why` says what failed. The previous cache is never opened.
inline bool WriteProtectedTemp(const std::string& tempPath, const std::string& data, bool isWine,
                               std::string* why) {
  NativeProtection protection;
  SECURITY_ATTRIBUTES* attributes = nullptr;
  if (!isWine) {
    if (!PrepareNativeProtection(&protection, why)) return false;
    attributes = &protection.attributes;
  }
  HANDLE file = INVALID_HANDLE_VALUE;
  if (isWine) {
    // A stale temp (crash leftover) is removed, never reused: its inode may have been opened by someone
    // else. The helper then creates the file new, already 600; this process only opens what exists.
    SetFileAttributesA(tempPath.c_str(), FILE_ATTRIBUTE_NORMAL);
    DeleteFileA(tempPath.c_str());
    if (GetFileAttributesA(tempPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
      *why = "a stale temp file could not be removed";
      return false;
    }
    if (!WineCreateProtectedTemp(tempPath, why)) {
      DeleteFileA(tempPath.c_str());
      return false;
    }
    file = CreateFileA(tempPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      *why = "cannot open the protected temp file: " + std::to_string(GetLastError());
      DeleteFileA(tempPath.c_str());
      return false;
    }
  } else {
    // Readers may open the temp file; the cache itself is only ever replaced by rename.
    file = CreateFileA(tempPath.c_str(), GENERIC_WRITE | WRITE_DAC | READ_CONTROL, FILE_SHARE_READ,
                       attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      *why = "cannot create the temp file: " + std::to_string(GetLastError());
      return false;
    }
    // CREATE_ALWAYS keeps an existing temp file's old DACL (a crash leftover): set it again, read it back.
    if (!ApplyAndVerifyNativeDacl(file, &protection, why)) {
      CloseHandle(file);
      DeleteFileA(tempPath.c_str());
      return false;
    }
  }

  bool written = true;
  std::size_t offset = 0;
  while (written && offset < data.size()) {
    std::size_t count = data.size() - offset;
    if (count > kChunkBytes) count = kChunkBytes;
#ifdef NEVR_TEST_HOOKS
    if (g_failAfterBytes != SIZE_MAX && offset + count > g_failAfterBytes) {
      *why = "injected write failure";
      written = false;
      break;
    }
#endif
    DWORD actual = 0;
    const BOOL ok = WriteFile(file, data.data() + offset, static_cast<DWORD>(count), &actual, nullptr);
    if (!ok) {
      *why = "WriteFile failed: " + std::to_string(GetLastError());
      written = false;
    } else if (actual == 0) {
      // TRUE with no bytes written sets no error: say so instead of reading a stale GetLastError().
      *why = "WriteFile reported success but wrote 0 bytes";
      written = false;
    } else {
      offset += actual;
#ifdef NEVR_TEST_HOOKS
      if (offset == actual && g_midWriteHook != nullptr) g_midWriteHook();
#endif
    }
  }
  if (written && !FlushFileBuffers(file)) {
    *why = "FlushFileBuffers failed: " + std::to_string(GetLastError());
    written = false;
  }
  const bool closed = CloseHandle(file) != 0;
  if (written && !closed) {
    *why = "CloseHandle failed: " + std::to_string(GetLastError());
    written = false;
  }
  if (!written) {
    DeleteFileA(tempPath.c_str());
    return false;
  }
  if (isWine) {
    // Written and closed: the mode is read back once more, immediately before the replace.
    std::string mode, detail;
    if (!WineHostScript(tempPath, "stat -c %a -- \\\"$1\\\"", &mode, &detail) || !ModeIs600(mode)) {
      *why = "host mode before the replace is not 600: " + (detail.empty() ? mode : detail);
      DeleteFileA(tempPath.c_str());
      return false;
    }
  }
  return true;
}

// Atomically replaces the cache with the temp file. The old file is untouched if this fails.
inline bool ReplaceCache(const std::string& tempPath, const std::string& finalPath, std::string* why) {
  // A cache an earlier build left HIDDEN/READONLY is refused by the replace with access denied (measured
  // under Wine): clear the attributes for the move and put them back if the move fails.
  const DWORD existing = GetFileAttributesA(finalPath.c_str());
  const bool cleared = existing != INVALID_FILE_ATTRIBUTES &&
                       (existing & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_READONLY)) != 0;
  if (cleared) SetFileAttributesA(finalPath.c_str(), FILE_ATTRIBUTE_NORMAL);
  if (MoveFileExA(tempPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
  *why = "cannot replace the cache: " + std::to_string(GetLastError());
  if (cleared) SetFileAttributesA(finalPath.c_str(), existing);
  DeleteFileA(tempPath.c_str());
  return false;
}

// Puts the temp file in place as the cache only when there is no cache: a rename that does not replace, so a sign-in
// another process wrote after the caller looked is never overwritten. The temp file is gone afterwards, whatever the answer.
inline InstallResult InstallCacheIfAbsent(const std::string& tempPath, const std::string& finalPath, std::string* why) {
  if (MoveFileExA(tempPath.c_str(), finalPath.c_str(), MOVEFILE_WRITE_THROUGH)) return InstallResult::kInstalled;
  const DWORD error = GetLastError();
  DeleteFileA(tempPath.c_str());
  SetLastError(error);
  if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) return InstallResult::kAlreadyExists;
  *why = "cannot install the cache: " + std::to_string(error);
  return InstallResult::kFailed;
}

#else  // POSIX

inline bool WriteProtectedTemp(const std::string& tempPath, const std::string& data, bool /*isWine*/,
                               std::string* why) {
  // A stale temp (crash leftover) is removed, never reused or followed: the file is created new with
  // O_EXCL|O_NOFOLLOW and 0600 at creation, so it is never readable by anyone else, not even briefly.
  ::unlink(tempPath.c_str());
  const int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (fd < 0) {
    *why = std::string("cannot create the temp file: ") + std::to_string(errno);
    return false;
  }
  // The creation mode is narrowed by the umask only; state the mode outright and fail if it cannot be set.
  if (::fchmod(fd, 0600) != 0) {
    *why = std::string("fchmod failed: ") + std::to_string(errno);
    ::close(fd);
    ::unlink(tempPath.c_str());
    return false;
  }
  bool written = true;
  std::size_t offset = 0;
  while (written && offset < data.size()) {
    std::size_t count = data.size() - offset;
    if (count > kChunkBytes) count = kChunkBytes;
#ifdef NEVR_TEST_HOOKS
    if (g_failAfterBytes != SIZE_MAX && offset + count > g_failAfterBytes) {
      *why = "injected write failure";
      written = false;
      break;
    }
#endif
    const ssize_t n = ::write(fd, data.data() + offset, count);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      *why = n < 0 ? std::string("write failed: ") + std::to_string(errno) : "write returned 0 bytes";
      written = false;
    } else {
      offset += static_cast<std::size_t>(n);
#ifdef NEVR_TEST_HOOKS
      if (offset == static_cast<std::size_t>(n) && g_midWriteHook != nullptr) g_midWriteHook();
#endif
    }
  }
  if (written && ::fsync(fd) != 0) {
    *why = std::string("fsync failed: ") + std::to_string(errno);
    written = false;
  }
  if (::close(fd) != 0 && written) {
    *why = std::string("close failed: ") + std::to_string(errno);
    written = false;
  }
  if (!written) {
    ::unlink(tempPath.c_str());
    return false;
  }
  return true;
}

inline bool ReplaceCache(const std::string& tempPath, const std::string& finalPath, std::string* why) {
  if (::rename(tempPath.c_str(), finalPath.c_str()) == 0) {
    // Make the rename itself durable: fsync the directory. Best effort; the cache is already in place.
    const std::size_t slash = finalPath.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : finalPath.substr(0, slash));
    const int dirFd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dirFd >= 0) {
      if (::fsync(dirFd) != 0) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.AUTH] directory fsync after the credential replace failed: %d", errno);
      }
      ::close(dirFd);
    }
    return true;
  }
  *why = std::string("cannot replace the cache: ") + std::to_string(errno);
  ::unlink(tempPath.c_str());
  return false;
}

inline InstallResult InstallCacheIfAbsent(const std::string& tempPath, const std::string& finalPath, std::string* why) {
  // link(2) fails with EEXIST where rename(2) would replace: the cache is created or left alone, never overwritten.
  const int linked = ::link(tempPath.c_str(), finalPath.c_str());
  const int linkError = errno;
  ::unlink(tempPath.c_str());
  if (linked == 0) return InstallResult::kInstalled;
  if (linkError == EEXIST) return InstallResult::kAlreadyExists;
  *why = std::string("cannot install the cache: ") + std::to_string(linkError);
  return InstallResult::kFailed;
}

#endif

}  // namespace nevr_credential_write

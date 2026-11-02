// Built with -fno-exceptions (dlopen_hook.cpp, which calls RewriteLoadName from its handler, is built that way).
#include "quest/plugin/script_redirect.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include "hook_log.h"

namespace nevr_quest::script_redirect {

namespace {

constexpr std::size_t kHashDigits = 16;
constexpr std::size_t kMissingCapacity = kMaxEntries * (kNameCapacity + 1);

std::atomic<std::uint32_t> g_count{0};  // names in force; written once, release-stored after g_hashes
std::atomic<bool> g_busy{false};        // a registration holds the claim: it has not yet stored g_count or given the claim up
std::atomic<bool> g_logEveryLoad{false};  // the development build's per-load lines (SetLogEveryLoad)
std::atomic<void (*)() noexcept> g_beforeClaimHook{nullptr};  // tests only (SetBeforeClaimHookForTest)
std::uint64_t g_hashes[kMaxEntries] = {};

// The last '/'-separated component of `name`.
const char* FinalComponent(const char* name) noexcept {
  const char* const slash = std::strrchr(name, '/');
  return slash != nullptr ? slash + 1 : name;
}

// True when `path` names a regular file. A directory or a link to one with the library's name is not a library.
bool IsRegularFile(const char* path) noexcept {
  struct stat st {};
  return ::stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// What each refusal token means, in words. The log's `reason` is always the token; this text is its own field, `detail`.
const char* ReasonDetail(const char* token) noexcept {
  struct Row {
    const char* token;
    const char* detail;
  };
  static constexpr Row kRows[] = {
      {"argument_out_of_range", "the plugin id or hash list is null, or the count is outside 1..16"},
      {"duplicate_hash", "the hash list names one stock library twice"},
      {"no_library_directory", "the host has no native library directory to check the combat libraries in"},
      {"already_registered", "a registration is already in force, or one is being decided"},
      {"dladdr_failed", "the loader could not say which file the sentinel was loaded from"},
      {"library_path_missing", "the loader gave no usable path for the sentinel's own library"},
      {"library_inside_apk", "the native library is mapped from the APK, not extracted to disk"},
      {"library_path_has_no_directory", "the sentinel's library path has no directory part"},
      {"library_path_too_long", "the sentinel's library directory does not fit the host's path buffer"},
  };
  for (const Row& row : kRows) {
    if (std::strcmp(row.token, token) == 0) return row.detail;
  }
  return "";
}

NevrScriptRedirectResult Refuse(NevrScriptRedirectResult result, const char* who, const char* reason) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kError, "script_redirect",
                      {{"op", "register"}, {"result", ResultName(result)}, {"plugin", who}, {"registry", 0},
                       {"reason", reason}, {"detail", ReasonDetail(reason)}});
  return result;
}

}  // namespace

bool RedirectName(std::uint64_t hash, char* out, std::size_t cap) noexcept {
  static constexpr char kHex[] = "0123456789abcdef";
  static constexpr char kTail[] = "_q15t.so";  // with its NUL
  const std::size_t length = 3 + kHashDigits + sizeof kTail - 1;
  if (out == nullptr || cap < length + 1) return false;
  out[0] = 'l';
  out[1] = 'i';
  out[2] = 'b';
  for (std::size_t i = 0; i < kHashDigits; ++i) {
    out[3 + i] = kHex[(hash >> (60 - 4 * i)) & 0xFu];
  }
  std::memcpy(out + 3 + kHashDigits, kTail, sizeof kTail);
  return true;
}

bool ParseScriptName(const char* name, std::uint64_t* hash) noexcept {
  if (name == nullptr || hash == nullptr) return false;
  const char* const base = FinalComponent(name);
  const std::size_t length = std::strlen(base);
  const char* digits = nullptr;
  if (length == 3 + kHashDigits + 3 && std::strncmp(base, "lib", 3) == 0) {
    digits = base + 3;
  } else if (length == kHashDigits + 3) {
    digits = base;
  } else {
    return false;
  }
  if (std::strcmp(digits + kHashDigits, ".so") != 0) return false;
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < kHashDigits; ++i) {
    const char c = digits[i];
    std::uint64_t nibble = 0;
    if (c >= '0' && c <= '9') {
      nibble = static_cast<std::uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      nibble = static_cast<std::uint64_t>(c - 'a' + 10);
    } else {
      return false;
    }
    value = (value << 4) | nibble;
  }
  *hash = value;
  return true;
}

const char* RewriteLoadName(const char* name, char* buf, std::size_t cap) noexcept {
  if (name == nullptr) return name;
  std::uint64_t hash = 0;
  if (!ParseScriptName(name, &hash)) return name;  // not a script library: no line, the name passes unchanged
  const std::uint32_t count = g_count.load(std::memory_order_acquire);
  bool registered = false;
  for (std::uint32_t i = 0; i < count && !registered; ++i) registered = g_hashes[i] == hash;
  const bool redirect = registered && RedirectName(hash, buf, cap);
  if (registered && !redirect) {
    // A name the registry holds that cannot be rewritten (the caller's buffer is too small): the game gets the stock
    // library, which is not what the registration promised.
    sentinel::LogFields(sentinel::LogLevel::kWarn, "script_redirect_skipped",
                        {{"name", name}, {"reason", "buffer_too_small"}, {"buffer", static_cast<long long>(cap)},
                         {"consequence", "the game loads the stock library"}});
    return name;
  }
  if (!redirect) {
    // Per-item detail: the development build's log only (SetLogEveryLoad). The sentinel has no Debug level; the plugin
    // loader's Debug lines take the same shape (Info, dropped in a release build).
    if (g_logEveryLoad.load(std::memory_order_relaxed)) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "script_load",
                          {{"name", name}, {"registry_set", count != 0 ? "true" : "false"}, {"redirected", "false"},
                           {"verbosity", "debug"}});
    }
    return name;
  }
  char hex[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "script_redirected",
                      {{"from", name}, {"to", buf}, {"hash", sentinel::HexString(hex, hash)}});
  return buf;
}

void* OpenRedirected(void* (*loader)(const char* name, int flags), const char* name, int flags, char* buf,
                     std::size_t cap) noexcept {
  const char* const loadName = RewriteLoadName(name, buf, cap);
  void* handle = loader(loadName, flags);
  if (handle == nullptr && loadName != name) {
    // A redirected load that failed fails loudly: the game gets the loader's null, and the names and dlerror() say why.
    const int savedErrno = errno;
    const char* const reason = dlerror();  // reading the loader's error clears it
    sentinel::LogFields(sentinel::LogLevel::kError, "script_redirect_failed",
                        {{"from", name}, {"to", loadName}, {"dlerror", reason != nullptr ? reason : "(none)"}});
    // The game's next dlerror() must still say why its load failed, so the same open is issued again: it fails the same
    // way and the loader's text for the redirected name is readable once more. (The stock name is never opened here: that
    // would load the library the redirect replaces.) An open that succeeds on this second try is the game's handle.
    handle = loader(loadName, flags);
    if (handle != nullptr) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "script_redirect_recovered",
                          {{"from", name}, {"to", loadName},
                           {"consequence", "the second open of the combat library succeeded; the game gets its handle"}});
    }
    errno = savedErrno;
  }
  return handle;
}

NevrScriptRedirectResult RegisterIn(const char* libraryDir, const char* pluginId, const std::uint64_t* hashes,
                                    std::uint32_t count) noexcept {
  const char* const who = pluginId != nullptr ? pluginId : "(null)";
  if (pluginId == nullptr || hashes == nullptr || count == 0 || count > kMaxEntries) {
    return Refuse(NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT, who, "argument_out_of_range");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    for (std::uint32_t j = i + 1; j < count; ++j) {
      if (hashes[i] == hashes[j]) return Refuse(NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT, who, "duplicate_hash");
    }
  }
  if (libraryDir == nullptr) return Refuse(NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR, who, "no_library_directory");
  // Claim first, then look: g_count only changes while the claim is held, so a registration that finished before the
  // claim is seen here, and one still deciding makes the claim fail. (Looking at g_count before the claim would let a
  // second registration slip in between and overwrite g_hashes under the dlopen handler's readers.) The plugin loader
  // runs plugin init under one mutex, so this is not reached concurrently today; the claim keeps it true if that changes.
  if (const auto hook = g_beforeClaimHook.load(std::memory_order_acquire)) hook();
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return Refuse(NEVR_SCRIPT_REDIRECT_ALREADY_ARMED, who, "already_registered");
  }
  if (g_count.load(std::memory_order_acquire) != 0) {
    g_busy.store(false, std::memory_order_release);
    return Refuse(NEVR_SCRIPT_REDIRECT_ALREADY_ARMED, who, "already_registered");
  }
  // Every name must be a file in the library directory before any is registered (all or nothing).
  char missing[kMissingCapacity] = {};
  std::size_t missingLength = 0;
  std::uint32_t missingCount = 0;
  char name[kNameCapacity];
  char path[1024];
  for (std::uint32_t i = 0; i < count; ++i) {
    if (!RedirectName(hashes[i], name, sizeof name)) continue;  // cannot happen: the buffer holds the name
    std::snprintf(path, sizeof path, "%s/%s", libraryDir, name);
    if (IsRegularFile(path)) continue;
    ++missingCount;
    const std::size_t length = std::strlen(name);
    if (missingLength + length + 2 <= sizeof missing) {
      if (missingLength != 0) missing[missingLength++] = ',';
      std::memcpy(missing + missingLength, name, length);
      missingLength += length;
      missing[missingLength] = '\0';
    }
  }
  if (missingCount != 0) {
    g_busy.store(false);
    sentinel::LogFields(sentinel::LogLevel::kError, "script_redirect",
                        {{"op", "register"}, {"result", ResultName(NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY)},
                         {"plugin", who}, {"registry", 0}, {"missing_count", missingCount}, {"missing", missing},
                         {"consequence", "no script library is redirected; the game loads the stock files"}});
    return NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY;
  }
  for (std::uint32_t i = 0; i < count; ++i) g_hashes[i] = hashes[i];
  g_count.store(count, std::memory_order_release);
  g_busy.store(false);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "script_redirect",
                      {{"op", "register"}, {"result", ResultName(NEVR_SCRIPT_REDIRECT_OK)}, {"plugin", who},
                       {"registry", count}, {"status", "armed"},
                       {"consequence", "the game's loads of these stock script libraries open the combat build"}});
  return NEVR_SCRIPT_REDIRECT_OK;
}

const char* LibraryDirectoryOf(const char* path, char* dir, std::size_t cap) noexcept {
  if (path == nullptr || dir == nullptr || cap == 0) return "library_path_missing";
  // A library the loader maps out of the APK (a store APK that does not extract native libraries) is named
  // base.apk!/lib/<abi>/<file>: that directory is not on disk, so the names in it cannot be checked as files.
  if (std::strstr(path, ".apk!/") != nullptr) return "library_inside_apk";
  const char* const slash = std::strrchr(path, '/');
  if (slash == nullptr) return "library_path_has_no_directory";
  if (static_cast<std::size_t>(slash - path) + 1 > cap) return "library_path_too_long";
  const std::size_t dirLength = static_cast<std::size_t>(slash - path);
  std::memcpy(dir, path, dirLength);
  dir[dirLength] = '\0';
  return nullptr;
}

NevrScriptRedirectResult RegisterFromLibraryPath(const char* pluginId, const char* libraryPath,
                                                const std::uint64_t* hashes, std::uint32_t count) noexcept {
  const char* const who = pluginId != nullptr ? pluginId : "(null)";
  char dir[512];
  if (const char* const reason = LibraryDirectoryOf(libraryPath, dir, sizeof dir)) {
    return Refuse(NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR, who, reason);
  }
  return RegisterIn(dir, pluginId, hashes, count);
}

NevrScriptRedirectResult Register(const char* pluginId, const std::uint64_t* hashes, std::uint32_t count) noexcept {
  const char* const who = pluginId != nullptr ? pluginId : "(null)";
  // This is the same dladdr-then-directory step as plugin_runtime.cpp's OwnDirectory and plugin_platform.cpp's DirectoryOf,
  // kept apart on purpose: those return std::string, which allocates and throws, and this file is built -fno-exceptions
  // and linked alone into the dlopen handler's host tests (plugin_platform.cpp pulls in the whole loader). The buffer
  // form here also refuses an APK-mapped library, which the string form would hand back as a directory that is not on disk.
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&Register), &info) == 0 || info.dli_fname == nullptr) {
    return Refuse(NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR, who, "dladdr_failed");
  }
  return RegisterFromLibraryPath(pluginId, info.dli_fname, hashes, count);
}

void LogState() noexcept {
  const std::uint32_t count = g_count.load(std::memory_order_acquire);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "script_redirect",
                      {{"op", "state"}, {"site", "dlopen"}, {"registry", count},
                       {"status", count != 0 ? "armed" : "pass-through"}});
}

std::uint32_t ArmedCount() noexcept { return g_count.load(std::memory_order_acquire); }

const char* ResultName(NevrScriptRedirectResult result) noexcept {
  switch (result) {
    case NEVR_SCRIPT_REDIRECT_OK: return "ok";
    case NEVR_SCRIPT_REDIRECT_BAD_ARGUMENT: return "bad_argument";
    case NEVR_SCRIPT_REDIRECT_ALREADY_ARMED: return "already_armed";
    case NEVR_SCRIPT_REDIRECT_MISSING_LIBRARY: return "missing_library";
    case NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR: return "no_library_dir";
  }
  return "unknown";
}

void SetLogEveryLoad(bool on) noexcept { g_logEveryLoad.store(on, std::memory_order_relaxed); }

bool LogsEveryLoad() noexcept { return g_logEveryLoad.load(std::memory_order_relaxed); }

void SetBeforeClaimHookForTest(void (*hook)() noexcept) noexcept { g_beforeClaimHook.store(hook, std::memory_order_release); }

void ResetForTest() noexcept {
  g_count.store(0, std::memory_order_release);
  g_busy.store(false);
  g_logEveryLoad.store(false);
  g_beforeClaimHook.store(nullptr);
}

}  // namespace nevr_quest::script_redirect

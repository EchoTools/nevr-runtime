// Built with -fno-exceptions (dlopen_hook.cpp, which calls RewriteLoadName from its handler, is built that way).
#include "quest/plugin/script_redirect.h"

#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "hook_log.h"

namespace nevr_quest::script_redirect {

namespace {

constexpr std::size_t kHashDigits = 16;
constexpr std::size_t kMissingCapacity = kMaxEntries * (kNameCapacity + 1);

std::atomic<std::uint32_t> g_count{0};  // names in force; written once, release-stored after g_hashes
std::atomic<bool> g_busy{false};        // a registration is between its checks and its store
std::uint64_t g_hashes[kMaxEntries] = {};

// The last '/'-separated component of `name`.
const char* FinalComponent(const char* name) noexcept {
  const char* const slash = std::strrchr(name, '/');
  return slash != nullptr ? slash + 1 : name;
}

NevrScriptRedirectResult Refuse(NevrScriptRedirectResult result, const char* who, const char* reason) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kError, "script_redirect",
                      {{"op", "register"}, {"result", ResultName(result)}, {"plugin", who}, {"registry", 0},
                       {"reason", reason}});
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
  const std::uint32_t count = g_count.load(std::memory_order_acquire);
  if (name == nullptr || count == 0) return name;
  std::uint64_t hash = 0;
  if (!ParseScriptName(name, &hash)) return name;
  bool registered = false;
  for (std::uint32_t i = 0; i < count && !registered; ++i) registered = g_hashes[i] == hash;
  if (!registered || !RedirectName(hash, buf, cap)) return name;
  char hex[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "script_redirected",
                      {{"from", name}, {"to", buf}, {"hash", sentinel::HexString(hex, hash)}});
  return buf;
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
  bool expected = false;
  if (g_count.load(std::memory_order_acquire) != 0 || !g_busy.compare_exchange_strong(expected, true)) {
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
    if (::access(path, F_OK) == 0) continue;
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

NevrScriptRedirectResult Register(const char* pluginId, const std::uint64_t* hashes, std::uint32_t count) noexcept {
  const char* const who = pluginId != nullptr ? pluginId : "(null)";
  Dl_info info{};
  if (dladdr(reinterpret_cast<const void*>(&Register), &info) == 0 || info.dli_fname == nullptr) {
    return Refuse(NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR, who, "dladdr_failed");
  }
  char dir[512];
  const std::size_t length = std::strlen(info.dli_fname);
  const char* const slash = std::strrchr(info.dli_fname, '/');
  if (slash == nullptr || static_cast<std::size_t>(slash - info.dli_fname) + 1 > sizeof dir || length == 0) {
    return Refuse(NEVR_SCRIPT_REDIRECT_NO_LIBRARY_DIR, who, "library_path_has_no_directory");
  }
  const std::size_t dirLength = static_cast<std::size_t>(slash - info.dli_fname);
  std::memcpy(dir, info.dli_fname, dirLength);
  dir[dirLength] = '\0';
  return RegisterIn(dir, pluginId, hashes, count);
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

void ResetForTest() noexcept {
  g_count.store(0, std::memory_order_release);
  g_busy.store(false);
}

}  // namespace nevr_quest::script_redirect

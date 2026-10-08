#include "quest/login/login_hook.h"

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <string>

namespace QuestLogin {

namespace {

// Pinned artifact: store APK v4987566 (docs/adr/0003 "Pinned artifact"). The hook is installed
// only when both libraries carry these GNU build ids.
constexpr const char* kPnsovr = "libpnsovr.so";
constexpr const char* kPnsovrBuildId = "ca47bb8d03e6f43c1825133bbb9c15f174705c51";
constexpr const char* kR15 = "libr15.so";
constexpr const char* kR15BuildId = "b243509c08ce677aeb95fa348016949b3fc45230";

constexpr const char* kHookedSymbol = "_ZN10NRadEngine7CNSUser16SendLogInRequestERNS_5CJsonE";

// CNSUser layout, measured in both libr15.so and libpnsovr.so: CNSUser::AccountID() const reads
// [this+0x88]; CNSUser::SendLogInRequest copies [this+0x90] into the SNSUserID platform word.
// These are the same offsets the PCVR bridge patches (ws_bridge.cpp, user+0x88 / user+0x90).
constexpr std::size_t kAccountIdOffset = 0x88;
constexpr std::size_t kPlatformWordOffset = 0x90;

using SetStringFn = void (*)(void*, const char*, const char*);
using SetIntFn = void (*)(void*, const char*, long long);
using SetBooleanFn = void (*)(void*, const char*, unsigned);
using TStringFn = const char* (*)(const void*, const char*, const char*, unsigned);
using IntFn = long long (*)(const void*, const char*, long long, unsigned);
using BooleanFn = unsigned (*)(const void*, const char*, unsigned, unsigned);
using SendLogInRequestFn = void (*)(void*, void*);

struct CJsonApi {
  SetStringFn set_string = nullptr;
  SetIntFn set_int = nullptr;
  SetBooleanFn set_boolean = nullptr;
  TStringFn t_string = nullptr;
  IntFn get_int = nullptr;
  BooleanFn get_boolean = nullptr;
};

struct HookState {
  IdentitySource* source = nullptr;
  BuildInfo build;
  LogFn log = nullptr;
  CJsonApi api;
};

std::atomic<bool> g_installed{false};
HookState g_state;
// Written by the hook backend (originalOut) at install; read by Hook(). A void* because the
// backend's contract is `void**`; converted to the call signature only at the call.
void* g_original = nullptr;

void Log(LogFn log, Level level, const char* line) {
  if (log != nullptr) log(level, line);
}

struct BuildIdProbe {
  const char* name;
  std::string build_id;
  bool found = false;
};

std::string Hex(const std::uint8_t* bytes, std::size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(digits[bytes[i] >> 4]);
    out.push_back(digits[bytes[i] & 0xf]);
  }
  return out;
}

int PhdrCallback(dl_phdr_info* info, std::size_t, void* data) {
  auto* probe = static_cast<BuildIdProbe*>(data);
  if (info->dlpi_name == nullptr) return 0;
  const char* slash = std::strrchr(info->dlpi_name, '/');
  const char* base = slash != nullptr ? slash + 1 : info->dlpi_name;
  if (std::strcmp(base, probe->name) != 0) return 0;
  probe->found = true;
  for (int i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr)& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_NOTE) continue;
    const std::uint8_t* cursor = reinterpret_cast<const std::uint8_t*>(info->dlpi_addr + ph.p_vaddr);
    const std::uint8_t* end = cursor + ph.p_memsz;
    while (cursor + sizeof(ElfW(Nhdr)) <= end) {
      ElfW(Nhdr) note;
      std::memcpy(&note, cursor, sizeof(note));
      const std::uint8_t* name = cursor + sizeof(note);
      const std::size_t name_size = (note.n_namesz + 3u) & ~3u;
      const std::uint8_t* desc = name + name_size;
      const std::size_t desc_size = (note.n_descsz + 3u) & ~3u;
      if (desc + desc_size > end) break;
      if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
          std::memcmp(name, "GNU", 4) == 0) {
        probe->build_id = Hex(desc, note.n_descsz);
        return 1;
      }
      cursor = desc + desc_size;
    }
  }
  return 1;
}

BuildIdProbe ProbeBuildId(const char* soName) {
  BuildIdProbe probe;
  probe.name = soName;
  dl_iterate_phdr(&PhdrCallback, &probe);
  return probe;
}

// CNSUser identity words, guarded: the object must be a CNSOVRUser, whose vtable lives in
// libpnsovr.so. A pointer that fails the check is never written through.
class LiveUser final : public UserAccess {
 public:
  explicit LiveUser(void* user) : user_(user) {}

  bool Valid() const {
    if (user_ == nullptr || (reinterpret_cast<std::uintptr_t>(user_) & 7u) != 0) return false;
    void* vtable = nullptr;
    std::memcpy(&vtable, user_, sizeof(vtable));
    Dl_info info;
    if (vtable == nullptr || dladdr(vtable, &info) == 0 || info.dli_fname == nullptr) return false;
    const char* slash = std::strrchr(info.dli_fname, '/');
    return std::strcmp(slash != nullptr ? slash + 1 : info.dli_fname, kPnsovr) == 0;
  }

  bool Read(UserIdWords& out) const override {
    if (!Valid()) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(user_);
    std::memcpy(&out.account_id, bytes + kAccountIdOffset, sizeof(out.account_id));
    std::memcpy(&out.platform_word, bytes + kPlatformWordOffset, sizeof(out.platform_word));
    return true;
  }

  bool Write(const UserIdWords& words) override {
    if (!Valid()) return false;
    auto* bytes = static_cast<std::uint8_t*>(user_);
    std::memcpy(bytes + kAccountIdOffset, &words.account_id, sizeof(words.account_id));
    std::memcpy(bytes + kPlatformWordOffset, &words.platform_word, sizeof(words.platform_word));
    return true;
  }

 private:
  void* user_;
};

// NRadEngine::CJson through its exported members. A missing key returns the caller's fallback
// unchanged (docs/adr/0003 "Config-string seam"), so presence is "the answer differs from at
// least one of two different fallbacks".
class LiveJson final : public JsonAccess {
 public:
  LiveJson(const CJsonApi& api, void* json) : api_(api), json_(json) {}

  void SetString(const char* path, const char* value) override { api_.set_string(json_, path, value); }
  void SetInt(const char* path, std::int64_t value) override {
    api_.set_int(json_, path, static_cast<long long>(value));
  }
  void SetBoolean(const char* path, bool value) override { api_.set_boolean(json_, path, value ? 1u : 0u); }

  std::string GetString(const char* path, bool& present) const override {
    static const char kFallback[] = "";
    const char* got = api_.t_string(json_, path, kFallback, 0);
    present = got != nullptr && got != kFallback;
    return present ? std::string(got) : std::string();
  }

  std::int64_t GetInt(const char* path, bool& present) const override {
    const long long lo = api_.get_int(json_, path, std::numeric_limits<long long>::min(), 0);
    const long long hi = api_.get_int(json_, path, std::numeric_limits<long long>::max(), 0);
    present = lo == hi;
    return present ? static_cast<std::int64_t>(lo) : 0;
  }

  bool GetBoolean(const char* path, bool& present) const override {
    const unsigned lo = api_.get_boolean(json_, path, 0u, 0);
    const unsigned hi = api_.get_boolean(json_, path, 1u, 0);
    present = (lo != 0) == (hi != 0);
    return present && lo != 0;
  }

 private:
  const CJsonApi& api_;
  void* json_;
};

// The replacement for the slot. Always ends in the original call: a refused rewrite leaves the
// game's own login intact (docs/adr/0003 contract 4), and nothing thrown crosses this boundary.
void Hook(void* user, void* json) {
  HookState& state = g_state;
  try {
    if (state.source != nullptr && json != nullptr) {
      LiveUser live_user(user);
      LiveJson live_json(state.api, json);
      RewriteLogin(live_user, live_json, *state.source, state.build, state.log);
    }
  } catch (const std::exception&) {
    Log(state.log, Level::Error, "quest.login outcome=exception: rewrite threw, original login left as is");
  }
  reinterpret_cast<SendLogInRequestFn>(g_original)(user, json);
}

template <typename Fn>
bool Resolve(void* handle, const char* symbol, Fn& out) {
  void* address = dlsym(handle, symbol);
  if (address == nullptr) return false;
  out = reinterpret_cast<Fn>(address);
  return true;
}

}  // namespace

const char* InstallStateName(InstallState state) {
  switch (state) {
    case InstallState::Installed: return "installed";
    case InstallState::AlreadyInstalled: return "already-installed";
    case InstallState::ModuleNotLoaded: return "module-not-loaded";
    case InstallState::BuildMismatch: return "build-mismatch";
    case InstallState::SymbolMissing: return "symbol-missing";
    default: return "hook-failed";
  }
}

void AndroidLog(Level level, const char* line) {
  const int priority = level == Level::Error     ? ANDROID_LOG_ERROR
                       : level == Level::Warning ? ANDROID_LOG_WARN
                                                 : ANDROID_LOG_INFO;
  __android_log_write(priority, "NEVR-Login", line);
}

InstallState TryInstallLoginHook(IdentitySource* source, const BuildInfo& build,
                                 ImportHookFn hookImport, LogFn log) {
  if (g_installed.load(std::memory_order_acquire)) return InstallState::AlreadyInstalled;
  if (source == nullptr || hookImport == nullptr) return InstallState::HookFailed;

  char line[256];
  const BuildIdProbe pnsovr = ProbeBuildId(kPnsovr);
  const BuildIdProbe r15 = ProbeBuildId(kR15);
  if (!pnsovr.found || !r15.found) return InstallState::ModuleNotLoaded;
  if (pnsovr.build_id != kPnsovrBuildId || r15.build_id != kR15BuildId) {
    std::snprintf(line, sizeof(line),
                  "quest.login install=%s: pinned build ids differ (pnsovr_match=%d r15_match=%d), no hook installed",
                  InstallStateName(InstallState::BuildMismatch), pnsovr.build_id == kPnsovrBuildId ? 1 : 0,
                  r15.build_id == kR15BuildId ? 1 : 0);
    Log(log, Level::Error, line);
    return InstallState::BuildMismatch;
  }

  // CJson lives in libr15.so; the rewrite calls its exports directly.
  void* r15_handle = dlopen(kR15, RTLD_NOW | RTLD_NOLOAD);
  CJsonApi api;
  const bool resolved =
      r15_handle != nullptr &&
      Resolve(r15_handle, "_ZN10NRadEngine5CJson9SetStringEPKcS2_", api.set_string) &&
      Resolve(r15_handle, "_ZN10NRadEngine5CJson6SetIntEPKcx", api.set_int) &&
      Resolve(r15_handle, "_ZN10NRadEngine5CJson10SetBooleanEPKcj", api.set_boolean) &&
      Resolve(r15_handle, "_ZNK10NRadEngine5CJson7TStringEPKcS2_j", api.t_string) &&
      Resolve(r15_handle, "_ZNK10NRadEngine5CJson3IntEPKcxj", api.get_int) &&
      Resolve(r15_handle, "_ZNK10NRadEngine5CJson7BooleanEPKcjj", api.get_boolean);
  if (!resolved) {
    Log(log, Level::Error, "quest.login install=symbol-missing: a CJson export is absent, no hook installed");
    return InstallState::SymbolMissing;
  }

  g_state.source = source;
  g_state.build = build;
  g_state.log = log;
  g_state.api = api;
  if (!hookImport(kPnsovr, kHookedSymbol, reinterpret_cast<void*>(&Hook), &g_original) ||
      g_original == nullptr) {
    Log(log, Level::Error, "quest.login install=hook-failed: backend refused the libpnsovr SendLogInRequest slot");
    return InstallState::HookFailed;
  }
  g_installed.store(true, std::memory_order_release);
  Log(log, Level::Info, "quest.login install=installed module=libpnsovr.so slot=CNSUser::SendLogInRequest");
  return InstallState::Installed;
}

}  // namespace QuestLogin

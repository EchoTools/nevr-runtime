#include "quest/login/login_hook.h"

#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_log.h"

namespace QuestLogin {

namespace {

// Pinned artifact: store APK v4987566 (docs/adr/0003 "Pinned artifact"). Nothing is touched
// unless libpnsovr.so carries this GNU build id.
constexpr const char* kPnsovr = "libpnsovr.so";
constexpr const char* kPnsovrBuildId = "ca47bb8d03e6f43c1825133bbb9c15f174705c51";
constexpr const char* kHookedSymbol = "_ZN10NRadEngine7CNSUser16SendLogInRequestERNS_5CJsonE";
constexpr std::uint64_t kHookedSlotVaddr = 0x6dd1b8ULL;

// CNSOVRUser::AccountID() const @0x1ede14: adrp x8,0x70e000 / ldr x0,[x8,#0x3e0] / ret.
constexpr std::uint64_t kAccountIdFnVaddr = 0x1ede14ULL;
constexpr std::uint32_t kAccountIdFnCode[3] = {0xb0002908u, 0xf941f100u, 0xd65f03c0u};
constexpr std::uint64_t kAccountIdGlobalVaddr = 0x70e3e0ULL;

// CNSUser layout, measured in libr15.so and libpnsovr.so: SendLogInRequest copies
// [this+0x90] into the SNSUserID platform word; the account id comes from vtable+0x70.
constexpr std::size_t kPlatformWordOffset = 0x90;
constexpr std::size_t kAccountIdVtableOffset = 0x70;

using SetStringFn = void (*)(void*, const char*, const char*);
using SetIntFn = void (*)(void*, const char*, long long);
using SetBooleanFn = void (*)(void*, const char*, unsigned);
using ClearFn = void (*)(void*, const char*, unsigned);
using TStringFn = const char* (*)(const void*, const char*, const char*, unsigned);
using IntFn = long long (*)(const void*, const char*, long long, unsigned);
using BooleanFn = unsigned (*)(const void*, const char*, unsigned, unsigned);
using IsObjectFn = unsigned (*)(const void*, const char*);

// libpnsovr.so does not link libr15.so (no DT_NEEDED) and defines its own CJson, so the
// object this hook edits was built by, and must be edited with, libpnsovr's own functions.
struct CJsonApi {
  SetStringFn set_string = nullptr;
  SetIntFn set_int = nullptr;
  SetBooleanFn set_boolean = nullptr;
  ClearFn clear = nullptr;
  TStringFn t_string = nullptr;
  IntFn get_int = nullptr;
  BooleanFn get_boolean = nullptr;
  IsObjectFn is_object = nullptr;
};

struct State {
  IdentitySource* source = nullptr;
  BuildInfo build;
  LogFn log = nullptr;
  CJsonApi api;
  std::uint64_t* account_id_global = nullptr;
};

State g_state;
std::atomic<const State*> g_published{nullptr};
std::mutex g_install_mutex;
sentinel::GotHook g_hook;

struct LoginTag {};
using LoginThunk = sentinel::CallbackThunk<LoginTag, void(void*, void*)>;

// The CNSOVRUser the hook was handed. Every access is guarded: the object must have its
// vtable inside libpnsovr.so before anything is read or called through it.
class LiveUser final : public UserAccess {
 public:
  LiveUser(void* user, std::uint64_t* account_global) : user_(user), global_(account_global) {}

  bool Provider(std::uint64_t& code) const override {
    if (!Valid()) return false;
    std::memcpy(&code, static_cast<const std::uint8_t*>(user_) + kPlatformWordOffset, sizeof(code));
    return true;
  }

  bool WireAccountId(std::uint64_t& id) const override {
    if (!Valid()) return false;
    const auto* vtable = *static_cast<void* const* const*>(user_);
    using AccountIdFn = std::uint64_t (*)(const void*);
    const AccountIdFn fn = reinterpret_cast<AccountIdFn>(vtable[kAccountIdVtableOffset / sizeof(void*)]);
    id = fn(user_);
    return true;
  }

  bool SetAccountId(std::uint64_t id) override {
    if (!Valid() || global_ == nullptr) return false;
    previous_ = __atomic_load_n(global_, __ATOMIC_ACQUIRE);
    have_previous_ = true;
    __atomic_store_n(global_, id, __ATOMIC_RELEASE);
    return true;
  }

  void RestoreAccountId() override {
    if (have_previous_ && global_ != nullptr) __atomic_store_n(global_, previous_, __ATOMIC_RELEASE);
    have_previous_ = false;
  }

 private:
  bool Valid() const {
    if (user_ == nullptr || (reinterpret_cast<std::uintptr_t>(user_) & 7u) != 0) return false;
    void* vtable = nullptr;
    std::memcpy(&vtable, user_, sizeof(vtable));
    Dl_info info;
    if (vtable == nullptr || dladdr(vtable, &info) == 0 || info.dli_fname == nullptr) return false;
    const char* slash = std::strrchr(info.dli_fname, '/');
    return std::strcmp(slash != nullptr ? slash + 1 : info.dli_fname, kPnsovr) == 0;
  }

  void* user_;
  std::uint64_t* global_;
  std::uint64_t previous_ = 0;
  bool have_previous_ = false;
};

// NRadEngine::CJson through libpnsovr's exported members. A missing key returns the caller's
// fallback unchanged (docs/adr/0003 "Config-string seam"), so presence is "the answer differs
// from at least one of two different fallbacks".
class LiveJson final : public JsonAccess {
 public:
  LiveJson(const CJsonApi& api, void* json) : api_(api), json_(json) {}

  void SetString(const char* path, const char* value) override { api_.set_string(json_, path, value); }
  void SetInt(const char* path, std::int64_t value) override {
    api_.set_int(json_, path, static_cast<long long>(value));
  }
  void SetBoolean(const char* path, bool value) override { api_.set_boolean(json_, path, value ? 1u : 0u); }
  void Clear(const char* path) override { api_.clear(json_, path, 0u); }

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

  bool IsObject(const char* path) const override { return api_.is_object(json_, path) != 0; }

 private:
  const CJsonApi& api_;
  void* json_;
};

// The handler behind the GOT slot. The thunk calls it with the original function; the
// original is called last and always, so a refused or failed rewrite leaves the game's own
// login intact. RewriteLogin never throws.
void HandleSendLogInRequest(LoginThunk::Fn original, void* user, void* json) {
  const State* state = g_published.load(std::memory_order_acquire);
  if (state != nullptr && json != nullptr) {
    LiveUser live_user(user, state->account_id_global);
    LiveJson live_json(state->api, json);
    RewriteLogin(live_user, live_json, *state->source, state->build, state->log);
  }
  original(user, json);
}

template <typename Fn>
bool Resolve(void* handle, const char* symbol, Fn& out) {
  void* address = dlsym(handle, symbol);
  if (address == nullptr) return false;
  out = reinterpret_cast<Fn>(address);
  return true;
}

bool ResolveCJson(CJsonApi& api) {
  void* handle = dlopen(kPnsovr, RTLD_NOW | RTLD_NOLOAD);
  return handle != nullptr &&
         Resolve(handle, "_ZN10NRadEngine5CJson9SetStringEPKcS2_", api.set_string) &&
         Resolve(handle, "_ZN10NRadEngine5CJson6SetIntEPKcx", api.set_int) &&
         Resolve(handle, "_ZN10NRadEngine5CJson10SetBooleanEPKcj", api.set_boolean) &&
         Resolve(handle, "_ZN10NRadEngine5CJson5ClearEPKcj", api.clear) &&
         Resolve(handle, "_ZNK10NRadEngine5CJson7TStringEPKcS2_j", api.t_string) &&
         Resolve(handle, "_ZNK10NRadEngine5CJson3IntEPKcxj", api.get_int) &&
         Resolve(handle, "_ZNK10NRadEngine5CJson7BooleanEPKcjj", api.get_boolean) &&
         Resolve(handle, "_ZNK10NRadEngine5CJson8IsObjectEPKc", api.is_object);
}

// Proves the account-id global: the three instructions of CNSOVRUser::AccountID() are the
// pinned ones (so base+0x70e3e0 is the address that function loads), and the address lies in
// a writable PT_LOAD of the image. Returns nullptr unless both hold.
std::uint64_t* ProveAccountIdGlobal(const sentinel::ElfImage& image) {
  std::uint32_t code[3];
  std::memcpy(code, reinterpret_cast<const void*>(image.base + kAccountIdFnVaddr), sizeof(code));
  if (std::memcmp(code, kAccountIdFnCode, sizeof(code)) != 0) return nullptr;
  for (std::size_t i = 0; i < image.phnum; ++i) {
    const Elf64_Phdr& ph = image.phdr[i];
    if (ph.p_type != PT_LOAD || (ph.p_flags & PF_W) == 0) continue;
    if (kAccountIdGlobalVaddr >= ph.p_vaddr &&
        kAccountIdGlobalVaddr + sizeof(std::uint64_t) <= ph.p_vaddr + ph.p_memsz) {
      return reinterpret_cast<std::uint64_t*>(image.base + kAccountIdGlobalVaddr);
    }
  }
  return nullptr;
}

}  // namespace

const char* InstallStateName(InstallState state) {
  switch (state) {
    case InstallState::Installed: return "installed";
    case InstallState::AlreadyInstalled: return "already-installed";
    case InstallState::ModuleNotLoaded: return "module-not-loaded";
    case InstallState::BuildMismatch: return "build-mismatch";
    case InstallState::SlotInvalid: return "slot-invalid";
    case InstallState::SymbolMissing: return "symbol-missing";
    default: return "hook-failed";
  }
}

void SentinelLog(Level level, const char* line) {
  const sentinel::LogLevel mapped = level == Level::Error     ? sentinel::LogLevel::kError
                                    : level == Level::Warning ? sentinel::LogLevel::kWarn
                                                              : sentinel::LogLevel::kInfo;
  sentinel::LogEvent(mapped, "%s", line);
}

InstallState TryInstallLoginHook(IdentitySource* source, const BuildInfo& build, LogFn log) {
  if (log == nullptr) log = &SentinelLog;
  const std::lock_guard<std::mutex> lock(g_install_mutex);
  if (g_published.load(std::memory_order_acquire) != nullptr) return InstallState::AlreadyInstalled;
  if (source == nullptr) return InstallState::HookFailed;

  auto refuse = [&](InstallState state, const char* why) {
    char line[200];
    std::snprintf(line, sizeof(line), "event=quest_login_install state=%s reason=%s",
                  InstallStateName(state), why);
    log(Level::Error, line);
    return state;
  };

  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(kPnsovr, &image)) return InstallState::ModuleNotLoaded;
  char build_id[64] = {};
  if (!sentinel::ReadBuildId(image, build_id, sizeof(build_id)) ||
      std::strcmp(build_id, kPnsovrBuildId) != 0) {
    return refuse(InstallState::BuildMismatch, "pinned_build_id");
  }
  std::uint64_t* account_global = ProveAccountIdGlobal(image);
  if (account_global == nullptr) return refuse(InstallState::SlotInvalid, "account_id_global");
  CJsonApi api;
  if (!ResolveCJson(api)) return refuse(InstallState::SymbolMissing, "pnsovr_cjson_export");

  g_state.source = source;
  g_state.build = build;
  g_state.log = log;
  g_state.api = api;
  g_state.account_id_global = account_global;
  g_published.store(&g_state, std::memory_order_release);
  LoginThunk::Arm(&HandleSendLogInRequest);

  const sentinel::GotTarget target(kPnsovr, kHookedSymbol, sentinel::RelocKind::kJumpSlot,
                                   kPnsovrBuildId, kHookedSlotVaddr);
  if (g_hook.Install(target, LoginThunk::EntryAddress(), LoginThunk::OriginalOut()) !=
      sentinel::GotStatus::kOk) {
    LoginThunk::Arm(nullptr);
    g_published.store(nullptr, std::memory_order_release);
    return refuse(InstallState::HookFailed, "got_backend");
  }
  log(Level::Info, "event=quest_login_install state=installed slot=CNSUser::SendLogInRequest");
  return InstallState::Installed;
}

}  // namespace QuestLogin

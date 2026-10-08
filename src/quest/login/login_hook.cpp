#include "quest/login/login_hook.h"

#include <dlfcn.h>
#include <elf.h>

#include <atomic>
#include <cstdint>
#include <cstring>
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
constexpr std::uint64_t kOwnSendLogInRequestVaddr = 0x382a4cULL;

// CNSOVRUser::AccountID() const @0x1ede14: adrp x8,0x70e000 / ldr x0,[x8,#0x3e0] / ret.
constexpr std::uint64_t kAccountIdFnVaddr = 0x1ede14ULL;
constexpr std::uint32_t kAccountIdFnCode[3] = {0xb0002908u, 0xf941f100u, 0xd65f03c0u};
constexpr std::uint64_t kAccountIdGlobalVaddr = 0x70e3e0ULL;

// CNSUser layout, measured in libr15.so and libpnsovr.so: SendLogInRequest copies
// [this+0x90] into the SNSUserID platform word; the account id comes from vtable+0x70.
constexpr std::size_t kPlatformWordOffset = 0x90;
constexpr std::size_t kAccountIdVtableOffset = 0x70;

// CNSOVRUser's vtable pointer as stored in an instance: _ZTVN10NRadEngine10CNSOVRUserE (0x6a1280)
// plus the 0x10 header.
constexpr std::uint64_t kCNSOVRUserVptrVaddr = 0x6a1290ULL;

using SetStringFn = void (*)(void*, const char*, const char*);
using SetIntFn = void (*)(void*, const char*, long long);
using SetBooleanFn = void (*)(void*, const char*, unsigned);
using SetNullFn = void (*)(void*, const char*);
using ClearFn = void (*)(void*, const char*, unsigned);
using TStringFn = const char* (*)(const void*, const char*, const char*, unsigned);
using IntFn = long long (*)(const void*, const char*, long long, unsigned);
using BooleanFn = unsigned (*)(const void*, const char*, unsigned, unsigned);
using ValidFn = unsigned (*)(const void*, const char*);
using TypeOfFn = unsigned (*)(const void*, const char*);

// libpnsovr.so does not link libr15.so (no DT_NEEDED) and defines its own CJson; the login
// CJson is built by libpnsovr.so, so it is edited with libpnsovr's functions.
struct CJsonApi {
  SetStringFn set_string = nullptr;
  SetIntFn set_int = nullptr;
  SetBooleanFn set_boolean = nullptr;
  SetNullFn set_null = nullptr;
  ClearFn clear = nullptr;
  TStringFn t_string = nullptr;
  IntFn get_int = nullptr;
  BooleanFn get_boolean = nullptr;
  ValidFn valid = nullptr;
  TypeOfFn type_of = nullptr;
};

// Every CJson function the rewrite calls: its pinned link-time address in libpnsovr.so and,
// where libpnsovr's own calls go through a PLT slot, that slot's address. At install the
// export must resolve to base+function and the slot (BIND_NOW, so already resolved) must
// hold the same value; otherwise the game's own calls use a different copy and nothing is
// installed.
struct CJsonImport {
  const char* symbol;
  std::uint64_t function_vaddr;
  std::uint64_t slot_vaddr;  // 0: libpnsovr never calls it through the PLT
};
constexpr CJsonImport kCJsonImports[] = {
    {"_ZN10NRadEngine5CJson9SetStringEPKcS2_", 0x35917c, 0x6da310},
    {"_ZN10NRadEngine5CJson6SetIntEPKcx", 0x35bc14, 0x6d9038},
    {"_ZN10NRadEngine5CJson10SetBooleanEPKcj", 0x358ccc, 0x6db440},
    {"_ZN10NRadEngine5CJson7SetNullEPKc", 0x35c158, 0x6de7f8},
    {"_ZN10NRadEngine5CJson5ClearEPKcj", 0x358098, 0x6e1868},
    {"_ZNK10NRadEngine5CJson7TStringEPKcS2_j", 0x358bb4, 0x6db090},
    {"_ZNK10NRadEngine5CJson3IntEPKcxj", 0x359e24, 0x6da498},
    {"_ZNK10NRadEngine5CJson7BooleanEPKcjj", 0x35a0a8, 0x6df500},
    {"_ZNK10NRadEngine5CJson5ValidEPKc", 0x3595ac, 0x6dffc0},
    {"_ZNK10NRadEngine5CJson6TypeOfEPKc", 0x35a1d0, 0},
};

struct State {
  IdentitySource* source = nullptr;
  BuildInfo build;
  LogFn log = nullptr;
  CJsonApi api;
  std::uint64_t* account_id_global = nullptr;
  const void* expected_vptr = nullptr;
};

State g_state;
std::atomic<const State*> g_published{nullptr};
std::mutex g_install_mutex;
sentinel::GotHook g_hook;
std::mutex g_account_mutex;
OculusIdMemory g_oculus_id;  // guarded by g_account_mutex

struct LoginTag {};
using LoginThunk = sentinel::CallbackThunk<LoginTag, void(void*, void*)>;

// The CNSOVRUser the hook was handed. Every access is guarded: the object's vtable pointer
// must be exactly CNSOVRUser's before anything is read or called through it.
//
// After a successful send the global keeps the NEVR id: CNSUser::LogInSuccessCB builds
// {[this+0x90], AccountID()} and compares it with the server's reply, and the other readers
// (docs/adr/0003, "Login interception") must see the same id. On any outcome other than
// Rewritten it goes back to the Oculus id, because LogInInternal re-reads the Oculus org id
// only when the global holds -1 (0x1ec96c-0x1ec984) and the Oculus login then goes out.
// OculusIdMemory keeps the Oculus value across logins in this process, puts it back only while
// the global still holds the value written here, and never remembers 0 or -1.
//
// Threading: g_account_mutex serializes this adapter's own accesses. The game's writers of the
// global (GotLoggedInUserOrgIdCb 0x1ecef0/0x1ecf18, RadPluginShutdown 0x207074) are not under
// it, and set -> verify -> JSON -> restore is not atomic against them. The login path is taken
// to run on one game thread with no concurrent writer while a login is in flight (the fetch
// is gated on -1 at 0x1ec980, so the game does not write while the NEVR id is installed).
// That is an assumption, not a measurement.
class LiveUser final : public UserAccess {
 public:
  LiveUser(void* user, std::uint64_t* account_global, const void* expected_vptr)
      : user_(user), global_(account_global), expected_vptr_(expected_vptr) {}

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
    const std::lock_guard<std::mutex> lock(g_account_mutex);
    if (!g_oculus_id.NoteBeforeWrite(__atomic_load_n(global_, __ATOMIC_ACQUIRE), id)) return false;
    __atomic_store_n(global_, id, __ATOMIC_RELEASE);
    return true;
  }

  bool RestoreAccountId() override {
    if (global_ == nullptr) return false;
    const std::lock_guard<std::mutex> lock(g_account_mutex);
    std::uint64_t oculus = 0;
    if (!g_oculus_id.RestoreFor(__atomic_load_n(global_, __ATOMIC_ACQUIRE), oculus)) return false;
    __atomic_store_n(global_, oculus, __ATOMIC_RELEASE);
    return true;
  }

 private:
  bool Valid() const {
    if (user_ == nullptr || expected_vptr_ == nullptr || (reinterpret_cast<std::uintptr_t>(user_) & 7u) != 0) {
      return false;
    }
    const void* vptr = nullptr;
    std::memcpy(&vptr, user_, sizeof(vptr));
    return vptr == expected_vptr_;
  }

  void* user_;
  std::uint64_t* global_;
  const void* expected_vptr_;
};

// NRadEngine::CJson through libpnsovr's functions. TypeOf reports 0 for both a null value and
// an absent path, so Valid() (does the path resolve) separates them.
class LiveJson final : public JsonAccess {
 public:
  LiveJson(const CJsonApi& api, void* json) : api_(api), json_(json) {}

  JsonType TypeOf(const char* path) const override {
    if (api_.valid(json_, path) == 0) return JsonType::Absent;
    switch (api_.type_of(json_, path)) {
      case 1: return JsonType::String;
      case 2: return JsonType::Int;
      case 3: return JsonType::Real;
      case 4: return JsonType::Boolean;
      case 5: return JsonType::Array;
      case 6: return JsonType::Object;
      default: return JsonType::Null;
    }
  }
  std::string GetString(const char* path) const override {
    const char* got = api_.t_string(json_, path, "", 0);
    return got != nullptr ? std::string(got) : std::string();
  }
  std::int64_t GetInt(const char* path) const override {
    return static_cast<std::int64_t>(api_.get_int(json_, path, 0, 0));
  }
  bool GetBoolean(const char* path) const override { return api_.get_boolean(json_, path, 0u, 0) != 0; }

  void SetString(const char* path, const char* value) override { api_.set_string(json_, path, value); }
  void SetInt(const char* path, std::int64_t value) override {
    api_.set_int(json_, path, static_cast<long long>(value));
  }
  void SetBoolean(const char* path, bool value) override { api_.set_boolean(json_, path, value ? 1u : 0u); }
  void SetNull(const char* path) override { api_.set_null(json_, path); }
  void Clear(const char* path) override { api_.clear(json_, path, 0u); }

 private:
  const CJsonApi& api_;
  void* json_;
};

// Runs the rewrite for one login. The compiler inlines it into the handler, so the adapters
// (which have destructors) live in the handler's frame, which also calls the game's original.
// Every frame live during a call into the game is built -fno-exceptions and sits under the
// personality-free "zR" CIE: this function, the handler, RewriteLogin and the rest of
// login_apply.cpp (tests/quest TestLoginHookObjectsCarryNoPersonality). The one
// exceptions-enabled function it reaches, ComposePlan, calls no game code and has returned
// before the next game call.
void RunRewrite(const State& state, void* user, void* json) noexcept {
  LiveUser live_user(user, state.account_id_global, state.expected_vptr);
  LiveJson live_json(state.api, json);
  RewriteLogin(live_user, live_json, *state.source, state.build, state.log);
}

// The handler behind the GOT slot: rewrite, then the original, last and always, so a refused
// or failed rewrite leaves the game's own login intact. It has no cleanup of its own.
NEVR_HOOK_HANDLER void HandleSendLogInRequest(LoginThunk::Fn original, void* user, void* json) noexcept {
  const State* state = g_published.load(std::memory_order_acquire);
  if (state != nullptr && json != nullptr) RunRewrite(*state, user, json);
  original(user, json);
}

template <typename Fn>
void Assign(Fn& out, std::uint64_t address) {
  out = reinterpret_cast<Fn>(address);
}

// Resolves the CJson functions for the pinned image: each export must equal base+function,
// and where libpnsovr calls it through a PLT slot the slot must hold that same address.
bool ResolveCJson(const sentinel::ElfImage& image, CJsonApi& api) {
  void* handle = dlopen(kPnsovr, RTLD_NOW | RTLD_NOLOAD);
  if (handle == nullptr) return false;
  std::uint64_t resolved[sizeof(kCJsonImports) / sizeof(kCJsonImports[0])] = {};
  std::size_t index = 0;
  for (const CJsonImport& entry : kCJsonImports) {
    const std::uint64_t want = image.base + entry.function_vaddr;
    if (reinterpret_cast<std::uint64_t>(dlsym(handle, entry.symbol)) != want) return false;
    if (entry.slot_vaddr != 0) {
      std::uint64_t slot_value = 0;
      std::memcpy(&slot_value, reinterpret_cast<const void*>(image.base + entry.slot_vaddr), sizeof(slot_value));
      if (slot_value != want) return false;
    }
    resolved[index++] = want;
  }
  Assign(api.set_string, resolved[0]);
  Assign(api.set_int, resolved[1]);
  Assign(api.set_boolean, resolved[2]);
  Assign(api.set_null, resolved[3]);
  Assign(api.clear, resolved[4]);
  Assign(api.t_string, resolved[5]);
  Assign(api.get_int, resolved[6]);
  Assign(api.get_boolean, resolved[7]);
  Assign(api.valid, resolved[8]);
  Assign(api.type_of, resolved[9]);
  return true;
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

void SentinelLog(Level level, const char* event, const LogKv* fields, std::size_t count) {
  const sentinel::LogLevel mapped = level == Level::Error     ? sentinel::LogLevel::kError
                                    : level == Level::Warning ? sentinel::LogLevel::kWarn
                                                              : sentinel::LogLevel::kInfo;
  using sentinel::LogField;
  auto field = [&](std::size_t i) {
    return fields[i].text != nullptr ? LogField(fields[i].key, fields[i].text)
                                     : LogField(fields[i].key, fields[i].number);
  };
  switch (count) {
    case 0: sentinel::LogFields(mapped, event, {}); break;
    case 1: sentinel::LogFields(mapped, event, {field(0)}); break;
    case 2: sentinel::LogFields(mapped, event, {field(0), field(1)}); break;
    case 3: sentinel::LogFields(mapped, event, {field(0), field(1), field(2)}); break;
    case 4: sentinel::LogFields(mapped, event, {field(0), field(1), field(2), field(3)}); break;
    case 5: sentinel::LogFields(mapped, event, {field(0), field(1), field(2), field(3), field(4)}); break;
    case 6:
      sentinel::LogFields(mapped, event, {field(0), field(1), field(2), field(3), field(4), field(5)});
      break;
    case 7:
      sentinel::LogFields(mapped, event,
                          {field(0), field(1), field(2), field(3), field(4), field(5), field(6)});
      break;
    default:
      sentinel::LogFields(mapped, event,
                          {field(0), field(1), field(2), field(3), field(4), field(5), field(6), field(7)});
      break;
  }
}

InstallState TryInstallLoginHook(IdentitySource* source, const BuildInfo& build, LogFn log) {
  if (log == nullptr) log = &SentinelLog;
  const std::lock_guard<std::mutex> lock(g_install_mutex);
  if (g_published.load(std::memory_order_acquire) != nullptr) return InstallState::AlreadyInstalled;

  auto refuse = [&](InstallState state, const char* reason) {
    const LogKv fields[] = {{"op", "install", 0}, {"state", InstallStateName(state), 0}, {"reason", reason, 0}};
    log(Level::Error, "quest_login_install", fields, 3);
    return state;
  };

  if (source == nullptr) return refuse(InstallState::HookFailed, "no_identity_source");
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
  if (!ResolveCJson(image, api)) return refuse(InstallState::SymbolMissing, "pnsovr_cjson_binding");

  g_state.source = source;
  g_state.build = build;
  g_state.log = log;
  g_state.api = api;
  g_state.account_id_global = account_global;
  g_state.expected_vptr = reinterpret_cast<const void*>(image.base + kCNSOVRUserVptrVaddr);
  g_published.store(&g_state, std::memory_order_release);
  LoginThunk::Arm(&HandleSendLogInRequest);

  // The slot must hold libpnsovr's own CNSUser::SendLogInRequest (0x382a4c); libr15 exports
  // the same symbol at 0x1932838, and a slot bound there is refused instead of hooked.
  const sentinel::GotTarget target(kPnsovr, kHookedSymbol, sentinel::RelocKind::kJumpSlot,
                                   kPnsovrBuildId, kHookedSlotVaddr,
                                   reinterpret_cast<const void*>(image.base + kOwnSendLogInRequestVaddr));
  if (g_hook.Install(target, LoginThunk::EntryAddress(), LoginThunk::OriginalOut()) !=
      sentinel::GotStatus::kOk) {
    LoginThunk::Arm(nullptr);
    g_published.store(nullptr, std::memory_order_release);
    return refuse(InstallState::HookFailed, "got_backend");
  }
  const LogKv fields[] = {{"op", "install", 0}, {"state", "installed", 0}, {"slot", "CNSUser::SendLogInRequest", 0}};
  log(Level::Info, "quest_login_install", fields, 3);
  return InstallState::Installed;
}

}  // namespace QuestLogin

#include "quest/login/login_hook.h"

#include <dlfcn.h>
#include <elf.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#include "quest/login/login_prerequisites.h"
#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_install.h"
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

// CNSUser::DeferredLogInFailed(ENSResponseCode, char const*) @0x382e44: `str w1,[x0,#0xa0];
// str x2,[x0,#0xa8]; ret`. The game's own deferred failure: CNSIUsers::Update (0x36bf68) and
// CNSUser::Update (0x36c02c) see [user+0xa0] != 200 on the next update, call LogInFailed through
// vtable+0x10 with a fresh empty CJson and reset it to 200. CNSUser::SendLogInRequest uses the same
// idiom for a dead connection (0x382bf4). Used when the send gate refuses a login: nothing is sent
// and the failure runs on the next update, outside the OVR callback (no re-entrancy). The message
// pointer must outlive that update (kPrerequisitesMissingText is a string literal).
constexpr const char* kDeferredFailedSymbol = "_ZN10NRadEngine7CNSUser19DeferredLogInFailedENS_15ENSResponseCodeEPKc";
constexpr std::uint64_t kDeferredFailedVaddr = 0x382e44ULL;
constexpr std::uint32_t kDeferredFailedCode[3] = {0xb900a001u, 0xf9005402u, 0xd65f03c0u};

// The user-name buffer: SCallbacks::GotLoggedInUserCb @0x1ed0fc/0x1ed100 `adrp x8,0x70e000;
// add x8,x8,#0x470` is the address it copies the Oculus id into (36 bytes, 0x1ed118-0x1ed17c).
constexpr std::uint64_t kUserNameCodeVaddr = 0x1ed0fcULL;
constexpr std::uint32_t kUserNameCode[2] = {0xb0002908u, 0x9111c108u};
constexpr std::uint64_t kUserNameVaddr = 0x70e470ULL;
constexpr std::size_t kUserNameBytes = 0x24;

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

using DeferredFailedFn = void (*)(void* user, int code, const char* msg);

struct State {
  IdentitySource* source = nullptr;
  BuildInfo build;
  LogFn log = nullptr;
  CJsonApi api;
  std::uint64_t* account_id_global = nullptr;
  const void* expected_vptr = nullptr;
  DeferredFailedFn deferred_failed = nullptr;  // nullptr when the symbol or its code did not prove
  char* user_name = nullptr;                   // the 36-byte buffer 0x70e470, nullptr when unproven
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
// to run on one game thread with no concurrent writer while a login is in flight; that is an
// assumption, not a measurement. The game's writers are 0x1ec998 (LogInInternal, 0),
// 0x1ecef0 (-1), 0x1ecf18 (the org id) and 0x207074 (0), and an org-id fetch can also start at
// plugin init (0x2069bc) or from the error callback (0x1ecf80), not only at the -1 gate
// (0x1ec980). A send needs the global to be neither 0 nor -1 (0 defers, -1 refetches and
// defers, a pending login seen with -1 fails with 500), so the refusal to remember 0 or -1 can
// only be hit by a writer racing between the check and the send and never blocks a login the
// game would send (inference from the code, not run). See docs/adr/0003.
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
    const std::uint64_t current = __atomic_load_n(global_, __ATOMIC_ACQUIRE);
    if (!g_oculus_id.NoteBeforeWrite(current, id, StandIn::IsOrgId(current))) return false;
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

 // True when the object is a CNSOVRUser (its vtable pointer is exactly CNSOVRUser's), so a call that
  // treats it as a CNSUser is sound.
  bool IsCNSOVRUser() const { return Valid(); }

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

// The game's prerequisite state over the proven globals: the org-id global 0x70e3e0 and the
// user-name buffer 0x70e470. Written only from the login send hook, which runs inside
// GotUserProofCB on the OVR pump thread, the thread on which the game's own writers of these two
// values (the org-id and user callbacks) run.
class LivePrerequisiteState final : public PrerequisiteState {
 public:
  LivePrerequisiteState(std::uint64_t* org_global, char* user_name) : org_(org_global), name_(user_name) {}

  bool UserNameIsStandIn() const override {
    return name_ != nullptr && StandIn::IsOculusId(name_, kUserNameBytes);
  }
  void ResetUserNameToRefetch() override {
    if (name_ == nullptr) return;
    std::memset(name_, 0, kUserNameBytes);
    name_[0] = '?';  // string 0x61d3cb: LogInInternal re-fetches the user (0x1ec9f4)
  }
  void SetUserName(const char* name) override {
    if (name_ == nullptr || name == nullptr) return;
    std::memset(name_, 0, kUserNameBytes);
    for (std::size_t i = 0; i + 1 < kUserNameBytes && name[i] != '\0'; ++i) name_[i] = name[i];
  }
  void ResetOrgIdToRefetch() override {
    if (org_ != nullptr) __atomic_store_n(org_, kRefetchOrgId, __ATOMIC_RELEASE);
  }

 private:
  std::uint64_t* org_;
  char* name_;
};

// Runs the rewrite and the send gate for one login. The compiler inlines it into the handler, so
// the adapters (which have destructors) live in the handler's frame, which also calls the game's
// original. Every frame live during a call into the game is built -fno-exceptions and sits under
// the personality-free "zR" CIE: this function, the handler, RewriteLogin, FinishLogin and the rest
// of login_apply.cpp (tests/quest TestLoginHookFramesCarryNoPersonality). The one exceptions-enabled
// function it reaches, ComposePlan, calls no game code and has returned before the next game call.
//
// The gate (FinishLogin/DecideSend, login_rewrite.h) reads the CURRENT wire state after the rewrite:
// a login whose account id, access_token or nonce is a stand-in, or whose account id is 0 or -1, is
// refused whatever the outcome and whether or not NEVR is ready. A refused login is not sent; the
// game's own deferred failure runs on its next update with the game's own text, and the stand-in
// org id and user name are put back to the game's re-fetch markers.
void RunLogin(const State& state, LoginThunk::Fn original, void* user, void* json) noexcept {
  LiveUser live_user(user, state.account_id_global, state.expected_vptr);
  LiveJson live_json(state.api, json);
  LivePrerequisiteState prereq(state.account_id_global, state.user_name);
  const Outcome outcome = RewriteLogin(live_user, live_json, *state.source, state.build, state.log);
  const FinishResult finish = FinishLogin(live_user, live_json, prereq, outcome, state.log);
  EndPrerequisiteAttempt();
  if (finish.decision == SendDecision::SendOriginal) {
    original(user, json);
    return;
  }
  // Refused: send nothing. Drive the game's own deferred failure only on a proven CNSOVRUser.
  if (state.deferred_failed != nullptr && live_user.IsCNSOVRUser()) {
    state.deferred_failed(user, kLoginFailedCode, kPrerequisitesMissingText);
    return;
  }
  if (state.log != nullptr) {
    const LogKv fields[] = {{"op", "send", 0}, {"decision", "withheld", 0}, {"reason", "no_deferred_failure_entry", 0}};
    state.log(Level::Error, "quest_login_send", fields, 3);
  }
}

// The handler behind the GOT slot. With the hook state published, the rewrite and the gate decide;
// without it the game's own login goes out exactly as it would without this hook.
void HandleSendLogInRequest(LoginThunk::Fn original, void* user, void* json) noexcept {
  const State* state = g_published.load(std::memory_order_acquire);
  if (state == nullptr || json == nullptr) {
    original(user, json);
    return;
  }
  RunLogin(*state, original, user, json);
}

// The hook's record: {thunk entry, handler} in the nevr_hook_records section, which is what the
// frame sensor walks from and what Thunk::Arm accepts.
NEVR_HOOK_RECORD(kLoginHook, LoginThunk, &HandleSendLogInRequest);

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

// Resolves CNSUser::DeferredLogInFailed for the pinned image: the export must equal base+0x382e44
// and its three instructions must be the pinned ones. nullptr otherwise (the gate then withholds a
// refused login without driving a failure, and says so).
DeferredFailedFn ProveDeferredFailed(const sentinel::ElfImage& image) {
  void* handle = dlopen(kPnsovr, RTLD_NOW | RTLD_NOLOAD);
  if (handle == nullptr) return nullptr;
  const std::uint64_t want = image.base + kDeferredFailedVaddr;
  if (reinterpret_cast<std::uint64_t>(dlsym(handle, kDeferredFailedSymbol)) != want) return nullptr;
  std::uint32_t code[3];
  std::memcpy(code, reinterpret_cast<const void*>(want), sizeof(code));
  if (std::memcmp(code, kDeferredFailedCode, sizeof(code)) != 0) return nullptr;
  return reinterpret_cast<DeferredFailedFn>(want);
}

// True when [vaddr, vaddr + bytes) lies inside a writable PT_LOAD of the image.
bool InWritableSegment(const sentinel::ElfImage& image, std::uint64_t vaddr, std::size_t bytes) {
  for (std::size_t i = 0; i < image.phnum; ++i) {
    const Elf64_Phdr& ph = image.phdr[i];
    if (ph.p_type != PT_LOAD || (ph.p_flags & PF_W) == 0) continue;
    if (vaddr >= ph.p_vaddr && vaddr + bytes <= ph.p_vaddr + ph.p_memsz) return true;
  }
  return false;
}

// Proves the user-name buffer: the two instructions of GotLoggedInUserCb that form its address are
// the pinned ones, and the 36 bytes lie in a writable PT_LOAD. nullptr unless both hold.
char* ProveUserName(const sentinel::ElfImage& image) {
  std::uint32_t code[2];
  std::memcpy(code, reinterpret_cast<const void*>(image.base + kUserNameCodeVaddr), sizeof(code));
  if (std::memcmp(code, kUserNameCode, sizeof(code)) != 0) return nullptr;
  if (!InWritableSegment(image, kUserNameVaddr, kUserNameBytes)) return nullptr;
  return reinterpret_cast<char*>(image.base + kUserNameVaddr);
}

// ReadyFn for the prerequisites: a real NEVR login is ready when the installed identity source
// says so (IdentitySource::Ready, one lock-free atomic load by contract); the default source
// answers false, so an unwired source fails closed and nothing is stood in.
bool LoginIdentityReady() noexcept {
  const State* state = g_published.load(std::memory_order_acquire);
  return state != nullptr && state->source != nullptr && state->source->Ready();
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
  if (count > 8) {
    // The switch below carries eight fields; say so instead of dropping the rest silently.
    sentinel::LogFields(sentinel::LogLevel::kError, "quest_login_log_overflow",
                        {LogField("event", event), LogField("fields", count)});
  }
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
  g_state.deferred_failed = ProveDeferredFailed(image);
  g_state.user_name = ProveUserName(image);
  g_published.store(&g_state, std::memory_order_release);
  LoginThunk::Arm(kLoginHook);

  // The slot must hold libpnsovr's own CNSUser::SendLogInRequest (0x382a4c); libr15 exports
  // the same symbol at 0x1932838, and a slot bound there is refused instead of hooked.
  const sentinel::GotTarget target(kPnsovr, kHookedSymbol, sentinel::RelocKind::kJumpSlot,
                                   kPnsovrBuildId, kHookedSlotVaddr,
                                   reinterpret_cast<const void*>(image.base + kOwnSendLogInRequestVaddr));
  if (sentinel::InstallThunk<LoginThunk>(g_hook, target) != sentinel::GotStatus::kOk) {
    LoginThunk::Disarm();
    g_published.store(nullptr, std::memory_order_release);
    return refuse(InstallState::HookFailed, "got_backend");
  }
  // The four Oculus answers the game needs before it calls SendLogInRequest; installed here so
  // they are in place before RadPluginMain issues the first requests. Substitution is gated on the
  // identity source being Ready (fail closed), logged by the install summary. Logs its own summary.
  InstallLoginPrerequisites(image, &LoginIdentityReady);
  const LogKv fields[] = {{"op", "install", 0}, {"state", "installed", 0}, {"slot", "CNSUser::SendLogInRequest", 0}};
  log(Level::Info, "quest_login_install", fields, 3);
  return InstallState::Installed;
}

}  // namespace QuestLogin

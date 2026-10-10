// Host test for the Quest config-string redirect (src/quest/redirect).
//
// Three layers, all against production code:
//   1. ServiceRedirector behind the typed CallbackThunk handlers, with a fake CJson::TString as the
//      "original": which keys and values are redirected, pointer stability, failure fallbacks,
//      logging, allocation.
//   2. The same through a real GOT hook: two fixture shared objects import CJson::TString by its
//      mangled name through a PLT slot; InstallRedirectHooksWith patches their slots with GotHook.
//   3. Concurrency on the cache.
// Nothing here edits a slot or a counter by hand to fake a result.
//
// Run: redirect_test <fixture-dir>

#include <dlfcn.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "got_hook.h"
#include "hook_log.h"
#include "quest/redirect/hook_adapter.h"
#include "quest/redirect/service_redirector.h"
#include "quest/redirect/tstring_thunks.h"
#include "quest/sentinel/quest_config.h"
#include "quest/tests/test_check.h"
#include "runtime/lifecycle/stable_string_pool.h"

// ---- allocation counting ----------------------------------------------------

namespace {
std::atomic<bool> g_countAllocations{false};
std::atomic<long> g_allocations{0};
}  // namespace

// The malloc/free pair sits behind noinline helpers so an optimising build cannot inline `free`
// into a caller that inlined `operator new` (-Wmismatched-new-delete).
namespace {
__attribute__((noinline)) void* RawAllocate(std::size_t size) { return std::malloc(size == 0 ? 1 : size); }
__attribute__((noinline)) void RawFree(void* p) { std::free(p); }
}  // namespace

void* operator new(std::size_t size) {
  if (g_countAllocations.load(std::memory_order_relaxed)) g_allocations.fetch_add(1, std::memory_order_relaxed);
  void* p = RawAllocate(size);
  if (p == nullptr) throw std::bad_alloc();
  return p;
}
void operator delete(void* p) noexcept { RawFree(p); }
void operator delete(void* p, std::size_t) noexcept { RawFree(p); }

namespace {

using namespace nevr_quest::redirect;
using nevr_quest::EmbeddedDefaults;
using nevr_quest::ResolvedConfig;
using sentinel::GotStatus;

// The thunks' own types are not visible here (callback_thunk.h needs -fno-exceptions); the entry
// points are called through the same ABI with `this` as an opaque pointer.
constexpr const char* kTStringSymbol = "_ZNK10NRadEngine5CJson7TStringEPKcS2_j";

constexpr const char* kDefaultLogin = "wss://login.readyatdawn.com/rad/rad15_live";
constexpr const char* kDefaultConfig = "wss://config.readyatdawn.com/rad/rad15_live";
constexpr const char* kDefaultMatchmaker = "wss://matchmaker.readyatdawn.com/rad/rad15_live";
constexpr const char* kSocketTarget = "wss://nevr.example/ws";
constexpr const char* kHttpTarget = "https://api.nevr.example:7350";

constexpr const char* kRedirectOn =
    R"({"nevr_socket_uri":"wss://nevr.example/ws","nevr_http_uri":"https://api.nevr.example:7350",)"
    R"("features":{"redirect":true}})";
constexpr const char* kRedirectAndBridgeOn =
    R"({"nevr_socket_uri":"wss://nevr.example/ws","nevr_http_uri":"https://api.nevr.example:7350",)"
    R"("features":{"redirect":true,"bridge":true}})";

ResolvedConfig Config(const std::string& fileText, const EmbeddedDefaults& defaults = EmbeddedDefaults()) {
  return nevr_quest::ResolveConfig(defaults, &fileText).config;
}

// ---- the pool, with fault injection -----------------------------------------

nevr::lifecycle::StableStringPool& Pool() {
  static nevr::lifecycle::StableStringPool pool;
  return pool;
}
nevr::lifecycle::InternResult InternReal(std::string_view value) { return Pool().Intern(value); }
nevr::lifecycle::InternResult InternRefuse(std::string_view) {
  return {nevr::lifecycle::InternStatus::kPoolCountExceeded, nullptr, 1024, 0};
}
nevr::lifecycle::InternResult InternThrow(std::string_view) { throw std::bad_alloc(); }

BridgeState g_bridge;
BridgeState BridgeProbeFn() { return g_bridge; }

// ---- log capture ------------------------------------------------------------

std::vector<std::string>& Lines() {
  static std::vector<std::string> lines;
  return lines;
}
std::mutex g_linesMutex;
void Sink(sentinel::LogLevel, const char* line) {
  const std::lock_guard<std::mutex> lock(g_linesMutex);
  Lines().emplace_back(line);
}

bool AnyLineContains(const char* needle) {
  for (const std::string& l : Lines()) {
    if (l.find(needle) != std::string::npos) return true;
  }
  return false;
}

std::size_t CountLinesContaining(const char* needle) {
  std::size_t n = 0;
  for (const std::string& l : Lines()) {
    if (l.find(needle) != std::string::npos) ++n;
  }
  return n;
}

// ---- the fake game ----------------------------------------------------------

// Insert-only storage with stable element addresses (a deque), so a pointer taken from a value
// stays valid while other keys are added. Lookup by strcmp so the fake itself never allocates.
class GameMap {
 public:
  void clear() { items_.clear(); }
  std::string& operator[](const std::string& key) {
    for (auto& item : items_) {
      if (item.first == key) return item.second;
    }
    items_.emplace_back(key, std::string());
    return items_.back().second;
  }
  const std::string* Find(const char* key) const {
    for (const auto& item : items_) {
      if (std::strcmp(item.first.c_str(), key) == 0) return &item.second;
    }
    return nullptr;
  }

 private:
  std::deque<std::pair<std::string, std::string>> items_;
};

GameMap& GameConfig() {
  static GameMap values;
  return values;
}

// CJson::TString as the game defines it: the stored value's pointer when the key is present,
// else the caller's fallback pointer itself.
const char* FakeTString(const void*, const char* key, const char* fallback, std::uint32_t) {
  const std::string* value = GameConfig().Find(key);
  return value == nullptr ? fallback : value->c_str();
}

using TStringFn = const char* (*)(const void*, const char*, const char*, std::uint32_t);

TStringFn R15Entry() { return reinterpret_cast<TStringFn>(ThunkEntry(Slot::kLibR15)); }
TStringFn MmEntry() { return reinterpret_cast<TStringFn>(ThunkEntry(Slot::kMatchmaking)); }

void PublishFakeOriginal() {
  *ThunkOriginalOut(Slot::kLibR15) = reinterpret_cast<void*>(&FakeTString);
  *ThunkOriginalOut(Slot::kMatchmaking) = reinterpret_cast<void*>(&FakeTString);
}

void ResetThunks() {
  ResetThunk(Slot::kLibR15);
  ResetThunk(Slot::kMatchmaking);
}

// One scenario: a redirector armed behind both thunks, torn down on destruction.
struct Scenario {
  std::unique_ptr<ServiceRedirector> redirector;

  Scenario(const ResolvedConfig& config, InternFn intern = &InternReal, BridgeProbe bridge = nullptr) {
    GameConfig().clear();
    Lines().clear();
    ResetCountersForTest();
    ResetThunks();
    PublishFakeOriginal();
    redirector = std::make_unique<ServiceRedirector>(config, intern, bridge);
    ArmHandlersForTest(redirector.get());
  }
  ~Scenario() {
    ArmHandlersForTest(nullptr);
    ResetThunks();
  }

  const char* R15(const char* key, const char* fallback) { return R15Entry()(nullptr, key, fallback, 0U); }
  const char* Mm(const char* key, const char* fallback) { return MmEntry()(nullptr, key, fallback, 0U); }
};

// ---- layer 1: decisions behind the thunks -----------------------------------

void KeyTable() {
  const char* const keys[] = {"config_host",      "configservice_host",      "login_host",
                              "loginservice_host", "transaction_host",        "transactionservice_host",
                              "matchmaker_host",   "matchingservice_host"};
  for (const char* k : keys) QCHECK(IsServiceHostKey(k));
  QCHECK(!IsServiceHostKey(nullptr));
  QCHECK(!IsServiceHostKey(""));
  QCHECK(!IsServiceHostKey("LOGIN_HOST"));
  QCHECK(!IsServiceHostKey("login_hosts"));
  QCHECK(!IsServiceHostKey("login_hos"));
  QCHECK(!IsServiceHostKey("radserverdb_host"));
  QCHECK(!IsServiceHostKey("server_plugin"));
  // The per-match-type formats libpnsradmatchmaking passes to TString.
  QCHECK(IsServiceHostKey("matchmaker_arena_host"));
  QCHECK(IsServiceHostKey("matchingservice_combat_host"));
  QCHECK(IsServiceHostKey("matchmaker_x_host"));
  QCHECK(!IsServiceHostKey("matchmaker__host"));        // empty <type>
  QCHECK(!IsServiceHostKey("matchmaker_arena_hosts"));
  QCHECK(!IsServiceHostKey("xmatchmaker_arena_host"));
  QCHECK(!IsServiceHostKey("matchmaker_arena_hos"));
  QCHECK(!IsServiceHostKey("matchingservice__host"));
  QCHECK(!IsServiceHostKey("login_arena_host"));
}

void PerMatchTypeKeysAreRedirected() {
  Scenario s(Config(kRedirectOn));
  // ConnectMatchmaker formats the key per match type, reads it with fallback '' and passes the
  // result to the second read as its fallback (libpnsradmatchmaking 0x1b2510, 0x1b2524).
  GameConfig()["matchmaker_arena_host"] = "wss://elsewhere.example/arena";
  QCHECK(std::strcmp(s.Mm("matchmaker_arena_host", ""), kSocketTarget) == 0);
  const char* second = s.Mm("matchingservice_arena_host", s.Mm("matchmaker_arena_host", ""));
  QCHECK(std::strcmp(second, kSocketTarget) == 0);
  // Absent per-type key with the empty fallback: nothing to redirect, the exact pointer comes back.
  const char* empty = "";
  QCHECK(s.Mm("matchmaker_combat_host", empty) == empty);
  // An unrelated key carrying a ws:// value is left alone under this rule.
  GameConfig()["matchmaker_queue_mode"] = "wss://elsewhere.example/q";
  const char* stored = GameConfig()["matchmaker_queue_mode"].c_str();
  QCHECK(s.Mm("matchmaker_queue_mode", "x") == stored);
}

void RedirectsDefaultsToNevrHost() {
  Scenario s(Config(kRedirectOn));
  const char* login = s.R15("login_host", kDefaultLogin);
  QCHECK(login != kDefaultLogin);
  QCHECK(std::strcmp(login, kSocketTarget) == 0);
  QCHECK(std::strcmp(s.R15("config_host", kDefaultConfig), kSocketTarget) == 0);
  QCHECK(std::strcmp(s.Mm("matchmaker_host", kDefaultMatchmaker), kSocketTarget) == 0);
  // The default string itself is left alone in the game's rodata.
  QCHECK(std::strcmp(kDefaultLogin, "wss://login.readyatdawn.com/rad/rad15_live") == 0);
}

void ChainedLookupKeepsThePointer() {
  // The game feeds the first result to the second lookup as its fallback (login_host, then
  // loginservice_host), and the second returns it when its own key is absent.
  Scenario s(Config(kRedirectOn));
  const char* first = s.R15("login_host", kDefaultLogin);
  const char* second = s.R15("loginservice_host", first);
  QCHECK(second == first);
  QCHECK(std::strcmp(second, kSocketTarget) == 0);
}

void ConfiguredWebSocketValueIsRedirectedByValue() {
  // A key the config carries a ws:// value for goes through the same policy as PCVR: any ws host.
  Scenario s(Config(kRedirectOn));
  GameConfig()["loginservice_host"] = "wss://elsewhere.example/other";
  const char* got = s.R15("loginservice_host", kDefaultLogin);
  QCHECK(std::strcmp(got, kSocketTarget) == 0);
}

void HttpsReadyAtDawnUsesTheHttpTarget() {
  Scenario s(Config(kRedirectOn));
  GameConfig()["configservice_host"] = "https://config.readyatdawn.com/rad/rad15_live";
  QCHECK(std::strcmp(s.R15("configservice_host", kDefaultConfig), kHttpTarget) == 0);
}

void UnrelatedValuesAreUntouched() {
  Scenario s(Config(kRedirectOn));
  // Service key, value the policy declines: the exact original pointer comes back.
  GameConfig()["login_host"] = "https://unrelated.example/path";
  const char* stored = GameConfig()["login_host"].c_str();
  QCHECK(s.R15("login_host", kDefaultLogin) == stored);
  // Not a service key, even though the value is a ws:// URL the policy would redirect.
  GameConfig()["server_plugin"] = "wss://other.example/x";
  const char* plugin = GameConfig()["server_plugin"].c_str();
  QCHECK(s.R15("server_plugin", "fallback") == plugin);
  const char* missing = "fallback-pointer";
  QCHECK(s.R15("publisher_lock", missing) == missing);
  // A null result and a null key pass through.
  QCHECK(R15Entry()(nullptr, "config_host", nullptr, 0U) == nullptr);  // absent key, null fallback
  const char* value = "value";
  QCHECK(s.redirector->Apply(nullptr, value) == value);
  QCHECK(s.redirector->counters().redirected == 0);
}

void FeatureOffLeavesEverythingUntouched() {
  // No file: every feature is off.
  EmbeddedDefaults embedded;
  embedded.socketUri = "wss://emb.example/nevr";
  embedded.httpUri = "https://emb.example:7350";
  Scenario s(nevr_quest::ResolveConfig(embedded, nullptr).config);
  QCHECK(!s.redirector->active());
  QCHECK(s.R15("login_host", kDefaultLogin) == kDefaultLogin);
  QCHECK(s.Mm("matchmaker_host", kDefaultMatchmaker) == kDefaultMatchmaker);
  QCHECK(s.redirector->counters().calls == 0);
  // Explicitly off in the file.
  Scenario off(Config(R"({"nevr_socket_uri":"wss://nevr.example/ws","features":{"redirect":false}})"));
  QCHECK(off.R15("login_host", kDefaultLogin) == kDefaultLogin);
}

void MalformedConfigLeavesEverythingUntouched() {
  EmbeddedDefaults embedded;
  embedded.socketUri = "wss://emb.example/nevr";
  embedded.httpUri = "https://emb.example:7350";
  const char* const bad[] = {"{not json", "[1,2,3]", "", R"({"features":{"redirect":"yes"}})"};
  for (const char* text : bad) {
    const std::string fileText = text;
    const nevr_quest::LoadResult loaded = nevr_quest::ResolveConfig(embedded, &fileText);
    Scenario s(loaded.config);
    QCHECK(!s.redirector->active());
    QCHECK(s.R15("login_host", kDefaultLogin) == kDefaultLogin);
  }
  // A file too large to accept is rejected whole.
  std::string big = R"({"features":{"redirect":true},"pad":")";
  big.append(nevr_quest::kMaxConfigBytes, 'x');
  big += "\"}";
  Scenario s(nevr_quest::ResolveConfig(embedded, &big).config);
  QCHECK(s.R15("login_host", kDefaultLogin) == kDefaultLogin);
  // Redirect requested but no target: forced off.
  Scenario noTarget(Config(R"({"features":{"redirect":true}})"));
  QCHECK(noTarget.R15("login_host", kDefaultLogin) == kDefaultLogin);
}

void PointerStableAcrossCallsAndPoolChurn() {
  Scenario s(Config(kRedirectOn));
  const char* first = s.R15("login_host", kDefaultLogin);
  QCHECK(std::strcmp(first, kSocketTarget) == 0);
  QCHECK(s.R15("login_host", kDefaultLogin) == first);

  // Churn the pool far past the redirector's own cache: 600 unrelated strings.
  for (int i = 0; i < 600; ++i) {
    const std::string filler = "churn-" + std::to_string(i) + "-" + std::string(64, 'x');
    QCHECK(Pool().Intern(filler).status == nevr::lifecycle::InternStatus::kSuccess);
  }
  QCHECK(std::strcmp(first, kSocketTarget) == 0);  // still readable, still the same bytes
  QCHECK(s.R15("login_host", kDefaultLogin) == first);

  // Push more distinct game values through than the cache holds; the first value must still map
  // to the same pool pointer afterwards (equal strings intern to one address).
  for (int i = 0; i < 3 * static_cast<int>(kMaxCachedValues); ++i) {
    GameConfig()["loginservice_host"] = "wss://v" + std::to_string(i) + ".example/x";
    QCHECK(s.R15("loginservice_host", kDefaultLogin) == first);
  }
  QCHECK(s.R15("login_host", kDefaultLogin) == first);
  QCHECK(std::strcmp(first, kSocketTarget) == 0);
}

void BridgeStateSelectsTheTarget() {
  Scenario s(Config(kRedirectAndBridgeOn), &InternReal, &BridgeProbeFn);
  g_bridge = {false, 0};
  const char* direct = s.R15("login_host", kDefaultLogin);
  QCHECK(std::strcmp(direct, kSocketTarget) == 0);
  g_bridge = {true, 53748};
  const char* bridged = s.R15("login_host", kDefaultLogin);
  QCHECK(std::strcmp(bridged, "ws://127.0.0.1:53748") == 0);
  QCHECK(bridged != direct);
  g_bridge = {true, 40001};
  QCHECK(std::strcmp(s.R15("login_host", kDefaultLogin), "ws://127.0.0.1:40001") == 0);
  g_bridge = {false, 0};
  QCHECK(s.R15("login_host", kDefaultLogin) == direct);
  // With the bridge feature off the probe is ignored.
  Scenario noBridge(Config(kRedirectOn), &InternReal, &BridgeProbeFn);
  g_bridge = {true, 53748};
  QCHECK(std::strcmp(noBridge.R15("login_host", kDefaultLogin), kSocketTarget) == 0);
  g_bridge = {false, 0};
}

void PoolRefusalFallsBackToTheOriginal() {
  Scenario s(Config(kRedirectOn), &InternRefuse);
  QCHECK(s.R15("login_host", kDefaultLogin) == kDefaultLogin);
  QCHECK(s.redirector->counters().failures == 1);
  QCHECK(GlobalCounters().poolRefused.load() == 1);
  QCHECK(Lines().empty());  // a hooked call never logs
}

void ExceptionFromThePoolFallsBackToTheOriginal() {
  Scenario s(Config(kRedirectOn), &InternThrow);
  QCHECK(s.R15("login_host", kDefaultLogin) == kDefaultLogin);
  QCHECK(s.redirector->counters().failures == 1);
  QCHECK(GlobalCounters().exceptions.load() == 1);
  QCHECK(Lines().empty());
}

void OverlongValueFallsBack() {
  Scenario s(Config(kRedirectOn));
  GameConfig()["login_host"] = "wss://" + std::string(kMaxValueBytes, 'a') + ".example/x";
  const char* stored = GameConfig()["login_host"].c_str();
  QCHECK(s.R15("login_host", kDefaultLogin) == stored);
  QCHECK(GlobalCounters().valueTooLong.load() == 1);
  QCHECK(Lines().empty());
  // A value exactly at the limit is still handled.
  GameConfig()["login_host"] = "wss://" + std::string(kMaxValueBytes - 6, 'a');
  QCHECK(std::strcmp(s.R15("login_host", kDefaultLogin), kSocketTarget) == 0);
}

void HookedCallsNeverLog() {
  // Logging is unsafe inside a hooked call (hook_log.h): the decision path only counts.
  Scenario s(Config(kRedirectOn), &InternReal, &BridgeProbeFn);
  g_bridge = {true, 53748};
  s.redirector->Prewarm();
  GameConfig()["loginservice_host"] = "wss://secret-token-host.example/x?token=SECRETTOKEN";
  (void)s.R15("loginservice_host", kDefaultLogin);
  (void)s.R15("login_host", kDefaultLogin);
  g_bridge = {false, 0};
  (void)s.R15("login_host", kDefaultLogin);
  QCHECK(Lines().empty());
  QCHECK(GlobalCounters().reads.load() > 0);
  QCHECK(GlobalCounters().redirected.load() > 0);
}

void HitsDoNotAllocateOrLog() {
  Scenario s(Config(kRedirectOn));
  s.redirector->Prewarm();
  const char* const redirected = s.R15("login_host", kDefaultLogin);
  GameConfig()["server_plugin"] = "wss://other.example/x";
  (void)s.R15("loginservice_host", redirected);  // first sight of this value is a miss; warm it
  Lines().clear();
  g_allocations.store(0);
  g_countAllocations.store(true);
  const char* last = nullptr;
  for (int i = 0; i < 2000; ++i) {
    last = s.R15("login_host", kDefaultLogin);
    (void)s.R15("config_host", kDefaultConfig);
    (void)s.Mm("matchmaker_host", kDefaultMatchmaker);
    (void)s.R15("loginservice_host", redirected);  // the chained second lookup
    (void)s.R15("server_plugin", "fallback");
    (void)s.R15("publisher_lock", "fallback");
  }
  g_countAllocations.store(false);
  QCHECK(last == redirected);
  QCHECK(g_allocations.load() == 0);
  QCHECK(Lines().empty());
}

void PrewarmMakesTheBuiltinDefaultsHits() {
  Scenario s(Config(kRedirectOn));
  s.redirector->Prewarm();
  const std::uint64_t runs = s.redirector->counters().policyRuns;
  QCHECK(runs == 4);
  (void)s.R15("login_host", kDefaultLogin);
  (void)s.R15("config_host", kDefaultConfig);
  (void)s.Mm("matchmaker_host", kDefaultMatchmaker);
  (void)s.R15("transaction_host", "wss://transaction.readyatdawn.com/rad/rad15_live");
  QCHECK(s.redirector->counters().policyRuns == runs);
}

void ThunkPassesExceptionsFromTheOriginal() {
  Scenario s(Config(kRedirectOn));
  struct Boom {};
  static auto thrower = +[](const void*, const char*, const char*, std::uint32_t) -> const char* {
    throw Boom();
  };
  *ThunkOriginalOut(Slot::kLibR15) = reinterpret_cast<void*>(thrower);
  bool caught = false;
  try {
    (void)s.R15("login_host", kDefaultLogin);
  } catch (const Boom&) {
    caught = true;
  }
  QCHECK(caught);
}

// The handler must apply the redirect to what the original returned, after it returned: a fake
// original records that it ran, and the apply function checks the flag and the value it was handed.
bool g_originalRan = false;
bool g_applyAfterOriginal = false;
const char* g_applySawResult = nullptr;
const char* g_originalReturned = nullptr;

const char* OrderOriginal(const void*, const char*, const char* fallback, std::uint32_t) {
  g_originalRan = true;
  g_originalReturned = fallback;
  return fallback;
}
const char* OrderApply(const char*, const char* result) noexcept {
  g_applyAfterOriginal = g_originalRan;
  g_applySawResult = result;
  return result;
}

void HandlerAppliesAfterTheOriginalToItsResult() {
  ResetThunks();
  *ThunkOriginalOut(Slot::kLibR15) = reinterpret_cast<void*>(&OrderOriginal);
  *ThunkOriginalOut(Slot::kMatchmaking) = reinterpret_cast<void*>(&OrderOriginal);
  SetApply(&OrderApply);
  ArmThunk(Slot::kLibR15, true);
  ArmThunk(Slot::kMatchmaking, true);
  for (int slot = 0; slot < 2; ++slot) {
    g_originalRan = g_applyAfterOriginal = false;
    g_applySawResult = g_originalReturned = nullptr;
    const char* const fallback = "fallback-marker";
    const char* const got = (slot == 0 ? R15Entry() : MmEntry())(nullptr, "login_host", fallback, 0U);
    QCHECK(g_originalRan);
    QCHECK(g_applyAfterOriginal);
    QCHECK(g_applySawResult == g_originalReturned);
    QCHECK(got == fallback);
  }
  SetApply(nullptr);
  ArmThunk(Slot::kLibR15, false);
  ArmThunk(Slot::kMatchmaking, false);
  ResetThunks();
}

// All 16 slots computed under the current bridge state: a further value is not remembered. A change
// of bridge state makes those entries stale, and a stale slot is recycled.
void CacheFullAndStaleBridgeBehaviour() {
  Scenario s(Config(kRedirectAndBridgeOn), &InternReal, &BridgeProbeFn);
  g_bridge = {false, 0};
  auto read = [&](int i) {
    GameConfig()["loginservice_host"] = "wss://v" + std::to_string(i) + ".example/x";
    return s.R15("loginservice_host", kDefaultLogin);
  };
  for (int i = 0; i < static_cast<int>(kMaxCachedValues); ++i) (void)read(i);
  QCHECK(s.redirector->counters().policyRuns == kMaxCachedValues);
  (void)read(100);
  (void)read(100);  // full under the current bridge state: not remembered, so the policy runs again
  QCHECK(s.redirector->counters().policyRuns == kMaxCachedValues + 2);
  (void)read(3);    // an entry that is cached stays a hit
  QCHECK(s.redirector->counters().policyRuns == kMaxCachedValues + 2);

  g_bridge = {true, 53748};
  const std::uint64_t before = s.redirector->counters().policyRuns;
  const char* bridged = read(0);  // every entry is stale now: this miss recycles one
  QCHECK(std::strcmp(bridged, "ws://127.0.0.1:53748") == 0);
  QCHECK(s.redirector->counters().policyRuns == before + 1);
  QCHECK(read(0) == bridged);
  QCHECK(s.redirector->counters().policyRuns == before + 1);  // remembered: no second policy run
  g_bridge = {false, 0};
}

// ---- layer 3: concurrency ---------------------------------------------------

void ConcurrentCallsAgree() {
  Scenario s(Config(kRedirectOn));
  s.redirector->Prewarm();
  const char* const expected = s.R15("login_host", kDefaultLogin);
  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < 20000; ++i) {
        const char* got = (i % 3 == 0) ? s.redirector->Apply("login_host", kDefaultLogin)
                                       : s.redirector->Apply("matchmaker_host", kDefaultMatchmaker);
        if (got != expected) mismatches.fetch_add(1);
        if (i % 997 == t) {
          // A rolling set of fresh values keeps the miss path busy on every thread.
          const std::string v = "wss://t" + std::to_string(t) + "-" + std::to_string(i) + ".example/x";
          if (s.redirector->Apply("config_host", v.c_str()) != expected) mismatches.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& th : threads) th.join();
  QCHECK(mismatches.load() == 0);
}

// ---- layer 2: a real GOT hook on fixture modules ----------------------------

using FxRead = const char* (*)(const char*, const char*);

void* OpenFixture(const std::string& dir, const char* name) {
  void* handle = dlopen((dir + "/" + name).c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) std::fprintf(stderr, "dlopen %s: %s\n", name, dlerror());
  return handle;
}

InstallOptions FixtureOptions(InternFn intern) {
  return {{sentinel::GotTarget("libredirfx_consumer_a.so", kTStringSymbol, sentinel::RelocKind::kJumpSlot),
           sentinel::GotTarget("libredirfx_consumer_b.so", kTStringSymbol, sentinel::RelocKind::kJumpSlot)},
          sentinel::FindLoadedImage, intern, nullptr};
}

// Another writer overwrites the slot between our store and the read-back: Install reports
// kWriteVerifyFailed, and the backend then refuses the slot as poisoned. The adapter stops there:
// no further attempt, no further log line, and the thunk stays disarmed.
void ClobberAfterStore(void** slot, void*, sentinel::StorePhase phase) {
  if (phase == sentinel::StorePhase::kAfterStore) *slot = reinterpret_cast<void*>(&FakeTString);
}

void PoisonedSlotIsNotRetried(void*, void*) {
  RemoveRedirectHooks();
  Lines().clear();
  sentinel::SetStoreObserver(&ClobberAfterStore);
  const InstallReport report = InstallRedirectHooksWith(Config(kRedirectOn), FixtureOptions(&InternReal));
  sentinel::SetStoreObserver(nullptr);
  QCHECK_STATUS(report.libr15, GotStatus::kWriteVerifyFailed);
  QCHECK_STATUS(InstallLibR15RedirectWith(sentinel::FindLoadedImage), GotStatus::kSlotPoisoned);
  Lines().clear();
  QCHECK_STATUS(InstallLibR15RedirectWith(sentinel::FindLoadedImage), GotStatus::kSlotPoisoned);
  QCHECK(Lines().empty());  // the stored poisoned status short-circuits: no attempt, no log line
  *ThunkOriginalOut(Slot::kLibR15) = reinterpret_cast<void*>(&FakeTString);
  GameConfig().clear();
  QCHECK(R15Entry()(nullptr, "login_host", kDefaultLogin, 0U) == kDefaultLogin);  // disarmed
  ResetThunk(Slot::kLibR15);
  RemoveRedirectHooks();
}

void RealHookEndToEnd(const std::string& dir) {
  ResetThunks();
  Lines().clear();

  // Feature off: nothing installs.
  EmbeddedDefaults embedded;
  embedded.socketUri = "wss://emb.example/nevr";
  const InstallReport off = InstallRedirectHooksWith(nevr_quest::ResolveConfig(embedded, nullptr).config, FixtureOptions(&InternReal));
  QCHECK(!off.featureEnabled);
  QCHECK_STATUS(off.libr15, GotStatus::kNotInstalled);

  // Feature on but neither module is loaded: both installs fail, nothing is hooked, and the
  // redirector stays so that a later call can retry.
  const InstallReport early = InstallRedirectHooksWith(Config(kRedirectOn), FixtureOptions(&InternReal));
  QCHECK(early.featureEnabled);
  QCHECK_STATUS(early.libr15, GotStatus::kModuleNotLoaded);
  QCHECK_STATUS(early.matchmaking, GotStatus::kModuleNotLoaded);
  // A failed install leaves the handler disarmed: a call through the entry is a pass-through.
  *ThunkOriginalOut(Slot::kLibR15) = reinterpret_cast<void*>(&FakeTString);
  GameConfig().clear();
  QCHECK(R15Entry()(nullptr, "login_host", kDefaultLogin, 0U) == kDefaultLogin);
  ResetThunk(Slot::kLibR15);

  // The sentinel retries the matchmaking slot after every dlopen until the module loads (#240: 113
  // retries in one run). The first attempt is logged once; a retry with the same status logs
  // nothing, neither this adapter's line nor the backend's.
  QCHECK(CountLinesContaining("\"target\":\"matchmaking_tstring\",\"status\":\"module_not_loaded\"") == 1);
  const std::size_t linesBeforeRetries = Lines().size();
  for (int i = 0; i < 5; ++i) {
    QCHECK_STATUS(InstallMatchmakingRedirectWith(sentinel::FindLoadedImage), GotStatus::kModuleNotLoaded);
  }
  QCHECK(Lines().size() == linesBeforeRetries);
  QCHECK(CountLinesContaining("\"target\":\"matchmaking_tstring\",\"status\":\"module_not_loaded\"") == 1);

  void* a = OpenFixture(dir, "libredirfx_consumer_a.so");
  QCHECK(a != nullptr);
  if (a == nullptr) return;
  const FxRead readA = reinterpret_cast<FxRead>(dlsym(a, "fx_read"));
  QCHECK(readA != nullptr);
  if (readA == nullptr) return;
  QCHECK(readA("login_host", kDefaultLogin) == kDefaultLogin);  // loaded but not yet hooked

  // The retry installs the libr15 slot; the matchmaking module is still not loaded.
  QCHECK_STATUS(InstallLibR15RedirectWith(sentinel::FindLoadedImage), GotStatus::kOk);
  QCHECK_STATUS(InstallLibR15RedirectWith(sentinel::FindLoadedImage), GotStatus::kAlreadyInstalled);
  const InstallReport on = InstallRedirectHooksWith(Config(kRedirectOn), FixtureOptions(&InternReal));
  QCHECK(on.featureEnabled);
  QCHECK_STATUS(on.libr15, GotStatus::kOk);
  QCHECK_STATUS(on.matchmaking, GotStatus::kModuleNotLoaded);
  const char* redirected = readA("login_host", kDefaultLogin);
  QCHECK(redirected != kDefaultLogin);
  QCHECK(std::strcmp(redirected, kSocketTarget) == 0);
  QCHECK(readA("login_host", kDefaultLogin) == redirected);
  const char* plain = "plain-pointer";
  QCHECK(readA("publisher_lock", plain) == plain);

  // The matchmaking module loads later; the retry installs its slot.
  void* b = OpenFixture(dir, "libredirfx_consumer_b.so");
  QCHECK(b != nullptr);
  if (b != nullptr) {
    const FxRead readB = reinterpret_cast<FxRead>(dlsym(b, "fx_read"));
    QCHECK(readB != nullptr);
    if (readB != nullptr) {
      QCHECK(readB("matchmaker_host", kDefaultMatchmaker) == kDefaultMatchmaker);  // not hooked yet
      QCHECK_STATUS(InstallMatchmakingRedirectWith(sentinel::FindLoadedImage), GotStatus::kOk);
      QCHECK(std::strcmp(readB("matchmaker_host", kDefaultMatchmaker), kSocketTarget) == 0);
      QCHECK_STATUS(InstallMatchmakingRedirectWith(sentinel::FindLoadedImage), GotStatus::kAlreadyInstalled);
    }
  }

  // A second install while installed changes nothing.
  const InstallReport again = InstallRedirectHooksWith(Config(kRedirectOn), FixtureOptions(&InternReal));
  QCHECK_STATUS(again.libr15, GotStatus::kOk);

  RemoveRedirectHooks();
  QCHECK(readA("login_host", kDefaultLogin) == kDefaultLogin);
  if (b != nullptr) {
    const FxRead readB = reinterpret_cast<FxRead>(dlsym(b, "fx_read"));
    if (readB != nullptr) QCHECK(readB("matchmaker_host", kDefaultMatchmaker) == kDefaultMatchmaker);
  }
  // The pool string handed out earlier is still readable after removal.
  QCHECK(std::strcmp(redirected, kSocketTarget) == 0);

  QCHECK(AnyLineContains("\"event\":\"redirect_install\""));
  QCHECK(!AnyLineContains("nevr.example"));

  // A pool that refuses leaves the redirect installed but the game's values untouched.
  const InstallReport refusing = InstallRedirectHooksWith(Config(kRedirectOn), FixtureOptions(&InternRefuse));
  QCHECK_STATUS(refusing.libr15, GotStatus::kOk);
  QCHECK(readA("login_host", kDefaultLogin) == kDefaultLogin);
  RemoveRedirectHooks();

  PoisonedSlotIsNotRetried(a, b);

  if (b != nullptr) dlclose(b);
  dlclose(a);
}

void PinnedTargetsMatchTheMeasuredBinaries() {
  const HookTargets t = PinnedTargets();
  QCHECK(std::strcmp(t.libr15.module, "libr15.so") == 0);
  QCHECK(std::strcmp(t.libr15.symbol, "_ZNK10NRadEngine5CJson7TStringEPKcS2_j") == 0);
  QCHECK(t.libr15.slotVaddr.has_value() && *t.libr15.slotVaddr == 0x36ebe08ULL);
  QCHECK(t.libr15.buildId != nullptr && std::strcmp(t.libr15.buildId, "b243509c08ce677aeb95fa348016949b3fc45230") == 0);
  QCHECK(std::strcmp(t.matchmaking.module, "libpnsradmatchmaking.so") == 0);
  QCHECK(std::strcmp(t.matchmaking.symbol, "_ZNK10NRadEngine5CJson7TStringEPKcS2_j") == 0);
  QCHECK(t.matchmaking.slotVaddr.has_value() && *t.matchmaking.slotVaddr == 0x6b4768ULL);
  QCHECK(t.matchmaking.buildId != nullptr && std::strcmp(t.matchmaking.buildId, "8c4fddc079eae65909530132a56c48da48b2708c") == 0);
}

// The production entry takes the caller's string pool and bridge probe (#237): a probe passed to
// InstallRedirectHooks reaches the redirector, so the bridge feature picks the loopback target. The
// pinned libr15 is not loaded in this process, so no slot is hooked; the redirector is still fixed.
void ProductionEntryPassesTheBridgeProbe() {
  RemoveRedirectHooks();
  ResetThunks();
  Lines().clear();
  const InstallReport report = InstallRedirectHooks(Config(kRedirectAndBridgeOn), &InternReal, &BridgeProbeFn);
  QCHECK(report.featureEnabled);
  ServiceRedirector* const redirector = InstalledRedirectorForTest();
  QCHECK(redirector != nullptr);
  if (redirector != nullptr) {
    g_bridge = {true, 53748};
    QCHECK(std::strcmp(redirector->Apply("login_host", kDefaultLogin), "ws://127.0.0.1:53748") == 0);
    g_bridge = {false, 0};
    QCHECK(std::strcmp(redirector->Apply("login_host", kDefaultLogin), kSocketTarget) == 0);
  }
  RemoveRedirectHooks();
  QCHECK(InstalledRedirectorForTest() == nullptr);
  ResetThunks();
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  sentinel::SetLogSink(&Sink);

  KeyTable();
  RedirectsDefaultsToNevrHost();
  ChainedLookupKeepsThePointer();
  ConfiguredWebSocketValueIsRedirectedByValue();
  HttpsReadyAtDawnUsesTheHttpTarget();
  UnrelatedValuesAreUntouched();
  FeatureOffLeavesEverythingUntouched();
  MalformedConfigLeavesEverythingUntouched();
  PointerStableAcrossCallsAndPoolChurn();
  BridgeStateSelectsTheTarget();
  PoolRefusalFallsBackToTheOriginal();
  ExceptionFromThePoolFallsBackToTheOriginal();
  OverlongValueFallsBack();
  HookedCallsNeverLog();
  PerMatchTypeKeysAreRedirected();
  HitsDoNotAllocateOrLog();
  PrewarmMakesTheBuiltinDefaultsHits();
  ThunkPassesExceptionsFromTheOriginal();
  HandlerAppliesAfterTheOriginalToItsResult();
  CacheFullAndStaleBridgeBehaviour();
  ConcurrentCallsAgree();
  PinnedTargetsMatchTheMeasuredBinaries();
  ProductionEntryPassesTheBridgeProbe();
  RealHookEndToEnd(dir);

  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "redirect_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("redirect_test: all checks passed\n");
  return 0;
}

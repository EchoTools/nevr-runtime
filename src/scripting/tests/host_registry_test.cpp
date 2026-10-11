// Unit tests for the host API registry (src/scripting/host_registry.h): the
// override and hook-point contract every plugin and script binding relies on.
#include "scripting/host_registry.h"
#include "scripting/memory_policy.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#include "quest/tests/mini_test.h"

namespace {

using nevr_script::FieldSpec;
using nevr_script::HookPoint;
using nevr_script::LogRecord;
using nevr_script::Registry;

struct Captured {
  std::string event, owner, other, target, detail;
};

struct Fixture {
  std::vector<Captured> log;
  Registry reg{[this](const LogRecord& r) {
    log.push_back({r.event, r.owner, r.other_owner, r.target, r.detail});
  }};
  const NevrHostApi* api = reg.Api();
  bool keys = reg.RegisterOverridePoint("physics.gravity", NEVR_VALUE_FLOAT) &&
              reg.RegisterOverridePoint("team.colour", NEVR_VALUE_STRING) &&
              reg.RegisterOverridePoint("match.rounds", NEVR_VALUE_INT) &&
              reg.RegisterOverridePoint("k", NEVR_VALUE_INT);
  HookPoint* add = reg.RegisterHookPoint(
      "test.add", {{"a", NEVR_VALUE_INT, true, false},
                   {"b", NEVR_VALUE_INT, true, false},
                   {"result", NEVR_VALUE_INT, true, true}});

  int Count(const char* event) const {
    int n = 0;
    for (const Captured& c : log) n += c.event == event;
    return n;
  }
  const Captured* Find(const char* event) const {
    for (const Captured& c : log) {
      if (c.event == event) return &c;
    }
    return nullptr;
  }
  // Invokes test.add(a, b) and returns the result field.
  int64_t Add(int64_t a, int64_t b) {
    NevrValue fields[3] = {};
    fields[0].type = NEVR_VALUE_INT;
    fields[0].as.i = a;
    fields[1].type = NEVR_VALUE_INT;
    fields[1].as.i = b;
    fields[2].type = NEVR_VALUE_INT;
    fields[2].as.i = 0;
    reg.Invoke(add, fields, [](NevrValue* f, void*) { f[2].as.i = f[0].as.i + f[1].as.i; }, nullptr);
    return fields[2].as.i;
  }
};

NevrValue Int(int64_t i) {
  NevrValue v{};
  v.type = NEVR_VALUE_INT;
  v.as.i = i;
  return v;
}
NevrValue Float(double f) {
  NevrValue v{};
  v.type = NEVR_VALUE_FLOAT;
  v.as.f = f;
  return v;
}
NevrValue Str(const char* s) {
  NevrValue v{};
  v.type = NEVR_VALUE_STRING;
  v.as.s = s;
  return v;
}

int64_t GetInt(const NevrHostApi* api, NevrHookCall* call, const char* field) {
  NevrValue v{};
  api->call_get(call, field, &v);
  return v.as.i;
}

}  // namespace

TEST(api_table_is_versioned) {
  Fixture f;
  CHECK_EQ(f.api->size, static_cast<uint32_t>(sizeof(NevrHostApi)));
  CHECK_EQ(f.api->version, static_cast<uint32_t>(NEVR_HOST_API_VERSION));
  CHECK(std::strcmp(f.api->status_name(NEVR_ERR_CONFLICT), "NEVR_ERR_CONFLICT") == 0);
}

TEST(override_round_trip_copies_strings) {
  Fixture f;
  CHECK(f.keys);
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  NevrValue gravity = Float(-5.0);
  CHECK_EQ(f.api->override_set(a, "physics.gravity", &gravity), NEVR_OK);
  char text[] = "blue";
  NevrValue colour = Str(text);
  CHECK_EQ(f.api->override_set(a, "team.colour", &colour), NEVR_OK);
  text[0] = 'X';  // the host copied it
  NevrValue out{};
  CHECK_EQ(f.api->override_get(a, "physics.gravity", &out), NEVR_OK);
  CHECK_EQ(out.type, NEVR_VALUE_FLOAT);
  CHECK(out.as.f == -5.0);
  CHECK_EQ(f.api->override_get(a, "team.colour", &out), NEVR_OK);
  CHECK(std::strcmp(out.as.s, "blue") == 0);
  CHECK_EQ(f.api->override_get(a, "nope", &out), NEVR_ERR_NOT_FOUND);
  CHECK_EQ(f.Count("override_set"), 2);
}

TEST(second_owner_on_a_key_conflicts_and_both_are_named) {
  Fixture f;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  NevrOwner* b = f.reg.OpenOwner("mod_b");
  NevrValue one = Int(1), two = Int(2), three = Int(3);
  CHECK_EQ(f.api->override_set(a, "match.rounds", &one), NEVR_OK);
  CHECK_EQ(f.api->override_set(b, "match.rounds", &two), NEVR_ERR_CONFLICT);
  CHECK(std::string(f.api->last_error(b)).find("mod_a") != std::string::npos);
  const Captured* c = f.Find("override_conflict");
  CHECK(c != nullptr);
  if (c) {
    CHECK_EQ(c->owner, std::string("mod_b"));
    CHECK_EQ(c->other, std::string("mod_a"));
    CHECK_EQ(c->target, std::string("match.rounds"));
  }
  NevrValue out{};
  f.api->override_get(b, "match.rounds", &out);
  CHECK_EQ(out.as.i, 1);
  // The holder may change its own value.
  CHECK_EQ(f.api->override_set(a, "match.rounds", &three), NEVR_OK);
  f.api->override_get(a, "match.rounds", &out);
  CHECK_EQ(out.as.i, 3);
}

namespace {
const NevrHostApi* g_api = nullptr;
std::vector<std::string> g_order;

NevrHookResult DoubleA(NevrHookCall* call, void*) {
  g_order.push_back("double_a");
  NevrValue v = Float(static_cast<double>(GetInt(g_api, call, "a") * 2));  // an integral number into an INT field
  return g_api->call_set(call, "a", &v) == NEVR_OK ? NEVR_HOOK_CONTINUE : NEVR_HOOK_FAILED;
}
NevrHookResult AddOneToResult(NevrHookCall* call, void*) {
  g_order.push_back("add_one");
  NevrValue v = Int(GetInt(g_api, call, "result") + 1);
  return g_api->call_set(call, "result", &v) == NEVR_OK ? NEVR_HOOK_CONTINUE : NEVR_HOOK_FAILED;
}
NevrHookResult Tag(NevrHookCall*, void* user) {
  g_order.push_back(static_cast<const char*>(user));
  return NEVR_HOOK_CONTINUE;
}
}  // namespace

TEST(override_keys_are_registered_and_typed) {
  Fixture f;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  NevrValue one = Int(1);
  CHECK_EQ(f.api->override_set(a, "no.such.key", &one), NEVR_ERR_UNKNOWN_KEY);
  const Captured* c = f.Find("override_unknown");
  CHECK(c && c->target == "no.such.key" && c->owner == "mod_a");
  NevrValue text = Str("x");
  CHECK_EQ(f.api->override_set(a, "match.rounds", &text), NEVR_ERR_TYPE_MISMATCH);
  CHECK(std::string(f.api->last_error(a)).find("is int, not string") != std::string::npos);
  NevrValue half = Float(2.5), three = Float(3.0), seven = Int(7);
  CHECK_EQ(f.api->override_set(a, "match.rounds", &half), NEVR_ERR_TYPE_MISMATCH);  // not integral
  CHECK_EQ(f.api->override_set(a, "match.rounds", &three), NEVR_OK);                // integral: stored as INT
  NevrValue out{};
  f.api->override_get(a, "match.rounds", &out);
  CHECK(out.type == NEVR_VALUE_INT && out.as.i == 3);
  NevrValue two53 = Float(9007199254740992.0);  // 2^53: past it a double does not hold every integer
  CHECK_EQ(f.api->override_set(a, "match.rounds", &two53), NEVR_ERR_TYPE_MISMATCH);
  NevrValue below = Float(9007199254740991.0);  // 2^53-1
  CHECK_EQ(f.api->override_set(a, "match.rounds", &below), NEVR_OK);
  CHECK_EQ(f.api->override_set(a, "physics.gravity", &seven), NEVR_OK);  // INT into FLOAT
  f.api->override_get(a, "physics.gravity", &out);
  CHECK(out.type == NEVR_VALUE_FLOAT && out.as.f == 7.0);
  CHECK(!f.reg.RegisterOverridePoint("match.rounds", NEVR_VALUE_INT));  // names are unique
}

TEST(pre_and_post_callbacks_change_the_call) {
  Fixture f;
  g_api = f.api;
  g_order.clear();
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  CHECK_EQ(f.Add(2, 3), 5);  // no callbacks: the original alone
  CHECK_EQ(f.api->hook_add(a, "test.add", NEVR_HOOK_PRE, DoubleA, nullptr), NEVR_OK);
  CHECK_EQ(f.api->hook_add(a, "test.add", NEVR_HOOK_POST, AddOneToResult, nullptr), NEVR_OK);
  CHECK_EQ(f.Add(2, 3), 8);  // (2*2)+3, then +1
}

TEST(callbacks_chain_in_owner_order_not_registration_order) {
  Fixture f;
  g_api = f.api;
  g_order.clear();
  NevrOwner* first = f.reg.OpenOwner("first");
  NevrOwner* second = f.reg.OpenOwner("second");
  static char kSecond[] = "second", kFirst[] = "first", kFirst2[] = "first_again";
  f.api->hook_add(second, "test.add", NEVR_HOOK_PRE, Tag, kSecond);
  f.api->hook_add(first, "test.add", NEVR_HOOK_PRE, Tag, kFirst);
  f.api->hook_add(first, "test.add", NEVR_HOOK_PRE, Tag, kFirst2);
  f.Add(1, 1);
  CHECK_EQ(g_order.size(), static_cast<size_t>(3));
  if (g_order.size() == 3) {
    CHECK_EQ(g_order[0], std::string("first"));
    CHECK_EQ(g_order[1], std::string("first_again"));
    CHECK_EQ(g_order[2], std::string("second"));
  }
}

namespace {
NevrStatus g_status_post_sets_a = NEVR_OK, g_status_wrong_type = NEVR_OK, g_status_unknown = NEVR_OK;
NevrHookResult PostTriesToSetA(NevrHookCall* call, void*) {
  NevrValue v = Int(9);
  g_status_post_sets_a = g_api->call_set(call, "a", &v);
  NevrValue s = Str("x");
  g_status_wrong_type = g_api->call_set(call, "result", &s);
  g_status_unknown = g_api->call_set(call, "nope", &v);
  return NEVR_HOOK_CONTINUE;
}
NevrHookResult SkipWithResult(NevrHookCall* call, void*) {
  NevrValue v = Int(42);
  g_api->call_set(call, "result", &v);
  return NEVR_HOOK_SKIP_ORIGINAL;
}
}  // namespace

TEST(fields_enforce_phase_type_and_name) {
  Fixture f;
  g_api = f.api;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  f.api->hook_add(a, "test.add", NEVR_HOOK_POST, PostTriesToSetA, nullptr);
  CHECK_EQ(f.Add(2, 3), 5);
  CHECK_EQ(g_status_post_sets_a, NEVR_ERR_READ_ONLY);
  CHECK_EQ(g_status_wrong_type, NEVR_ERR_TYPE_MISMATCH);
  CHECK_EQ(g_status_unknown, NEVR_ERR_UNKNOWN_FIELD);
}

TEST(pre_can_skip_the_original) {
  Fixture f;
  g_api = f.api;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  f.api->hook_add(a, "test.add", NEVR_HOOK_PRE, SkipWithResult, nullptr);
  CHECK_EQ(f.Add(2, 3), 42);
}

namespace {
NevrHookResult Fails(NevrHookCall* call, void*) { return g_api->call_fail(call, "boom at x.lua:3"); }
}  // namespace

TEST(failed_callback_is_logged_and_the_call_goes_on) {
  Fixture f;
  g_api = f.api;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  NevrOwner* b = f.reg.OpenOwner("mod_b");
  f.api->hook_add(a, "test.add", NEVR_HOOK_PRE, Fails, nullptr);
  f.api->hook_add(b, "test.add", NEVR_HOOK_POST, AddOneToResult, nullptr);
  CHECK_EQ(f.Add(2, 3), 6);  // original ran, b's post still ran
  const Captured* c = f.Find("callback_failed");
  CHECK(c != nullptr);
  if (c) {
    CHECK_EQ(c->owner, std::string("mod_a"));
    CHECK_EQ(c->target, std::string("test.add"));
    CHECK(c->detail.find("boom at x.lua:3") != std::string::npos);
  }
}

namespace {
Registry* g_reg = nullptr;
int g_runs = 0;
NevrHookResult DisablesItself(NevrHookCall* call, void*) {
  ++g_runs;
  g_reg->DisableOwner(const_cast<NevrOwner*>(call->current), "instruction budget exceeded");
  return NEVR_HOOK_CONTINUE;
}
}  // namespace

TEST(disabled_owner_is_skipped_and_loses_its_overrides) {
  Fixture f;
  g_api = f.api;
  g_reg = &f.reg;
  g_runs = 0;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  NevrValue v = Int(7);
  f.api->override_set(a, "k", &v);
  f.api->hook_add(a, "test.add", NEVR_HOOK_PRE, DisablesItself, nullptr);
  f.api->hook_add(a, "test.add", NEVR_HOOK_POST, AddOneToResult, nullptr);
  CHECK_EQ(f.Add(2, 3), 5);  // its post callback in the same call is skipped
  CHECK_EQ(f.Add(2, 3), 5);
  CHECK_EQ(g_runs, 1);
  NevrValue out{};
  CHECK_EQ(f.api->override_get(a, "k", &out), NEVR_ERR_NOT_FOUND);
  CHECK_EQ(f.api->override_set(a, "k", &v), NEVR_ERR_DISABLED);
  CHECK_EQ(f.api->hook_add(a, "test.add", NEVR_HOOK_PRE, Tag, nullptr), NEVR_ERR_DISABLED);
  CHECK_EQ(f.Count("owner_disabled"), 1);
  const Captured* c = f.Find("owner_disabled");
  CHECK(c && c->detail.find("instruction budget exceeded") != std::string::npos);
  // N2 (re-review of #458): what a disabled owner is refused, and a breach on an
  // owner already disabled, are recorded.
  CHECK_EQ(f.Count("refused_disabled"), 2);
  const Captured* refused = f.Find("refused_disabled");
  CHECK(refused && refused->target == "k");  // names what was refused
  f.reg.DisableOwner(a, "time budget exceeded");
  CHECK_EQ(f.Count("owner_disabled"), 2);
  CHECK(f.log.back().detail.find("already disabled; time budget exceeded") != std::string::npos);
}

TEST(reset_owner_keeps_its_place_in_the_order) {
  Fixture f;
  g_api = f.api;
  g_order.clear();
  NevrOwner* first = f.reg.OpenOwner("first");
  NevrOwner* second = f.reg.OpenOwner("second");
  static char kSecond[] = "second", kFirst[] = "first";
  f.api->hook_add(first, "test.add", NEVR_HOOK_PRE, Tag, kFirst);
  f.api->hook_add(second, "test.add", NEVR_HOOK_PRE, Tag, kSecond);
  f.reg.DisableOwner(first, "test");
  f.reg.ResetOwner(first);  // hot reload: it registers again, after `second` in time
  f.api->hook_add(first, "test.add", NEVR_HOOK_PRE, Tag, kFirst);
  f.Add(1, 1);
  CHECK_EQ(g_order.size(), static_cast<size_t>(2));
  if (g_order.size() == 2) {
    CHECK_EQ(g_order[0], std::string("first"));
    CHECK_EQ(g_order[1], std::string("second"));
  }
  CHECK_EQ(f.Count("owner_reset"), 1);
}

TEST(unknown_hook_is_refused_with_a_named_line) {
  Fixture f;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  CHECK_EQ(f.api->hook_add(a, "no.such.hook", NEVR_HOOK_PRE, Tag, nullptr), NEVR_ERR_UNKNOWN_HOOK);
  const Captured* c = f.Find("hook_unknown");
  CHECK(c && c->target == "no.such.hook" && c->owner == "mod_a");
  CHECK(f.reg.RegisterHookPoint("test.add", {}) == nullptr);  // names are unique
}

TEST(owner_log_is_attributed) {
  Fixture f;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  f.api->log(a, NEVR_LOG_INFO, "hello");
  const Captured* c = f.Find("owner_log");
  CHECK(c && c->owner == "mod_a" && c->detail == "hello");
}

namespace {
// Stands in for a binding's per-callback data: freed after the owner is reset.
struct Alive {
  std::atomic<bool> alive{true};
  std::atomic<int> used_after_free{0};
  std::atomic<int> runs{0};
};
NevrHookResult SlowCallback(NevrHookCall*, void* user) {
  Alive* a = static_cast<Alive*>(user);
  a->runs.fetch_add(1);
  for (int i = 0; i < 200; ++i) {
    if (!a->alive.load()) a->used_after_free.fetch_add(1);
    std::this_thread::yield();
  }
  return NEVR_HOOK_CONTINUE;
}
}  // namespace

// A game thread may be inside an owner's callback, or hold the old chain, while
// the owner is reset for a reload. Once ResetOwner returns, none of the owner's
// earlier callbacks may run, so the binding can free what they use.
TEST(reset_waits_out_callbacks_already_running) {
  Fixture f;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  std::atomic<bool> stop{false};
  std::vector<std::thread> game;
  for (int t = 0; t < 4; ++t) {
    game.emplace_back([&f, &stop] {
      while (!stop.load()) f.Add(1, 1);
    });
  }
  int total_runs = 0, violations = 0;
  for (int round = 0; round < 200; ++round) {
    auto data = std::make_unique<Alive>();
    f.api->hook_add(a, "test.add", NEVR_HOOK_PRE, SlowCallback, data.get());
    while (data->runs.load() == 0) std::this_thread::yield();  // a game thread is in it
    CHECK(f.reg.ResetOwner(a));
    data->alive.store(false);  // the binding frees its callback data now
    for (int i = 0; i < 2000; ++i) std::this_thread::yield();
    violations += data->used_after_free.load();
    total_runs += data->runs.load();
    // keep `data` allocated so a violation is counted, not a crash
    static std::vector<std::unique_ptr<Alive>> graveyard;
    graveyard.push_back(std::move(data));
  }
  stop.store(true);
  for (std::thread& t : game) t.join();
  CHECK_EQ(violations, 0);
  CHECK(total_runs >= 200);
}

namespace {
std::atomic<int> g_blocker_state{0};  // 0 idle, 1 inside and waiting, 2 released
NevrHookResult Blocker(NevrHookCall*, void*) {
  g_blocker_state.store(1);
  while (g_blocker_state.load() != 2) std::this_thread::yield();
  return NEVR_HOOK_CONTINUE;
}
}  // namespace

// Deterministic form of the window the stress test above rarely hits: a game
// thread holds the old chain, stalled in an earlier owner's callback, while a
// later owner is reset and its data freed. When the thread walks on, it must
// not call the later owner's retired callback.
TEST(reset_owner_is_not_called_from_a_chain_loaded_before_the_reset) {
  Fixture f;
  NevrOwner* blocker = f.reg.OpenOwner("blocker");
  NevrOwner* victim = f.reg.OpenOwner("victim");
  g_blocker_state.store(0);
  f.api->hook_add(blocker, "test.add", NEVR_HOOK_PRE, Blocker, nullptr);
  auto data = std::make_unique<Alive>();
  f.api->hook_add(victim, "test.add", NEVR_HOOK_PRE, SlowCallback, data.get());
  std::thread game([&f] { f.Add(1, 1); });
  while (g_blocker_state.load() != 1) std::this_thread::yield();
  CHECK(f.reg.ResetOwner(victim));  // the game thread is not inside victim's gate yet
  data->alive.store(false);         // freed
  g_blocker_state.store(2);
  game.join();
  CHECK_EQ(data->runs.load(), 0);
  CHECK_EQ(data->used_after_free.load(), 0);
}

namespace {
// F1 (review of #458): a callback still running while its owner is reset
// registers another callback. That one must not survive the reset.
NevrOwner* g_late_owner = nullptr;
Alive* g_late_data = nullptr;
std::atomic<NevrStatus> g_late_status{NEVR_OK};
std::atomic<int> g_registrar_entered{0};
NevrHookResult RegistersDuringReset(NevrHookCall*, void*) {
  // Read the generation before announcing entry: the test resets only after
  // the announcement, so the bump Quiesce makes is always after `before`.
  const uint64_t before = g_late_owner->generation.load();
  if (g_registrar_entered.fetch_add(1) != 0) return NEVR_HOOK_CONTINUE;
  while (g_late_owner->generation.load() == before) std::this_thread::yield();  // Quiesce has started
  g_late_status.store(g_api->hook_add(g_late_owner, "test.add", NEVR_HOOK_PRE, SlowCallback, g_late_data));
  return NEVR_HOOK_CONTINUE;
}
}  // namespace

TEST(callback_registered_by_a_running_callback_during_reset_does_not_survive) {
  Fixture f;
  g_api = f.api;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  auto late = std::make_unique<Alive>();
  g_late_owner = a;
  g_late_data = late.get();
  g_registrar_entered.store(0);
  f.api->hook_add(a, "test.add", NEVR_HOOK_PRE, RegistersDuringReset, nullptr);
  std::thread game([&f] { f.Add(1, 1); });
  while (g_registrar_entered.load() == 0) std::this_thread::yield();
  CHECK(f.reg.ResetOwner(a));
  game.join();
  late->alive.store(false);  // the binding frees the state the late callback would use
  for (int i = 0; i < 5; ++i) f.Add(1, 1);
  CHECK_EQ(g_late_status.load(), NEVR_ERR_DISABLED);
  CHECK_EQ(late->runs.load(), 0);
  CHECK_EQ(late->used_after_free.load(), 0);
}

// F2 (review of #458): last_error is per thread, so two game threads failing
// calls for one owner neither race nor read each other's reason.
TEST(last_error_is_per_thread) {
  Fixture f;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  std::atomic<int> wrong{0};
  auto worker = [&](const char* key) {
    NevrValue v = Int(1);
    for (int i = 0; i < 2000; ++i) {
      f.api->override_set(a, key, &v);  // unknown key: fails, naming it
      if (std::string(f.api->last_error(a)).find(key) == std::string::npos) wrong.fetch_add(1);
    }
  };
  std::thread t1(worker, "no.such.one");
  std::thread t2(worker, "no.such.two");
  t1.join();
  t2.join();
  CHECK_EQ(wrong.load(), 0);
}

namespace {
bool g_quiesce_from_inside = true;
NevrHookResult QuiescesItself(NevrHookCall* call, void*) {
  g_quiesce_from_inside = g_reg->Quiesce(const_cast<NevrOwner*>(call->current), std::chrono::milliseconds(50));
  return NEVR_HOOK_CONTINUE;
}
}  // namespace

TEST(quiesce_from_the_owners_own_callback_is_refused) {
  Fixture f;
  g_reg = &f.reg;
  NevrOwner* a = f.reg.OpenOwner("mod_a");
  f.api->hook_add(a, "test.add", NEVR_HOOK_PRE, QuiescesItself, nullptr);
  f.Add(1, 1);
  CHECK(!g_quiesce_from_inside);
  const Captured* c = f.Find("quiesce_failed");
  CHECK(c && c->detail.find("wait on itself") != std::string::npos);
}

TEST(cap_hit_verdicts) {
  using nevr_script::CapHit;
  using nevr_script::CapVerdict;
  using nevr_script::JudgeCapHit;
  const size_t cap = 16;
  CHECK(JudgeCapHit(CapHit{2, 4, cap, 1}) == CapVerdict::kGarbage);
  CHECK(JudgeCapHit(CapHit{2, 4, cap, 2}) == CapVerdict::kGarbage);
  CHECK(JudgeCapHit(CapHit{2, 4, cap, 3}) == CapVerdict::kRepeated);  // the third call in a row
  CHECK(JudgeCapHit(CapHit{9, 4, cap, 1}) == CapVerdict::kLiveSetTooLarge);
  CHECK(JudgeCapHit(CapHit{8, 8, cap, 1}) == CapVerdict::kGarbage);  // exactly half is not over half
  CHECK(JudgeCapHit(CapHit{2, 9, cap, 1}) == CapVerdict::kRequestTooLarge);
  CHECK(JudgeCapHit(CapHit{9, 9, cap, 3}) == CapVerdict::kLiveSetTooLarge);  // the most specific reason first
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }

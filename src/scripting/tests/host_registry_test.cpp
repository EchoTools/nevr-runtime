// Unit tests for the host API registry (src/scripting/host_registry.h): the
// override and hook-point contract every plugin and script binding relies on.
#include "scripting/host_registry.h"

#include <cstring>
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
  NevrValue v = Int(GetInt(g_api, call, "a") * 2);
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

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }

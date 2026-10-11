// Conformance tests for a script VM binding (src/scripting/script_vm.h). Every
// binding links this same file and must pass it unchanged; the scripts below
// stay in the subset Lua 5.1, LuaJIT, Lua 5.4 and Luau all parse.
//
//   conformance_test [filter]   the tests (T1-T8, sandbox probes); NEVR_SCRIPT_VERBOSE=1
//                               also writes every registry log record to stderr as JSON
//   conformance_test --bench    M1 per-call cost and M3 state size, one JSON object per line
//   conformance_test --pattern-dos
//                               T5d: a pattern-matching backtrack inside one C call;
//                               reports whether the binding stopped it, under a 20 s watchdog
//   conformance_test --gc-dos   T5e: a __gc finalizer that loops forever; same reporting
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/tests/mini_test.h"
#include "scripting/host_registry.h"
#include "scripting/script_vm.h"

namespace {

using nevr_script::FieldSpec;
using nevr_script::HookPoint;
using nevr_script::LogRecord;
using nevr_script::Registry;
using nevr_script::ScriptVm;
using nevr_script::VmLimits;

struct Captured {
  std::string event, owner, other, target, detail;
};

bool g_verbose = false;

void AddOriginal(NevrValue* f, void*) { f[2].as.i = f[0].as.i + f[1].as.i; }

struct Host {
  std::vector<Captured> log;
  Registry reg{[this](const LogRecord& r) {
    log.push_back({r.event, r.owner, r.other_owner, r.target, r.detail});
    if (g_verbose) {
      nlohmann::json line = {{"event", r.event}, {"owner", r.owner}, {"other_owner", r.other_owner},
                             {"target", r.target}, {"detail", r.detail}, {"level", r.level}};
      std::fprintf(stderr, "%s\n", line.dump().c_str());
    }
  }};
  const NevrHostApi* api = reg.Api();
  HookPoint* add = reg.RegisterHookPoint(
      "test.add", {{"a", NEVR_VALUE_INT, true, false},
                   {"b", NEVR_VALUE_INT, true, false},
                   {"result", NEVR_VALUE_INT, true, true}});
  std::unique_ptr<ScriptVm> vm;

  // The override points the scripts below use, as the runtime would register them.
  explicit Host(VmLimits limits = VmLimits()) {
    const std::pair<const char*, NevrValueType> keys[] = {
        {"physics.gravity", NEVR_VALUE_FLOAT}, {"team.colour", NEVR_VALUE_STRING},
        {"hud.visible", NEVR_VALUE_BOOL},      {"match.rounds", NEVR_VALUE_INT},
        {"mod_a.err", NEVR_VALUE_STRING},      {"mod_b.seen_error", NEVR_VALUE_STRING},
        {"escape", NEVR_VALUE_BOOL}};
    for (const auto& key : keys) reg.RegisterOverridePoint(key.first, key.second);
    vm = nevr_script::CreateScriptVm(reg, limits);
  }

  int64_t Add(int64_t a, int64_t b) {
    NevrValue fields[3] = {};
    for (NevrValue& v : fields) v.type = NEVR_VALUE_INT;
    fields[0].as.i = a;
    fields[1].as.i = b;
    reg.Invoke(add, fields, AddOriginal, nullptr);
    return fields[2].as.i;
  }

  // Loads `source` as a new owner named after the chunk ("mod_a.lua" -> "mod_a").
  NevrOwner* Load(const std::string& chunk, const std::string& source, bool expect_ok = true,
                  std::string* error_out = nullptr) {
    NevrOwner* owner = reg.OpenOwner(chunk.substr(0, chunk.rfind('.')));
    std::string error;
    const bool ok = vm->Load(owner, chunk, source, &error);
    if (ok != expect_ok) {
      std::fprintf(stderr, "  Load(%s) returned %s, expected %s; error: %s\n", chunk.c_str(),
                   ok ? "true" : "false", expect_ok ? "true" : "false", error.c_str());
      ++mini_test::Failures();
    }
    if (error_out) *error_out = error;
    return owner;
  }

  const Captured* Find(const char* event, const std::string& owner = "") const {
    for (const Captured& c : log) {
      if (c.event == event && (owner.empty() || c.owner == owner)) return &c;
    }
    return nullptr;
  }
  int Count(const char* event) const {
    int n = 0;
    for (const Captured& c : log) n += c.event == event;
    return n;
  }
  bool HasOverride(NevrOwner* asker, const char* key) {
    NevrValue v{};
    return api->override_get(asker, key, &v) == NEVR_OK;
  }
};

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

}  // namespace

// ---- T1 override through the C ABI ----------------------------------------------------------

TEST(t1_override_reaches_the_registry) {
  Host h;
  NevrOwner* a = h.Load("mod_a.lua", "nevr.override('physics.gravity', -5.0)\n"
                                     "nevr.override('team.colour', 'blue')\n"
                                     "nevr.override('hud.visible', false)\n");
  NevrValue v{};
  CHECK_EQ(h.api->override_get(a, "physics.gravity", &v), NEVR_OK);
  CHECK_EQ(v.type, NEVR_VALUE_FLOAT);
  CHECK(v.as.f == -5.0);
  CHECK_EQ(h.api->override_get(a, "team.colour", &v), NEVR_OK);
  CHECK(v.type == NEVR_VALUE_STRING && std::strcmp(v.as.s, "blue") == 0);
  CHECK_EQ(h.api->override_get(a, "hud.visible", &v), NEVR_OK);
  CHECK(v.type == NEVR_VALUE_BOOL && v.as.b == 0);
}

// ---- T2 conflict ----------------------------------------------------------------------------

TEST(t2_second_script_on_a_key_gets_a_conflict_naming_both) {
  Host h;
  h.Load("mod_a.lua", "assert(nevr.override('match.rounds', 3))\n");
  NevrOwner* b = h.Load("mod_b.lua",
                        "local ok, err = nevr.override('match.rounds', 5)\n"
                        "assert(ok == nil)\n"
                        "nevr.override('mod_b.seen_error', err)\n");
  NevrValue v{};
  CHECK_EQ(h.api->override_get(b, "mod_b.seen_error", &v), NEVR_OK);
  const std::string err = v.type == NEVR_VALUE_STRING ? v.as.s : "";
  CHECK(Contains(err, "NEVR_ERR_CONFLICT"));
  CHECK(Contains(err, "mod_a"));
  const Captured* c = h.Find("override_conflict");
  CHECK(c && c->owner == "mod_b" && c->other == "mod_a" && c->target == "match.rounds");
  CHECK_EQ(h.api->override_get(b, "match.rounds", &v), NEVR_OK);
  CHECK(v.type == NEVR_VALUE_INT && v.as.i == 3);  // a script number, stored as the key's INT
}

TEST(t2_unknown_or_mistyped_key_returns_nil_and_status) {
  Host h;
  NevrOwner* a = h.Load("mod_a.lua",
                        "local ok, err = nevr.override('no.such.key', 1)\n"
                        "assert(ok == nil)\n"
                        "local ok2, err2 = nevr.override('match.rounds', 2.5)\n"
                        "assert(ok2 == nil)\n"
                        "nevr.override('mod_a.err', err .. ' / ' .. err2)\n");
  NevrValue v{};
  CHECK_EQ(h.api->override_get(a, "mod_a.err", &v), NEVR_OK);
  const std::string err = v.type == NEVR_VALUE_STRING ? v.as.s : "";
  CHECK(Contains(err, "NEVR_ERR_UNKNOWN_KEY") && Contains(err, "NEVR_ERR_TYPE_MISMATCH"));
}

// ---- T3 pre/post callbacks ------------------------------------------------------------------

const char* kSample =
    "-- double a before the call, add one to the result after it\n"
    "assert(nevr.hook('test.add', {\n"
    "  pre = function(h) h:set('a', h:get('a') * 2) end,\n"
    "  post = function(h) h:set('result', h:get('result') + 1) end,\n"
    "}))\n";

TEST(t3_pre_and_post_callbacks_change_the_call) {
  Host h;
  CHECK_EQ(h.Add(2, 3), 5);
  h.Load("mod_a.lua", kSample);
  CHECK_EQ(h.Add(2, 3), 8);
  CHECK_EQ(h.Add(10, 1), 22);
}

TEST(t3_callbacks_chain_in_load_order) {
  Host h;
  h.Load("mod_a.lua", "nevr.hook('test.add', {post = function(h) h:set('result', h:get('result') * 10) end})\n");
  h.Load("mod_b.lua", "nevr.hook('test.add', {post = function(h) h:set('result', h:get('result') + 1) end})\n");
  CHECK_EQ(h.Add(2, 3), 51);  // (5 * 10) + 1: mod_a then mod_b
}

TEST(t3_pre_can_skip_the_original) {
  Host h;
  h.Load("mod_a.lua", "nevr.hook('test.add', {pre = function(h) h:set('result', 42) h:skip() end})\n");
  CHECK_EQ(h.Add(2, 3), 42);
}

TEST(t3_unknown_hook_returns_nil_and_status) {
  Host h;
  NevrOwner* a = h.Load("mod_a.lua",
                        "local ok, err = nevr.hook('no.such.hook', {pre = function(h) end})\n"
                        "assert(ok == nil)\n"
                        "nevr.override('mod_a.err', err)\n");
  NevrValue v{};
  CHECK_EQ(h.api->override_get(a, "mod_a.err", &v), NEVR_OK);
  CHECK(v.type == NEVR_VALUE_STRING && Contains(v.as.s, "NEVR_ERR_UNKNOWN_HOOK"));
  CHECK(h.Find("hook_unknown", "mod_a") != nullptr);
}

// ---- T4 errors are contained ----------------------------------------------------------------

TEST(t4_callback_error_is_contained_with_chunk_and_line) {
  Host h;
  NevrOwner* a = h.Load("t4_error.lua",
                        "nevr.hook('test.add', {\n"
                        "  pre = function(h)\n"
                        "    error('boom')\n"
                        "  end})\n");
  CHECK_EQ(h.Add(2, 3), 5);  // the original still ran
  CHECK_EQ(h.Add(2, 3), 5);
  CHECK_EQ(h.Count("callback_failed"), 2);  // an error does not disable the script
  const Captured* c = h.Find("callback_failed", "t4_error");
  CHECK(c && Contains(c->detail, "t4_error.lua:3:") && Contains(c->detail, "boom"));
  CHECK(!a->disabled.load());
}

TEST(t4_set_on_a_read_only_field_fails_the_callback) {
  Host h;
  h.Load("mod_a.lua", "nevr.hook('test.add', {post = function(h) h:set('a', 1) end})\n");
  CHECK_EQ(h.Add(2, 3), 5);
  const Captured* c = h.Find("callback_failed", "mod_a");
  CHECK(c && Contains(c->detail, "NEVR_ERR_READ_ONLY") && Contains(c->detail, "mod_a.lua:1:"));
}

TEST(t4_load_error_names_chunk_and_line) {
  Host h;
  std::string error;
  h.Load("t4_syntax.lua", "local x = 1\nlocal y = = 2\n", false, &error);
  CHECK(Contains(error, "t4_syntax.lua:2:"));
  h.Load("t4_runtime.lua", "local x = 1\nerror('top')\n", false, &error);
  CHECK(Contains(error, "t4_runtime.lua:2:") && Contains(error, "top"));
}

// ---- T5 runaway code is stopped -------------------------------------------------------------

namespace {
void ExpectStoppedAndDisabled(Host& h, NevrOwner* owner, double elapsed_ms) {
  CHECK(owner->disabled.load());
  const Captured* c = h.Find("owner_disabled", owner->name);
  CHECK(c != nullptr);
  if (c) {
    const bool named = Contains(c->detail, "instruction") || Contains(c->detail, "time");
    CHECK(named);
  }
  std::printf("  stopped after %.1f ms\n", elapsed_ms);
}
double MsSince(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

TEST(t5_runaway_callback_is_stopped_and_the_script_disabled) {
  Host h;
  NevrOwner* a = h.Load("mod_a.lua", "nevr.hook('test.add', {pre = function(h) while true do end end})\n");
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_EQ(h.Add(2, 3), 5);
  ExpectStoppedAndDisabled(h, a, MsSince(t0));
  CHECK_EQ(h.Add(2, 3), 5);  // skipped from now on
  CHECK_EQ(h.Count("owner_disabled"), 1);
}

TEST(t5_runaway_top_level_is_stopped) {
  Host h;
  const auto t0 = std::chrono::steady_clock::now();
  NevrOwner* a = h.Load("mod_a.lua", "local n = 0\nwhile true do n = n + 1 end\n", false);
  ExpectStoppedAndDisabled(h, a, MsSince(t0));
}

TEST(t5_runaway_inside_a_coroutine_is_stopped) {
  Host h;
  NevrOwner* a = h.Load("mod_a.lua",
                        "nevr.hook('test.add', {pre = function(h)\n"
                        "  coroutine.wrap(function() while true do end end)()\n"
                        "end})\n");
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_EQ(h.Add(2, 3), 5);
  ExpectStoppedAndDisabled(h, a, MsSince(t0));
}

TEST(t5_runaway_caught_by_pcall_is_still_stopped) {
  Host h;
  NevrOwner* a = h.Load("mod_a.lua",
                        "nevr.hook('test.add', {pre = function(h)\n"
                        "  while true do pcall(function() while true do end end) end\n"
                        "end})\n");
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_EQ(h.Add(2, 3), 5);
  ExpectStoppedAndDisabled(h, a, MsSince(t0));
}

// ---- T6 sandbox -----------------------------------------------------------------------------
// Each probe sets the override "escape" only if it reached what it must not.
// The control probe proves the sensor works.

namespace {
struct Probe {
  const char* name;
  const char* code;
};
const Probe kProbes[] = {
    {"control", "nevr.override('escape', true)"},
    {"io", "if io ~= nil then nevr.override('escape', true) end"},
    {"os", "if os ~= nil and (os.execute or os.remove or os.rename or os.getenv or os.exit or os.tmpname "
           "or os.setlocale) then nevr.override('escape', true) end"},
    {"package", "if package ~= nil then nevr.override('escape', true) end"},
    {"require", "if require ~= nil then local ok, m = pcall(require, 'os') "
                "if ok and m then nevr.override('escape', true) end end"},
    {"dofile_loadfile", "if dofile or loadfile then nevr.override('escape', true) end"},
    {"load_loadstring", "if load or loadstring then nevr.override('escape', true) end"},
    {"string_dump", "if string.dump then nevr.override('escape', true) end"},
    {"debug", "if debug ~= nil and (debug.getregistry or debug.getupvalue or debug.setupvalue or "
              "debug.sethook or debug.getlocal or debug.setlocal or debug.getmetatable or "
              "debug.setmetatable or debug.upvaluejoin) then nevr.override('escape', true) end"},
    {"collectgarbage", "if collectgarbage and pcall(collectgarbage, 'stop') then nevr.override('escape', true) end"},
    {"ffi_jit", "if ffi or jit then nevr.override('escape', true) end"},
    {"string_metatable", "if type(getmetatable('')) == 'table' then nevr.override('escape', true) end"},
    {"fenv", "if getfenv or setfenv then nevr.override('escape', true) end"},
    {"newproxy", "if newproxy then nevr.override('escape', true) end"},
};
}  // namespace

TEST(t6_sandbox_refuses_every_probe) {
  for (const Probe& p : kProbes) {
    Host h;
    NevrOwner* owner = h.Load(std::string(p.name) + ".lua", p.code);
    const bool escaped = h.HasOverride(owner, "escape");
    const bool control = std::strcmp(p.name, "control") == 0;
    std::printf("  probe %-18s %s\n", p.name,
                escaped == control ? (control ? "sensor works" : "refused") : "ESCAPED / sensor broken");
    if (escaped != control) ++mini_test::Failures();
  }
}

TEST(t6_globals_are_per_script) {
  Host h;
  // A VM may refuse the write to a library table (Luau's luaL_sandbox makes them
  // read-only); what matters is that mod_a cannot change what mod_b sees.
  h.Load("mod_a.lua", "shared_secret = 1\npcall(function() string.rep = nil end)\n");
  NevrOwner* b = h.Load("mod_b.lua",
                        "if shared_secret ~= nil then nevr.override('escape', true) end\n"
                        "if string.rep == nil then nevr.override('escape', true) end\n");
  CHECK(!h.HasOverride(b, "escape"));
}

// ---- T7 memory cap --------------------------------------------------------------------------

TEST(t7_memory_bomb_at_top_level_is_refused) {
  VmLimits limits;
  limits.memory_bytes = 4u << 20;
  // Only the memory cap may stop these: under emulation (qemu) the default
  // time budget fired first and the test measured the wrong limit.
  limits.instructions_per_call = UINT64_MAX;
  limits.millis_per_call = 60000;
  Host h(limits);
  NevrOwner* a = h.Load("mod_a.lua",
                        "local t = {}\n"
                        "for i = 1, 100000000 do t[i] = string.rep('x', 100) .. i end\n", false);
  CHECK(a->disabled.load());
  const Captured* c = h.Find("owner_disabled", "mod_a");
  CHECK(c && Contains(c->detail, "memory"));
  CHECK(h.vm->MemoryBytes(a) <= limits.memory_bytes);
}

TEST(t7_memory_bomb_in_a_callback_is_contained) {
  VmLimits limits;
  limits.memory_bytes = 4u << 20;
  // Only the memory cap may stop these: under emulation (qemu) the default
  // time budget fired first and the test measured the wrong limit.
  limits.instructions_per_call = UINT64_MAX;
  limits.millis_per_call = 60000;
  Host h(limits);
  NevrOwner* a = h.Load("mod_a.lua",
                        "nevr.hook('test.add', {pre = function(h) local s = string.rep('x', 64 * 1024 * 1024) end})\n");
  CHECK_EQ(h.Add(2, 3), 5);
  CHECK(a->disabled.load());
  const Captured* c = h.Find("owner_disabled", "mod_a");
  CHECK(c && Contains(c->detail, "memory"));
}

// ---- T8 hot reload --------------------------------------------------------------------------

TEST(t8_reload_replaces_behaviour_and_keeps_order) {
  Host h;
  NevrOwner* a = h.Load("mod_a.lua", "nevr.hook('test.add', {post = function(h) h:set('result', h:get('result') * 10) end})\n");
  h.Load("mod_b.lua", "nevr.hook('test.add', {post = function(h) h:set('result', h:get('result') + 1) end})\n");
  CHECK_EQ(h.Add(2, 3), 51);
  h.vm->Unload(a);
  h.reg.ResetOwner(a);
  std::string error;
  CHECK(h.vm->Load(a, "mod_a.lua",
                   "nevr.hook('test.add', {post = function(h) h:set('result', h:get('result') * 100) end})\n", &error));
  CHECK_EQ(h.Add(2, 3), 501);  // mod_a still runs first
}

// ---- M1 / M3 --------------------------------------------------------------------------------

namespace {

NevrHookResult NativeGetSet(NevrHookCall* call, void* user) {
  const NevrHostApi* api = static_cast<const NevrHostApi*>(user);
  NevrValue v{};
  api->call_get(call, "a", &v);
  v.as.i += 1;
  api->call_set(call, "a", &v);
  return NEVR_HOOK_CONTINUE;
}

double MedianNsPerCall(Host& h, int calls) {
  std::vector<double> runs;
  for (int r = 0; r < 5; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    int64_t sink = 0;
    for (int i = 0; i < calls; ++i) sink += h.Add(i, 1);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
    if (sink == 42) std::printf(" ");  // keep the loop observable
    runs.push_back(ns / calls);
  }
  std::sort(runs.begin(), runs.end());
  return runs[runs.size() / 2];
}

void Report(const Host& h, const char* metric, const char* scenario, double value, const char* unit) {
  nlohmann::json line = {{"vm", h.vm->Name()}, {"metric", metric}, {"case", scenario},
                         {"value", value}, {"unit", unit}};
  std::printf("%s\n", line.dump().c_str());
}

int Bench() {
  const int kCalls = 1000000;
  {
    Host h;
    Report(h, "M1", "no_callback", MedianNsPerCall(h, kCalls), "ns/call");
  }
  {
    Host h;
    NevrOwner* native = h.reg.OpenOwner("native");
    h.api->hook_add(native, "test.add", NEVR_HOOK_PRE, NativeGetSet, const_cast<NevrHostApi*>(h.api));
    Report(h, "M1", "native_get_set_pre", MedianNsPerCall(h, kCalls), "ns/call");
  }
  {
    Host h;
    h.Load("empty.lua", "nevr.hook('test.add', {pre = function(h) end})\n");
    Report(h, "M1", "script_empty_pre", MedianNsPerCall(h, kCalls), "ns/call");
  }
  {
    Host h;
    h.Load("getset.lua", "nevr.hook('test.add', {pre = function(h) h:set('a', h:get('a') + 1) end})\n");
    Report(h, "M1", "script_get_set_pre", MedianNsPerCall(h, kCalls), "ns/call");
  }
  {
    Host h;
    NevrOwner* a = h.Load("sample.lua", kSample);
    Report(h, "M3", "state_after_sample", static_cast<double>(h.vm->MemoryBytes(a)), "bytes");
  }
  return mini_test::Failures() == 0 ? 0 : 1;
}

// T5d: one C call that backtracks for a very long time. Instruction hooks do
// not run inside a C function, so only a binding that bounds the pattern
// functions themselves stops this. Reports what happened; a watchdog ends the
// run if the call never returns.
int PatternDos() {
  Host h;
  NevrOwner* a = h.Load("dos.lua",
                        "nevr.hook('test.add', {pre = function(h)\n"
                        "  local s = string.rep('a', 200000)\n"
                        "  string.find(s, '.-.-.-.-.-b')\n"
                        "end})\n");
  std::atomic<bool> done{false};
  std::thread watchdog([&done] {
    for (int i = 0; i < 200 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!done.load()) {
      std::printf("T5d pattern_dos: NOT STOPPED after 20000 ms (the call is still running)\n");
      std::fflush(stdout);
      std::_Exit(2);
    }
  });
  const auto t0 = std::chrono::steady_clock::now();
  h.Add(2, 3);
  const double ms = MsSince(t0);
  done.store(true);
  watchdog.join();
  std::printf("T5d pattern_dos: returned after %.1f ms, owner %s\n", ms,
              a->disabled.load() ? "disabled" : "still enabled");
  return 0;
}

// T5e: a finalizer that never returns. Lua 5.4 runs __gc with hooks switched
// off (lgc.c GCTM), so an instruction budget does not stop it; a binding passes
// by refusing __gc to scripts or by stopping it some other way. Reports what
// happened; a watchdog ends the run if the call never returns.
int GcDos() {
  Host h;
  NevrOwner* a = h.Load("gcdos.lua",
                        "nevr.hook('test.add', {pre = function(h)\n"
                        "  for i = 1, 200 do setmetatable({}, {__gc = function() while true do end end}) end\n"
                        "  local t = {}\n"
                        "  for i = 1, 200000 do t[i] = {i} end\n"
                        "end})\n");
  std::atomic<bool> done{false};
  std::thread watchdog([&done] {
    for (int i = 0; i < 200 && !done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!done.load()) {
      std::printf("T5e gc_dos: NOT STOPPED after 20000 ms (a finalizer is still running)\n");
      std::fflush(stdout);
      std::_Exit(2);
    }
  });
  const auto t0 = std::chrono::steady_clock::now();
  h.Add(2, 3);
  h.Add(2, 3);
  const double ms = MsSince(t0);
  done.store(true);
  watchdog.join();
  std::printf("T5e gc_dos: returned after %.1f ms, owner %s\n", ms, a->disabled.load() ? "disabled" : "still enabled");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const char* verbose = std::getenv("NEVR_SCRIPT_VERBOSE");
  g_verbose = verbose && *verbose == '1';
  if (argc > 1 && std::strcmp(argv[1], "--bench") == 0) return Bench();
  if (argc > 1 && std::strcmp(argv[1], "--pattern-dos") == 0) return PatternDos();
  if (argc > 1 && std::strcmp(argv[1], "--gc-dos") == 0) return GcDos();
  {
    Host h;
    std::printf("VM: %s\n", h.vm->Name());
  }
  return mini_test::RunAll(argc, argv);
}

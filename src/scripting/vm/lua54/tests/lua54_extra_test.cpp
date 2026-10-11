// Extra tests for the Lua 5.4 binding: the cases the shared conformance file does
// not reach (an out-of-memory error swallowed by pcall, no false positive on a
// recoverable allocation, escape routes through the string metatable and _G,
// error objects that are not strings, threads, reload).
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "quest/tests/mini_test.h"
#include "scripting/host_registry.h"
#include "scripting/script_vm.h"

namespace {

using nevr_script::LogRecord;
using nevr_script::Registry;
using nevr_script::ScriptVm;
using nevr_script::VmLimits;

struct Captured {
  std::string event, owner, detail;
};

void AddOriginal(NevrValue* f, void*) { f[2].as.i = f[0].as.i + f[1].as.i; }

struct Host {
  std::vector<Captured> log;
  Registry reg{[this](const LogRecord& r) { log.push_back({r.event, r.owner, r.detail}); }};
  const NevrHostApi* api = reg.Api();
  nevr_script::HookPoint* add = reg.RegisterHookPoint(
      "test.add", {{"a", NEVR_VALUE_INT, true, false},
                   {"b", NEVR_VALUE_INT, true, false},
                   {"result", NEVR_VALUE_INT, true, true}});
  std::unique_ptr<ScriptVm> vm;
  explicit Host(VmLimits limits = VmLimits()) {
    reg.RegisterOverridePoint("done", NEVR_VALUE_BOOL);
    reg.RegisterOverridePoint("escape", NEVR_VALUE_BOOL);
    vm = nevr_script::CreateScriptVm(reg, limits); }
  int64_t Add(int64_t a, int64_t b) {
    NevrValue fields[3] = {};
    for (NevrValue& v : fields) v.type = NEVR_VALUE_INT;
    fields[0].as.i = a;
    fields[1].as.i = b;
    reg.Invoke(add, fields, AddOriginal, nullptr);
    return fields[2].as.i;
  }
  NevrOwner* Load(const std::string& chunk, const std::string& src, bool expect_ok = true,
                  std::string* err = nullptr) {
    NevrOwner* owner = reg.OpenOwner(chunk.substr(0, chunk.rfind('.')));
    std::string error;
    const bool ok = vm->Load(owner, chunk, src, &error);
    if (ok != expect_ok) {
      std::fprintf(stderr, "  Load(%s) returned %d: %s\n", chunk.c_str(), ok ? 1 : 0, error.c_str());
      ++mini_test::Failures();
    }
    if (err) *err = error;
    return owner;
  }
  const Captured* Find(const char* event) const {
    for (const Captured& c : log) {
      if (c.event == event) return &c;
    }
    return nullptr;
  }
  bool Has(NevrOwner* o, const char* key) {
    NevrValue v{};
    return api->override_get(o, key, &v) == NEVR_OK;
  }
};

bool Contains(const std::string& s, const char* n) { return s.find(n) != std::string::npos; }

}  // namespace

TEST(x1_oom_swallowed_by_pcall_still_disables) {
  VmLimits lim;
  lim.memory_bytes = 4u << 20;
  Host h(lim);
  NevrOwner* a = h.Load("m.lua", "local ok = pcall(string.rep, 'x', 64 * 1024 * 1024)\n"
                                 "local n = 0\nfor i = 1, 10 do n = n + 1 end\n", false);
  CHECK(a->disabled.load());
  const Captured* c = h.Find("owner_disabled");
  CHECK(c && Contains(c->detail, "memory"));
}

TEST(x2_oom_inside_a_coroutine_disables) {
  VmLimits lim;
  lim.memory_bytes = 4u << 20;
  Host h(lim);
  NevrOwner* a = h.Load("m.lua", "local co = coroutine.create(function() return string.rep('x', 64 * 1024 * 1024) end)\n"
                                 "coroutine.resume(co)\n", false);
  CHECK(a->disabled.load());
  const Captured* c = h.Find("owner_disabled");
  CHECK(c && Contains(c->detail, "memory"));
}

TEST(x3_garbage_far_over_the_cap_is_not_a_breach) {
  VmLimits lim;
  lim.memory_bytes = 4u << 20;
  lim.instructions_per_call = 100000000;
  lim.millis_per_call = 5000;
  Host h(lim);
  NevrOwner* a = h.Load("m.lua", "local keep\nfor i = 1, 20000 do keep = string.rep('x', 4000) .. i end\n"
                                 "nevr.override('done', true)\n");
  CHECK(!a->disabled.load());
  CHECK(h.Has(a, "done"));
}

TEST(x4_pcall_loop_inside_a_prebuilt_coroutine_is_stopped) {
  Host h;
  NevrOwner* a = h.Load("m.lua",
                        "local co = coroutine.wrap(function()\n"
                        "  while true do pcall(function() while true do end end) end\n"
                        "end)\n"
                        "co()\n", false);
  CHECK(a->disabled.load());
}

TEST(x5_time_budget_names_time) {
  VmLimits lim;
  lim.instructions_per_call = 4000000000ull;
  lim.millis_per_call = 30;
  Host h(lim);
  NevrOwner* a = h.Load("m.lua", "while true do end\n", false);
  CHECK(a->disabled.load());
  const Captured* c = h.Find("owner_disabled");
  CHECK(c && Contains(c->detail, "time budget"));
}

TEST(x6_string_metatable_and_globals_are_closed) {
  Host h;
  const char* probes[] = {
      "if ('x').dump then nevr.override('escape', true) end",
      "if getmetatable('') ~= 'locked' then nevr.override('escape', true) end",
      "if not pcall(setmetatable, '', {}) == false then nevr.override('escape', true) end",
      "if _G.load or _G.io or _G.os.execute or _G.debug.getinfo then nevr.override('escape', true) end",
      "if rawget(_G, 'package') or rawget(_G, 'io') then nevr.override('escape', true) end",
      "if string.dump or os.getenv or os.remove or os.exit then nevr.override('escape', true) end",
      "if debug.sethook or debug.getinfo or debug.getupvalue then nevr.override('escape', true) end",
      "local ok = pcall(collectgarbage) if ok then nevr.override('escape', true) end",
      "local ok = pcall(collectgarbage, 'collect') if ok then nevr.override('escape', true) end",
      "if type(collectgarbage('count')) ~= 'number' then nevr.override('escape', true) end",
      "if coroutine.wrap(function() return load end)() then nevr.override('escape', true) end",
  };
  int i = 0;
  for (const char* p : probes) {
    NevrOwner* o = h.Load("p" + std::to_string(i++) + ".lua", p);
    if (h.Has(o, "escape")) {
      std::fprintf(stderr, "  probe %d escaped: %s\n", i - 1, p);
      ++mini_test::Failures();
    }
  }
}

TEST(x7_error_objects_that_are_not_strings) {
  Host h;
  h.Load("m.lua", "nevr.hook('test.add', {pre = function(h) error({}) end})\n");
  CHECK_EQ(h.Add(2, 3), 5);
  const Captured* c = h.Find("callback_failed");
  CHECK(c && Contains(c->detail, "error object is a table value"));
}

TEST(x8_int_field_needs_an_integral_number_and_h_expires) {
  Host h;
  h.Load("m.lua", "local kept\n"
                  "nevr.hook('test.add', {pre = function(h) kept = h; h:set('a', 1.5) end,\n"
                  "                       post = function(h) h:set('result', 3.0 + kept:get('result') * 0) end})\n");
  CHECK_EQ(h.Add(2, 3), 3);  // pre failed (1.5), post set result 3.0 -> integral INT 3
  const Captured* c = h.Find("callback_failed");
  CHECK(c && Contains(c->detail, "NEVR_ERR_TYPE_MISMATCH"));
}

TEST(x9_print_and_log_reach_the_registry) {
  Host h;
  h.Load("m.lua", "print('a', 1, true)\nnevr.log('warn', 'careful')\n");
  int logs = 0;
  bool tabbed = false;
  for (const Captured& c : h.log) {
    if (c.event == "owner_log") ++logs;
    if (c.detail == "a\t1\ttrue") tabbed = true;
  }
  CHECK_EQ(logs, 2);
  CHECK(tabbed);
}

TEST(x10_concurrent_callbacks_on_one_state) {
  Host h;
  h.Load("m.lua", "local n = 0\nnevr.hook('test.add', {pre = function(h) n = n + 1 h:set('a', h:get('a')) end})\n");
  std::vector<std::thread> threads;
  int64_t bad = 0;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&h, &bad] {
      for (int i = 0; i < 2000; ++i) {
        if (h.Add(i, 1) != i + 1) ++bad;
      }
    });
  }
  for (std::thread& t : threads) t.join();
  CHECK_EQ(bad, 0);
}

TEST(x12_gc_finalizers_are_refused_and_setmetatable_still_works) {
  Host h;
  NevrOwner* a = h.Load("m.lua",
                        "local ok, err = pcall(setmetatable, {}, {__gc = function() while true do end end})\n"
                        "if ok then nevr.override('escape', true) end\n"
                        "local t = setmetatable({}, {__index = function() return 7 end})\n"
                        "if t.x ~= 7 then nevr.override('escape', true) end\n"
                        "local p = setmetatable({}, {__metatable = 'p'})\n"
                        "if pcall(setmetatable, p, {}) then nevr.override('escape', true) end\n"
                        "if getmetatable(p) ~= 'p' then nevr.override('escape', true) end\n"
                        "nevr.override('done', true)\n");
  CHECK(!h.Has(a, "escape"));
  CHECK(h.Has(a, "done"));
}

// The same bomb as t7_memory_bomb_at_top_level_is_refused with the wall-clock budget out of
// the way: under qemu (emulated arm64) 50 ms ends the run before 4 MiB is allocated, and the
// owner is then disabled for "time", which is the budget doing its job.
TEST(x13_memory_bomb_with_a_long_time_budget_names_memory) {
  VmLimits lim;
  lim.memory_bytes = 4u << 20;
  lim.millis_per_call = 20000;
  lim.instructions_per_call = 4000000000ull;
  Host h(lim);
  NevrOwner* a = h.Load("m.lua", "local t = {}\n"
                                 "for i = 1, 100000000 do t[i] = string.rep('x', 100) .. i end\n", false);
  CHECK(a->disabled.load());
  const Captured* c = h.Find("owner_disabled");
  CHECK(c && Contains(c->detail, "memory"));
  CHECK(h.vm->MemoryBytes(a) <= lim.memory_bytes);
}

TEST(x11_unload_stops_the_callbacks) {
  Host h;
  NevrOwner* a = h.Load("m.lua", "nevr.hook('test.add', {post = function(h) h:set('result', 99) end})\n");
  CHECK_EQ(h.Add(1, 1), 99);
  CHECK(h.vm->MemoryBytes(a) > 0);
  h.vm->Unload(a);
  CHECK_EQ(h.vm->MemoryBytes(a), static_cast<size_t>(0));
  CHECK_EQ(h.Add(1, 1), 2);  // chain not yet reset: the closed state is skipped, not used
}

int main(int argc, char** argv) {
  Host h;
  std::printf("VM: %s\n", h.vm->Name());
  return mini_test::RunAll(argc, argv);
}

// What the shared-state mode of the Luau binding adds to the conformance suite
// (src/scripting/vm/luau/luau_vm.cpp, NEVR_LUAU_SHARED_STATE=ON): many scripts in
// one lua_State, each in its own thread and its own memory category. The shared
// behaviour (src/scripting/tests/conformance_test.cpp) is not repeated here; these
// tests are about one script not reaching, charging or stopping another.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "quest/tests/mini_test.h"
#include "scripting/host_registry.h"
#include "scripting/script_vm.h"

namespace {

using nevr_script::HookPoint;
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
  HookPoint* add = reg.RegisterHookPoint("test.add", {{"a", NEVR_VALUE_INT, true, false},
                                                       {"b", NEVR_VALUE_INT, true, false},
                                                       {"result", NEVR_VALUE_INT, true, true}});
  std::unique_ptr<ScriptVm> vm;

  explicit Host(VmLimits limits = VmLimits()) { vm = nevr_script::CreateScriptVm(reg, limits); }

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
      std::fprintf(stderr, "  Load(%s) returned %s, expected %s; error: %s\n", chunk.c_str(), ok ? "true" : "false",
                   expect_ok ? "true" : "false", error.c_str());
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
};

bool Contains(const std::string& haystack, const char* needle) { return haystack.find(needle) != std::string::npos; }

// Limits that leave only the memory cap (or only the instruction budget) to stop a script.
VmLimits MemoryOnly(size_t cap) {
  VmLimits limits;
  limits.memory_bytes = cap;
  limits.instructions_per_call = UINT64_MAX;
  limits.millis_per_call = 60000;
  return limits;
}

// A script that counts its own calls in a global and adds the count to the result.
const char* kCounter =
    "n = 0\n"
    "nevr.hook('test.add', {post = function(h) n = n + 1; h:set('result', h:get('result') + n) end})\n";

}  // namespace

// (a) A global one script sets is invisible to the others, and the shared
// libraries are the same read-only tables for all of them.
TEST(shared_globals_of_two_scripts_never_see_each_other) {
  Host h;
  h.Load("mod_a.lua",
         "secret = 42\n"
         "nevr.hook('test.add', {post = function(h) h:set('result', h:get('result') + secret) end})\n");
  h.Load("mod_b.lua",
         "assert(secret == nil, 'mod_a global leaked into mod_b')\n"
         "secret = 7\n"
         "nevr.hook('test.add', {post = function(h) h:set('result', h:get('result') + secret) end})\n");
  CHECK_EQ(h.Add(2, 3), 5 + 42 + 7);  // each reads its own `secret`
  CHECK(!h.Find("owner_disabled"));
  std::string error;
  h.Load("mod_c.lua", "string.leak = 1\n", false, &error);  // the library tables are shared and read-only
  CHECK(Contains(error, "mod_c.lua"));
  h.Load("mod_d.lua", "assert(string.leak == nil)\n");
}

// The point of the mode: scripts are not a state each. The state holds the
// libraries once; the second script adds a thread's worth, not a second state.
TEST(shared_two_scripts_cost_one_state_not_two) {
  Host h;
  NevrOwner* a = h.Load("mod_a.lua", kCounter);
  const size_t one = h.vm->TotalMemoryBytes();
  NevrOwner* b = h.Load("mod_b.lua", kCounter);
  const size_t two = h.vm->TotalMemoryBytes();
  const size_t sum = h.vm->MemoryBytes(a) + h.vm->MemoryBytes(b);
  std::printf("  one script: %zu bytes total; two: %zu; their own: %zu + %zu\n", one, two, h.vm->MemoryBytes(a),
              h.vm->MemoryBytes(b));
  // TotalMemoryBytes is what the allocator holds (pages and not-yet-collected
  // garbage included), so it can even shrink between loads. The first script's
  // total includes the libraries; one state per script costs that again for
  // every script. Two scripts here must cost about one state.
  CHECK(two < one + one / 4);
  CHECK(two > sum);                // the shared part (libraries) is charged to no script
  CHECK(two - sum > 16u * 1024u);  // the libraries, once
}

// (b) One script past its memory cap is disabled; the other keeps its state and runs.
TEST(shared_memory_breach_disables_only_that_script) {
  Host h(MemoryOnly(4u << 20));
  NevrOwner* a = h.Load("mod_a.lua",
                        "nevr.hook('test.add', {pre = function(h) local s = string.rep('x', 64 * 1024 * 1024) end})\n");
  NevrOwner* b = h.Load("mod_b.lua", kCounter);
  CHECK_EQ(h.Add(1, 1), 2 + 1);  // mod_a breaches in its pre; mod_b's post still runs: n = 1
  CHECK(a->disabled.load());
  const Captured* c = h.Find("owner_disabled", "mod_a");
  CHECK(c && Contains(c->detail, "memory"));
  CHECK(!b->disabled.load());
  CHECK(!h.Find("owner_disabled", "mod_b"));
  CHECK_EQ(h.Add(1, 1), 2 + 2);  // mod_b's global `n` survived mod_a's breach
  CHECK_EQ(h.Add(1, 1), 2 + 3);
  CHECK(h.vm->MemoryBytes(a) <= 4u << 20);
}

// (c) One script's runaway loop is stopped and disables only that script.
// The cap is per script: what another script holds, and the shared libraries,
// are not charged to it. Under a 6 MiB cap, mod_a keeps about 3 MiB live (its peak
// while building them is about 5 MiB); mod_b then does the same and must not be
// refused for mod_a's bytes, although the two together are over 6 MiB.
TEST(shared_one_scripts_memory_does_not_count_against_anothers_cap) {
  Host h(MemoryOnly(6u << 20));
  NevrOwner* a = h.Load("mod_a.lua",
                        "keep = {}\n"
                        "for i = 1, 3 do keep[i] = string.rep('a', 1024 * 1024) .. i end\n");
  NevrOwner* b = h.Load("mod_b.lua",
                        "keep = {}\n"
                        "for i = 1, 3 do keep[i] = string.rep('b', 1024 * 1024) .. i end\n");
  std::printf("  mod_a: %zu bytes, mod_b: %zu bytes, total %zu\n", h.vm->MemoryBytes(a), h.vm->MemoryBytes(b),
              h.vm->TotalMemoryBytes());
  CHECK(!a->disabled.load());
  CHECK(!b->disabled.load());
  CHECK(h.vm->TotalMemoryBytes() > (6u << 20));  // both really hold their 3 MiB
}

TEST(shared_runaway_loop_disables_only_that_script) {
  Host h;
  NevrOwner* a = h.Load("mod_a.lua", "nevr.hook('test.add', {pre = function(h) while true do end end})\n");
  NevrOwner* b = h.Load("mod_b.lua", kCounter);
  CHECK_EQ(h.Add(1, 1), 2 + 1);
  CHECK(a->disabled.load());
  const Captured* c = h.Find("owner_disabled", "mod_a");
  CHECK(c && (Contains(c->detail, "instruction") || Contains(c->detail, "time")));
  CHECK(!b->disabled.load());
  CHECK_EQ(h.Add(1, 1), 2 + 2);
  CHECK_EQ(h.Add(1, 1), 2 + 3);
}

// (d) MemoryBytes(owner) is the owner's own category: it grows with the owner's
// allocations and does not with the other's.
TEST(shared_memory_bytes_tracks_each_scripts_own_allocations) {
  Host h(MemoryOnly(16u << 20));
  NevrOwner* a = h.Load("mod_a.lua",
                        "nevr.hook('test.add', {pre = function(h)\n"
                        "  if h:get('a') == 1 then keep = string.rep('x', 1024 * 1024) end\n"
                        "end})\n");
  NevrOwner* b = h.Load("mod_b.lua",
                        "nevr.hook('test.add', {pre = function(h)\n"
                        "  if h:get('a') == 2 then keep = string.rep('y', 2 * 1024 * 1024) end\n"
                        "end})\n");
  const size_t a0 = h.vm->MemoryBytes(a), b0 = h.vm->MemoryBytes(b);
  h.Add(1, 1);  // mod_a allocates 1 MiB
  const size_t a1 = h.vm->MemoryBytes(a), b1 = h.vm->MemoryBytes(b);
  std::printf("  mod_a: %zu -> %zu; mod_b: %zu -> %zu\n", a0, a1, b0, b1);
  // Both scripts' pre callbacks run on every Add, so both categories move by a stack
  // frame or two; what must not move is mod_b's by mod_a's 1 MiB string.
  constexpr size_t kSlack = 64u * 1024u;
  CHECK(a1 >= a0 + (1u << 20));
  CHECK(b1 < b0 + kSlack);
  h.Add(2, 1);      // mod_b allocates 2 MiB
  const size_t a2 = h.vm->MemoryBytes(a), b2 = h.vm->MemoryBytes(b);
  std::printf("  mod_a: %zu -> %zu; mod_b: %zu -> %zu\n", a1, a2, b1, b2);
  CHECK(b2 + kSlack >= b1 + (2u << 20));
  CHECK(a2 < a1 + kSlack);
  CHECK(h.vm->TotalMemoryBytes() >= a2 + b2);
}

namespace {

// Loads `count` trivial scripts s0..s<count-1>; the first also hooks test.add.
void LoadMany(Host& h, int count, std::vector<NevrOwner*>* owners) {
  for (int i = 0; i < count; ++i) {
    const std::string name = "s" + std::to_string(i);
    owners->push_back(h.Load(name + ".lua", i == 0 ? kCounter : "x = 1\n"));
  }
}

constexpr int kScriptLimit = 255;  // memory categories 1..255; category 0 is the shared part

}  // namespace

// (e) The 256th script is refused with an error that names the limit; the 255
// loaded ones keep working.
TEST(shared_the_256th_script_is_refused_by_name) {
  Host h;
  std::vector<NevrOwner*> owners;
  LoadMany(h, kScriptLimit, &owners);
  std::string error;
  h.Load("one_too_many.lua", "x = 1\n", false, &error);
  std::printf("  refused: %s\n", error.c_str());
  CHECK(Contains(error, "one_too_many.lua"));
  CHECK(Contains(error, "memory categories"));
  CHECK(Contains(error, "255"));
  CHECK_EQ(h.Add(1, 1), 2 + 1);  // s0 still runs
  CHECK(!owners[0]->disabled.load());
}

// (f) A category an unloaded script held is handed to the next script.
TEST(shared_unload_then_load_recycles_a_category) {
  Host h;
  std::vector<NevrOwner*> owners;
  LoadMany(h, kScriptLimit, &owners);
  h.Load("refused.lua", "x = 1\n", false);
  h.vm->Unload(owners[100]);
  CHECK(!h.Find("category_reserved"));  // its bytes all went back
  h.Load("recycled.lua", "x = 2\n");
  h.Load("refused_again.lua", "x = 1\n", false);
  h.vm->Unload(owners[101]);
  h.vm->Unload(owners[102]);
  h.Load("recycled_2.lua", "x = 3\n");
  h.Load("recycled_3.lua", "x = 4\n");
  h.Load("refused_third.lua", "x = 1\n", false);
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }

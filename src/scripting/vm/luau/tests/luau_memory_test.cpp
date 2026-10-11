// Luau-only memory tests: paths that depend on Luau having no emergency
// collection in the allocator, which a VM with one never takes.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "quest/tests/mini_test.h"
#include "scripting/host_registry.h"
#include "scripting/memory_policy.h"
#include "scripting/script_host.h"
#include "scripting/script_vm.h"

namespace nevr_script {
bool LuauTestThunksRefuseForeignArguments();  // luau_vm.cpp, NEVR_LUAU_TEST_HOOKS
}  // namespace nevr_script

namespace {

using namespace nevr_script;
namespace fs = std::filesystem;

struct Captured {
  std::string event, detail;
};

void Original(NevrValue*, void*) {}

}  // namespace

// The binding's three-in-a-row path end to end (reproduction from the review of
// #458): each call keeps a 4 MiB temporary live across safepoints, drops it, then
// asks for just under half the cap, which is refused because the dropped
// temporary is not collected yet. After collecting, the live set and the request
// are within half the cap, so only the repeat count disables the script.
TEST(cap_reached_on_three_calls_in_a_row_disables) {
  std::vector<Captured> log;
  Registry reg([&log](const LogRecord& r) { log.push_back({r.event, r.detail}); });
  HookPoint* point = reg.RegisterHookPoint("test.a", {{"a", NEVR_VALUE_INT, true, true}});
  std::unique_ptr<ScriptVm> vm = CreateScriptVm(reg, VmLimits());  // 16 MiB cap
  NevrOwner* owner = reg.OpenOwner("capthree");
  std::string error;
  CHECK(vm->Load(owner, "capthree.lua",
                 "keep = string.rep('k', 7 * 1024 * 1024)\n"
                 "nevr.hook('test.a', {pre = function(h)\n"
                 "  local t = string.rep('t', 4 * 1024 * 1024)\n"
                 "  local n = 0\n"
                 "  for i = 1, 10 do n = n + #t end\n"
                 "  t = nil\n"
                 "  local big = string.rep('b', 7 * 1024 * 1024 + 900 * 1024)\n"
                 "end})\n",
                 &error));
  std::vector<bool> disabled_after;
  for (int i = 0; i < kCapHitsInARowLimit; ++i) {
    NevrValue field{};
    field.type = NEVR_VALUE_INT;
    reg.Invoke(point, &field, Original, nullptr);
    disabled_after.push_back(owner->disabled.load());
  }
  for (int i = 0; i + 1 < kCapHitsInARowLimit; ++i) CHECK(!disabled_after[static_cast<size_t>(i)]);
  CHECK(disabled_after.back());
  const Captured* disabled = nullptr;
  for (const Captured& c : log) {
    if (c.event == "owner_disabled") disabled = &c;
  }
  CHECK(disabled && disabled->detail.find(CapVerdictReason(CapVerdict::kRepeated)) != std::string::npos);
}

// Same-thread re-entry (verification of #458): while a script's callback runs,
// something on the same thread invokes a hook point that calls the same script
// again (here the log sink does). The state's lock is not recursive, so the
// binding must refuse the inner call instead of waiting on itself.
TEST(reentry_into_the_same_script_on_one_thread_is_refused) {
  std::vector<Captured> log;
  Registry* reg_ptr = nullptr;
  HookPoint* point = nullptr;
  int depth = 0;
  Registry reg([&](const LogRecord& r) {
    log.push_back({r.event, r.detail});
    if (std::string(r.event) == "owner_log" && depth == 0) {
      ++depth;
      NevrValue field{};
      field.type = NEVR_VALUE_INT;
      reg_ptr->Invoke(point, &field, Original, nullptr);
      --depth;
    }
  });
  reg_ptr = &reg;
  point = reg.RegisterHookPoint("test.a", {{"a", NEVR_VALUE_INT, true, true}});
  std::unique_ptr<ScriptVm> vm = CreateScriptVm(reg, VmLimits());
  NevrOwner* owner = reg.OpenOwner("reenter");
  std::string error;
  CHECK(vm->Load(owner, "reenter.lua", "nevr.hook('test.a', {pre = function(h) nevr.log('info', 'inside') end})\n", &error));
  NevrValue field{};
  field.type = NEVR_VALUE_INT;
  reg.Invoke(point, &field, Original, nullptr);  // deadlocks without the guard; the test deadline reports it
  bool refused = false;
  for (const Captured& c : log) {
    refused = refused || (c.event == "callback_failed" && c.detail.find("re-entrant") != std::string::npos);
  }
  CHECK(refused);
  CHECK(!owner->disabled.load());
}

// A -> B -> A on one thread (re-check of #458): A's callback leads (through the
// sink) to B's, and B's back to A's while A's lock is still held. The guard
// must see every script running on the thread, not only the innermost.
TEST(reentry_through_another_script_is_refused) {
  std::vector<Captured> log;
  Registry* reg_ptr = nullptr;
  HookPoint* first = nullptr;
  HookPoint* second = nullptr;
  int depth = 0;
  Registry reg([&](const LogRecord& r) {
    log.push_back({r.event, r.detail});
    if (std::string(r.event) != "owner_log" || depth >= 2) return;
    ++depth;
    NevrValue field{};
    field.type = NEVR_VALUE_INT;
    reg_ptr->Invoke(std::string(r.detail) == "a" ? second : first, &field, Original, nullptr);
    --depth;
  });
  reg_ptr = &reg;
  first = reg.RegisterHookPoint("test.a", {{"a", NEVR_VALUE_INT, true, true}});
  second = reg.RegisterHookPoint("test.b", {{"a", NEVR_VALUE_INT, true, true}});
  std::unique_ptr<ScriptVm> vm = CreateScriptVm(reg, VmLimits());
  NevrOwner* a = reg.OpenOwner("script_a");
  NevrOwner* b = reg.OpenOwner("script_b");
  std::string error;
  CHECK(vm->Load(a, "a.lua", "nevr.hook('test.a', {pre = function(h) nevr.log('info', 'a') end})\n", &error));
  CHECK(vm->Load(b, "b.lua", "nevr.hook('test.b', {pre = function(h) nevr.log('info', 'b') end})\n", &error));
  NevrValue field{};
  field.type = NEVR_VALUE_INT;
  reg.Invoke(first, &field, Original, nullptr);  // a -> (sink) b -> (sink) a: deadlocks without the guard
  int refused = 0;
  for (const Captured& c : log) refused += c.event == "callback_failed" && c.detail.find("re-entrant") != std::string::npos;
  CHECK_EQ(refused, 1);
}

// The re-check of #458: with debug out of the surface, the reachable-functions
// probe in the conformance suite returns at once, so the guard on the two setup
// thunks needs its own test.
TEST(setup_thunks_refuse_arguments_the_binding_did_not_pass) {
  CHECK(LuauTestThunksRefuseForeignArguments());
}

// Dev reload of a breached script through the real binding (the condition for
// the shared-state default): script_host_test covers ScriptHost on a fake VM;
// this runs the Luau unload and load underneath it, in whichever mode the
// binding was built. A runaway and a memory breach each disable the script; a
// changed file brings it back in a dev build with its new value and none of the
// old version's memory counted against it, and leaves it off otherwise. Each
// kept string has its own length: Luau interns strings, so equal ones are one.
TEST(breached_script_comes_back_on_dev_reload_through_the_binding) {
  const char* base = std::getenv("NEVR_TEST_TMPDIR");
  const fs::path dir = (base ? fs::path(base) : fs::temp_directory_path()) / "nevr_luau_breach_reload";
  const std::string head = "--[[nevr\n{\"name\": \"breacher\", \"version\": \"1.0.0\", \"api\": 1, "
                           "\"overrides\": [\"k\"], \"hooks\": [\"test.a\"]}\n]]\n";
  struct Breach {
    const char* name;
    const char* source;
    const char* reason;  // in the owner_disabled record
  };
  const Breach breaches[] = {
      {"runaway", "nevr.hook('test.a', {pre = function(h) while true do end end})\n", "budget exceeded"},
      {"memory", "kept = {}\n"
                 "nevr.hook('test.a', {pre = function(h) kept[#kept + 1] = string.rep('m', 3 * 1024 * 1024 + #kept) end})\n",
       "memory limit exceeded"}};
  for (const Breach& breach : breaches) {
    for (const bool dev : {true, false}) {
      const int failures_before = mini_test::Failures();
      fs::remove_all(dir);
      fs::create_directories(dir);
      const std::string path = (dir / "breacher.lua").string();
      std::ofstream(path, std::ios::binary) << head << "nevr.override('k', 1)\n" << breach.source;
      std::vector<Captured> log;
      Registry reg([&log](const LogRecord& r) { log.push_back({r.event, r.detail}); });
      HookPoint* point = reg.RegisterHookPoint("test.a", {{"a", NEVR_VALUE_INT, true, true}});
      CHECK(reg.RegisterOverridePoint("k", NEVR_VALUE_INT));
      std::unique_ptr<ScriptVm> vm = CreateScriptVm(reg, VmLimits());
      ScriptHost host(reg, *vm, dev);
      CHECK_EQ(host.LoadAll({path}), 1);
      NevrOwner* owner = reg.FindOwner("breacher");
      CHECK(owner != nullptr);
      if (!owner) continue;
      for (int i = 0; i < 10 && !owner->disabled.load(); ++i) {
        NevrValue field{};
        field.type = NEVR_VALUE_INT;
        reg.Invoke(point, &field, Original, nullptr);
      }
      CHECK(owner->disabled.load());
      bool reason = false;
      for (const Captured& c : log) {
        reason = reason || (c.event == "owner_disabled" && c.detail.find(breach.reason) != std::string::npos);
      }
      CHECK(reason);
      std::ofstream(path, std::ios::binary) << head << "nevr.override('k', 22)\n";
      host.PollReload();
      NevrOwner* probe = reg.OpenOwner("probe");
      NevrValue v{};
      const bool effective = reg.Api()->override_get(probe, "k", &v) == NEVR_OK;
      if (dev) {
        CHECK(!owner->disabled.load());
        CHECK(effective && v.as.i == 22);
        CHECK(vm->MemoryBytes(owner) < (1u << 20));
      } else {
        CHECK(owner->disabled.load());
        CHECK(!effective);
      }
      if (mini_test::Failures() > failures_before) {
        std::fprintf(stderr, "  breach %s, dev %d; the registry's records:\n", breach.name, dev ? 1 : 0);
        for (const Captured& c : log) std::fprintf(stderr, "    %s: %s\n", c.event.c_str(), c.detail.c_str());
      }
    }
  }
  fs::remove_all(dir);
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv, std::chrono::seconds(10)); }

// Luau-only memory tests: paths that depend on Luau having no emergency
// collection in the allocator, which a VM with one never takes.
#include <string>
#include <vector>

#include "quest/tests/mini_test.h"
#include "scripting/host_registry.h"
#include "scripting/memory_policy.h"
#include "scripting/script_vm.h"

namespace {

using namespace nevr_script;

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

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv, std::chrono::seconds(10)); }

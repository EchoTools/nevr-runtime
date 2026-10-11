// Host-side probe: does the LuaJIT compiler engage in the binding's own state, with and without the
// budget hook? Prints one JSON-ish line per case. Not part of the contract and not run by CI.
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <string>

#include "scripting/vm/luajit/luajit_vm.h"

namespace {

using nevr_script::LogRecord;
using nevr_script::Registry;
using nevr_script::VmLimits;

void Original(NevrValue* f, void*) { f[2].as.i = f[0].as.i + f[1].as.i; }

void RunCase(const char* label, bool budget_hook, const char* body, uint32_t millis) {
  Registry reg([](const LogRecord& r) {
    if (std::string(r.event) == "owner_disabled" || std::string(r.event) == "callback_failed") {
      std::printf("  [%s] %s\n", r.event, r.detail);
    }
  });
  nevr_script::HookPoint* add = reg.RegisterHookPoint(
      "test.add", {{"a", NEVR_VALUE_INT, true, false}, {"b", NEVR_VALUE_INT, true, false},
                   {"result", NEVR_VALUE_INT, true, true}});
  VmLimits limits;
  limits.instructions_per_call = 2000000000ull;  // keep the budget out of the way of the loop
  limits.millis_per_call = millis;
  auto vm = nevr_script::CreateLuajitVm(reg, limits, budget_hook);
  NevrOwner* owner = reg.OpenOwner("probe");
  std::string error;
  const std::string src =
      std::string("nevr.hook('test.add', {pre = function(h)\n") + body + "\nend})\n";
  if (!vm->Load(owner, "probe.lua", src, &error)) {
    std::printf("%s: load failed: %s\n", label, error.c_str());
    return;
  }
  NevrValue fields[3] = {};
  for (NevrValue& v : fields) v.type = NEVR_VALUE_INT;
  fields[0].as.i = 1;
  fields[1].as.i = 2;
  const auto t0 = std::chrono::steady_clock::now();
  reg.Invoke(add, fields, Original, nullptr);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const nevr_script::LuajitInspection in = nevr_script::InspectLuajit(*vm, owner);
  std::printf("%s: %s, call took %.1f ms, owner %s, jit.status()=%s, compiled traces=%d%s%s\n", label,
              vm->Name(), ms, owner->disabled.load() ? "disabled" : "enabled", in.jit_on ? "on" : "off",
              in.traces, in.ok ? "" : " inspect error: ", in.ok ? "" : in.error.c_str());
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // 20M additions: long enough for the JIT to compile the loop, short enough to finish interpreted.
  const char* kHot = "local x = 0\nfor i = 1, 20000000 do x = x + i % 7 end\nh:set('result', x)";
  RunCase("hot loop, budget hook on ", true, kHot, 60000);
  RunCase("hot loop, budget hook off", false, kHot, 60000);
  // `probe spin` also runs the endless loop, which only a build that can interrupt compiled code survives.
  if (argc > 1 && std::string(argv[1]) == "spin") {
    RunCase("while true do end, hook on ", true, "while true do end", 200);
  }
  // No "hook off" spin case: without a hook (or the watchdog build) nothing can stop it.
  return 0;
}

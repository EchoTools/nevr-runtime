// Test-only extras of the LuaJIT binding, for tools that need to look inside a
// state. The contract (script_vm.h) is all production code uses.
#pragma once

#include <memory>
#include <string>

#include "scripting/script_vm.h"

namespace nevr_script {

struct LuajitInspection {
  bool jit_on = false;     // jit.status() in the owner's state
  int traces = 0;          // compiled traces, counted with jit.util.traceinfo
  bool ok = false;
  std::string error;
};

// Like CreateScriptVm; `budget_hook` false leaves the state without its count hook, so the
// JIT is free to compile (the probe uses this to show what the hook costs).
std::unique_ptr<ScriptVm> CreateLuajitVm(Registry& registry, const VmLimits& limits, bool budget_hook);

// Reads the JIT state of the owner's Lua state. `vm` must come from CreateLuajitVm/CreateScriptVm.
LuajitInspection InspectLuajit(ScriptVm& vm, const NevrOwner* owner);

}  // namespace nevr_script

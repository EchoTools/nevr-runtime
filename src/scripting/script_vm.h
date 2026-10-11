// The seam between the host API registry and a script VM. A VM binding exposes
// the NevrHostApi table to scripts one to one and enforces the limits below;
// the conformance tests (src/scripting/tests/conformance_test.cpp) hold every
// binding to the same behaviour.
//
// The script-visible API, identical in every binding (a global table `nevr`):
//
//   nevr.override(key, value)    value: boolean | number | string. A number is
//                                sent as NEVR_VALUE_FLOAT (or INT where the VM has
//                                integers); the registry converts it to the
//                                override point's type. Returns true, or nil and
//                                "<STATUS>: <last_error>".
//   nevr.hook(name, {pre = fn, post = fn})
//                                Either callback may be absent. Returns true, or
//                                nil and "<STATUS>: <last_error>".
//   nevr.log(level, message)     level: "debug" | "info" | "warn" | "error".
//   print(...)                   = nevr.log("info", <args joined by tabs>).
//
//   A callback receives a call object `h`:
//   h:get(field)                 the field's value (boolean | number | string).
//   h:set(field, value)          raises an error on any status but NEVR_OK. A
//                                number on an INT field must be integral and
//                                within ±2^53 (the registry converts it).
//
//   An INT outside ±2^53 is never handed to or taken from a VM whose numbers are
//   doubles: h:get raises rather than return a nearby integer.
//   h:skip()                     pre only: the original is not called.
//
// Limits: a script runs in its own VM state, under its own owner. Its top-level
// chunk and every callback run under `instructions_per_call` and
// `millis_per_call`; on a breach the binding stops the code, calls
// Registry::DisableOwner with a reason that names the limit, and the call fails.
// The state's allocations are capped at `memory_bytes`, counting what the
// allocator has handed out (garbage not yet collected included); an allocation
// past the cap fails. A binding keeps garbage from reaching the cap (collecting
// within the call), and after a refusal collects and disables the owner when its
// live set is still over half the cap, when one refused request was over half
// the cap, or after three calls in a row at the cap; otherwise only that call
// failed and the script continues. Holding a large live set must not make every
// call slower (t7_large_live_set_does_not_slow_every_call). An error raised
// by a callback becomes NEVR_HOOK_FAILED with the message "<chunk>:<line>: <msg>"
// as the reason; the owner stays enabled.
//
// Sandbox: a script sees only `nevr`, `print` and the safe parts of the standard
// library (string, table, math, coroutine, pairs/ipairs/select/type/tostring/
// tonumber/pcall/error/assert/next/unpack/rawequal/rawget/rawset/rawlen/setmetatable/
// getmetatable, utf8 where the VM has it, os.clock/os.time/os.date). It sees no io,
// the rest of os, package, require, dofile, loadfile, load, loadstring, string.dump,
// debug (beyond traceback/info), collectgarbage other than "count", getfenv, setfenv,
// newproxy, ffi or jit, and getmetatable('') does not return the string metatable.
// Probes for each are in conformance_test.cpp (t6_*).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "scripting/host_registry.h"

namespace nevr_script {

struct VmLimits {
  uint64_t instructions_per_call = 1000000;
  uint32_t millis_per_call = 50;
  size_t memory_bytes = 16u << 20;
};

class ScriptVm {
 public:
  virtual ~ScriptVm() = default;
  // "<implementation> <version>", e.g. "Lua 5.4.7".
  virtual const char* Name() const = 0;
  // Creates the script's state and runs its top-level chunk as `owner`.
  // `chunkname` is how errors name the script ("mod.lua"). On failure returns
  // false and sets *error to "<chunk>:<line>: <message>" where there is a line.
  virtual bool Load(NevrOwner* owner, const std::string& chunkname, const std::string& source,
                    std::string* error) = 0;
  // Bytes the owner's script holds now, as counted by the binding's allocator.
  virtual size_t MemoryBytes(const NevrOwner* owner) const = 0;
  // Bytes every loaded script holds together, including whatever the binding
  // shares between them (a shared state's libraries).
  virtual size_t TotalMemoryBytes() const = 0;
  // Closes the owner's state (hot reload, shutdown). The registry entries are
  // the caller's to reset.
  virtual void Unload(NevrOwner* owner) = 0;
};

// Each binding defines exactly this; the build links one binding.
std::unique_ptr<ScriptVm> CreateScriptVm(Registry& registry, const VmLimits& limits);

}  // namespace nevr_script

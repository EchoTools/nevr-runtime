# Lua scripting

Status: prototype (refs #440). Mods are Luau scripts that reach the game only through the runtime's host API. The VM
choice and its measurements are in `docs/adr/0008-luau-for-scripting.md`. This document covers what the
prototype does and proves, how to build and test it, and what is left.

## A script

```lua
--[[nevr
{
  "name": "low_gravity",
  "version": "1.0.0",
  "api": 1,
  "description": "Halves gravity and adds one to every test.add result",
  "overrides": ["physics.gravity"],
  "hooks": ["test.add"]
}
]]
nevr.override("physics.gravity", -4.9)

nevr.hook("test.add", {
  pre  = function(h) h:set("a", h:get("a") * 2) end,
  post = function(h) h:set("result", h:get("result") + 1) end,
})
```

- **One file.** The manifest is a JSON object in the leading `--[[nevr ... ]]` block, so the file is still
  valid Lua.
- **Read before running.** `ReadManifests` (`src/scripting/script_host.h`) parses the manifest without running
  anything, so a launcher or policy engine can see what a script will touch.
- **Enforced.** A script cannot set an override or add a hook its manifest does not declare. The call fails
  with `NEVR_ERR_UNDECLARED` and writes an `undeclared` record.
- **Strict parsing.** Unknown keys are refused, so a misspelt `"hook"` is an error, not an empty declaration.

| Script API | Host API call (`src/extension/host_api.h`) | On failure |
| --- | --- | --- |
| `nevr.override(key, value)` | `override_set` | returns `nil, "<STATUS>: <reason>"` |
| `nevr.hook(name, {pre=, post=})` | `hook_add`, once per phase | returns `nil, "<STATUS>: <reason>"` |
| `h:get(field)` / `h:set(field, v)` | `call_get` / `call_set` | `h:set` raises; the callback fails |
| `h:skip()` (pre only) | the callback returns `NEVR_HOOK_SKIP_ORIGINAL` | |
| `nevr.log(level, msg)`, `print(...)` | `log` | |

The full contract (script API, sandbox, limits) is `src/scripting/script_vm.h`. Every binding is held to it by
`src/scripting/tests/conformance_test.cpp`.

## The host API (#440)

`src/extension/host_api.h` is a plain, versioned C ABI: a `NevrHostApi` function table, `size` and `version`
first, fields only appended. A native plugin and the script binding call the same table, and nothing in it is a
pointer into the game. The runtime side is `src/scripting/host_registry.{h,cpp}`.

- **Owners.** The host issues one `NevrOwner` per plugin or script, in `plugins:` order. Every state change names
  its owner, so the host can attribute it and remove all of it at once. A script cannot act under another
  owner's name.
- **Override points.** A key exists only once the runtime registers it, with a type
  (`Registry::RegisterOverridePoint`). The runtime maps each key to the engine values it covers on this build.
  The engine reads typed values through `NRadEngine::CJson` path readers, one copy per module: on Quest
  `libr15.so` TString `0xfa2e7c`, Int `0xfa40ec`, LReal `0xfa4204` and Boolean `0xfa4370`, behind resolver
  `0xfa0950` and reached through GOT slots; on Windows `echovr.exe` `CJson_NavigatePath` `0x1405fcea0`, called
  directly.
  - Values: a number becomes INT when it is integral, and an INT is accepted for a FLOAT key.
  - Unknown key: `NEVR_ERR_UNKNOWN_KEY` plus an `override_unknown` record.
  - Conflict: the first owner keeps the key; a second owner gets `NEVR_ERR_CONFLICT`, and one
    `override_conflict` record names both owners.
- **Hook points.** The runtime installs the only detour on a named function and registers it as a hook point,
  with typed fields and the phase in which each may be written.
  - Callbacks chain by owner order, then by registration order within an owner. No owner installs a detour, so
    two owners on one function chain instead of colliding.
  - Chains are copy-on-write, so a game thread that is invoking a hook point never sees its chain change.
- **Failure policy.**
  - A failed callback (`call_fail`, a script error) is logged as `callback_failed` with the reason, and the
    call goes on.
  - An owner that breaches a limit is disabled: its callbacks are skipped from that moment, even later in the
    same call. Its overrides are dropped, and `owner_disabled` names the limit.
- **Freeing safely.** `Registry::Quiesce` reuses `nevr::ReaderGate` (`src/core/reader_gate.h`). It retires the
  owner's callbacks and waits for every game thread still inside one to leave. Only then may a binding free
  their state, on hot reload, on a failed load, and at shutdown.
- **Records.** Every event is one structured record with a stable `event` name, the owner, the other owner on a
  conflict, the key or hook point, and a detail. The event names are listed on `LogRecord` in
  `src/scripting/host_registry.h`.

## The Luau binding

`src/scripting/vm/luau/luau_vm.cpp`; Luau is the `extern/luau` submodule at tag `0.742`.

- **Isolation.** Each script gets its own `lua_State`. Upstream's `luaL_sandbox` makes library tables, the
  string metatable and the globals read-only, and each script runs in a `luaL_sandboxthread` thread, so its
  globals are its own.
- **Sandbox.** Removed after `luaL_sandbox`'s defaults: `getfenv`, `setfenv`, `newproxy` and `gcinfo`.
  `collectgarbage` is limited to `"count"`, and `getmetatable('')` returns `"locked"`. Scripts cannot load
  bytecode; the only `luau_load` call takes what `luau_compile` produced from source.
- **Budget.**
  - The interrupt callback counts safepoints and checks the wall clock. It also runs inside the string pattern
    matcher.
  - A breach is sticky: the interrupt raises at every later safepoint, so a script's `pcall` cannot swallow it.
  - After the call, the owner is disabled.
- **Memory.** A capped `lua_Alloc` refuses growth past `VmLimits::memory_bytes`. A refusal disables the owner,
  with "memory" in the reason.
- **Errors.** Script errors carry `<file>:<line>: <message>`.

## Typed API stubs and the checker

`GenerateLuauDefinitions` (`src/scripting/script_stubs.h`) writes a Luau definition file from what the runtime
registered. The types therefore come from the ABI tables, not from a second list kept by hand.

- **Override keys and hook names** are singleton string types.
- **Each hook point has two call types,** one per phase. Each type's `set` accepts only the fields writable in
  that phase, and only the pre type has `skip`.

`luau-lsp` loads the file through `luau-lsp.types.definitionFiles`. `nevr_script_check`
(`src/scripting/check/`, a host tool built on upstream's `Luau.Analysis`) type-checks scripts against it in strict
mode, without running them, and prints `<file>:<line>:<column>: TypeError: ...`.

`src/scripting/check/check_test.sh` runs the checker on the sample script `src/scripting/samples/low_gravity.lua`,
which must check clean. It also runs one sample per mistake, and each must be rejected on its line:

| Sample | Mistake |
| --- | --- |
| `bad_key.lua` | misspelt key |
| `bad_value.lua` | `"yes"` for a boolean key |
| `bad_read_only.lua` | `h:set("a", …)` after the call |
| `bad_skip_in_post.lua` | `h:skip()` in a post callback |
| `bad_hook.lua` | misspelt hook name |

```sh
cmake -S src/scripting/check -B build/script-check -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/script-check
src/scripting/check/check_test.sh <build>/nevr_script_stubs_test build/script-check/nevr_script_check /tmp/nevr-check
```

## Build and test

The prototype is a standalone CMake project, built with the same toolchains as the real targets:

```sh
json=<dir holding nlohmann/json.hpp>   # e.g. build/mingw-release/vcpkg_installed/x64-mingw-static/include
git submodule update --init extern/luau
cmake -S src/scripting -B build/scripting-mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake -DNEVR_SCRIPT_VM=luau -DNEVR_JSON_INCLUDE_DIR=$json
cmake --build build/scripting-mingw
wine build/scripting-mingw/nevr_script_conformance.exe   # also: nevr_script_registry_test, nevr_script_host_test
cmake -S src/scripting -B build/scripting-android -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static \
  -DNEVR_SCRIPT_VM=luau -DNEVR_JSON_INCLUDE_DIR=$json
cmake --build build/scripting-android
qemu-aarch64 build/scripting-android/nevr_script_conformance
```

`nevr_script_conformance --bench` prints the per-call cost and state size as JSON lines. `--pattern-dos` and
`--gc-dos` check that a runaway inside a C call, and a runaway finalizer, are stopped. `-DNEVR_SCRIPT_VM=null`
builds the same executables with no VM; that is the size baseline.

## What the prototype proves

At `lane-lua/design`, both targets give the same results: mingw-w64 under wine and NDK arm64 under qemu.

| Executable | Result |
| --- | --- |
| `nevr_script_registry_test` | 16/16 |
| `nevr_script_host_test` | 10/10 |
| `nevr_script_conformance` | 19/19, 14 of 14 sandbox probes refused |
| `--pattern-dos` | stopped |
| `--gc-dos` | stopped |

## What is left

1. **Embedding.** Link `nevr_script_host` and the Luau binding into `BugSplat64.dll` and the Quest library.
   Then hand `NevrHostApi` to native plugins through `NvrGameContext`, which is a plugin ABI change.
2. **The first real override point.** Hook the CJson readers per module and register real keys:
   - **Windows:** detour through `Hooking::AttachPublished`.
   - **Quest:** pin the Real, Int and Boolean GOT slots (`0x36dc158`, `0x36e2050`, `0x36f6988`) next to the
     TString slot `0x36ebe08` in `src/quest/sentinel/pinned_targets.h`.
   - **Still open:** whether player physics values are read through CJson at all, and how often reads happen
     on the tick path.
3. **The first real hook point.** A named game function, detoured once by the runtime and invoked through
   `Registry::Invoke`.
4. **Per-script memory.** One shared state with a `luaL_sandboxthread` per script and `lua_setmemcat`
   accounting, measured against today's ~370 KB per script.
5. **Real arm64 timing.** Run `nevr_script_conformance --bench` on a Quest (adb, no game).
6. **Report the manifests.** Add the script manifests to the login's plugin report
   (`src/runtime/ext/plugin_manifest.h`).

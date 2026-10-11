# Lua scripting

Status: prototype (refs #440). This document says what the prototype proves and what is left.

## What a script can do

A script is one Lua file. It changes the game through the same plain C ABI a native plugin uses
(`src/extension/host_api.h`), wrapped one to one:

```lua
-- mod.lua
nevr.override("physics.gravity", -5.0)          -- a data override: the runtime applies it

nevr.hook("test.add", {                         -- a runtime-owned hook point
  pre  = function(h) h:set("a", h:get("a") * 2) end,
  post = function(h) h:set("result", h:get("result") + 1) end,
})
```

| Script API | C ABI it wraps | Failure |
| --- | --- | --- |
| `nevr.override(key, value)` | `override_set` | returns `nil, "<STATUS>: <reason>"` |
| `nevr.hook(name, {pre=, post=})` | `hook_add` (one call per phase) | returns `nil, "<STATUS>: <reason>"` |
| `h:get(field)` / `h:set(field, v)` | `call_get` / `call_set` | `h:set` raises; the callback fails |
| `h:skip()` | pre callback returns `NEVR_HOOK_SKIP_ORIGINAL` | |
| `nevr.log(level, msg)`, `print(...)` | `log` | |

The full script-visible contract, the sandbox and the limits are specified in `src/scripting/script_vm.h`
and held by `src/scripting/tests/conformance_test.cpp`.

## The host API (#440)

`src/extension/host_api.h` is the published surface; `src/scripting/host_registry.{h,cpp}` is the
runtime side.

- **Owners.** The host issues one `NevrOwner` per plugin or script, in `plugins:` order. Every state
  change names its owner, so the host can attribute it, remove all of it at once (disable, hot reload),
  and a script cannot act under another owner's name.
- **Data overrides.** The first owner to set a key keeps it; a later owner gets `NEVR_ERR_CONFLICT`,
  and one `override_conflict` record names both. The holder may change its own value.
- **Hook points.** The runtime installs the only detour on a named function and registers it as a hook
  point with declared, typed fields and the phase each may be written in. Callbacks chain by owner
  order, then by registration within an owner; collisions between owners cannot happen because no
  owner installs a detour. Chains are copy-on-write, so a game thread invoking a hook point never sees
  a chain change under it.
- **Failure policy.** A callback that fails (`NEVR_HOOK_FAILED`, a script error) is logged
  (`callback_failed`, with the reason) and the call goes on. An owner that breaches a limit is disabled:
  its callbacks are skipped from that moment, even later in the same call, and its overrides are
  dropped (`owner_disabled`, with the limit named).
- **Records.** Every registry event is one structured record with a stable `event` name
  (`owner_opened`, `override_set`, `override_conflict`, `hook_added`, `hook_unknown`, `callback_failed`,
  `owner_disabled`, `owner_reset`, `owner_log`), the owner, the other owner on a conflict, the key or
  hook point, and a detail.

## Choosing the VM

Pending the prototype measurements.

## What is left

Pending.

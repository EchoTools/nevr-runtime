# nevr_example -- Reference Plugin (BAC for nEVR Plugin API v5)

This is THE reference plugin for the nEVR runtime plugin API. Community plugin
authors copy from it. Every plugin feature the host supports is demonstrated in
`plugins/example/src/plugin.cpp` with comments that explain WHY, not just WHAT.

**This file is the Behavioral Acceptance Criterion for the plugin API.** When the
API adds a feature, this file demonstrates it. When a community author asks "how
do I...", the answer is in `plugins/example/src/plugin.cpp`.

## What it demonstrates

| Concept                             | Implementation                                                                   |
| ----------------------------------- | -------------------------------------------------------------------------------- |
| Plugin metadata (REQUIRED)          | `NvrPluginGetInfo()` -- name, description, semver                                |
| API versioning (optional, v1+)      | `NvrPluginGetApiVersion()` -- returns `NEVR_PLUGIN_API_VERSION` (currently 5)   |
| Capability declaration (optional, v3+) | `NvrPluginGetCapabilities()` -- bitmask of what this plugin DOES               |
| v4 args-aware init (optional)       | `NvrPluginInitEx(ctx, args_json)` -- with ctx_size check and args parsing       |
| v3 legacy init (optional)           | `NvrPluginInit(ctx)` -- fallback when InitEx absent                             |
| Per-frame tick (optional)           | `NvrPluginOnFrame(ctx)` -- stub with usage docs                                |
| Game state transitions (optional)   | `NvrPluginOnGameStateChange(ctx, old, new)` -- stub with usage docs            |
| Cleanup (optional)                  | `NvrPluginShutdown()` -- reverse-order hook removal                             |
| ctx_size runtime discovery (v5+)    | Compares `ctx->ctx_size` against `sizeof(NvrGameContext)` to discover query API |
| Plugin query API (v5+)              | `ctx->get_plugin_count()` and `ctx->get_plugin_info(n)` for neighbour discovery |
| Structured logging                  | `NEVR_DEFINE_PLUGIN_LOG` macro from `plugin_logger.h`                           |
| Config args from config.yaml (v4+)  | Parses `args_json` with `nlohmann::json` -- logs known-safe keys only          |
| Local config loading                | `LoadConfigFile("_local/config.json")` with graceful absence handling           |
| Safe hooking                        | ResolveVA_Checked + ValidatePrologue + HookManager::CreateAndEnable             |
| Graceful degradation                | Plugin works without the game binary (all hooks skip when unmapped)             |
| `NEVR_PLUGIN_API` macro             | `extern "C"` dllexport/dllimport via `NEVR_PLUGIN_EXPORTS`                      |
| `NEVR_PLUGIN_EXPORTS` define        | Switches the macro from dllimport to dllexport                                  |

## Plugin API lifecycle

```
DLL load
  LoadLibrary("nevr_example.dll")

1. NvrPluginGetInfo()              REQUIRED -- first call
     Returns name, description, semver. The host logs this before
     any init so operators can see what loaded. The struct is
     returned BY VALUE; string pointers must point to static storage
     (string literals or global buffers -- not stack variables).

2. NvrPluginGetApiVersion()         OPTIONAL (v1+)
     Returns NEVR_PLUGIN_API_VERSION (currently 5). Absent = v1
     (pre-versioning). The host uses this to decide whether the
     plugin understands newer context fields.

3. NvrPluginGetCapabilities()       OPTIONAL (v3+)
     Returns a bitmask of NvrPluginCapabilities. Absent = UNDECLARED
     (0x00), which a strict server treats as unsafe. Declare what
     your plugin DOES -- there is no enforcement, only trust.

4. NvrPluginInitEx(ctx, args_json)  OPTIONAL (v4+)
     OR NvrPluginInit(ctx)          OPTIONAL (v3 fallback)
     Called ONCE, after game modules load, before the first tick.
     The host prefers InitEx when both are exported; it does NOT
     call both. Return 0 for success, non-zero to abort load.
     A `required: true` plugin returning non-zero is FATAL on a
     server -- the process aborts.

── Game running ─────────────────────────────────────────

5. NvrPluginOnFrame(ctx)            OPTIONAL
     Called every server/client tick (~60-90 Hz). Runs on the main
     thread. MUST NOT block, allocate heavily, or contend on locks.

6. NvrPluginOnGameStateChange(ctx, old, new)  OPTIONAL
     Called on ENetGameState transitions AFTER the transition.
     Use to defer init of game-state-dependent systems.

── Shutdown ──────────────────────────────────────────────

7. NvrPluginShutdown()              OPTIONAL
     Called in REVERSE load order before FreeLibrary. Remove your
     hooks, close sockets, join threads. After this returns, your
     DLL is unmapped -- any thread still running is a crash.
```

## File layout

```
plugins/example/
├── CMakeLists.txt      # Build target (follows standard plugin conventions)
├── README.md           # This file
└── src/
    └── plugin.cpp      # Single-source plugin implementation (THE reference)
```

## Building

The plugin is wired into the main build via `plugins/CMakeLists.txt`.
Build alongside all other components:

```sh
just build
```

The output DLL lands at `build/<preset>/bin/plugins/nevr_example.dll`.

## NvrGameContext fields

Passed to init, on-frame, and on-state-change callbacks.

| Field          | Type         | Present | Description                                                   |
| -------------- | ------------ | ------- | ------------------------------------------------------------- |
| `base_addr`    | `uintptr_t`  | v1+     | echovr.exe ImageBase. ALWAYS resolve hook VAs through this -- ASLR can rebase the image. |
| `net_game`     | `void*`      | v1+     | CR15NetGame* if available, else nullptr. Valid only when `NEVR_HOST_HAS_NETGAME` is set in flags. |
| `game_state`   | `uint32_t`   | v1+     | ENetGameState enum value at call time.                        |
| `flags`        | `uint32_t`   | v1+     | NvrHostFlags bitmask describing the current host.             |
| `ctx_size`     | `uint32_t`   | v5+     | sizeof(NvrGameContext) as the host sees it. Compare against your own sizeof to discover trailing fields. |
| `get_plugin_count` | `int (*)(void)` | v5+ | Number of plugins loaded so far. The count never decreases. |
| `get_plugin_info`  | `const NvrLoadedPluginInfo* (*)(int)` | v5+ | Plugin at index n. Process-lifetime stable pointer. nullptr if out of range. |

### Host capability flags (NvrHostFlags)

| Flag                         | Value  | Meaning                                   |
| ---------------------------- | ------ | ----------------------------------------- |
| `NEVR_HOST_HAS_NETGAME`      | 0x01   | `net_game` pointer is valid              |
| `NEVR_HOST_IS_SERVER`        | 0x02   | Running as dedicated server               |
| `NEVR_HOST_IS_CLIENT`        | 0x04   | Running as a client                       |
| `NEVR_HOST_COMBAT_MODE`      | 0x08   | Game is in combat mode                   |
| `NEVR_HOST_IS_HEADLESS`      | 0x10   | No graphics/audio (headless server mode)  |

### Plugin capability bits (NvrPluginCapabilities)

Returned by `NvrPluginGetCapabilities()`. Bitwise OR to combine.

| Capability                        | Value  | Description                                                       |
| --------------------------------- | ------ | ----------------------------------------------------------------- |
| `NEVR_PLUGIN_CAP_UNDECLARED`      | 0x00   | Absence of a claim. NOT a claim of harmlessness.                 |
| `NEVR_PLUGIN_CAP_OBSERVES_ONLY`   | 0x01   | Reads game state, never writes it.                               |
| `NEVR_PLUGIN_CAP_COSMETIC`        | 0x02   | Visuals/audio only, no gameplay effect.                          |
| `NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY` | 0x04   | Changes physics, weapons, movement.                              |
| `NEVR_PLUGIN_CAP_ALTERS_RULES`    | 0x08   | Changes match rules, scoring, or the game mode itself.           |
| `NEVR_PLUGIN_CAP_NETWORK`         | 0x10   | Opens sockets or talks to an external service.                   |
| `NEVR_PLUGIN_CAP_HOOKS_ENGINE`    | 0x20   | Installs its own detour infrastructure (separate MinHook, VEH). |

### What you must NOT do

- **Don't block** in `NvrPluginOnFrame`. Runs on the main thread.
- **Don't leak** handles, memory, or threads. Shutdown must clean up everything.
- **Don't log secrets**. `args_json` can contain keys/tokens. Log individual known keys, not the whole string.
- **Don't call `MH_DisableHook(MH_ALL_HOOKS)`**. That disables hooks belonging to other plugins and the host.
- **Don't call `MH_Uninitialize()`**. The host owns the MinHook lifecycle.
- **Don't assume load order**. Use the query API (v5+) to discover neighbours.
- **Don't return different `NvrPluginInfo` on different calls**. The host may call `NvrPluginGetInfo` more than once.
- **Don't point `NvrPluginInfo` string fields at stack/temporary storage**. They must outlive the call.

## Helper libraries

All plugins link against `nevr_plugin_common`, an INTERFACE library providing:

| Header                    | Provides                                                                    |
| ------------------------- | --------------------------------------------------------------------------- |
| `plugin_interface.h`      | Types, enums, `NEVR_PLUGIN_API` export macro                               |
| `plugin_logger.h`         | `NEVR_DEFINE_PLUGIN_LOG(prefix)` -- printf-style logging to stderr         |
| `hook_manager.h`          | `nevr::HookManager` -- scoped MinHook lifecycle (CreateAndEnable, RemoveAll) |
| `nevr_common.h`           | `ResolveVA_Checked`, `ResolveVA_Unchecked`, `ValidatePrologue`, `LoadConfigFile` |
| `address_registry.h`      | Verified VA constants for game functions                                   |
| `yaml_config.h`           | `ParseYamlConfig` -- YAML subset parser                                    |

Add `nlohmann_json` and/or `minhook::minhook` to the plugin's own
`target_link_libraries` when those are needed.

## Hook safety checklist

When adding a hook to your plugin:

1. **Resolve the VA**: `nevr::ResolveVA_Checked(base, VA)` -- returns nullptr if
   the page is unmapped. Prefer `_Checked` for new code; `_Unchecked` is for hot
   paths where the address is known-good.
2. **Validate the prologue**: `nevr::ValidatePrologue(addr, expected_bytes, len)`
   -- catches wrong game versions BEFORE the hook writes anything. A blind write
   at the wrong VA silently corrupts the process.
3. **Use a HookManager instance**: Call `RemoveAll()` in your shutdown function
   so your hooks don't outlive your plugin.
4. **Never call `MH_DisableHook(MH_ALL_HOOKS)`** -- that disables every hook in
   the process (crash handler, log system, other plugins).
5. **Never call `MH_Uninitialize()`** -- the host owns the MinHook lifecycle and
   will uninitialize after all plugins shut down.
6. **Match the calling convention**: Game functions use `__fastcall` (first two
   integer/pointer args in RCX, RDX). Getting this wrong produces crashes that
   look like heap corruption.
7. **Check `MH_ERROR_ALREADY_INITIALIZED`**: `MH_Initialize()` returns this
   harmlessly if the host or an earlier plugin already called it. Do not treat
   it as an error.

## ctx_size pattern (v5+ query API discovery)

The host always fills `ctx->ctx_size = sizeof(NvrGameContext)` from the host's
perspective. A plugin compares this against its own compile-time `sizeof` to
discover whether the trailing query API fields are present:

```cpp
if (ctx->ctx_size >= sizeof(NvrGameContext)) {
    // Safe: host's struct is at least as large as ours.
    // get_plugin_count and get_plugin_info pointers are valid.
    int count = ctx->get_plugin_count();
    for (int i = 0; i < count; i++) {
        const NvrLoadedPluginInfo* other = ctx->get_plugin_info(i);
        // ...
    }
} else {
    // Pre-v5 host sent a smaller struct. Trailing fields are at
    // offsets the host didn't write -- their values are undefined.
}
```

This is a RUNTIME check, not a compile-time `#ifdef`. The same binary works on
v4 and v5 hosts, degrading gracefully when the query API is absent.

## API version compatibility

NEVR_PLUGIN_API_VERSION is currently **5** (N134 S8).

| Version | Added                                                          | Breaking? |
| ------- | -------------------------------------------------------------- | --------- |
| v1      | `NvrPluginGetInfo`, `NvrPluginInit`, `NvrPluginShutdown`      | --        |
| v2      | `NvrPluginGetApiVersion` (compatibility signaling)             | No        |
| v3      | `NvrPluginGetCapabilities` (N114)                             | No        |
| v4      | `NvrPluginInitEx(ctx, args_json)` + config-driven loading (N134 S6) | No       |
| v5      | `ctx_size` + `get_plugin_count` / `get_plugin_info` query API (N134 S8) | No       |

All additions are tail-extensions -- a plugin compiled against an older version
loads unchanged on a newer host, and a plugin compiled against a newer version
degrades gracefully on an older host by checking `ctx_size` at runtime.

The host resolves `NvrPluginGetApiVersion` via `GetProcAddress`. If absent, the
plugin is v1 (pre-versioning). Fully backward-compatible -- existing plugins
don't need recompilation.

### Config args contract (v4+)

When `NvrPluginInitEx` is exported, the host passes the plugin's `args` map from
its `config.yaml` entry serialized to a flat JSON object string:

- Keys are flattened dotted paths (a nested YAML `a: {b: 1}` becomes `"a.b"`)
- Values are strings (post-interpolation; the host resolves `${VAR}` refs before serialization)
- When the entry declared no args, `args_json` is the empty object `"{}"` (never null)

Example: `{"greeting":"hi","limits.max":"5"}`

## Writing a new plugin

1. Copy this directory as a starting point.
2. Rename the target in `CMakeLists.txt` and the `OUTPUT_NAME`.
3. Replace the name/description in `NvrPluginGetInfo`.
4. Choose a log prefix that matches the binary name.
5. Set `NEVR_PLUGIN_EXPORTS` in your build (or define it before the include).
6. Implement `NvrPluginInitEx` for v4+ args support and `NvrPluginInit` as fallback.
7. Declare your capabilities honestly in `NvrPluginGetCapabilities`.
8. Add your hooks and logic following the hook safety checklist above.
9. Add `add_subdirectory(your_plugin)` to `plugins/CMakeLists.txt`.

# nevr_example — Minimal Reference Plugin

A minimal, fully-documented plugin for the nEVR runtime plugin API v3.
Use this as a starting point when creating new plugins.

## What it demonstrates

| Concept | Implementation |
|---|---|
| Plugin metadata | `NvrPluginGetInfo()` returns name, description, version |
| API versioning | `NvrPluginGetApiVersion()` returns `NEVR_PLUGIN_API_VERSION` |
| Capability declaration | `NvrPluginGetCapabilities()` returns what the plugin **does** |
| Structured logging | `NEVR_DEFINE_PLUGIN_LOG` macro from `plugin_logger.h` |
| Config loading | Reads `_local/config.json` with `LoadConfigFile` + `nlohmann::json` |
| Safe hooking | `nevr::ResolveVA` + `nevr::ValidatePrologue` before `HookManager::CreateAndEnable` |
| Graceful degradation | Plugin works without the game binary (hooks are skipped) |
| Cleanup | `NvrPluginShutdown()` removes all hooks via `HookManager::RemoveAll` |

## File layout

```
plugins/example/
├── CMakeLists.txt      # Build target (follows standard plugin conventions)
├── README.md           # This file
└── src/
    └── plugin.cpp      # Single-source plugin implementation
```

## Building

The plugin is wired into the main build via `plugins/CMakeLists.txt`.
Build alongside all other components:

```sh
just build
```

The output DLL lands at `build/<preset>/bin/nevr_example.dll`.

## Plugin API lifecycle

```
NvrPluginGetInfo()       — required: returns metadata used by the plugin loader
NvrPluginGetApiVersion() — optional: API version compiled against. Absent = v1.
NvrPluginGetCapabilities() — optional but PLEASE DECLARE: what this plugin does.
                             Absent = UNDECLARED, which a server reads as
                             "unknown", not as "harmless".
NvrPluginInit(ctx)       — optional: init. Called once after game modules load.
                            Receives NvrGameContext with base_addr and flags.
                            Return 0 for success, non-zero to abort load.
NvrPluginShutdown()      — optional: cleanup before DLL unload.
```

Additional optional exports (not shown here): `NvrPluginOnFrame(ctx)` for
per-frame ticks and `NvrPluginOnGameStateChange(ctx, old, new)` for game
state transitions.

## Helper libraries

All plugins link against `nevr_plugin_common`, an INTERFACE library providing:

| Header | Provides |
|---|---|
| `plugin_logger.h` | `NEVR_DEFINE_PLUGIN_LOG(prefix)` — printf-style logging macro |
| `hook_manager.h` | `nevr::HookManager` — scoped MinHook lifecycle (CreateAndEnable, RemoveAll) |
| `nevr_common.h` | `ResolveVA`, `ValidatePrologue`, `LoadConfigFile` |
| `address_registry.h` | Verified VA constants for game functions |
| `yaml_config.h` | `ParseYamlConfig` — YAML subset parser |
| `nevr_plugin_interface.h` | Plugin lifecycle types and `NEVR_PLUGIN_API` export macro |

Add `nlohmann_json` and/or `minhook::minhook` to the plugin's own
`target_link_libraries` when those are needed.

## Hook safety checklist

When adding a hook to your plugin:

1. Resolve the VA: `nevr::ResolveVA(base, VA)` — returns nullptr if unmapped.
2. Validate the prologue: `nevr::ValidatePrologue(addr, expected_bytes, len)` —
   catches wrong game versions before the hook corrupts anything.
3. Use a `HookManager` instance — call `RemoveAll()` in your shutdown function
   so your hooks don't outlive your plugin.
4. Never call `MH_DisableHook(MH_ALL_HOOKS)` — that disables every hook in the
   process (crash handler, log filter, other plugins).
5. The detour must match the original calling convention. Game functions use
   `__fastcall` (first two args in RCX, RDX).

## Writing a new plugin

1. Copy this directory as a starting point.
2. Rename the target in `CMakeLists.txt` and the `OUTPUT_NAME`.
3. Replace the name/description in `NvrPluginGetInfo`.
4. Choose a log prefix that matches the binary name.
5. Add your hooks and logic in `NvrPluginInit`.
6. Add `add_subdirectory(your_plugin)` to `plugins/CMakeLists.txt`.

## API version compatibility

| Plugin exports | API version |
|---|---|
| No `NvrPluginGetApiVersion` | v1 (pre-versioning) |
| `NvrPluginGetApiVersion` returns 2 | v2 |
| `NvrPluginGetApiVersion` returns 3 | v3 (current) — adds `NvrPluginGetCapabilities` |

### Declaring capabilities

`NvrPluginGetCapabilities()` returns a bitwise OR of `NvrPluginCapabilities`:
`OBSERVES_ONLY`, `COSMETIC`, `ALTERS_GAMEPLAY`, `ALTERS_RULES`, `NETWORK`,
`HOOKS_ENGINE`. A server needs this to judge whether a session is valid when
community game-mode plugins are loaded.

It is a **declaration, not an enforcement** — the host cannot verify it, and a
plugin that lies is believed. That is precisely why an inaccurate declaration is
worse than none: it spends trust the ecosystem needs. Declare honestly and
specifically.

The host logs a warning if a plugin requires a newer API version but loads it
anyway. Adding new optional exports or host flags does not require a version
bump.

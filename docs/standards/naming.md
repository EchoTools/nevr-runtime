# Naming: one name, three cases, four letters

How everything in nevr-runtime is named. One rule governs all of it: the runtime's own name is spelled
`n-e-v-r`, and the case it appears in is determined by what the name *is*, not by the author's mood. A name
that can't say which case it is — or that drops a letter, or re-capitalises one — is wrong and gets fixed.

This is the reference the naming pass (#131) applies. It documents the pattern first, and the anti-patterns
the pass will reconcile, because the pattern has to exist in writing before a rename touches anything —
including the published plugin/module C ABI (#130), which is a breaking change.

## The three cases

| case | spells | what it names | example |
|---|---|---|---|
| `NEVR` | UPPER_SNAKE_CASE | macros, `#define` constants, enumerator values, DLL export/import markers, log tags | `NEVR_PLUGIN_API_VERSION`, `NEVR_HOST_IS_SERVER`, `NEVR_PLUGIN_API` |
| `Nevr` | PascalCase | types (class/struct/enum/alias), functions, C-ABI symbols, namespaces | `NevrConfig`, `NevrPluginInit`, `Nevr::Lifecycle` |
| `nevr` | lowercase | files, directories, libraries, DLLs, config keys, log categories, git refs | `nevr_config.h`, `libnevr_core.a`, `nevr.dll`, `nevr_http_uri` |

The three cases are positional, not interchangeable. `Nevr` in a filename is wrong for the same reason
`nevr` on a macro is wrong: the case is part of the name's meaning.

## The spelling

Four letters — `n`, `e`, `v`, `r`, in order. No letter dropped, none re-capitalised.

### Anti-patterns — in the tree now, do not repeat

| anti-pattern | what's wrong | where | fix |
|---|---|---|---|
| `Nvr…` | dropped the `e` | the whole published plugin/module C ABI — `NvrPluginInfo`, `NvrGameContext`, `NvrHostFlags`, `NvrModuleContext`, `NvrPluginInit`, … (`src/extension/plugin_interface.h`, `src/extension/module_interface.h`) | `Nevr…`. Breaking: a published third-party ABI, so it is a deliberate, versioned migration (#130), not an inline rename. New C-ABI surface uses `Nevr`; never extend `Nvr`. |
| `NevRUPnPConfig` | re-capitalised the middle `r` | `src/runtime/hook/patching.h:57` | `NevrUpnpConfig` |
| `NevrCfgGetFlat` | abbreviated `Config` to `Cfg` | `src/runtime/lifecycle/service_config.h:18` | `NevrConfigGetFlat` — abbreviations are not allowed, and its sibling type is already `NevrConfig` |
| `NEVR_GetUPnPConfig` | a function spelled in macro case | `src/runtime/lifecycle/initialize.cpp:230` | `NevrGetUPnPConfig` — a function is `Nevr`, a macro is `NEVR`; a name can't be both |

## Namespaces

PascalCase. The runtime's own namespace is `Nevr`; sub-namespaces nest under it in PascalCase:
`Nevr::Lifecycle`, `Nevr::Config`, `Nevr::PluginConfig`.

The tree spells the runtime namespace four ways today, and three of them are wrong:

| spelling | where | verdict |
|---|---|---|
| `Nevr::Lifecycle` | `src/runtime/lifecycle/system_module_loader.cpp:7` | correct |
| `nevr` (and `nevr::lifecycle`) | `src/core/nevr_config.h:22`, `src/core/curl_global.h:15`, `src/core/schannel_cred_guard.h:15`, `src/runtime/lifecycle/login_redirect_override.h:5` | migrate to `Nevr` / `Nevr::Lifecycle` |
| `nevr_cfg` | `src/runtime/lifecycle/service_map.h:28` | migrate to `Nevr::Config` |
| `nevr_plugincfg` | `src/runtime/ext/plugin_load_plan_build.h:17` | migrate to `Nevr::PluginConfig` |

Subsystem namespaces already use PascalCase and stay as they are: `GameServer`, `Social`, `TokenAuth`,
`Hooking`, `HookGuard`, `ScenarioControl`, `CrashRecovery`. Those are component names, not the runtime's own
name, so they do not take the `Nevr` prefix — see [Component names](#component-names).

**Exempt (not ours to rename):** the game-ABI namespace `EchoVR::` (`src/abi/echovr.h`) is the game's own
reconstructed name — a proper noun in the game's spelling, not subject to the `Nevr` rule. The generated
protobuf namespace `rtapi::v1::` is BSR-owned and never hand-edited. Third-party namespaces (`ix`, `std`) are
the libraries' own.

## Component names

A component (subsystem, directory, library target) is named by what it does — PascalCase when it is a
type/namespace, lowercase when it is a file/directory. The `Nevr` prefix is reserved for the runtime's *own*
name; a component that is part of the runtime sits inside the `Nevr` namespace rather than repeating the prefix.

Correct: `GameServer`, `Social`, `TokenAuth` (types/namespaces); `src/runtime/server/`, `src/core/`
(directories); `libnevr_core.a`, `libnevr_abi.a` (the two libraries that ARE the runtime's own core and ABI,
so they carry the `nevr` stem).

Wrong: a subsystem type named `NevrGameServer` (the runtime prefix stacked on the component name); a directory
named `src/Nevr/`.

## Per-category reference

| kind | case | rule | example |
|---|---|---|---|
| file / directory | lowercase (`_`-joined when multi-word) | the runtime's own files use the `nevr` stem | `nevr_config.cpp`, `auth_token.h`, `src/runtime/lifecycle/` |
| library / DLL | lowercase | `nevr` stem, `_`-joined | `libnevr_core.a`, `libnevr_abi.a`, `nevr.dll` |
| macro / `#define` | `NEVR_` + UPPER_SNAKE | the `NEVR_` prefix is mandatory on the runtime's own macros | `NEVR_PLUGIN_API_VERSION`, `NEVR_MODULE_API` |
| enumerator | `NEVR_` + UPPER_SNAKE | enumerators are macro-shaped even inside an `enum` | `NEVR_HOST_IS_SERVER`, `NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY` |
| type (class/struct/enum/alias) | PascalCase | `Nevr` prefix only on the runtime's own public types; subsystem types are unprefixed | `NevrConfig`, `NevrConfigError`, `ServerContext`, `AuthConfig` |
| function | PascalCase | `Nevr` prefix only on the runtime's own entry-point/exported functions | `GetString`, `LoadFromFile`, `ReadUPnPConfig`, `NevrPluginInit` |
| C-ABI symbol | `Nevr` + PascalCase | the cross-DLL surface is always `Nevr`, never `Nvr` | `NevrPluginInit`, `NevrModuleGetApiVersion` |
| variable / member | lowercase `_`-joined; members get a trailing `_` | | `is_server`, `old_state`, `plugins_`, `empty_` |
| parameter / local | lowercase `_`-joined | | `flat_key`, `yaml`, `path` |
| config key (flat) | lowercase `_`-joined, `nevr_` prefix on runtime-owned keys | | `nevr_http_uri`, `nevr_discord_id`, `asset_cdn_url` |
| config path (dotted) | lowercase, `.`-joined | | `services.matchmaking`, `services.serverdb` |
| git ref | lowercase, `/`-joined, disposable | | `feat/bugsplat-split-impl`, `docs/naming-standards` |

## Acronyms

An acronym inside a PascalCase name is treated as a word — first letter upper, the rest lower:
`TcpBroadcaster`, `NevrUpnpConfig`, `WsBridge`. The exception is the first-class protocol/data names the
project writes in caps everywhere (`UPnP`, `HTTP`, `TLS`, `JSON`, `YAML`), which stay uppercase when they
appear as a term (`UPnPHelper`, `JsonTraceRecord`). The rule is: one spelling per term, used everywhere.
`NevR` is never right — there is no re-capitalised middle letter.

## How it binds

- New names follow this table. A review that would otherwise pass flags a name that ignores it.
- Existing violations are the apply phase of #131. They are not fixed opportunistically one-file-at-a-time in
  unrelated changes — each is a named migration, so the rename and its test updates land as a unit.
- The published ABI rename (`Nvr…` → `Nevr…`) is #130 and is breaking for third-party plugins/modules; it is
  done as its own versioned change with the ABI-version bump called out, never folded silently into another
  commit.
- This doc is the source of truth. Where it and a name in the tree disagree, the tree is wrong and the doc is
  the fix target. If the doc itself is wrong, change the doc first, then the tree.

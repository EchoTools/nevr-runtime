# NEVR Naming Standard

**Required reading** for anyone adding a name to this repository. It covers how the project name is
spelled inside a name, and which spellings already in the tree are not models. It does not set case
rules for locals, members, enumerators or macros other than the project prefix: the tree has no single
convention for them to codify.

The rules below are for **new** names. The tree contains older names that break them; the section
"Names in the tree that are not models" lists the kinds, so nobody copies one.

## The spelling family

One family, three cases:

| Spelling | Use for new | Examples |
| --- | --- | --- |
| `NEVR` | macros, log tags, environment variables, DLL export symbols | `NEVR_PLUGIN_API`, `[NEVR.PATCH]`, `NEVR_SOCKET_URI`, `NEVR_RegisterResourceOverride` |
| `Nevr` | C++ type and function names that carry the project name | `NevrConfig` (`src/core/nevr_config.h`) |
| `nevr` | files, directories, CMake targets, namespaces, flat config keys the runtime owns | `src/core/nevr_config.h`, `nevr_core`, `namespace nevr`, `nevr_socket_uri` |

- Those three are the only spellings of the project name in an identifier. `Nvr` (no `e`) and every
  other mixed case (`nEVR`, `NeVR`, `NEvR`, `NevR`, ...) are anti-patterns.
- A name states the project once. Inside `namespace nevr`, a type is `Config`, not `NevrConfig`.
- A log tag is `[NEVR.<AREA>]` with an upper-case area. The areas in use are the `[NEVR.` tags in
  `src/` (`[NEVR.WS]`, `[NEVR.AUTH]`, `[NEVR.GAMESERVER]`, ...); `docs/standards/logging.md` says what
  a log line carries.
- A DLL export is `NEVR_` followed by a PascalCase verb phrase (`NEVR_GetUPnPConfig`). An export is
  never renamed in place: another DLL may resolve it by name (three are, in
  `plugins/common/include/resource_registry.h`).
- A flat config key the runtime owns carries the `nevr_` prefix (`nevr_http_uri`, `nevr_server_key`).

| Surface | New names |
| --- | --- |
| Directory, source file | lower case, words joined by `_` (`src/runtime/server/gameserver.cpp`) |
| CMake target | `nevr_<name>` (`nevr_core`, `nevr_abi`, `nevr_runtime`) |
| Namespace | `nevr` or `nevr_<area>` (`nevr_cfg`, `nevr_plugincfg`) |
| Macro, environment variable | `NEVR_<NAME>` |
| Log tag | `[NEVR.<AREA>]` |
| DLL export | `NEVR_<Verb><Noun>` |
| Published C ABI (new interface) | `Nevr*` types and functions, `NEVR_*` macros; never `Nvr*` |
| Flat config key | `nevr_<snake_case>` |
| Git branch | no rule: branch names are disposable and carry nothing (`AGENTS.md`, "Landing") |

Names that belong to the game or to third parties (`EchoVR::`, `BugSplat64`, `IServerLib`) keep their
own spelling.

## The frozen exception: the `Nvr*` plugin and module C ABI

`src/extension/plugin_interface.h` and `src/extension/module_interface.h` publish the C ABI that
third-party DLLs compile against, and the names there spell the prefix `Nvr`: `NvrPluginInfo`,
`NvrGameContext`, `NvrPluginInit`, `NvrPluginInitEx`, `NvrModuleContext`, and the rest. The host
resolves the eight plugin entry points with `GetProcAddress` (`src/runtime/ext/plugin_loader.cpp`), so
renaming one stops every already-built plugin from loading; the module names are the typedefs and
context structs statically linked modules compile against.

That set is frozen: it does not grow, and a new published surface uses `Nevr`/`NEVR_`. The set is
written out in `tools/tests/test_naming.py` (`FROZEN_NVR`), so adding or renaming a name is an edit
to that file. Whether the existing set is migrated (with a compatibility shim) or stays as a permanent
separate namespace is an open ABI decision, issue #130.

## Names in the tree that are not models

- The brand spelling `nEVR`: the `-n` program name in `cmake/codesign/sign.sh`, the certificate subjects
  under `certs/` (a certificate subject is a trust identity, so changing one is the owner's call), and the
  reference plugin's reported description (`plugins/example/src/plugin.cpp`, a string that reaches logs).
  `LEGACY_SPELLINGS` in `tools/tests/test_naming.py` lists each file and token; the list only shrinks.
- Three spellings of one lifecycle namespace: `Nevr::Lifecycle` (`src/runtime/lifecycle/system_module_loader.h`),
  `nevr_runtime::lifecycle` (`src/runtime/lifecycle/stable_string_pool.h`) and `nevr::lifecycle`
  (`src/runtime/lifecycle/login_redirect_override.h`).
- Exports without the `NEVR_` prefix: `TokenAuth_GetToken`, `token_auth_Init`, `platform_compat_Init`
  and their siblings (declared with `NEVR_MODULE_API`).
- Targets whose name is the name of a file users touch or of a third party, so `nevr_` is not applied:
  `echovr_server` (the launcher), `LibOVRPlatform64_1` (the stub DLL), `ovrplatformloader` (the Quest
  loader library) and `breakpad_client`.
- Flat config keys without it (`asset_cdn_url`, `telemetry_uri`, `upnp`), macros without `NEVR_`
  (`PROJECT_VERSION`, `GIT_COMMIT_HASH`), log tags outside `[NEVR.<AREA>]` (`[TELEMETRY.DIAG]`,
  a bare `[NEVR]`), and the PascalCase namespaces (`GameServer`, `TokenAuth`).
- `NEVRProtobufJSONMessageV1` (`src/abi/symbols.h`): its spelling is fixed by the hashed protocol string.
- `NevrCfg*` functions are global, not in `namespace nevr`; `NevrConfig` and `NevrConfigError` are in it.

`tools/tests/test_naming.py` enforces the two machine-checkable rules: no `Nvr` identifier outside the
frozen set, and no non-canonical spelling of the project name in a file (or token) that does not
already carry one. The other rules above are checked in review.

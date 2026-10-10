# nEVR Naming Standard

**Required reading** for anyone adding a name to this repository. It covers how the project name is
spelled inside a name, and which spellings already in the tree are not models. It does not set case
rules for locals, members, enumerators or macros other than the project prefix: the tree has no single
convention for them to codify.

The identifier rules below are for **new** names. The tree contains older names that break them; the section
"Names in the tree that are not models" lists the kinds, so nobody copies one.

## The spelling family

One family, three cases:

| Spelling | Use for new | Examples |
| --- | --- | --- |
| `NEVR` | macros, log tags, environment variables, DLL export symbols | `NEVR_PLUGIN_API`, `[NEVR.PATCH]`, `NEVR_SOCKET_URI`, `NEVR_RegisterResourceOverride` |
| `Nevr` | C++ type and function names that carry the project name | `NevrConfig` (`src/core/nevr_config.h`) |
| `nevr` | files, directories, CMake targets, namespaces, flat config keys the runtime owns | `src/core/nevr_config.h`, `nevr_core`, `namespace nevr`, `nevr_socket_uri` |

- Those three are the only spellings of the project name in an identifier. `Nvr` (no `e`) and every
  other mixed case (`nEVR`, `NeVR`, `NEvR`, `NevR`, ...) are anti-patterns in an identifier. In prose the
  project brand is spelled `nEVR`.
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
| Git branch | use the branch format in `AGENTS.md` under "Branch lifecycle" |
| Markdown document | kebab-case file name (`docs/standards/naming.md`, `docs/reference/client-exit-path.md`); a date prefix is `YYYY-MM-DD-` (`docs/design/2026-10-01-social-scenario-harness.md`); an ADR is `NNNN-<kebab-case>.md` |

Names that belong to the game or to third parties (`EchoVR::`, `BugSplat64`, `IServerLib`) keep their
own spelling.

Markdown. A file name is kebab-case, except the root scaffolding names that tools and GitHub look for by
exact name (`README.md`, `AGENTS.md`, `CONTRIBUTING.md`, `SECURITY.md`, `CHANGELOG.md`, `LICENSE`, `NOTICE`, and
the like) and the date prefixes and ADR numbers already in the tree. A heading is a descriptive phrase in
sentence case, as in `docs/standards/` and `docs/adr/` (an ADR's H1 is `ADR NNNN: <phrase>`).

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

- The brand spelling `nEVR` outside prose: the `-n` program name in `cmake/codesign/sign.sh`, the certificate
  subjects under `certs/` (a certificate subject is a trust identity, so changing one is the owner's call), and
  the reference plugin's reported description (`plugins/example/src/plugin.cpp`, a string that reaches logs).
  `LEGACY_SPELLINGS` in `tools/tests/test_naming.py` lists each file and token; the list only shrinks.
- Exports without the `NEVR_` prefix: `TokenAuth_GetToken`, `token_auth_Init`, `platform_compat_Init`
  and their siblings (declared with `NEVR_MODULE_API`).
- Library targets without `nevr_` (`platform_compat`, `token_auth`, `crash_handler`, `log_filter`),
  flat config keys without it (`asset_cdn_url`, `telemetry_uri`, `upnp`), macros without `NEVR_`
  (`PROJECT_VERSION`, `GIT_COMMIT_HASH`), log tags outside `[NEVR.<AREA>]` (`[TELEMETRY.DIAG]`,
  a bare `[NEVR]`), and the PascalCase namespaces (`GameServer`, `TokenAuth`).
- `NEVRProtobufJSONMessageV1` (`src/abi/symbols.h`): its spelling is fixed by the hashed protocol string.
- `NevrCfg*` functions are global, not in `namespace nevr`; `NevrConfig` and `NevrConfigError` are in it.

`tools/tests/test_naming.py` enforces the machine-checkable rules: no `Nvr` identifier outside the frozen set,
no non-canonical spelling of the project name in a file (or token) that does not already carry one
(`LEGACY_SPELLINGS`, which only shrinks, and `BRAND_PROSE_SPELLINGS`, the documents whose prose spells the brand
`nEVR`), one spelling of the lifecycle namespace (`nevr::lifecycle`), and the brand spelling `nEVR` in the
prose of documents. The other rules above are checked in review.

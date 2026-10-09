# NEVR Naming Standard

**Required reading** for anyone adding a name to this repository: a component, file, interface, log
tag, export, namespace, config key, CMake target or environment variable. The point is that a name is
chosen from this page, not by whoever happens to be writing the code that day.

## The spelling family

One family, three cases. Which case a name takes is decided by what kind of name it is.

| Spelling | Used for | Examples in the tree |
| --- | --- | --- |
| `NEVR` | preprocessor macros, log tags, DLL export symbols, environment variables | `NEVR_PLUGIN_API`, `NEVR_HOST_IS_SERVER`, `[NEVR.PATCH]`, `NEVR_RegisterResourceOverride` (`src/runtime/exports.def`), `NEVR_SOCKET_URI` |
| `Nevr` | C++ type and function names whose identifier carries the project name | `NevrConfig` (`src/core/nevr_config.h`), `NevrConfigError` (`src/core/nevr_config.cpp`) |
| `nevr` | files, directories, CMake targets, namespaces, library names, flat config keys | `nevr_config.h`, `nevr_core`, `nevr_abi`, `namespace nevr`, `"nevr_socket_uri"` |

Rules that follow from the table:

- The three spellings are the only spellings of the project name in an identifier. `Nvr` (no `e`) and
  the mixed cases `nEVR`, `NeVR`, `NEvR` are anti-patterns.
- A name states the project once. Inside `namespace nevr`, a type is `Config`, not `NevrConfig`;
  the existing `NevrConfig` and `NevrCfg*` names predate that and are not a model for new code.
- Log tags are `[NEVR.<AREA>]` with an upper-case area (`[NEVR.WS]`, `[NEVR.AUTH]`,
  `[NEVR.GAMESERVER]`). The area list is `docs/standards/logging.md`.
- Runtime-owned flat config keys carry the `nevr_` prefix (`nevr_http_uri`, `nevr_server_key`).
- A DLL export is `NEVR_` followed by a PascalCase verb phrase (`NEVR_GetUPnPConfig`). Exports are
  looked up by name by other DLLs, so an export is never renamed in place.

## The frozen exception: the `Nvr*` plugin and module C ABI

`src/extension/plugin_interface.h` and `src/extension/module_interface.h` publish the C ABI that
third-party DLLs compile against, and every symbol there spells the prefix `Nvr`: `NvrPluginInfo`,
`NvrGameContext`, `NvrPluginInit`, `NvrPluginInitEx`, `NvrModuleContext`, `NvrModuleInit`, and the
rest of the set the two headers define. The host resolves the function names with `GetProcAddress`,
so renaming one stops every already-built plugin from loading.

That set is frozen: it is the only place the `Nvr` spelling may appear, and no symbol is added to it
under a new `Nvr` name. A new published surface uses the `Nevr`/`NEVR_` spellings from the table.
Whether the existing set is migrated (with a compatibility shim) or stays as a permanent separate
namespace is an open ABI decision, issue #130.

`tools/tests/test_naming.py` enforces both halves: any `Nvr`-prefixed identifier in tracked source
must be one the two headers define, and the mixed-case spellings may not appear in a file that does
not already carry one.

## Surfaces

| Surface | Spelling | Check against |
| --- | --- | --- |
| Repository, project, prose | `nevr-runtime`, "NEVR" | `README.md` |
| Directory | lower case; a component directory is `_` or `-` joined as its neighbours are (`src/modules/token-auth`, `src/nevr_api`) | `ls src` |
| Source file | snake case, `.cpp` / `.h` | `src/runtime/server/gameserver.cpp` |
| CMake target | `nevr_<name>` for libraries the project owns | `nevr_core`, `nevr_abi`, `nevr_runtime` |
| Namespace | `nevr` or `nevr_<area>` | `namespace nevr`, `nevr_cfg`, `nevr_plugincfg` |
| Macro | `NEVR_<NAME>` | `NEVR_PLUGIN_API_VERSION` |
| Environment variable | `NEVR_<NAME>` | `NEVR_API_KEY`, `NEVR_SOCKET_URI` |
| Log tag | `[NEVR.<AREA>]` | `src/runtime/lifecycle/boot.cpp` |
| DLL export | `NEVR_<Verb><Noun>` | `src/runtime/exports.def` |
| Plugin/module C ABI | `Nvr<...>` (frozen, see above) | `src/extension/` |
| Flat config key | `nevr_<snake_case>` | `nevr_socket_uri` in `src/runtime/lifecycle/config.cpp` |

Names that belong to the game or to third parties (`EchoVR::`, `BugSplat64`, `IServerLib`) keep
their own spelling.

## Open items

- The `nEVR` brand spelling appears in prose and in the code-signing certificate subjects under
  `certs/`; it is not an identifier. A certificate subject is a trust identity, so changing it is the
  owner's call.
- Case rules for locals, members, enumerators and macros other than the project prefix are not
  standardised here: the tree has no single convention for them to codify.

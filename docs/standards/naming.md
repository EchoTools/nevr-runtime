# nEVR Naming Standard

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT", "SHOULD", "SHOULD NOT", "RECOMMENDED",
"NOT RECOMMENDED", "MAY", and "OPTIONAL" in this document are to be interpreted as described in BCP 14
([RFC 2119](https://www.rfc-editor.org/rfc/rfc2119), [RFC 8174](https://www.rfc-editor.org/rfc/rfc8174)) when, and only
when, they appear in all capitals, as shown here.

**Required reading** for anyone adding a name to this repository. It covers how the project name is
spelled inside a name, and which spellings already in the tree are not models. It does not set case
rules for locals, members, enumerators or macros other than the project prefix: the tree has no single
convention for them to codify. It also fixes the opening lines of an ADR, its read contract.

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
  other mixed case (`nEVR`, `NeVR`, `NEvR`, `NevR`, ...) are anti-patterns in an identifier; the one
  place `Nvr` remains is the deprecated alias of the plugin and module C ABI (below). In prose the
  project brand is spelled `nEVR`.
- A name states the project once. Inside `namespace nevr`, a type is `Config`, not `NevrConfig`.
- A log tag is `[NEVR.<AREA>]` with an upper-case area. The areas in use are the `[NEVR.` tags in
  `src/` (`[NEVR.WS]`, `[NEVR.AUTH]`, `[NEVR.GAMESERVER]`, ...); `docs/standards/logging.md` says what
  a log line carries.
- A DLL export is `NEVR_` followed by a PascalCase verb phrase (`NEVR_GetUPnPConfig`). An export is
  never renamed in place: another DLL may resolve it by name (three are, in
  `sdk/include/nevr/pc/resource_registry.h`).
- A flat config key the runtime owns carries the `nevr_` prefix (`nevr_http_uri`, `nevr_server_key`).

| Surface | New names |
| --- | --- |
| Directory, source file | lower case, whole words joined by `_`; a component's files are its name plus one role suffix ("Component names, file roles and placement") |
| CMake target | `nevr_<name>` (`nevr_core`, `nevr_abi`, `nevr_runtime`) |
| Namespace | `nevr` or `nevr_<area>` (`nevr_cfg`, `nevr_plugincfg`) |
| Macro, environment variable | `NEVR_<NAME>` |
| Log tag | `[NEVR.<AREA>]` |
| DLL export | `NEVR_<Verb><Noun>` |
| Published C ABI (plugins and modules) | `Nevr*` types, typedefs, functions and exports, `NEVR_*` macros; never `Nvr*` |
| Flat config key | `nevr_<snake_case>` |
| Runtime DLL file | `nevr-runtime.dll` (the file `nevr_host::ResolveRuntimePath` names, `kRuntimeFileName` in `src/runtime/host/runtime_load.cpp`; the file name is hyphenated, a CMake target is still `nevr_<name>`; `tools/tests/test_runtime_dll_name.py` forbids the one-word spelling) |
| Git branch | use the branch format in `AGENTS.md` under "Branch lifecycle" |
| Markdown document | kebab-case file name (`docs/standards/naming.md`, `docs/reference/client-exit-path.md`); a date prefix is `YYYY-MM-DD-` (`docs/design/2026-10-01-social-scenario-harness.md`); an ADR is `NNNN-<kebab-case>.md` |

Names that belong to the game or to third parties (`EchoVR::`, `BugSplat64`, `IServerLib`) keep their
own spelling. A plugin someone else publishes is named as they name it (`NvrContentOverlay`); the names
that docs and config examples carry keep that spelling.

Markdown. A file name is kebab-case, except the root scaffolding names that tools and GitHub look for by
exact name (`README.md`, `AGENTS.md`, `CONTRIBUTING.md`, `SECURITY.md`, `CHANGELOG.md`, `LICENSE`, `NOTICE`, and
the like), the date prefixes and ADR numbers already in the tree, and a component's doc, which carries the component's name exactly (see "Directories"). A heading is a descriptive phrase in
sentence case, as in `docs/standards/` and `docs/adr/` (an ADR's H1 is `ADR NNNN: <phrase>`).

## The read contract at the top of an ADR

Every file in `docs/adr/` opens with the same six lines, then a blank line, then its `# ADR NNNN: <phrase>` heading:

```
---
# atwd read contract
read: whole
read_via: cat docs/adr/<this file's own name>
audience: all
---
```

The block says how the document is to be read before anyone acts on it. An ADR states a decision in one section, the
alternatives in another and the consequences in a third, and cites other ADRs by file and line, so a reader who takes one
section can act on a decision whose limits are in a part they did not read. The block makes the whole read a stated rule
of the document. `atwd` ("all the way down") is the maintainers' name for this convention, in which each document of a
body of rules declares its own extent and audience. The heading line is a YAML comment that marks the block, and the
three keys are:

| Key | Meaning | Value in an ADR |
| --- | --- | --- |
| `read` | How much of the document is to be read before acting on it. `whole` is every line, as one read: no `head`, `tail`, `sed -n`, `grep` or offset and limit. The convention also has `head` (the first lines are required, the rest is read on demand) and `query` (the document is read only through a query tool). | `whole` |
| `read_via` | The command that reads the document the way `read` requires, run from the repository root. | `cat docs/adr/<own file name>` |
| `audience` | Whom the contract binds. `all` is every reader of the repository, a person or an agent, not one role or project. The convention also has narrower values (roles, seats, a project's seats). | `all` |

A new ADR carries the block, with `read_via` naming its own file. Nothing in this repository enforces it: no test or
check reads the block, and the hook that warns on a partial read of a file that declares one runs on one maintainer's
machine, outside the repository. For a reader without that hook, this section is the whole rule: read the ADR to its
last line, with the `read_via` command, before relying on any part of it.

## Component names, file roles and placement

A **component** is one behaviour the runtime adds to or changes in the game, with its own switch, its own log events or
its own hook: a module of `src/quest/combat/` (ADR 0046 rule 1), a patch in `src/runtime/patch/`, an EchoCombat feature in
`src/runtime/combat/`. Its name is the stem of its files, the last component of its namespace, the middle of its log
events and its config key, so a grep for the name finds all of it.

### The name

- A component name MUST be lower-case whole words joined by `_` (`impact_sound`, `drone_muzzle`, `refresh_rate`). It
  MUST NOT squash words together (`impactsnd`, `dronemuzzle`, `tablestep`) or shorten a word (`snd`, `inst`, `wd`,
  `ctx`, `pfx`).
- An abbreviation MAY be a word of the name only when it passes one of two tests, and the review states which:
  1. **The game spells it that way.** It is a whole word, in that spelling, of the game's own symbol, script element or
     resource for the thing the component acts on (`bvh` from `CBVHTree`, `ai` from `CR15NetAIScenarios`), and the
     component doc cites the symbol.
  2. **It is on this list**, of terms with one meaning in general computing: `ai`, `bvh`, `cdn`, `elf`, `got`, `gpu`,
     `hud`, `id`, `obb`, `tls`, `ui`, `uri`, `url`, `voip`, `xpid`. A term is added to the list in a change to this
     section, never in the change that first uses it.
  A word of a third party's product name (EchoCombat's `ec`, VeilNet, DroneFix, PowerBoost; the reference build's
  `afk_*`) passes neither test.
- A component has **one name on PC and Quest.** Where both platforms carry it, every surface spells it the same way:
  the file stems (`src/runtime/<area>/<name>.cpp`, `src/quest/<area>/<name>.cpp`), the shared rules header
  (`<name>_rules.h`), the namespace's last component (`nevr_quest::combat::<name>`), the log events
  (`combat_<name>_*`), the config key (`combat.<name>`, and `combat.<name>_<option>` for its options), the doc
  (`docs/quest/combat/<name>.md`), the mutant file (`mutants.d/<name>.sh`) and the tests (`<name>_test.cpp`, and
  `<name>_rules_test.cpp` for the shared rules header).
- A component's name states the area once: inside `src/quest/combat/` it is `music`, not `combat_music`. The area's own
  infrastructure, which is no component (`combat_install`, `combat_tick`, `combat_level`), carries the area's name.
- The name a component had upstream (a q15t-upstream module, an EchoCombat plugin file, a reference-build `afk_*` file)
  MUST NOT be a stem, namespace, event, key or doc name. It survives in one place: the origin note ADR 0046 rule 9
  requires at the top of the component's header, which cites the upstream file and line at its commit. A string the
  game's content or another binary resolves by name (an export such as `re33_ec_page`, a config key a released build
  reads, a symbol a script calls) keeps its spelling; the exports rule above already says an export is never renamed
  in place.
- A config key that a released `vX.Y.Z` tag reads is renamed only with a former-key step: the old key is no longer
  read, and finding it writes one Warning that names the new key and says the old value was not applied, the same rule
  ADR 0046 rule 4 sets for a former switch file. The step goes with the rename, and its removal is tracked by an issue
  that a `KNOWN-WRONG #<issue>` marker at the old key names.
- Two names in one directory MUST NOT be equal once case, `_` and `-` are ignored (`table_step.cpp` beside
  `tablestep.cpp`, `tools/package-release/` beside `tools/package_release.py`): each reads as a typo of the other. Files
  that share a stem and differ in extension (`music.h`, `music.cpp`) are one name.

### File roles

A component's files are its name plus at most one role suffix from this list. A suffix not on the list is a word of
the name and has to make sense as one.

| Suffix | Role | Holds | Never holds |
| --- | --- | --- | --- |
| (none) `<name>.h`, `<name>.cpp` | the platform half | the hooks, the install, the reads of game memory, the log lines | a rule a host test could drive without the game |
| `<name>_rules.h` | the decisions | pure C++: what the component decides from values it is handed. Header only | a read of game memory, a call into the game or the OS, or a log write, other than through a seam the caller passes (an `Env`, an `Ops`); an address |
| `<name>_targets.h` | the pins | the game's addresses, slots and instruction words the component hooks or reads, with the compile-time checks that tie each offset to its word | logic |
| `_entry.inc` | a reviewed assembly entry | the assembly a manager slot enters (ADR 0046 rule 2), included at global scope by the platform half and by its qemu test, so the test runs the text the build ships | C++ |

- `<name>_rules.h` is the only suffix for the decisions, whether one platform uses them or both. A component has at most one
  `<name>_rules.h`. `<name>_core.h` is not a role suffix.
- `hook` is a word, not a role. The platform half that holds a component's hook is the bare name on both platforms
  (`src/runtime/patch/ui_texture_mips.cpp`, `src/quest/sentinel/ui_texture_mips.cpp`). A component whose name is a hook
  mechanism keeps the word (`got_hook`, `dll_load_hook`). `swap`, `layout`, `policy`, `model` and `check` are words too:
  each has to be part of a name that says what the file holds.
- `_entry.inc` is the only suffix for an assembly entry. `_thunk.inc` is not a role suffix (`thunk` is the manager's
  C++ wrapper, `callback_thunk.h`).
- A file MUST NOT carry the platform in its stem after the stem's first word (`ui_text_quest`, `crash_report_win_common`,
  `hwdump_android`; the words `quest`, `win`, `pc`, `android`): the directory names the platform, and includes are
  path-qualified, so `quest/invite/ui_text.h` and `runtime/patch/ui_text.h` are two files of one component. A stem
  whose first word is a platform word (`quest_config`, `android_main`) names a thing of that platform, not one half of
  a component, and is left to review. A suffix that names a binary format or a binary's role (`_pe`, `_elf`, `_host`)
  is a word, not a platform.
- A test is named for the stem it tests. A C++ test is `<stem>_test.cpp`; a freestanding qemu test is
  `<stem>_qemu.cpp`, where `<stem>` is the stem of a file the test runs (its `<stem>.cpp` or `<stem>_entry.inc`); a Python test is `test_<stem>.py` and a Go test `<stem>_test.go`, as their test runners require.

### Placement

- A `<name>_rules.h` shared by PC and Quest lives in `src/runtime/<area>/`, in the directory of the PC half that includes it,
  by the membership test of that directory (`AGENTS.md`, "Architecture"): `splash_skip_rules.h` beside `splash_skip.cpp`
  in `patch/`, `<name>_rules.h` beside the drone code in `combat/`. A rule with no PC half lives with its only
  platform half.
- The Quest half of a shared component lives in the Quest directory of its area and takes the component's name:
  `src/quest/sentinel/ui_texture_mips.cpp` for `src/runtime/patch/ui_texture_mips_rules.h`.
- A PC file ported whole from an upstream file that holds several components (EchoCombat's `dronefix.cpp`) is not split
  by a rename: it takes the name of the player-facing feature it implements, which is also its config key
  (`combat.<feature>`, so `<feature>.cpp`), and each component in it is named, by its component name, in a section comment, so a
  grep for the name finds its PC code. Splitting it is a change of its own.

### Directories

- A directory under `src/`, `sdk/`, `installer/` and `tools/` is lower case, words joined by `_`. Two kinds keep the
  name an outside tool gives them: a vcpkg overlay port (`src/quest/vcpkg-overlay-ports/<port>`, named as the port is)
  and a Go command (`tools/cmd/<command>`, named as the binary is).
- A component's Markdown doc is `docs/<area>/<name>.md` with the component's name exactly, `_` included: one spelling
  for the name everywhere outranks the kebab-case rule for Markdown, which every other document keeps.

### What checks it

The review checks (no automated check reads these):

- **N-COLLIDE.** No two entries of one directory (files by stem, and directories) are equal after lower-casing and
  removing `_` and `-`. Known breaks are listed in `KNOWN_COLLISIONS`, each marked `KNOWN-WRONG #<issue>`; the list
  only shrinks, and an entry that no longer collides fails the test until it is removed.
- **N-ROLES.** Under `src/`: no stem ends in `_core`; no `.inc` file, and no frame check (`.want`) of one, has a stem
  that ends in `_thunk`; no stem has `quest`, `win`, `pc` or `android` as a word after its first; no C++ file is named
  `test_<stem>.cpp`; and every `<stem>_qemu.cpp` names a stem that has a `<stem>.cpp` or a `<stem>_entry.inc` in the
  tree. Known breaks: `KNOWN_ROLE_BREAKS`, same form, one entry per file.
- **N-COMBAT.** Every file in `src/quest/combat/`, `src/quest/combat/mutants.d/`, `src/quest/combat/check/` and
  `docs/quest/combat/` (its `adr/` aside, and the scaffolding names `CMakeLists.txt`, `README.md`, `mutants.sh`) has a stem that is a name in `COMBAT_NAMES` (the components and the area's infrastructure), optionally with a
  role suffix from the table (or, for a mutant file, `_<n>`). A new module is added to `COMBAT_NAMES` in the change
  that creates it, where the review applies the name rule. `COMBAT_NAMES` holds the names that pass, and `KNOWN_COMBAT_BREAKS` holds the stems that break the rule
  today, each marked `KNOWN-WRONG #<issue>`; the rename table of that issue gives each one's proposed name.
- **N-RULES-HOME.** Every `src/runtime/**/<x>_rules.h` that a file under `src/quest/` includes has a non-test file under
  `src/runtime/` that includes it in the same directory, and a non-test file under `src/quest/` whose stem is `<x>`.
  A component whose hook is shared with another component's file on both platforms has no Quest file of its own
  name; it is listed in `RULES_HOME_SHARED_HOOK` with that file and the reason (`lobby_failure_message`: its hook is
  the lobby-full hook of `lobby_full_backoff`). That list is an allowed exception, not a break: its entries carry no
  `KNOWN-WRONG` marker, and an entry whose header no longer needs it fails the test. Known breaks: same form.
- **N-DIRS.** No directory under `src/`, `sdk/`, `installer/` or `tools/` has a `-` in its name, the two kinds above
  aside. Known breaks: `KNOWN_DIRECTORY_BREAKS`, same form.

Every `KNOWN-WRONG #<n>` marker names an open issue. The review checks
what no test reads: that a word is whole, that an abbreviation passes its test, and that one component has one name
across platforms where its files do not include a shared header.

## The plugin and module C ABI prefix: `Nevr`, with `Nvr` deprecated

`sdk/include/nevr/plugin_interface.h` and `src/extension/module_interface.h` publish the C ABI that third-party DLLs
compile against. Its prefix is `Nevr`: `NevrPluginInfo`, `NevrGameContext`, `NevrPluginInit`, `NevrPluginInitEx`,
`NevrModuleContext` and the rest, and a plugin exports `NevrPluginGetInfo`, `NevrPluginGetApiVersion`,
`NevrPluginGetCapabilities`, `NevrPluginInit`, `NevrPluginInitEx`, `NevrPluginOnFrame`, `NevrPluginOnGameStateChange` and
`NevrPluginShutdown`. A new published type, function or export is `Nevr*`; its macros are `NEVR_*`.

`Nvr*` is deprecated, not gone, so that a plugin already built keeps loading:

- The two headers declare each old type name as a `[[deprecated("use Nevr...")]]` alias of the new one, and
  `NvrModuleApiVersionSupported` as a deprecated wrapper. Source written against the old names compiles with a warning.
- Both plugin loaders (`src/runtime/ext/plugin_loader.cpp`, `src/quest/plugin/plugin_loader.cpp`) resolve each entry
  point's `Nevr*` export first and the `Nvr*` export when the plugin has no other, and log one Info line per plugin that
  loaded through a deprecated export (`loaded with deprecated Nvr exports`).
- Nothing else in the tree is spelled `Nvr`. It is allowed in the two alias headers
  and in (the fixtures that prove an old plugin loads, and the loaders'
  messages), and nowhere else.

The aliases and the fallback go when no plugin that ships still exports `Nvr*`: when the Info line above has not been logged
for a plugin the project distributes or knows of. `docs/adr/0043-nevr-is-the-plugin-abi-prefix.md` records the decision and
the alternatives.

## Names in the tree that are not models

- The brand spelling `nEVR` outside prose: the `-n` program name in `cmake/codesign/sign.sh` and the certificate
  subjects under `certs/` (a certificate subject is a trust identity, so changing one is the owner's call).
  The set of such files and tokens only shrinks.
- Exports without the `NEVR_` prefix: `TokenAuth_GetToken`, `token_auth_Init`, `platform_compat_Init`
  and their siblings (declared with `NEVR_MODULE_API`).
- Targets whose name is the name of a file users touch or of a third party, so `nevr_` is not applied:
  `echovr_server` (the launcher), `LibOVRPlatform64_1` (the stub DLL), `ovrplatformloader` (the Quest
  loader library) and `breakpad_client`.
- Flat config keys without the `nevr_` prefix (`asset_cdn_url`, `telemetry_uri`, `upnp`): a key is a name
  users write in their config file.
- Log tags outside `[NEVR.<AREA>]`: `[NETGAME]`, `[NSLOBBY]`, `[RBX]`, `[SAVE_LOADOUT]`, `[SAVE_SUCCESS]`,
  `[CURRENT_LOADOUT]`, `[TELEMETRY.DIAG]` and a bare `[NEVR]`: scripts and the score log grep the text.
- Two PascalCase namespaces: `Hooking` (`src/core/hooking.h`, the hooking abstraction every patch calls; it
  predates the rule) and `EchoVR::Symbols` with its nested `Social` and `Tcp` (`src/abi/symbols.h`, named after
  the game's symbol tables).
- The game's own namespaces that `src/reconstruction` takes from the binary (`NRadEngine::CMemory`, `NRadGame`,
  `NWriteLog`, `CVR`, ...) keep the game's spelling.
  A namespace the reconstruction wrote takes the rule on its last component, after the game's prefix:
  `NRadEngine::nevr_reconstruction`, `NRadEngine::NRadGame::nevr_reconstruction`.
- `NEVRProtobufJSONMessageV1` (`src/abi/symbols.h`): its spelling is fixed by the hashed protocol string.
- `NevrCfg*` functions are global, not in `namespace nevr`; `NevrConfig` and `NevrConfigError` are in it.

The review enforces these rules: `Nvr` only as the deprecated aliases of the plugin and module C ABI,
no non-canonical spelling of the project name in a file (or token) that does not already carry one
(`LEGACY_SPELLINGS`, which only shrinks, and `BRAND_PROSE_SPELLINGS`, the documents whose prose spells the brand
`nEVR`), one spelling of the lifecycle namespace (`nevr::lifecycle`), the brand spelling `nEVR` in the
prose of documents, `NEVR_` project macros, `nevr_` CMake targets (four named exceptions), every namespace
`nevr` or `nevr_<area>` (the exceptions above and the game's namespaces, listed in the test), and the old names of renamed
namespaces (`tools/tests/renamed_namespaces/*.json`). `python3 tools/naming_inventory.py` lists what the tree
still holds that breaks a rule, by category, with `file:line`. The other rules above are checked in review.

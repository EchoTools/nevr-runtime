# ADR 0019: a plugin loader for the Quest build

Status: Proposed.

Not yet observed on a headset: every behaviour below that depends on the Android loader, the headset or the game's call order is a prediction, and the "Predictions" table is what the first headset run must check. Tracking issue: #959.

## Context

The PC runtime has a plugin host. `LoadPlugins` (`src/runtime/ext/plugin_loader.cpp:1100`) loads each DLL the ordered
`plugins:` list of `config.yaml` names, resolves the C ABI in `sdk/include/nevr/plugin_interface.h` (API version 7), and
runs the plugin's init inside the game process. Plugins on PC are gated by the signature mode in ADR 0010.

The Quest build has no plugin host:

- The sentinel is one shared object, `ovrplatformloader` (`src/quest/sentinel/CMakeLists.txt:36-52`), loaded as a
  `DT_NEEDED` dependency of the game's library (`src/quest/sentinel/entry.cpp:149-158`). Nothing in `src/quest`
  references an `NvrPlugin*` export, a plugin path, or a plugin file (see Evidence).
- The config parser already accepts `plugins:` with the same keys as PC (`src/core/nevr_config.cpp:242-285`), but no
  Quest code acts on the list. The Quest login rewrite drops the `nevr_plugins` key (`src/quest/tests/login_rewrite_test.cpp:282`).
- The only in-process hook backend on Quest is the GOT import hook (`src/quest/sentinel/got_hook.h:4-13`). It cannot
  reach a direct call, an inlined call or a vtable call. An inline backend is explicitly deferred until an ADR proves
  it (ADR 0003, "Binary and hook discovery gate").
- Every Quest behaviour change today is compiled into the sentinel, with its own pinned addresses
  (`src/quest/sentinel/pinned_targets.h`). A change that must ship separately, or be built by someone else, has no route.

The question this record answers: how a separately built artifact can load into the Quest process and hook the game
under the same rules the sentinel already follows (build-ID pins, checked writes, one owner per slot, loud failure),
without each plugin carrying its own hook engine. The PC ABI already names that failure mode: `NEVR_PLUGIN_CAP_HOOKS_ENGINE`
exists because a second copy of a hook engine does not share the host's table (`sdk/include/nevr/plugin_interface.h:259-260`).

## Decision

Summary of the recommendations. Each has alternatives and a reason in its section.

1. **Artifact.** A plugin is an AArch64 shared object named `lib<name>.so`, built with the existing `android-arm64`
   preset and packaged into the APK at `lib/arm64-v8a` by the same `zip -0` route the sentinel and the original loader
   already use. Shipped plugins are trusted through the APK signature. Nothing outside the APK is loaded in v1.
2. **Manifest.** Packaged plugins load by default. The repack writes a manifest of the plugins it packages
   (`lib/arm64-v8a/libnevr_plugin_manifest.so`, section 2), and the loader loads each one with no config entry. The
   ordered `plugins:` list in config.yaml overrides a packaged plugin (`enabled`, `required`, `args`, `target`; `early`
   can be set, not cleared) and names the rest. On Quest `file` is a basename inside the native library directory only.
   Manifest order, then config-only entries in config order, is load order.
3. **Install moments.** Two: (a) inside the sentinel constructor, after the reporter starts, for `early` entries only and
   only if the constructor-time `dlopen` prediction holds; (b) at the first call of the post-load hook (the `dlopen` hook
   on the game's own `dlopen` slot), for every other entry. The plan (config read, file checks, one record per entry)
   runs earlier, before the counters.
4. **ABI.** The existing C ABI, unchanged in every signature and struct layout. Quest adds one `flags` bit and
   ctx-tail fields gated by `ctx_size`. No version bump in v1; the PC-only meanings are listed in section 4.
5. **Hook API.** The host, not the plugin, owns the hook machinery. The host offers four operations: (i) GOT import
   hook (exists), (ii) a data-word swap for vtable and RELRO function-pointer slots, (iii) a checked data write (data
   only, never code), and (iv) an inline backend, not in v1. One owner per slot, no chaining in v1, one process-wide lock.
6. **Helpers.** The host offers the three things that need host state: a fault-tolerant read, a game-thread tick
   (multiplexed over the one `CR15NetGame::Update` hook the sentinel already holds), and a module-base resolver. Pure
   helpers live in the header-only plugin SDK (`sdk/`), which the host does not need to run.
7. **Version pinning.** A plugin declares the libr15 build IDs it supports through a new optional export. The loader
   refuses every other build with one line, before init. The host refuses a plugin hook target that carries no build ID.
8. **Content.** Plugins carry no game-derived bytes. Game-derived data is generated on the device from the owner's own
   installed copy. A plugin reaches the owner's data directory and a named generate step through the host; it never
   fetches game data.
9. **Registry.** One process-wide slot registry, the single copy of the GOT backend, seeded with every slot the sentinel
   names in `pinned_targets.h`, installed or not. A plugin claim on a registered slot is refused loudly and names the owner.
10. **Logging.** Plugin lines keep the Rule 15 shape `[NEVR.PLUGIN] plugin=<id> ...` and reach the sentinel logger
    (logcat and `nevr-sentinel.log`). The loader writes exactly one `plugin_load` record per plan entry, including
    refusals and disabled entries.
11. **Verification.** The ADR 0004 regime: pin binaries, host tests with fixture shared objects, and an explicit blocker
    list for every Bionic-only claim. Predictions and falsifiers are written in section 11 before any code.
12. **Plan.** Six slices (0 to 5), each with an exit test (section 12). Slice 5, trust for plugins outside the APK, is
    a separate decision.

---

## 1. The artifact and where it lives

**Recommendation (R1).** Plugins are shared objects in the APK's `lib/arm64-v8a`, named `lib<name>.so`. The repack
recipe (`justfile:174-214`) already copies two `.so` files into that directory with `zip -q -0`, then signs the APK. A
plugin is one more entry in that list, and each plugin object runs the same static-init check as the shim
(`tools/check_quest_static_init.sh --plugin`, run by the repack on every plugin it packages: only the two compiler-rt
entries may run at load time). The repack refuses a plugin whose file name is the name of any `lib/` entry of the store
APK or of a library it stages itself, and checks in the signed APK that every store library (the loader aside, and
`libr15.so`, which must be the patcher's output for the store's, ADR 0021 section 9) and every staged library has its
bytes (`tools/quest_repack_libs.py`).

The loader finds the directory from the sentinel's own mapping, not from JNI. `dladdr` on the constructor
(`nevr_sentinel_ctor`, `src/quest/sentinel/entry.cpp:151`) returns the path the linker mapped the sentinel from; its
directory is the native library directory. If the linker maps the library straight out of the APK, that path has the
`base.apk!/lib/...` form, and the loader must accept it by entry name rather than by directory. Which form a given
device produces is UNVERIFIED (Predictions P1).

**Trade-off in the repack.** The repack step changes from "inject two objects" to "inject the shim, the original and
each declared plugin". The plugin list comes from one source (a build-time list of plugin objects), so a plugin cannot
reach the APK without also going through the static-init check and the signature. The keystore route is unchanged
(`justfile:171-173`).

**Alternatives.**

- **A2: the app's private files directory.** A plugin could be copied there after install and updated without an APK
  reinstall. It needs a trust pin of its own (section 1, trust below). It is also the riskiest load path: Android 10 and
  later restrict loading native code from app-private storage, and the rule's reach for this build's `targetSdk` is
  UNVERIFIED (the design record measured `targetSdkVersion="29"`, `docs/design/2026-07-13-quest-crash-reporter-injection.md:59`).
  Rejected for v1; it is reconsidered only if P3 shows it loads.
- **A3: both.** Shipped plugins in the library directory; a development-only directory in development builds. The
  development gate already exists (`nevr_build::kIsDev`, `src/quest/sentinel/entry.cpp:105-107`). Deferred until after slice 1.

**Trust (R1 trust).** A shipped plugin is trusted because the APK signature covers every entry of the APK, and the repack
signs last. A player cannot add a plugin without re-signing the APK, and a re-signed APK cannot update an installed app
signed with another key (platform behaviour, to be confirmed on a device in P12). A plugin the player puts anywhere
else is not loaded in v1. The repack writes each packaged plugin's sha-256 into the manifest, and the loader refuses a
file whose bytes differ from it (`manifest_hash_mismatch`) before it maps the file. The hash belongs to the file: every
entry that loads a packaged file is checked against it, whichever config entry it is.

**Alternatives for trust.**

- **T2: a hash in `config.yaml`.** Rejected. The config is data a player can write, the same reason ADR 0010 keeps the
  signature mode out of config (`docs/adr/0010-signed-plugins-only.md:224-225`).
- **T3: a signed manifest inside the APK, pinning the SHA-256 of files outside it.** Only meaningful with A2. It still
  does not stop a same-account writer, which is the residual threat ADR 0010 already states (`docs/adr/0010-signed-plugins-only.md:29-32`).
- **T4: a project key, checked inside the sentinel, for plugins outside the APK.** Deferred to slice 5 and its own ADR.
  The crypto is available (OpenSSL is already a Quest dependency, `src/quest/CMakeLists.txt:145-146`). It needs a
  denylist plan of the kind ADR 0010 section 2 has for the PC pipeline.

## 2. Manifest and load order

**Recommendation (R2).** Two sources, one plan. The repack writes a manifest of the plugins it packages,
`lib/arm64-v8a/libnevr_plugin_manifest.so`: JSON with `schema` 1 and one entry per packaged plugin (`name`, `file`, `enabled`,
`early`, and `sha256` of the staged file), keys sorted, no spaces. Its name is `lib*.so`, so the installer extracts it
beside the plugins. The repack writes it only when a plugin is packaged, so a build with none is unchanged. The loader
reads it at launch and loads each packaged plugin with no config entry.

The `plugins:` list in config.yaml is read with the shared parser's keys (`name`, `file`, `enabled`, `required`, `early`,
`target`, `args`; `src/core/nevr_config.cpp:264-281`). Its entries merge with the packaged ones as follows:

- A config entry whose resolved `file` equals a packaged entry's file (ASCII case-insensitive; `<name>.dll` is read as
  `lib<name>.so`) overrides that entry in place. `enabled`, `required`, `args` and `target` come from config. `early` is the
  packaged value or the config value, so config can set `early` and cannot clear it. The packaged `sha256` is kept.
- A config entry that names no packaged file, or a second entry for a file an earlier entry already overrode, is a
  config-only entry. It is appended after the packaged entries, in config order. Config cannot reorder packaged plugins. A
  config-only entry whose file is a packaged file's (the first entry disabled it, or the file is spelled in another case)
  carries that file's manifest hash, so config changes how a packaged plugin runs and never which bytes run; a second
  enabled entry for the file is refused as `duplicate_file`.
- A config file that is absent, or rejected as a whole, leaves every packaged plugin on with its manifest values. The
  loss is said on the line: when the file was rejected, every `plugin_load` record of that launch carries
  `config_rejected=1`, and the file's own Error line names the rejection. The rule is load-and-say-so, not hold: holding
  every packaged plugin until the file is readable would disable all of them for one bad key.

The Quest differences from the PC list are unchanged:

- `file` is a basename only. A path separator, a NUL, an empty value, `.` or `..` in `file` is a refusal with reason
  `file_outside_directory`. This mirrors the PC check `PluginFileStaysInDirectory` (`src/runtime/ext/plugin_loader.cpp`,
  `StageOne`).
- The default `file` is `lib<name>.so`, not `<name>.dll`. The parser's default (`nevr_config.cpp:265`) is a PC name,
  so the Quest plan computes its own default. The shared parser is not changed for this.
- `required` on a client logs and continues. This is already the client rule in the reference config
  (`docs/reference/example-config.yaml:436-438`). A required Quest plugin that fails does not stop the game.

**Load order.** Within each moment: packaged entries in manifest order, then config-only entries in config order. `early`
entries run at moment (a), in that order. The rest run at moment (b). This matches the PC rule that the early pass loads
only `early: true` entries (N146, `AGENTS.md:191`).

**Alternatives.**

- **A separate Quest operator manifest.** Rejected: a second source of truth for the operator's configuration, and the PC
  list is already the documented one (`docs/reference/example-config.yaml:431`). The packaging manifest above is build
  output: the repack writes it from the staged bytes, and no operator edits it, so it does not compete with `plugins:`.
- **Send the plugin list to login (adopted, Q5).** The PC loader reports `nevr_plugins` at login (`src/runtime/compat/login_profile.cpp:46`).
  Quest does the same; the login field is service-visible, so the decision is recorded in section 13 (Q5).

**`early` on Quest.** The key stays so one config file works on every platform. On Quest it means "init at moment (a)".
If P10 fails (a `dlopen` from the constructor does not complete), `early` entries are refused with reason
`early_unsupported` and logged, and nothing else changes.

## 3. Where the loader runs, and the two install moments

The sentinel constructor order is fixed in ADR 0003 ("Order", `docs/adr/0003-quest-networking-port.md:1744-1752`):
(1) crash reporter, (2) configuration, (3) every counter of every hook that will be installed, (4) the single reporter
start, (5) the clock hook, (6) token auth, (7) prompt hooks, (8) bridge, (9) redirect, (10) social facade, (11) the hook
on the game's `dlopen` slot.

**Recommended placement.**

- **Plan, after step (2).** Read the `plugins:` list, resolve each entry to a file in the native library directory,
  record the file's SHA-256 and GNU build ID, check the plugin's machine type is AArch64 and its build-ID support list
  (section 7), and write one `plugin_load` record per entry with outcome `planned`, `refused` or `disabled`. No plugin
  code runs at this step. Its counters are registered at step (3) with the other counters.
- **Early init, after step (4).** For `early` entries only: `dlopen` the file, resolve the exports, call the init. The
  reporter is running, so the plugin's first line reaches the same sinks. Placed after (4) and before (5) so that a
  plugin's hooks, if any, are armed after the reporter exists. Gated by P10.
- **Normal init, at moment (b).** The post-load hook (`src/quest/integration/dlopen_hook.cpp:27`) calls `AfterDlopen`
  (`src/quest/integration/post_load.cpp:97`) after the game's own `dlopen` returns. The loader's normal pass runs after
  `AfterDlopen`'s own work, on the same thread, so no sentinel frame is live while a plugin runs. A normal plugin that
  never gets its init (because the `dlopen` hook was refused, or the game never calls `dlopen` again) is reported as
  `not_started` with the reason, not silently dropped.

**The two moments and what each can touch.**

- **(a) Constructor time.** The sentinel's constructor runs while the game's library is being brought in, before the
  game's own code (`docs/design/2026-07-13-quest-crash-reporter-injection.md:89-94`). Whether the game library's
  relocations are applied at that point, and whether `dlopen` is safe there, is UNVERIFIED (P10). A plugin init at (a)
  may therefore use only host services that touch no game slot. A GOT install at (a) is refused until the slot's
  expected original is known to be the relocated value (`src/quest/sentinel/got_hook.h:15-31`).
- **(b) After the game's first `dlopen`.** The game library is mapped and running. GOT installs and checked writes are
  allowed. The cost is that the plugin's init runs on the thread that called the game's `dlopen`, inside the game's
  load path. P8 measures the time it adds.

**Alternatives.**

- **Run everything at (b).** Simplest; `early` becomes a no-op on Quest. This is the fallback if P10 fails.
- **A dedicated loader thread that waits for a readiness signal.** Keeps plugin init off the game's load path. Rejected
  for v1: no readiness signal exists in the tree that is measured to be safe. Decision Q4 (section 13) accepts moment (b)
  with P8's bound; the deferred thread is a later change if P8 fails.

## 4. The ABI

**Recommendation (R4).** The existing C ABI is the base, with no changes to any existing signature or struct layout
(`sdk/include/nevr/plugin_interface.h:28-106`, `:108-176`). The exports a Quest plugin may provide are the optional set
the PC plugin already uses: `NvrPluginGetInfo` (required), `NvrPluginGetApiVersion`, `NvrPluginGetCapabilities`,
`NvrPluginInitEx` or `NvrPluginInit`, `NvrPluginOnFrame`, `NvrPluginOnGameStateChange`, `NvrPluginShutdown`.
`NvrPluginShutdown` is not relied on: Quest processes end by `_exit` at the end of `android_main` (ADR 0020), and the
header already says shutdown is not guaranteed on process exit (`plugin_interface.h:119-128`).

**Quest additions (all additive).**

- `NEVR_HOST_PLATFORM_QUEST = 0x20` in the `NvrHostFlags` enum. The header says new flags need no bump
  (`plugin_interface.h:183`).
- Tail fields, appended after `log` in `NvrGameContext`, each gated by `ctx_size` the way the v5 and v6 tails are
  (`plugin_interface.h:85-96`, `:190-205`):
  - `const char* game_build_id`: the libr15 build ID in lowercase hex, read from the ELF note (`ReadBuildId`,
    `src/quest/sentinel/got_hook.h:149`).
  - `const char* data_dir`: the owner's data root as `data_root::Resolve` returns it (`docs/adr/0013-quest-game-data-from-the-launcher.md:25-27`).
  - `const NevrQuestHostApi* quest`: a versioned struct of host function pointers (section 5), with its own `struct_size` so
    it can grow.

**Which fields are PC-only, and what Quest carries instead.**

| Field | PC meaning | Quest v1 meaning |
| --- | --- | --- |
| `base_addr` | echovr.exe ImageBase (`plugin_interface.h:79`) | libr15 load base, from the loaded image lookup (`got_hook.h:142`). A Quest plugin reads it only when `NEVR_HOST_PLATFORM_QUEST` is set. |
| `net_game` | `CR15NetGame*` (`:80-81`) | null. Not measured for Quest. |
| `game_state` | `ENetGameState` (`:81`) | 0 until a measured source exists (not established). |
| `flags` `IS_SERVER`, `IS_CLIENT`, `IS_HEADLESS`, `HAS_NETGAME` | server, client, headless, netgame presence (`:100-105`) | `IS_CLIENT` set; the others clear. Not established for the headless bit beyond "this is a client build". |
| `NEVR_RegisterAsset`, `NEVR_DeregisterAssets` (v7) | PC only; the Quest asset path is a follow-up (`docs/reference/plugin-assets.md:93`) | not exported. A plugin that needs assets on Quest is refused the capability and logs it. |

Every pointer in the context and in `NevrQuestHostApi` is process-lifetime, like `plugin_id` (`plugin_interface.h:93-95`).

**Version bump rule.** No bump in v1. All changes are additive tail fields gated by `ctx_size` and one flag, the
pattern the header describes for v5 and v6. Bump to 8 only when an existing field or signature changes meaning or
layout. A PC DLL cannot load on Quest (wrong machine type, section 7), so the PC meanings never reach a Quest plugin.

**Alternative: a separate `NevrQuestGameContext` with its own export and version.** Rejected. Two lifecycles mean two
entry points, two loaders and two sets of tests. The cost of the chosen design is that `base_addr` changes meaning by
platform flag, which section 4's table states.

## 5. The hook API the host gives a plugin

Plugins never carry a hook engine. The host implements every write, under one lock, against one registry (section 9).
A plugin asks for an operation through `NevrQuestHostApi`. A plugin's handler is wrapped by the host's thunk installer
(`InstallThunk`, `src/quest/sentinel/got_hook.h:226-231`), so the handler runs under the sentinel's frame contract.

**(i) GOT import hook (exists).** Operation: `GotInstall(target, handler, &original)`, with the `GotTarget` fields
(`got_hook.h:114-131`): module, symbol, relocation kind, build ID, optional pinned slot, expected original.

- *Preconditions* (as implemented, `got_hook.h:15-31`): the module is loaded and its build ID matches; a JUMP_SLOT only
  in a BIND_NOW module; exactly one matching relocation, or the pinned one; no addend; the slot in a writable segment;
  the slot holds the expected original, or an executable address; no other owner. Added for plugins: a null build ID is
  refused (`pinned` is not optional for a plugin).
- *Failure policy.* The status is returned and logged once (`got_hook.h:41-48`). A failed install leaves the slot and the
  original pointer as they were, with the one documented exception (poisoned reservation).
- *Unhook and lifecycle.* `GotHook::Remove` writes the original back over the plugin's own value, compare-and-swap
  (`got_hook.h:215`, `got_hook.cpp:769`). The handle has no destructor by design (`got_hook.h:201-206`), so the host keeps
  it in the registry for the process lifetime.
- *Thread safety.* One process-wide write lock; Install and Remove are compare-and-swap (`got_hook.h:33-39`).
- *Chaining.* Refused in v1: a second claim on a slot is a refusal (section 9). Chaining is a later ADR.
- *Handler rules.* A plugin handler must be `noexcept` and built without exception support. The loader cannot check the
  plugin's frames. It refuses, at install time, a plugin whose dynamic imports include the C++ personality routine
  (`__gxx_personality_v0`) when that plugin asks for a hook; the exact import name in the NDK's runtime is to be
  measured in slice 2, with a host test for the refusal.

**(ii) Data-word swap (new).** Operation: `SwapWord(module, slot_vaddr, expected_word, new_word, &original)`. For a
pointer-sized word no relocation names: a vtable entry, or a function pointer in `.data.rel.ro`.

- *Preconditions.* Build ID pin. The slot lies in a writable segment, RELRO included (the same protection rule as the GOT
  writer, `got_hook.h:36-38`). Aligned to pointer size. The current value equals `expected_word` under the lock.
  `new_word` is an address in an executable mapping (the existing plausibility rule, `OriginalImplausible`). No other
  owner of the address.
- *Failure policy.* Loud, one line per outcome. If the store landed and the protection restore failed, the slot is
  marked poisoned in the registry, as `kSlotPoisoned` does for the GOT backend (`got_hook.h:43-48`, `:107`).
- *Unhook.* Compare-and-swap back to `original`.
- *Thread safety.* The word is pointer-sized and aligned, so a reader sees the old or the new value, never a mix. The
  handler is published before the store. Readers take no lock; the host's lock serialises writers only.

**(iii) Checked data write (new).** Operation: `CheckedWrite(addr, expected_bytes, new_bytes, len)`.

- *Scope.* Data only. The operation refuses any range in an executable mapping. Code patching is a different operation
  with its own prologue rule (AGENTS.md:984: binary patches require prologue validation), and is not in v1.
- *Preconditions.* Build ID pin; the range inside one writable mapping; `len` at most 64 bytes (a proposal, to be
  tuned in slice 3); the current bytes equal `expected_bytes` under the lock.
- *Sequence.* Compare, change protection (from `/proc/self/maps`, RELRO fallback, as the GOT writer does), write, read back
  and compare, restore protection. A read-back mismatch restores the original bytes and logs.
- *Failure policy.* Loud. If the write landed and the restore failed, poisoned, as in (ii).

**(iv) Inline backend.** Not in v1. The sentinel's own records say an internal hook needs a backend that validates the
exact prologue, relocates PC-relative instructions, handles branch range, builds an original-call trampoline and restores
instruction-cache coherence, and that a blind branch at a cached address is refused (`docs/adr/0003-quest-networking-port.md:1290-1296`).
Any future inline backend is its own ADR with binary evidence.

## 6. Helper services

The host offers what needs host state. A plugin-side library offers what does not.

- **Fault-tolerant read (host).** `SafeRead(addr, len, buf)` returns a status and never faults for an unmapped address.
  Candidate mechanisms: `process_vm_readv` on the process's own pid (returns `EFAULT`), or a range check against
  `/proc/self/maps` before a copy. Neither is in the tree today (no `process_vm_readv` or `dladdr` under `src/quest`,
  Evidence). A plugin may not install its own SIGSEGV handler: the crash reporter owns the fault path, so a second
  handler would be a conflict. Decision Q8 (section 13) fixes the mechanism: the address is validated against the loaded
  image's mapped segments before the read, and no signal handler is installed. P11 tests it in slice 4.
- **Game-thread tick (host).** `TickRegister(callback)`. The host multiplexes it over the `CR15NetGame::Update` hook that
  the login prompt already installs (`src/quest/sentinel/login_prompt_hook.cpp:726-731`). The Update hook runs on the
  game's thread, per the comments at `src/quest/sentinel/login_prompt_hook.cpp:99` and `:456`. The host does not add a
  second hook on that slot. Taking the slot over changes the login prompt code: Decision Q7 (section 13) moves the login prompt hook behind this tick, so the host owns the Update slot and dispatches.
- **Module-base resolver (host).** `ModuleBase(soname)` through the existing loaded-image lookup (`got_hook.h:142`).
- **Plugin-side library (no host state).** A header-only set, in the plugin SDK (`sdk/include/nevr/`) beside the PC headers: bounded
  byte comparison, a fixed-width pointer read on a checked copy, string and key builders. No byte-signature scanner in
  v1. There is none in `src/quest` today (Evidence), and a scanner would be a second way to find an address, which
  the build-ID pin exists to prevent.

**Alternative: the plugin-side library does all of it.** Rejected for tick and base: they need the sentinel's state
(the Update slot, the image list), and a copy in each plugin would be a second owner of the same slot.

## 7. Version pinning

**Recommendation (R7).** A plugin declares the libr15 build IDs it supports as an ELF note in its own file
(`NEVR_DECLARE_SUPPORTED_BUILD_IDS("<id> <id>")`, `sdk/include/nevr/plugin_interface.h`): owner `NEVR`, type 1, the text of
lowercase hex IDs separated by spaces. It is data, not a function, because the loader must know the list before it maps
the object: mapping runs the object's constructors, and a plugin must not run any of its code for a build it does not
support. The loader reads the note from the file bytes it has already read and hashed (`SupportedBuildIdsOf`), inside the
APK signature, and compares it with the running libr15's build ID, read from its ELF note (`ReadBuildId`,
`src/quest/sentinel/got_hook.h:149`). The pinned value is `kLibR15StockBuildId` (`src/quest/sentinel/pinned_targets.h`). When libr15 is patched at build time, the loader compares the plugin's list with the plugin identity, not the live note (`docs/adr/0021-quest-libr15-patched-at-build-time.md`, section 12).
The same file-bytes pass decides everything else that precedes `dlopen`: the file is readable (`file_unreadable`, no
unchecked mapping), is an AArch64 shared object (`machine_type`) and declares the running build. A plugin that is
mapped and then fails (`no_plugin_info`, `init_failed`, an exception) is `dlclose`d.

- A plugin with no such note is refused on Quest with reason `build_ids_undeclared`. This is stricter than PC, where
  an absent capability declaration means UNDECLARED and still loads (`plugin_interface.h:250-263`). Quest is stricter
  because a plugin's addresses are only valid for the build it names.
- A mismatch is refused before init, with one line: `plugin_load outcome=refused reason=build_id_unsupported`, the game
  build ID, and the plugin's list. The game still starts (client rule, R2).
- A plugin whose list names the running build is refused with reason `libr15_not_verified` unless the load saw our patched
  libr15: the file `Attach` verified, whose live note still equals the ID verification covered (ADR 0021 section 12). A
  matching note on the stock file, or on any libr15 nothing verified, is not enough, because the plugin's addresses are
  properties of the patched file's stock identity. The check runs after the build-ID verdict, so the earlier reasons keep
  their meaning, and it applies in the early and the normal pass alike.
- The note is additive and Quest-only (the PC loader does not read it), so it needs no bump (`plugin_interface.h:183`).

**Rules that hold regardless.** No address is used without a pin: a `GotTarget` with a null build ID is refused in the host
API. A plugin's `SwapWord` or `CheckedWrite` target needs the same pin. This is the ADR 0003 identity gate (`docs/adr/0003-quest-networking-port.md:1276-1281`)
and ADR 0004 step 0 (`docs/adr/0004-quest-verification-regime.md:38-48`), applied to plugins.

**Alternative: the list in `config.yaml`.** Rejected. The operator's config would decide which builds a plugin may
touch, and the plugin's author is the party who knows which builds the addresses hold for.

## 8. Content policy

**Recommendation (R8).**

- A plugin carries no game-derived bytes: no game assets, manifests, packages, textures, extracted OBB content, or
  copies of game files. This is a rule about what a shipped plugin contains. The loader cannot enforce it, and review
  and the APK signature are the controls.
- Game-derived data is made on the device from the owner's own installed copy, at first run, the way the data step
  already does (`docs/adr/0013-quest-game-data-from-the-launcher.md:9-12`: a release build never writes, replaces or
  deletes the player's data, and never fetches it in a release).
- A plugin reaches the data through two host calls: `data_dir` (section 4) and `RequestGenerate(name)`. The host runs a
  named generator, which writes only into the owner's data directory and logs. The generators are not designed here.
- A plugin never fetches game data from any host. The loader does not provide a network API. It also cannot stop a
  native plugin from opening its own socket. That is a limit to state, not a control to claim.

## 9. Interaction with the sentinel's hooks: the slot registry

**Recommendation (R9).** One process-wide registry, owned by the single GOT backend. The build links that backend once
and says why (`src/quest/CMakeLists.txt:206-208`: a second copy would carry its own lock and registry). Plugins reach it
only through `NevrQuestHostApi`. The key is the module's build ID plus the link-time slot address.

**Seeded set.** Every slot the sentinel names in `pinned_targets.h`, installed or declared, so a plugin cannot take one
even if the sentinel has not installed it yet. From the file (`src/quest/sentinel/pinned_targets.h:399-510`):

- `clock_gettime` 0x36c33e8; `CJson::TString` 0x36ebe08; `CSysHttp::CreateConnection` 0x36e8028;
  `SetDelimitedErrorMessage` 0x36e9170; `CR15NetGame::Update` 0x36e05f8; `android_main` 0x36eb4c8;
  `CSysInfo::RequestPermissions` 0x36e4320; `CSysInfo::CheckPermission` 0x36c37c8;
  `CR15UIPage2EnablePageNode::Enter` 0x36c1cf8; `AStorageManager_mountObb` 0x36c39d0;
  `AStorageManager_getMountedObbPath` 0x36f5b48; `CSysInfo::GetDataRootDir` 0x36f1df8;
  `CSysModule::FindMethod` 0x36db9f8; `vrapi_GetDeviceExtensionsVulkan` 0x36dd440;
  `vrapi_GetSystemPropertyInt` 0x36fa728; `CResourceManager::FillResourceFileRequest` 0x36efa80;
  `SNSConfigRequestv2::Send` 0x37015e0; the two GLOB_DAT login slots 0x3715620 and 0x372b698; and the game's `dlopen`
  slot 0x36c6380 (ADR 0003, "Hook activation").
- The matchmaking library's `CJson::TString` slot (`pinned_targets.h`, `MatchmakingTString`) is in the same registry under
  its own module and build ID.

**Rules.**

- A plugin claim on a registered slot is refused, loudly, with `slot_owned_by=<owner>` where the owner is the sentinel's
  hook name or `plugin:<id>`.
- Two plugins claiming one slot: the second is refused with `slot_owned_by=plugin:<first id>`. The first is unaffected.
- A poisoned slot stays reserved for the process lifetime (`got_hook.h:164-176`).
- The registry holds at most `kRegistryCapacity` entries (`kRegistryFull`, `got_hook.h`); the last `kCriticalReserve` of
  them are for the login and the matchmaking redirect only, and `hook_budget.h` keeps the sum of every declared handle plus
  headroom inside it. A plugin claim that would exceed what its priority may use is refused with `registry_full`. The host
  logs the count, and names the hook that lost, at the moment of refusal.

## 10. Logging

Shaped by AGENTS.md (logging is part of the work) and `docs/standards/logging.md` Rule 15 (`:525-555`).

- **Plugin lines.** Through `ctx->log` (`sdk/include/nevr/plugin_interface.h:69-70`) into the sentinel logger. Format
  `[NEVR.PLUGIN] plugin=<id> <message> <key>=<value> ...` (Rule 15, `logging.md:534-536`), so one filter works on every
  platform. The `plugin=` value is the host's name for the plugin (`ctx->plugin_id`), not text the plugin chooses.
  Lines reach logcat (tag `NEVR-Sentinel`) and `nevr-sentinel.log` (`docs/adr/0003-quest-networking-port.md:1801-1806`).
- **Loader record.** Exactly one `plugin_load` record per plan entry, whatever its outcome. Fields: `name`, `file`,
  `sha256` of the bytes the loader read, `plugin_build_id`, `game_build_id`, `phase` (`early` or `normal`), `outcome`
  (`planned`, `loaded`, `refused`, `disabled`, `not_started`, `failed`), and `reason` as a stable token. A plan entry
  with no record is a defect (P9). Two fields carry the packaging: `source` (`packaged`, from the manifest, or `config`)
  and `overridden` (`1` when a config entry named a packaged file). A separate field, `config_rejected` (`1` on every record of
  a launch whose config.yaml was rejected as a whole, section 2), says the operator's file did not load. `expected_sha256` is the manifest's hash for the file
  (empty when the file is not packaged), beside `sha256`. The reason `manifest_hash_mismatch` (outcome `refused`) is a
  packaged file whose bytes are not the manifest's hash.
- **Manifest record.** One `plugin_manifest` line per launch: `status` (`read`, `absent`, `unreadable` or `invalid`),
  `packaged` (the count), `file`, and for `unreadable` and `invalid` a `reason` token and a `msg` that says no packaged
  plugin loads from this APK and that `config.yaml` entries still load as `source=config`. `absent` (Info) is a manifest
  file that is not there, the normal case for a build with no packaged plugin. `unreadable` (Warning) is a file that is there
  and cannot be used: `no_library_dir`, `symlink`, `not_regular_file`, `too_large`, `open_failed` or `read_failed`. `invalid`
  (Error) is a file that was read and is not a manifest this loader understands: `not_json`, `bad_schema`, `bad_entry`,
  `bad_file`, `bad_sha256`, `duplicate_file`, `too_large`, `too_many`. A manifest the build wrote that the sentinel cannot
  use is a defect the operator must see, and it plans no packaged plugin.
- **Secrets.** `args` values are interpolated config values and may hold secrets. The loader logs the argument keys
  only, never the values.
- **Stderr.** A plugin does not write to stderr (`logging.md:527-530`). The Quest sentinel does not collect it.
- **Durability.** `nevr-sentinel.log` lives in the app's files directory and survives a process restart. The sentinel
  rotates it: once it reaches `kMaxDiskLogBytes` (1 MiB) it is renamed to a name not yet taken and never deleted
  (`src/quest/sentinel/sentinel_log.cpp:119`, `:290`). Keep-by-default applies: nothing in this ADR shortens it.
- **Loud failure.** If the file sink cannot write a record, the failure must be visible. The logcat path is a separate
  sink. The file sink reports its own write failure once, to logcat (`src/quest/sentinel/sentinel_log.cpp:286`), and the
  failure must be detectable (`docs/standards/logging.md:250`); Decision Q6 (section 13) records both.

## 11. Verification and predictions

**Regime (ADR 0004).**

- *Step 0, pin.* Record each plugin's SHA-256, ELF build ID, machine type, SONAME, `DT_NEEDED`, undefined symbols and
  relocations (`readelf`), and the libr15 build ID the fixture was taken from. Fixtures go under
  `tests/quest/fixtures/<build-id>/` (`docs/adr/0004-quest-verification-regime.md:38-48`).
- *Host tests.* The loader's plan, refusal, registry and checked-write logic run on the host against fixture shared
  objects and in-memory images, as `src/quest/tests/got_hook_test.cpp` does for the GOT backend. Tests that need `dlopen`
  of a fixture object run on the host too.
- *Bionic-only claims.* `dladdr` path form, constructor-time `dlopen`, the `SafeRead` mechanism, the install moments and
  the timing P8 measures are Bionic or device facts. Each is reported as a blocker until a run on a headset shows it
  (ADR 0004 `:139-142`). Cross-compiling proves none of them.
- *Mutants.* For each rule in sections 5, 7 and 9, a mutant that drops the rule must make a named test fail: drop the
  expected-word check, drop the read-back, drop the protection restore, drop the build-ID check, drop the registry check.

**Predictions.** Written before any code. Each row says what would change the plan.

| ID | Prediction (result named before the check) | Falsifier | Tested in |
| --- | --- | --- | --- |
| P1 | The sentinel's constructor-time `dladdr` names a path whose directory holds the shipped `lib<name>.so`, after install from the repacked APK. | The path names a file outside the native library directory, or the plugin object is absent from it. Then the directory rule (R1) is rewritten. | S1 |
| P2 | `dlopen` of the shipped plugin object at moment (b) returns a handle, and its `NvrPluginGetInfo` returns the name in the manifest. | `dlopen` fails, or the name differs. | S1 |
| P3 | `dlopen` of a plugin object from the app's private files directory fails with a permission error on this build. | It succeeds. Then A2 is reconsidered (section 1). | S1 (probe only) |
| P4 | A plugin whose build-ID list omits the running build is refused before init, with one `plugin_load` line naming `build_id_unsupported`, and the game reaches the main menu. | The plugin's init runs; or the game does not reach the main menu. | S1 |
| P5 | Two plugins claiming one slot: the second install returns a non-OK status and logs `slot_owned_by=plugin:<first>`; the first's hook stays in place. | Both installs report OK, or the second overwrites the first. | S2 |
| P6 | A plugin claim on `android_main`'s slot is refused with `slot_owned_by=<sentinel hook>`. | The claim installs. | S2 |
| P7 | A `SwapWord` on a RELRO word of the pinned build: the read-back equals the new word, and the protection after the call is read-only. | Read-back differs, or the page stays writable. | S3 (headset) |
| P8 | A no-op normal plugin adds less than 50 ms to the game's first `dlopen` call. | The added time is 50 ms or more. Then the deferred path of Q4 is the next change. | S1 |
| P9 | Every plan entry (loaded, refused, disabled, not started) has exactly one `plugin_load` record. | A plan entry with no record, or two. | S1 |
| P10 | A `dlopen` from the sentinel constructor completes, and the early plugin's init runs before the game's first `RAD:` log line. | `dlopen` hangs for more than 2 s, returns an error, or the init runs after the first game line. Then `early` is refused on Quest (section 2). | S1 |
| P11 | `SafeRead` of an unmapped address returns an error status and the process stays alive. | The process dies, or the read returns data for an unmapped address. | S4 |
| P12 | After repack, the plugin object is listed in `lib/arm64-v8a` of the signed APK and the APK signature verifies. An APK re-signed with another key is refused as an update of the installed app. | The object is missing from the signed APK, verification fails, or the re-signed APK installs over the existing app. | S1 |
| P13 | A PC-built DLL in the `plugins:` list is refused on Quest with `machine_type` (not AArch64 ELF), and the game still starts. | The DLL loads or the game does not start. | S1 |
| P14 | On a headset, a repacked APK with a packaged plugin installs `libnevr_plugin_manifest.so` beside the plugins, `plugin_manifest status=read packaged=1` is logged, and the plugin loads with no `plugins:` entry in config.yaml. | `plugin_manifest status=absent` on that APK. Then the manifest is not extracted beside the plugins, and the loader needs another way to find the packaged names. | S1 |

**Sign-off order for a headset run.** P1, P12, P13 and P14 first (packaging and refusal, cheapest to read), then P10 and P2
(load paths), then P8 and P9, then P4 and P5 (the refusal and registry logic on the device), then P3, P7 and P11.

## 12. Staged plan

Each slice lands in one step (`AGENTS.md`, Landing) and carries its own exit test. Nothing in a later slice starts before
the earlier slice's exit test passes on the device where the test is a device test.

- **Slice 0: this ADR and the tracking issue.** Exit: ADR accepted by the maintainers; the tracking issue lists slices 1 to 5
  with their predictions; a reviewer who did not draft it has read it (`AGENTS.md:812`).
- **Slice 1: the loader reads `plugins:`, loads a no-op plugin that logs.** Covers R1, R2, R3 (plan, moments (a) and (b)),
  R7 (build-ID export), and the loader record. Exit: host tests for the plan, refusals and records; a device run whose
  logs show one `plugin_load outcome=loaded` line and one `[NEVR.PLUGIN] plugin=<id>` line from the no-op plugin; P1, P2,
  P4, P8, P9, P10, P12, P13 decided.
- **Slice 2: GOT hooks for plugins (i).** Covers the registry (R9) and the build-ID rule on targets. Exit: a no-op plugin
  hooks one libr15 import that the registry does not hold (chosen from `readelf -rW` in this slice), the hook's counter moves
  on the device, and `Remove` restores the original; P5 and P6 pass on the host and P5 on the device.
- **Slice 3: data-word swap (ii) and checked write (iii).** Exit: host tests with fake memory, each mutant from section 11
  failing; a device run that performs one `SwapWord` on a RELRO word of the pinned build and reads it back (P7).
- **Slice 4: helpers.** `SafeRead`, `TickRegister` (multiplexed over the existing Update slot), `ModuleBase`, `data_dir`
  and `RequestGenerate` (the interface only). Exit: P11 on the device; a tick count that moves once per frame on the
  device, stated with the measured count.
- **Slice 5: plugins outside the APK (trust T4).** Starts only after a separate ADR picks the trust model and its denylist.
  Exit: a plugin signed outside the project is refused with one `plugin_load` line naming the reason.

## 13. Non-goals and decisions

**Non-goals (v1).** PC plugins on Quest, which are refused by machine type. Third-party or community plugins ahead of the
first-party slices (Q2). Inline hooks (iv). Plugin-carried assets on Quest (`docs/reference/plugin-assets.md:93` marks it a
follow-up). Chaining of hooks on one slot. A plugin-side hook engine. Byte-signature scanning.
Any change to the PC loader or the ADR 0010 gate. The design of any game-data generator.

**Decisions (recorded 2026-10-26).** Each question keeps its text; its decision follows it.

- **Q1.** Native library directory (recommended) or the files directory as well? Default: the library directory.
  **Decision:** the native library directory only. The files directory (A2) is not used in v1, and the development
  directory (A3) is not part of v1.
- **Q2.** Are third-party Quest plugins a goal? If yes, slice 5 comes before slice 2.
  **Decision:** yes. Third-party Quest plugins are a goal, but after the first-party slices; slice 5 stays last.
- **Q3.** May a plugin's init run at constructor time at all? Decided by P10 on a device, not by this record.
  **Decision:** decided on a device by P10. If P10 fails, `early` is a logged no-op: the entry is refused with
  `early_unsupported` (section 2).
- **Q4.** Plugin init at moment (b) runs on the game's load path. Accept it with P8's bound, or require a deferred thread
  and a readiness signal?
  **Decision:** accept moment (b) with P8's bound for v1. A deferred thread is a later change if P8 fails.
- **Q5.** Does Quest report plugins to login (`nevr_plugins`)? That is a service-visible change.
  **Decision:** yes. Quest reports plugins to login the same way PC does, so neither platform is left behind.
- **Q6.** Does `nevr-sentinel.log` rotate or truncate? Does its file sink report its own write failures?
  **Decision:** the sentinel log rotates and reports its own write failures. Both hold in the tree today
  (`src/quest/sentinel/sentinel_log.cpp:119`, `:286`), so no issue is needed. The failure rule it follows is
  `docs/standards/logging.md:250`.
- **Q7.** The `CR15NetGame::Update` slot is held by the login prompt hook. The host's tick takes it over: does the login prompt
  hook move behind the host, or do both share the handler?
  **Decision:** the login prompt hook moves behind the host's tick, so the host owns the Update slot and dispatches.
- **Q8.** `SafeRead` mechanism, given that the crash reporter owns the fault path.
  **Decision:** `SafeRead` validates the address against the loaded image's mapped segments before reading, and never
  installs a signal handler, because the crash reporter owns the fault path.
- **Q9.** The bump policy in section 4: is "additive tail plus flag, no bump" the rule the maintainers want for Quest?
  **Decision:** yes. An additive tail plus a flag with no bump, the same rule as PC (`sdk/include/nevr/plugin_interface.h:183`).

## Consequences

- The repack gains a per-plugin entry and a per-plugin static-init check. The build gains one preset target per plugin.
- A plugin can ship without a new sentinel release, and it is trusted through the same APK signature as the sentinel.
- The game's hook surface on Quest is the registry plus the host API. A plugin cannot add a hook the registry does not
  see, and cannot hook a slot the sentinel names.
- The loader adds a second kind of failure, `not_started` for a normal plugin whose moment never comes. It is logged, but
  an operator must read the log to see it.
- Constructor-time `dlopen` (`early`) may be impossible. If P10 fails, `early` is a no-op on Quest with a logged refusal.
- Plugin init at moment (b) runs on the game's load path. P8 bounds the cost; a slow plugin is a visible cost on the game.
- A native plugin can still reach the network or the filesystem. The content and trust rules are policy and review, not a sandbox.

## Not yet observed on a headset

Everything in sections 1, 3 and 5 that touches the Android loader, the constructor-time call order, the install moments or
the RELRO writes is not observed on a Quest. The predictions in section 11 are what the first device runs must decide. The
host-side plan, the refusal and the registry logic are testable without a headset (section 11, "Host tests").

## Evidence

Paths are repo-relative to the nevr-runtime tree. Line numbers were checked against this record's base commit. Where a
line moves, the symbol named beside it is the anchor.

- Plugin host on PC and its ABI: `src/runtime/ext/plugin_loader.cpp:79` (`HostPluginLog`), `:123` (`BuildPluginManifestJson`), `:1100` (`LoadPlugins`);
  `sdk/include/nevr/plugin_interface.h:17-25` (export macro), `:28-34` (`NvrPluginInfo`), `:49-70` (log levels, `NevrLogFn`),
  `:78-97` (`NvrGameContext`, v5 and v6 tails), `:99-106` (host flags), `:108-128` (lifecycle, shutdown not guaranteed),
  `:141-176` (exports), `:178-220` (versioning, v5 to v7 notes), `:183` (additive exports need no bump), `:190-196` (no log before init),
  `:236-241` (capabilities are a declaration), `:249-264` (capability flags), `:259-260` (`HOOKS_ENGINE`), `:266-299` (assets).
- Plugin config: `src/core/nevr_config.cpp:195-201` (allowed top-level keys), `:242-285` (`ParsePlugins`), `:246` (`kBadPlugins`),
  `:264-265` (`file` default), `:273-274` (`early`); `docs/reference/example-config.yaml:412-444` (plugins block), `:431`
  (list is the only source), `:436-438` (client `required` rule), `:443-444` (`early`).
- Quest sentinel: `src/quest/sentinel/CMakeLists.txt:36-52` (`ovrplatformloader` target and output name), `:93-97` (`patchelf` DT_NEEDED);
  `src/quest/sentinel/entry.cpp:105-107` (development-build gates in `InstallPermissionWaitHook`, `nevr_build::kIsDev`),
  `src/quest/sentinel/entry.cpp:149-158` (constructor, calls `RunSentinelConstructor`), `:162-165` (`JNI_OnLoad`).
- GOT backend: `src/quest/sentinel/got_hook.h:4-13` (scope, no vtable or inline), `:15-31` (preconditions), `:33-39` (lock and CAS),
  `:36-38` (protection source, RELRO fallback), `:41-48` (one log line, failed install, poisoned exception), `:60` (`RelocKind`),
  `:106` (`kRegistryFull`), `:107` (`kSlotPoisoned`), `:114-131` (`GotTarget`), `:142` (`FindLoadedImage`), `:149` (`ReadBuildId`),
  `:164-176` (`ReleasePoisonedSlotsIn`), `:201-206` (no destructor), `:215` (`Remove`), `:226-234` (private install, thunk friend);
  `src/quest/sentinel/got_hook.cpp:61` (`ReserveSlot`), `:268` (`WriteSlot`), `:457` (`ResolveSlot`), `:705` (`GotHook::Install`), `:769` (`GotHook::Remove`).
- Pinned targets: `src/quest/sentinel/pinned_targets.h:29` (libr15 build ID), `:399-423` (clock, TString, CreateConnection, SetDelimitedErrorMessage, NetGame::Update),
  `:428-435` (`android_main`, `RequestPermissions`), `:440-446` (CheckPermission, UIPage2 Enter), `:451-461` (mountObb, getMountedObbPath, GetDataRootDir),
  `:466-485` (FindMethod, vrapi slots, FillResourceFileRequest), `:490` (matchmaking TString), `:496-510` (config request send, GLOB_DAT slots).
- Callers and thread: `src/quest/sentinel/login_prompt_hook.cpp:99`, `:456` (Update runs on the game thread, per comments); `:512` (`HookedNetGameUpdate`),
  `:726-731` (install on the Update slot).
- Post-load and sentinel order: `src/quest/integration/dlopen_hook.cpp:27` (`AfterDlopen` call), `src/quest/integration/post_load.cpp:97` (`AfterDlopen`, outside-game-call annotation).
- Quest build: `src/quest/CMakeLists.txt:11-16` (ANDROID-only), `:145-146` (OpenSSL and CURL), `:206-208` (single GOT backend, the comment on a second copy).
- Repack: `justfile:124-131` (static-init check on the shim), `:171-173` (keystore, not printed), `:174-214` (`android-repack-apk`), `:193` (extract the store library),
  `:199` (shim copied in), `:210-211` (`zip -0` of the two objects).
- ADRs: `docs/adr/0003-quest-networking-port.md:120` (DT_NEEDED, no `JNI_OnLoad`), `:989-1003` (`dlopen` hook), `:1276-1281` (identity gate),
  `:1290-1296` (GOT only, inline needs a backend), `:1744-1752` (constructor order), `:1778-1783` (post-load installs), `:1792-1799` (outside-game-call sensor),
  `:1801-1806` (stage logs), `:1808-1811` (not run on a headset), `:1817-1821` (activation gates);
  `docs/adr/0004-quest-verification-regime.md:38-48` (step 0), `:139-142` (Bionic blocker);
  `docs/adr/0006-quest-hardware-dump.md:26` (18 GOT hooks);
  `docs/adr/0020-quest-process-end.md:9` (`android_main` slot, process ends on return);
  `docs/adr/0009-quest-permission-wait.md:3-4` (status), `:17` (slot);
  `docs/adr/0010-signed-plugins-only.md:8-11` (run mode), `:29-32` (threat model), `:41-58` (PC checks), `:224-225` (no config key sets the mode);
  `docs/adr/0013-quest-game-data-from-the-launcher.md:9-12` (requirement, player data), `:25-27` (data root);
  `docs/design/2026-07-13-quest-crash-reporter-injection.md:59` (`targetSdkVersion="29"`), `:89-99` (load chain, on-device confirmation item).
- Standards and reference: `docs/standards/logging.md:525-555` (Rule 15), `:527-530` (stderr), `:534-536` (identity), `:547-552` (when);
  `docs/reference/plugin-assets.md:93` (Quest asset follow-up).
- AGENTS.md: `:191` (N146, early pass), `:228` (N135, plugin logging), `:248` (N84, HookGuard and re-verify), `:466-468` (plugin logging via host),
  `:491-493` (ABI header is a breaking change), `:812` (review by a non-author), `:885-905` (documentation and public prose), `:984` (binary patches require prologue validation).
- Absence checks (run over `src/quest`, `src/core`, `src/extension`): no match for `FindPattern|ScanPattern|SigScan|FindSignature|PatternScan|ScanForBytes|BytePattern`,
  `process_vm_readv`, `dladdr`, or `nativeLibraryDir`. The only `dl_iterate_phdr` uses are `src/quest/sentinel/got_hook.cpp:143` and `:407`, and
  `src/quest/social/social_install.h:50`. `src/quest/tests/login_rewrite_test.cpp:282` drops `nevr_plugins` from the Quest login.

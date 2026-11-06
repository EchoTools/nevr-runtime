---
# atwd read contract
read: whole
read_via: cat docs/adr/0046-combat-is-part-of-the-quest-runtime.md
audience: all
---

# ADR 0046: combat is part of the Quest runtime, and the q15t plugin is folded into it

Status: proposed. Applies to the Quest build. Builds on ADR 0019 (the plugin loader), ADR 0021 (libr15 patched at build time), ADR 0034
(one hook manager per process), ADR 0035 (every shipped file and reconstruction traces to its source), ADR 0038 (the hot-path rule), ADR 0039 (one owner
per game behaviour) and ADR 0040 (every build names its variant). Tracking issue: #2168.

## Context

The combat behaviour that the q15t and combat APKs add is a plugin, q15t: `libq15t.so` and `libq15t_early.so`, built from
`src/quest/plugins/q15t/` by this tree's CMake and packaged beside the sentinel by the variant recipes (`release-quest-variant-apk`,
`release-quest-combat-apk` in `justfile`). The plugin boundary kept combat code apart from the runtime; the code is in this public tree
and is built by its CMake, so the boundary separates nothing. What it costs:

- **A second hook mechanism.** q15t writes GOT and vtable words itself (`src/quest/plugins/q15t/src/got_swap.cpp`: its own lock,
  `mprotect` and compare-and-swap), outside the manager's registry; ADR 0034 names it (H1, H2). Where the sentinel holds a slot first,
  q15t's install refuses by code: #2000, the refresh-rate and clock-level hooks (not yet seen in a headset log).
- **Two owners of one behaviour.** q15t's clock module and the sentinel's `clock_levels` both decide the clock levels (ADR 0039 O1).
- **Switches outside the config.** 35 files under `files/q15t` turn modules on or off (`module_switch.h` and module-local copies); 41
  build options set the defaults. N133 says every config key is read from `config.yaml`.
- **A second library for timing.** heaps and memsys must act before `android_main` reads the heap sizes, so they are a second plugin in
  the loader's early phase, which depends on a constructor-time `dlopen` never observed on a headset (ADR 0019 P10).
- **Its own log path, packaging and harness.** Lines through `ctx->log`, dropped without a context (`plugin_log.cpp`); variant APKs
  whose plugin libraries a recipe takes from outside the tree (`release-quest-variant-apk <tag> <plugins>`, the release timer's
  `NEVR_VARIANT_DIR`); a test project of 128 files beside the `test-quest-*` groups.

`src/quest/combat/` already holds combat behaviour built into the sentinel and hooked through the manager (`match_tags.cpp`,
`equip_refire.cpp`), which install on every launch (`src/quest/sentinel/entry.cpp`, `InstallPermissionWaitHook`).

## Decision

This section decides names that are not in the tree today: `combat::Active()`, `combat_config`, `combat.force`, the `combat:`
section of `config.yaml`, the `combat` host-test group, `test-quest-combat`, and the sentinel's export of `re33_ec_page`. The units of
#2168 create them.

1. **Combat is runtime code.** Each combat behaviour is a module in `src/quest/combat/` (target `nevr_quest_combat`, namespace
   `nevr_quest::combat::<module>`, log events `combat_<module>_*`), linked into the sentinel and tested in a `combat` host-test group.
   The q15t plugin is removed when its last module has moved. The plugin loader, its manifest, the hook service and the script-redirect
   registry stay: they serve third-party plugins (ADR 0019). The hook service stays in the development API only, as today; the release
   API stays without it, because the plugin that would have used it (#2002) is folded into the runtime instead.
2. **Every combat hook goes through the manager.** A named JUMP_SLOT or GLOB_DAT is a pinned target installed with `InstallThunk`. The
   manager gains three operations, each under its lock and registry, with the expected original and one log line: a pinned ABS64
   target (a vtable or factory word that names a symbol); a checked write of a data slot (a GLOB_DAT that holds a data pointer,
   ADR 0019 section 5 (iii), kept in the manager's file by N151); and an install path for a reviewed assembly entry that needs its
   caller's registers, declared by a record the frame sensor walks, which stays the newest handler on its slot.
3. **One owner per behaviour.** The clock levels belong to the sentinel's `clock_levels`; q15t's clock module is deleted and its
   `args: {clock: raise}` is `diagnostics.clock_levels_override: true`. The refresh rate belongs to the combat refresh-rate rule, a
   decision the render-configuration handler asks, as it asks `clock_levels`; render_config logs the value applied. The handler runs in every APK, so
   the rule answers with the game's own rate unless `combat::Active()` (rule 5). `combat::Active()` is per APK and arena runs on the same APK as
   combat, so the decision also depends on the loaded level (`combat_level.h`): only a combat map (and, with `combat.refresh_rate_lobby`, the
   combat lobby) asks for the level rate, and an Echo Arena level, and every lobby arena players use, always keeps the game's own rate. Headset
   facts come from the identity step (ADR 0031 decision 4), not from a module's own reads.
4. **Switches are `config.yaml` keys** under a `combat:` section, one per module and one per option, each defaulting to the shipped
   behaviour, parsed by `quest_config`, logged once with value and source (`combat_config`). A module's switch file stops being read in
   the change that moves the module, and a former switch file found is a Warning. Files that are player state (the new-profile gun-hand
   markers under `files/q15t`) stay where they are.
5. **The folded modules run in the q15t and combat variants only.** The packaging step writes the APK's variant word (ADR 0040 rule 1);
   the sentinel reads it before its first line, writes it in that line beside the build kind (`nevr_build::kIsDev`, unchanged), and
   `combat::Active()` is true for `q15t` and `combat`. Modules folded from q15t install only when it is true; a development build may
   force it with `combat.force`, a release build may not. Combat-area code that ships to every APK (`match_tags`, `equip_refire`) is outside this gate
   and says so in its header. The combat code is compiled into every sentinel, so the release loader asset serves every variant.
6. **Install moments are the plugin's.** Modules that must precede `android_main` install in a constructor step after Attach and before
   the clock hook; the rest install in a post-load action of their own, right after the normal plugin pass when there is one, at the
   first post-load call.
7. **A variant's plugin libraries are built from its tag.** Until the plugin is gone, a variant or combat APK carries the q15t libraries
   its own tag builds, so a module is in exactly one place in any APK built from a tag. During the move a plugin module and a runtime
   module never share a slot, except the game tick, where the runtime's handler joins the plugin's wrapper through the manager's
   first-handler rule until the plugin's last ticked module has moved (marked `KNOWN-WRONG #2168` at the site). That
   rule holds only when the plugin installs first, so the combat area's post-load step runs after the plugin pass has finished, on the
   thread that ran it.
8. **A move keeps a hook's form and needs no ADR 0038 measurement.** ADR 0038's scope is "hooks and patches added from this ADR on, in
   `src/runtime` and `src/quest`"; q15t's hooks were in `src/quest` before it. A move within `src/quest` that keeps the hook a hook adds
   none. A module that changes a hook into a hot-path form, or a later hook, follows ADR 0038.
9. **Origin and credit travel with the code.** A moved module's header names its origin: the reference build's source file and line at
   its commit (`afk_*` files, as #1737 cites them), and EchoCombat's upstream file and line at its commit where the module ports its
   rules (VeilNet, PowerBoost, DroneFix). The reconstruction index of ADR 0035 gets its row, and the commit credits the people whose work
   it draws on (AGENTS.md, "Commits, versions and requirement words").

## Finding a violation

Scope: `src/quest`, `justfile`, `tools/`, `config/`.

- **C1.** A combat module that writes a code pointer or a data pointer of a game library other than through the manager (ADR 0034 H1,
  H2); after the fold, a plugin under `src/quest/plugins/` that hooks libr15 other than through the host's hook service.
- **C2.** A switch of a folded module read from anything but `config.yaml`: a file-existence check under the app's files directory that
  turns behaviour on or off, other than the player-state files of rule 4.
- **C3.** A module folded from q15t that installs, or a folded decision that a sentinel hook asks and that answers anything but the
  game's own value, in a run whose variant is not `q15t` or `combat`, except a development build with `combat.force`.
- **C4.** A combat module and another module that both change the same behaviour (ADR 0039 O1).
- **C5.** A moved module whose header cites no origin, or whose commit lacks the credit of rule 9.
- **C6.** A variant or combat recipe that packages q15t libraries it did not build from the tag.

## Consequences

- #2000 is fixed when the refresh-rate rule and the clock module leave the plugin; ADR 0034's q15t consequence is met; `got_swap.cpp`
  leaves N151's allowlist; #2002 is superseded.
- heaps and memsys stop depending on ADR 0019 P10.
- The release timer and the variant recipes stop reading libraries from outside the tree (part of #2009, ADR 0035 P4).
- `test-quest-q15t` is replaced by `test-quest-combat`.
- The sentinel will export `re33_ec_page` for the EC script libraries, a third dynamic export, defined outside the archive that
  `--exclude-libs` hides; the export allowlist test (`TestExportAllowlist`) gains it in the same change.
- `config.yaml` gains a top-level `combat` key; an older runtime rejects a file that has it.

## Alternatives

- **Keep the plugin and move its hooks onto the hook service (#2002).** Fixes H1 and H2 and keeps the two libraries, the early phase,
  the packaging and the file switches. The service would need a longer symbol limit (28 of the hooked symbols exceed its 64 bytes),
  release-build availability, eight modules' rollbacks rewritten onto `unregister_slot_hook`, process-lifetime originals, and an exception
  to its handler contract (no lock, no log, no call into the game). Rejected: it pays for a boundary that separates nothing.
- **Use the hook service only as a bridge during the fold.** Same costs, then deleted. Rejected for the same reason; the one shared slot
  (the game tick) is handled by the manager's first-handler rule instead.
- **Move the whole plugin in one change.** About 15,000 lines of code and 128 test files in one PR, with `got_swap` inside the runtime
  until a second pass. Rejected: not reviewable, and it puts H1 into the sentinel.
- **A second loader build with combat compiled in, for the combat APK.** Rejected: one attested loader asset per tag serves every
  variant (ADR 0040 rule 1).
- **Read the switches through a new host-API member (`config_get`) so the plugin's switch files retire before its modules move.**
  Rejected: the fold does not need it (each switch retires with its module, once), a plugin already has a configuration channel (its
  `config.yaml` entry's `args`, handed to `NevrPluginInitEx`), and a new member of the published SDK struct is a public ABI addition that
  nothing asks for.

## Not claimed

- The cost of the thunk's extra call level on the moved per-frame hooks; rule 8 says no measurement is required for a move, and none is
  claimed.
- That the constructor step runs before `android_main` reads the heap sizes on a headset; it follows from the load order (the sentinel's
  constructor runs while the linker resolves libr15's `DT_NEEDED` closure, before the activity's entry) and is the first headset run's
  question.
- That the EC script libraries reach `re33_ec_page` in the sentinel by `dlsym(RTLD_DEFAULT)` on bionic.

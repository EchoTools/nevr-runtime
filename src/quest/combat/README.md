# src/quest/combat

Combat behaviour that is part of the Quest runtime (docs/adr/0046-combat-is-part-of-the-quest-runtime.md). The code is compiled into every
sentinel, built into `nevr_quest_combat` without exceptions and with hidden visibility, and hooked through the sentinel's hook manager
(`nevr_quest_got_hook`). It runs in the `q15t` and `combat` APKs only (`combat_active.h`), except the two files that say on their header
that they ship to every APK (`match_tags`, `equip_refire`).

## Layout

| File | What it holds |
| --- | --- |
| `combat_active.{h,cpp}` | The gate: `combat::Active()`, from the APK's variant and `combat.force` (a development build only). |
| `combat_install.{h,cpp}` | The two install steps, `InstallEarly()` and `InstallNormal()`, the registration table they run, the `combat_install_module` record each module's step writes and the `combat_install` line each phase writes, and the `combat_config` line. |
| `combat_scope.{h,cpp}` | Whether a combat level is loaded, kept fresh from the game-thread tick and read as one relaxed load and a test: what a hook on a slot arena also uses asks (rule 9). |
| `<module>.{h,cpp}` | One module: namespace `nevr_quest::combat::<module>`, log events `combat_<module>_*`. |
| `<module>_targets.h` | The module's pinned slots, each checked by the module's own pinned test (`src/quest/tests/<module>_pinned_test.cpp`, run by `just test-quest-hooks-pinned`-style recipes against the store APK). |
| `mutants.sh`, `mutants.d/<module>.sh` | The module's mutation run: `src/quest/combat/mutants.sh <module>` applies each `runm <name> <file> <sed>` line of the module's list to the checkout, builds `<module>_test` and runs it; a mutant is killed when the test fails, a mutant that does not compile is not a kill, and the file is put back after each. Not part of `just test-quest-combat`. |
| `table_step.{h,cpp}` | What the four resource table-step modules (`meshstep`, `instmodel`, `shaderstep`, `tablestep`) share: the scope they act in (`CurrentScope`: a combat map is loaded and no Echo Arena level is), the code-word pin check, the libr15 identity check, the correction of a shader-set step and the warning for a plugin kill file. |
| `check/<module>.sh` | The module's arm64 checks (the NDK, and qemu where a thunk is run). `just test-quest-combat` runs them after the host tests. |
| `mutants.sh`, `mutants.d/<module>.sh` | The module's mutation run: `src/quest/combat/mutants.sh <module>` applies each `runm <name> <file> <sed>` line of the module's list to the checkout, builds `<module>_test` and runs it; a mutant is killed when the test fails, a mutant that does not compile is not a kill, and the file is put back after each. Not part of `just test-quest-combat`. |

## When a module installs

- **Early** (`InstallEarly`): a constructor step of the sentinel's sequence, `install_combat_early`, after Attach and the early plugin pass and
  before the clock hook. A module that must act before `android_main` reads its own configuration has an early function.
- **Normal** (`InstallNormal`): a post-load action, after the sentinel's login and matchmaking installs and after the normal plugin pass has
  returned, on the thread that ran it, at the first non-null `dlopen` the game makes. A module that needs the game's libraries has a normal function.

Both steps ask `combat::Active()` and install nothing when it is false. A plain release prints one `combat_install` line with
`"state":"skipped"` and no module runs. A normal step that never got its moment (the post-load hook could not go live, or the game's
`android_main` returned first) prints one `"state":"not_started"` line with the reason.

## The module rules

1. A module registers one function per phase it has a step in, as one row of the table in `combat_install.cpp`, at the position the q15t
   plugin installed it. The order is explicit; decals stays after impactsnd.
2. Every hook goes through the hook manager (`InstallThunk` and its siblings). A module writes no code pointer and no data pointer of a
   game library any other way (ADR 0046 C1).
3. A module's switch is a key under `combat:` in `config.yaml`, read by `quest_config` and reported by its own `combat_config_switch` record, generated from the module's row in the registration table (`Module::switchKey`, `Module::switchMember`; the modules that predate the record are on the `combat_config` line, or a `combat_config_<module>` line beside it when their keys do not fit its 768-byte buffer). No file under
   the app's files directory turns a module on or off (ADR 0046 C2). The key set is `KnownSections` in `src/core/nevr_config.cpp`,
   `CombatConfig` in `src/quest/sentinel/quest_config.h` and `docs/reference/example-config.yaml`.
4. A module's install function is `noexcept`, returns whether it installed and a fixed status token, and writes its own lines
   (`combat_<module>_*`): each hook's outcome by name, and one aggregate (N17).
5. A module's header names its origin: the reference build's source file and line, and the upstream file and line where it ports another
   project's rules (ADR 0046 rule 9).
6. A module's pinned slots are in its own `<module>_targets.h` with its own pinned test, so two module units never edit one file. The files
   every module unit edits are shared and merge in sequence: the registration table, the `combat:` key set, `hook_budget.h`,
   `CMakeLists.txt`, `host_tests/groups/combat.cmake`, and the plugin's module list while it exists.
7. A module's host test is a program in the `combat` group with the module's name in its own, so `just test-quest-combat <module>` finds it.
8. An early function does not touch a slot that `libq15t.so` installs at its normal pass. The early step runs before the plugin's hooks are in,
   so it would take the slot first and q15t would refuse it (the #2000 shape); such a module installs in the normal step, after the plugin pass.
9. A hook on a slot that arena's scripts also call tests `scope::SeenCombatOrLobby()` or `scope::SeenCombatMap()` (`combat_scope.h`) first and, outside a combat
   level, calls the game's function with the same arguments and does nothing else: no read of the game's data, no write, no counter, no line. The handler is
   that test and a tail call to the game's function or to a `[[gnu::noinline]]` body for the combat case, so it has no frame (check the arm64 object with
   `llvm-objdump -d`); the body starts with `scope::LookIsFresh()`, because a true answer older than `kStaleAfterMs` is not believed (the tick stopped).
   The refresh is registered by the `scope` row of the install table, after the `tick` row, and only while a reader is on (`ScopeNormal()` in `combat_install.cpp` lists the readers' switches: a module that reads the scope adds its switch there); a module does not register it, it asks `scope::Live()` from its
   install function, before it hooks anything, and refuses (`scope_not_live`) when the answer cannot change. The module's host test pins the arena path with data
   pointers nobody could read and by count (`SetReadCounterForTest`, `level::ScanCalls()`: zero reads and zero scans), and prints its cost. Arena and combat run on the same APK, so `combat::Active()` does not do this.
   The one exception is a crash-only guard: it stops a fault in code arena shares (a null dereference, a write through a null page, a double destroy), so a level answer that lags a level change would leave the crash in place. It may act in every level when its header says so and why, and it passes every path that does not crash through unchanged: the game's function is called with the same arguments and answers what it would have. `voipdrop`, `droneguard` and `custctx`'s destroy and Grow guards are such guards; a guard that changes a value the game computes or writes the game's memory (`fxtrail`, `peerclamp`, `custctx`'s pool refusal) is not, and follows the rule above.

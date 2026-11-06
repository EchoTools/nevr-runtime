# Module `fxtrail` (row R29)

Files: `src/quest/combat/fxtrail.{h,cpp}`, `src/quest/combat/fxtrail_targets.h` (the pins), `src/quest/combat/level_scope.{h,cpp}` (the level answer it is
scoped by), `src/quest/tests/fxtrail_test.cpp`, `src/quest/combat/mutants.d/fxtrail.sh`. The module is a normal-phase row of the combat install table
(`src/quest/combat/combat_install.h`): it runs in the `q15t` and `combat` APKs and in no other. Switch in `config.yaml` (docs/adr/0046 rule 4):
`combat.fxtrail` (default true). The q15t plugin's `fxtrail.off` file is no longer read; one found in the files folder's `q15t` directory is a Warning
(`combat_fxtrail_legacy_switch`) that names the key.

Origin: Ducky's echoquest-combat reference build, afk_fxtrail.cpp (the rule at :1-24, the install at :60-77; docs/audits/2026-11-05-q15t-origin-citations.md
rows 63-68 and 212-213), reconstructed in the q15t plugin (imported in #1591) and moved here. The plugin's 11 code-word pins were an addition to the source's;
the level scope is an addition made in the move.

## Row covered

R29: `NRadEngine::CParticleEffectCS::EffectTime(SCompactPoolHandle) const`, JUMP_SLOT ELF 0x36e7b98, real function ELF 0x16647e4 (disassembly 0x17647e4,
56 bytes). `readelf -rW` of the store libr15 (sha256 `8dd9a961...`, build ID `b243509c...`) lists
`00000000036e7b98  R_AARCH64_JUMP_SLOT  00000000016647e4 _ZNK10NRadEngine17CParticleEffectCS10EffectTimeENS_18SCompactPoolHandleE`. The function, by the disassembly:

```
16647e4 ldr x8, [x0, #0xc8]          the index table
16647e8 and x9, x1, #0xffff
16647ec lsl x9, x9, #0x2             4 bytes an entry
16647f0 mov w10, #0x18
16647f4 ldrh w8, [x8, x9]            the data index
16647f8 ldr x9, [x0, #0xf8]          the data array
1664800 madd x8, x8, x10, x9         0x18 bytes an entry
1664804 ldr x8, [x8, #0x10]          the instance
1664808 cbz x8, 0x1664814
166480c ldr s0, [x8, #0x340]         the effect's time
1664810 ret
1664814 fmov s0, wzr                 no instance: 0.0
```

## What it does

Why (the reference build's fxtrail:1-24): a bullet is freed only once every effect slot reports done; a trail slot finishes only when `EffectTime(h)` is above
the trail's time after impact. The Quest build creates no particle effect (the reference build's claim; not re-measured here), so the effect has no instance,
`EffectTime` returns 0.0, no trail slot ever finishes, no bullet with a trail is ever freed, and `CR15NetBulletCS::UpdateFX` queues one deferred decal-disable call per
bullet per frame until the queue overflows (SIGTRAP "CDeferredMethodQueue buffer has exceeded max number of calls (960)", about 53 s into combat). Its only caller in libr15
is `FXUpdateTrail`, so the slot reaches every caller; script libraries call it through their own GOTs and are not touched.

The handler (a `CallbackThunk` on the slot, installed by the hook manager): call the game's function first and keep its value; if a combat map is loaded, read
`[cs+0xc8]`, `[cs+0xf8]`, the u16 at `index + 4 * (h & 0xffff)` and the instance word at `data + idx * 0x18 + 0x10`, each with the never-faulting read
(`combat/safe_read.h`); a read that fails, or a non-null instance, answers the game's value; a null instance answers `FLT_MAX` and is counted.

## Arena cost and arena behaviour (arena and combat share the APK)

- **Behaviour.** The guard acts only while a combat map is loaded. `level_scope.h` publishes `InCombatMap()` from the combat tick (a look every half second; an Echo
  Arena level that is loaded wins over any combat level still loaded during a transition). In an arena match, in every lobby (the combat lobby too: bullets exist in a map)
  and in any other level the handler returns the game's value after one relaxed atomic load and reads no game memory. This is a scoped guard because it changes a value the
  game computes (a trail that "never ran its course" now has), and the premise is the reference build's, not shown in arena; a guard that only stops a crash acts in every level (voipdrop,
  custctx, droneguard). `fxtrail_test` runs an effect with no instance in an Echo Arena level, in the lowspec arena level, in the tutorial, in the lobbies arena players use, in the combat
  lobby, with nothing loaded, before the scope's first look and with an arena level and a combat map both loaded: the game's value every time, no read, nothing counted.
- **Cost.** `EffectTime` runs once per effect handle per query; how often per frame is not known: `combat_fxtrail_stats` counts every call (`calls`) and those made in a combat map
  (`in_combat`), written at the end of the process and every 16384 combat-map calls, so a headset run gives the rate (ADR 0038). In arena each call pays the thunk's counter and one atomic
  load; the scope's tick adds one clock read per frame and, every half second, one `level::Scan` (two table reads and, per gamespace, three more `process_vm_readv` reads). In a combat map
  each call also pays up to four `process_vm_readv` calls (the plugin's cost, kept). Host numbers (x86-64, -O2, 2,000,000 calls through the installed thunk on a libr15 image built in memory, a stand-in `EffectTime`, three runs, a temporary benchmark that is not committed, on a machine that was building at the time): an unhooked slot 1.5 to 2.5 ns per call; hooked, in an Echo Arena level, 5.5 to 11.2 ns per call (4 to 9 ns added); hooked, in a combat map, 3.1 to 3.4 us per call (the four reads). At 3.4 us the guard stays inside ADR 0038's 10 us per hook up to about 3 calls per frame in a combat map; the rate is what `combat_fxtrail_stats` measures. Not measured on a headset.

## Gate: no file hash

The gates are the patched build ID the install checks, the 11 code words below (read before the slot is touched) and the slot's own expected word: it must hold exactly
`base + 0x16647e4`, so a word something else replaced is refused, not chained over. A word that differs is refused with one Error `combat_fxtrail_install` line
(`reason` `pinned_word_mismatch`, `pin`, `found`, `expected`; `found` is `unreadable` for a page that cannot be read). The pins are `kWords` in `fxtrail_targets.h`;
`got_pinned_test` reads every one in the store libr15. `fxtrail_test` flips each in turn, reads one from an unreadable page, and pins the table by a fingerprint.

## Lines (sentinel log)

- `combat_fxtrail_install`: `result` `installed|refused|off`, `reason`, `hook` (the manager's status), `scope` `combat_map`, `consequence`. Info for `installed` and `off` (reason
  `off_config`), Error for a refusal (`module_not_loaded`, `no_build_id`, `build_id_mismatch`, `pinned_word_mismatch`, `tick_table_full`, `slot_refused`).
- `combat_fxtrail_ended`: Info at the 1st, 2nd, 4th, 8th ... ended trail (`n`).
- `combat_fxtrail_stats`: `calls`, `in_combat`, `ended`, `level_looks`, `final`; Debug every 16384 combat-map calls, Info once at the end of the process when the guard is installed.
  `level_looks` 0 says the scope never ran (then nothing is ended).
- `combat_fxtrail_legacy_switch`: Warning, when the plugin's `fxtrail.off` is found.

## Prediction (written before any headset run)

On the pinned build, on a combat APK, in a combat match: `combat_fxtrail_install` `installed` at start, then `combat_fxtrail_ended` `n=1` within the first shots that hit something, later
`n=2`, `n=4`, and `combat_fxtrail_stats` with `level_looks` above 0 and `in_combat` below `calls` when the match is in an arena level before it. The SIGTRAP at about 53 s no longer occurs in a normal match.
In an Echo Arena match on the same APK: `ended` stays 0 and `in_combat` stays 0.

## Falsifiers

- `combat_fxtrail_install` `refused` with `pinned_word_mismatch` on a build the sentinel accepted by build ID: the code at that address is not what the handler was derived from.
- The SIGTRAP occurs with `ended` above 0: `EffectTime` is not the only thing keeping bullets alive.
- The SIGTRAP occurs and `ended` is 0 with `level_looks` above 0 and `in_combat` above 0: the instance is non-null (the Quest build does create effects: the premise is wrong).
- `level_looks` 0: the combat tick did not run, so the scope never said "combat map" and nothing was ended.
- `ended` above 0 in an Echo Arena match: the scope is wrong.
- A visible change in trails or a crash inside `FXUpdateTrail`: the `FLT_MAX` answer reaches a caller that does not expect it.

## Not established

- On a headset: that the install succeeds on a combat APK, the call rate of `EffectTime`, the cost per call, and that the SIGTRAP is gone.
- That the Quest build never creates a particle effect, and that `FXUpdateTrail` is the only caller (the reference build's claims).

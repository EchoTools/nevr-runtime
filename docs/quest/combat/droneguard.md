# Module `droneguard` (row R35)

Files: `src/quest/combat/droneguard.{h,cpp}`, `src/quest/combat/droneguard_targets.h` (the pins), `src/quest/tests/droneguard_test.cpp`,
`src/quest/combat/mutants.d/droneguard.sh`. The module is a normal-phase row of the combat install table (`src/quest/combat/combat_install.h`): it runs in the
`q15t` and `combat` APKs and in no other. Switches in `config.yaml` (docs/adr/0046 rule 4): `combat.droneguard` (default true) and `combat.droneguard_stage1`
(default false; read only when the first is on). The plugin had no switch file; its build option for the second stage is gone.

Origin: Ducky's echoquest-combat reference build, afk_droneguard.cpp (the rule at :1, the constants at :42-48, :66-80, `IdxSkip` :86-99, `StateLine` :108-124,
`FollowHook` :126-146; docs/audits/2026-11-05-q15t-origin-citations.md rows 34-36 and 149-150), reconstructed in the q15t plugin (imported in #1591) and moved
here. The reference build marks the module "TEST ONLY"; its wave19a on-list turns it on, and so does the default here. The word pins are the plugin's addition to
the source's.

## Row covered

R35: `CR15NetFollowPlayerCS::Follow(SCompactPoolHandle)`, JUMP_SLOT ELF 0x36f1218, real function ELF 0x2149764 (disassembly 0x2249764, 908 bytes). `readelf -rW` of the
store libr15 (sha256 `8dd9a961...`, build ID `b243509c...`) lists `00000000036f1218  R_AARCH64_JUMP_SLOT  0000000002149764 _ZN10NRadEngine8NRadGame21CR15NetFollowPlayerCS6FollowENS_18SCompactPoolHandleE`;
its one caller is `Update`, through the PLT stub. The words (ELF vaddr, word):

```
2149744 97b7a7c7 bl <PLT Follow>               the one call (Update)
21497bc f940a274 ldr x20, [x19, #0x140]        the followed system, no null test
21497c8 97b70b0a bl <PLT RefreshCache>
21497cc b9419261 ldr w1, [x19, #0x190]         cached index
214980c 79432268 ldrh w8, [x19, #0x190]
1cdb3e0 7941e008 ldrh w8, [x0, #0xf0]          IsComponentValid
1cdb3f8 6b41413f cmp w9, w1, LSR #0x10
```
`RefreshCache` is defined at 0x1210848 (568 bytes) and `IsComponentValid` at 0x1cdb3e0 (68 bytes).

## What it does

The crash (reference build :1-14): the drone's actor carries a follow component enabled by the shared magazine script before any orbit target exists; `Follow`
loads `[cs+0x140]` (a `CR15PlayerNavCS*`) and passes it as `this` to `FinalTransform` unchecked.

- **Stage 0 (the null half).** The handler (a `CallbackThunk` on the slot, installed by the hook manager) reads the four fields it needs (`[cs+0x140]`, `+0x188`, `+0x190`, `+0x16c`) with plain loads: `Follow` loads `[cs+0xc8]` as its first data access (0x2149784,
  disassembly), so the game dereferences the same object on every call and the loads are as safe as the stock call. Target null: the game's `Follow` is not called, the skip is counted, and `combat_droneguard_skip` is written for the first skip of a run (then a total every 600th skip, at
  most 8). Target set: if a run of skips was in progress one `combat_droneguard_target_set`, then the game's function with the same two arguments. Every call also feeds the
  read-only `combat_droneguard_state` line: the first call and each change of (raw index word, flag byte at +0x16c, target null, handle system null), at most 16 lines. A
  null `cs` goes to the game's function (an unreadable non-null one faults in `Follow` itself, with or without the guard).
- **Stage 1 (the idx guard, `combat.droneguard_stage1`).** After the null test: `RefreshCache(cs+0x148)`, read the cache again, then skip when the u16 `[cs+0x190]` is 0xffff
  (`idx ffff`), the cached system `[cs+0x188]` is 0 (`handle-sys 0`), or `IsComponentValid(target, idx)` refuses (`refused but cached`); `combat_droneguard_idx_skip` for the first
  skip of a run, `combat_droneguard_idx_resolved` when a call goes through again, a total every 600th at most 8 times. A cache unreadable after the refresh: Follow runs. It needs its own three
  words and both engine functions at exactly their pinned addresses (dlsym on the loaded game library); otherwise one Error `combat_droneguard_stage1` line and the null half only.
- No access-violation handler (as in the reference build): a non-null but stale pointer still faults.

## Arena cost and arena behaviour (arena and combat share the APK)

- **Behaviour.** A crash-only guard, and it acts in every level on purpose: it asks nothing of the combat level answer (`combat_scope.h`) and is not a reader of the scope row. The fault happens when the drone's actor is enabled, which can be in the first
  frames of a level load, before a level look up to a quarter of a second old would see the level. It changes what the game does only where the stock call would dereference NULL (stage 0) or use
  a stale cache (stage 1): that component's follow update is skipped for that frame. Every other call reaches the game's `Follow` with the same two arguments
  (`droneguard_test`: set target, healthy player, argument mutants). `TheGuardHasNoLevelScope` runs a null target with no level look ever made and sees it skipped. Whether the arena has
  `CR15NetFollowPlayerCS` components at all is not established here; `combat_droneguard_stats` gives the call count on a headset. Stage 1 is off unless asked for.
- **Cost.** Per `Follow` call: the thunk's counter, four plain loads, the state-key compare and a branch; stage 1 adds a `RefreshCache` call (the game's), a never-faulting read of the refreshed cache and, for
  a resolved player, an `IsComponentValid` call. `Follow` runs once per enabled follow component per frame. Host numbers (x86-64, -O2, 2,000,000 calls through the installed thunk on a libr15 image built in memory, a stand-in `Follow`, a scratch benchmark that is not committed):
  before the change to plain loads (one `process_vm_readv` of the span): an unhooked slot 1.6 ns per call; stage 0 with a target set 423 ns; skipping a null target 434 ns; stage 1 with a healthy
  player 839 ns (the same machine, one session). After: an unhooked slot 1.5 ns; stage 0 with a target set 6.1 ns; skipping a null target 8.0 ns; stage 1 with a healthy player 427 ns (the
  never-faulting read of the refreshed cache is what is left of it). At 8 ns the guard stays inside ADR 0038's 10 us per hook up to about 1,000 `Follow` calls per frame (stage 0); the call rate is
  not known (`combat_droneguard_stats`). Not measured on a headset.

## Gate: no file hash

The gates are the patched build ID the install checks, the four stage-0 code words (read before the slot is touched) and the slot's own expected word: it must hold exactly `base + 0x2149764`, so a word
something else replaced is refused, not chained over. A word that differs is one Error `combat_droneguard_install` line (`reason` `pinned_word_mismatch`, `pin` (the word's name), `vaddr`, `found`, `expected`; `found` is
`unreadable` for a page that cannot be read). Stage 1's three words are checked after those, and a mismatch is an Error `combat_droneguard_stage1` line naming the pin with the null half still installed.
The tables are `kWords` and `kIdxWords` in `droneguard_targets.h` (`pinned::Word`; the follow offsets are tied to stage 0's words by `static_assert`; checked by `pinned::FirstFailure`); `got_pinned_test` reads each in the store libr15. `droneguard_test` flips each in turn, reads one from an unreadable page, and pins both tables by a
fingerprint.

## Lines (sentinel log)

- `combat_droneguard_install`: `result` `installed|refused|off`, `reason` (`ok`, `off_config`, `module_not_loaded`, `no_build_id`, `build_id_mismatch`, `pinned_word_mismatch`, `slot_refused`), `hook`, `stage1`
  (`off|on|null_half_only`), `consequence`. Info for installed and off, Error for refused. The table's status for an installed guard whose stage 1 was asked and is not available is `installed_null_half`.
- `combat_droneguard_stage1`: `result` `on|null_half_only`, `reason`; Info for on, Error otherwise.
- `combat_droneguard_state`, `combat_droneguard_skip`, `combat_droneguard_skips`, `combat_droneguard_target_set`, `combat_droneguard_idx_skip`, `combat_droneguard_idx_skips`, `combat_droneguard_idx_resolved`: Info, capped as above.
- `combat_droneguard_stats`: `calls`, `skips`, `idx_skips`, `final`; Debug every 16384 calls, Info once at the end of the process when the guard is installed.

## Prediction (written before any headset run)

On a combat APK in a combat match with a drone: `combat_droneguard_install` `installed`; at the drone's spawn one `combat_droneguard_skip`, then when a script runs `R15FollowPlayerSetOrbitNode` one
`combat_droneguard_target_set` with the skip count and Follow runs from then on; no crash at the null `FinalTransform` call. A match with no drone shows no skip line. With `combat.droneguard_stage1: true`
the stage-1 line says `on`; idx skips appear only around a player leaving.

## Falsifiers

- `combat_droneguard_install` `refused` with `pinned_word_mismatch` on a build the sentinel accepted by build ID: the code at that address is not what the handler was derived from.
- A crash in `CR15PlayerNavCS::FinalTransform` with a null `this` despite the skip lines: the null test reads the wrong word (offset 0x140).
- `combat_droneguard_skip` lines in a match with no drone: the null target occurs in stock play and the guard changes behaviour there.
- `combat_droneguard_state` shows `target` `set` but the crash persists: the stale-pointer case (not covered), stage 1 is needed.
- The drone never follows after `combat_droneguard_target_set`: the game's call was not made with the engine's handle.
- `combat_droneguard_stage1` `null_half_only` with `engine_functions_missing`: `RefreshCache` / `IsComponentValid` are not at the pinned addresses.

## Not established

- On a headset: that the install succeeds on a combat APK, the call rate of `Follow` in combat and in an arena match, the cost per call, and that the crash is gone.
- The call convention (x0 the component, x1 the handle, nothing read from x0 after the call), and the `RefreshCache` and `IsComponentValid` parameter types (reference build :66-80). The words above and the two function rows were read, not Follow's whole body.

# Module `droneguard` (row R35)

Files: `src/droneguard.{h,cpp}`, `src/quest/tests/q15t/droneguard_test.cpp`, `src/quest/tests/q15t/mod/droneguard.sh`, `src/quest/tests/q15t/mutants.d/droneguard.sh`.

Option macros for the main seat to wire: `Q15T_DRONEGUARD`, default **ON** (#1625: the source is "TEST ONLY", but its wave19a on-list turns `droneguard.on` on; source: "TEST ONLY", the source build's droneguard:1;
`Switch()` returns false unless `droneguard.on` or `debug.<app>.droneguard 1`, `:245-246`), and `Q15T_DRONEGUARD_IDX`,
default **OFF** (stage 1, source `droneguard_idx.on`, `:21-24, 253`), only read when the first is on. `Q15T_DRONEGUARD_IDX` is read
by `droneguard.cpp` itself (a `#ifndef ... #define 0` in `droneguard.h`); the main seat only needs to pass it as a compile definition.
API: `InstallResult InstallDroneGuard(std::uintptr_t base)`; seam `InstallDroneGuardAt(base, slot, real, idx_stage, fns)`;
`GetDroneGuardStats()`. No tick. No file switch is kept (both source switches are enabling switches, replaced by the options).
Capability note for the main seat: this changes gameplay logic (skips a per-frame call); the plugin's capability flags should include
ALTERS_GAMEPLAY when it is on.

## Row covered

R35: `CR15NetFollowPlayerCS::Follow(SCompactPoolHandle)`, JUMP_SLOT ELF 0x36f1218, real function ELF 0x2149764 = disassembly 0x2249764
(size 908). Read this session:
- `R0_readelf_r.txt:131799`: `00000000036f1218  0000475100000402 R_AARCH64_JUMP_SLOT    0000000002149764 _ZN10NRadEngine8NRadGame21CR15NetFollowPlayerCS6FollowENS_18SCompactPoolHandleE + 0`
- Disassembly `functions`: `2149764 | Follow | 908`; `R1_functions.md:48` (1 caller, `Update` through the PLT stub).
- Words (all 7 of the source's droneguard words equal disassembly):
```
2149744 97b7a7c7 bl <PLT Follow>               the one call (Update)
21497bc f940a274 ldr x20, [x19, #0x140]        the followed system, no null test
21497c8 97b70b0a bl <PLT RefreshCache>
21497cc b9419261 ldr w1, [x19, #0x190]         cached index (stage 1)
214980c 79432268 ldrh w8, [x19, #0x190]
1cdb3e0 7941e008 ldrh w8, [x0, #0xf0]          IsComponentValid (stage 1)
1cdb3f8 6b41413f cmp w9, w1, LSR #0x10
```
- Disassembly `functions`: `1210848 | RefreshCache | 568`, `1cdb3e0 | IsComponentValid | 68`.
Source: constants `the source build's droneguard:42-48, 66-80`, `IdxSkip :86-99`, `StateLine :108-124`, `FollowHook :126-146`.

## What it does

Crash (source `:1-14`): the drone's actor carries a follow component enabled by the shared magazine script before any orbit target
exists; `Follow` loads `[cs+0x140]` (a `CR15PlayerNavCS*`) and passes it as `this` to `FinalTransform` unchecked.
- **Stage 0 (null half).** Wrapper `(void* cs, uint64_t handle)`: one SafeRead of the span `[cs+0x140, cs+0x194)`. Target null: do not call
  Follow, count, one INFO `droneguard first skip: follow target null` (then a total every 600th skip, at most 8). Target set: if a skip run was in
  progress one INFO `droneguard target set after skips`, then the real function with the same two arguments. Every call also feeds
  the read-only state line: `droneguard state ...` on the first call and each change of (raw index word, flag byte at +0x16c,
  target null, handle-system null), at most 16 lines. An unreadable `cs` goes to the real function.
- **Stage 1 (idx guard, only with `Q15T_DRONEGUARD_IDX`).** After the null test: `RefreshCache(cs+0x148)` (engine export 0x1210848), re-read
  the cache, then skip when u16 `[cs+0x190] == 0xffff` ("idx ffff"), the cached system `[cs+0x188]` is 0 ("handle-sys 0"), or
  `IsComponentValid(target, idx)` (export 0x1cdb3e0) refuses ("refused but cached"); `first idx skip` / `player resolved after idx skips`
  / a total every 600th at most 8 times. A cache unreadable after the refresh: Follow runs. Needs both exports at exactly their pinned
  addresses (dlsym), else one ERROR line and the null half only.
- No access-violation handler (as in the source, DN-6): a non-null but stale pointer still faults.

Mechanism: `InstallImportHook(symbol, base, 0x36f1218, &FollowWrapper, &g_real, 0x2149764)`; the slot must hold exactly `base +
0x2149764` or the install is refused. Logging: INFO `droneguard on (TEST ONLY)` / `idx guard on`, ERROR `droneguard refused`, ERROR
`droneguard idx guard off`; no per-call lines.

## Gate: no MD5, no file hash

The only gates are: the plugin's build-id check in InitEx, the code-word pins below (`src/quest/plugins/q15t/src/word_pins.h`, `CheckWordPins`: every
word is read with SafeRead at `base + ELF vaddr` before any slot is touched) and the slot-target check (the slot must hold exactly
`base + the real function's vaddr`). **There is no MD5 gate and no whole-file hash of any kind**: a rebuilt, repacked or custom-server
build that still carries these words is not refused. A pin that fails refuses the module (nothing swapped, reason `word_pin_mismatch`)
with one ERROR line naming it: `<module> refused: code word pin mismatch, ... pin=0x<ELF vaddr> found=0x<word read, or unreadable>
expected=0x<word pinned>`. A slot-target refusal logs ERROR with `pin=<slot ELF vaddr> found=<word at the slot> expected=<base + real
vaddr>`. Pin addresses in this note are ELF vaddrs (disassembly VA = ELF + 0x100000). Stage 0 has 4 pins (`the source build's droneguard:43-44, 57-58`: the `ldr x20,[x19,#0x140]`, the Update call site, the two index loads); stage 1 has 3 more of its own (`:76-77`: RefreshCache call site, IsComponentValid first words), checked by `ResolveDroneGuardFns` before the dlsym: a stage-1 pin mismatch is one ERROR line ("null half only") and the null half still installs.
All pins read in the disassembly equal the tables in `src/quest/plugins/q15t/src/droneguard.h` (one query whose VALUES are built from the header tables:
`MATCH | 88` over the five modules' tables, no NOT_SHOWN, no MISMATCH). Host test: every pin flipped in turn is refused and named, an
unreadable text is refused and named, the table's fingerprint is pinned, the installer refuses on a changed word (`src/quest/tests/q15t/pin_check.h`).

## Prediction (written before any headset run)

With `Q15T_DRONEGUARD=1` in an EC drone match: `droneguard on (TEST ONLY) ... status=ok`; at the drone's spawn one `droneguard
first skip: follow target null`, then when a script runs `R15FollowPlayerSetOrbitNode` one `droneguard target set after skips skips=<n>`
and Follow runs from then on; no crash at the null `FinalTransform` call. A stock combat leg with no drone shows **zero** skip lines
(only the first-call `droneguard state` line if Follow is called at all). With `_IDX=1` additionally `droneguard idx guard on`; idx skips
appear only around a player leaving.

## Falsifiers

- `droneguard refused: code word pin mismatch` (ERROR; pin / found / expected named) on the build the plugin accepted by build id: the code at that
  address is not what the wrapper was derived from (or the pin table has a typo); nothing is swapped, the game runs stock.
- Crash in `CR15PlayerNavCS::FinalTransform` with a null `this` despite the skip lines: the null test reads the wrong word (offset 0x140).
- `first skip` lines in a leg with no drone: the null target occurs in stock play, the guard changes behaviour there.
- `droneguard state` shows `target=set` but the crash persists: the stale-pointer case (not covered) or stage 1 needed.
- Drone never follows after `target set`: the real call was not made with the engine's handle (arguments changed).
- `droneguard idx guard off`: `RefreshCache` / `IsComponentValid` are not at the pinned addresses.

## Needs a word patch / not built

- **Needs a word patch:** none.
- Not built: the md5 gate (deliberately none; the source had none either), file/property enabling switches (replaced by the options), an access-violation handler (not in the source either).
- Unverified: the call convention (`x0` = system, `x1` = handle, nothing read from `x0` after the call, source DN-1); the `RefreshCache`
  and `IsComponentValid` parameter types (source `:66-80`); I read the seven words and the two function rows above, not Follow's whole body.

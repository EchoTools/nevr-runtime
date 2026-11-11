# Module `wallfire`: no shot through a wall (hooks R26-R28 of the reference build)

Pinned build: libr15 build id b243509c08ce677aeb95fa348016949b3fc45230 (disassembly VA = ELF vaddr + 0x100000).
Origin: the reference build's runtime/native/afkxr/afk_wallfire.cpp (the rule :1-13, the pins :34-36, the default :17,78); the commit is in
`docs/audits/2026-11-05-q15t-origin-citations.md`. The module is Ducky's (kotorvr's echoquest-combat): every address below is in upstream's afk_wallfire.cpp.
Code: `src/quest/combat/wallfire.h`, `wallfire.cpp`, `wallfire_targets.h`. Test: `src/quest/tests/wallfire_test.cpp` (the real bvh module on an image built in
memory, a synthetic room mesh), `src/quest/tests/got_pinned_test.cpp` (the pins against the real libr15).
**Depends on `bvh`** (`bvh::TreeInstalled()`): without the static BVH answering `CBVHTree::Occluded` the segment query is never blocked, so wallfire installs
nothing (`bvh_not_installed`, Warning). Both default to true; `combat.wallfire: true` with `combat.bvh: false` is that refusal.

## 1. Hooks

| Row | What | Address | Stock | Action |
|---|---|---|---|---|
| R26 | `CncaGameSpace::QueryCollision(this, SPhQueryCollisionRequest&, CPhId)` | JUMP_SLOT ELF 0x36e0c38 (disassembly 0x37e0c38); function ELF 0x1806784 / disassembly 0x1906784 | 104-byte function (physics bodies only) | slot hooked by `OnQuery` |
| R27 | `CR15NetGunCS::FireGunLocal`, the return site after `bl QueryCollision@plt` | ELF 0x215566c / disassembly 0x225566c, word `0x3941b3e8` (`ldrb w8, [sp, #0x6c]`) | not written | **read only**: the word is checked at install, and the hook acts only when it returns to this address |
| R28 | `CncaGameSpace::StaticRaycastOccluded(this, C3Vector*, C3Vector*)` | JUMP_SLOT ELF 0x36ce380; function ELF 0x18066f4 / disassembly 0x19066f4 | 16-byte virtual thunk | **called**, not hooked: the slot is checked to hold base + 0x18066f4 and the function is called directly |

`readelf -rW` of the stock libr15 (sha256 `8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20`):

    :123419: 00000000036e0c38 R_AARCH64_JUMP_SLOT 0000000001806784 _ZNK10NRadEngine13CncaGameSpace14QueryCollisionERNS_24SPhQueryCollisionRequestENS_5CPhIdE + 0
    :113924: 00000000036ce380 R_AARCH64_JUMP_SLOT 00000000018066f4 _ZNK10NRadEngine13CncaGameSpace21StaticRaycastOccludedERKNS_8C3VectorES3_ + 0

**Needs a word patch: none.** R26 is a data-page GOT hook through the hook manager (`InstallThunk`, `wallfire_targets.h`); R27 is a read of a code word, R28 a call.

## 2. What it does, and the mechanism

PC and Quest run the same `CR15NetGunCS::FireGunLocal` (afk_wallfire.cpp:1-4). Before a shot it builds an
`SPhQueryCollisionRequest` on its stack for the segment player position -> muzzle position, calls `QueryCollision` and, on a hit, returns
without firing. On the PC the query sees the static level; on the Quest it reaches physics bodies only (the static level lives in
CBVHTree, compiled out on Android and served by `bvh`), so a hand or muzzle through a wall fires (afk_wallfire.cpp:1-10).

`OnQuery(original, caller, gs, req, id)` (a `kCaller` thunk):
1. is told its return address (the slot's PLT stub is `adrp; ldr; add; br`, so it is the caller's); with no combat map or combat lobby loaded (or a look older than a second) it calls the game's function and returns; else it runs the game's `QueryCollision`;
2. returns the stock answer unless **all** hold: the return address is `base + 0x215566c`, the stock answer was "nothing", `gs != 0`;
3. copies the 24 bytes at `req` as six floats, the segment start xyz and end xyz; any non-finite value: the stock answer (counter `skipped_nan`);
4. checks the game structure the call will walk (below); if it is not as pinned: the stock answer, counter `skipped_game`, one WARNING;
5. calls `CncaGameSpace::StaticRaycastOccluded(gs, &seg[0], &seg[3])` (the pinned function at `base + 0x18066f4`);
   blocked -> returns 1, so `FireGunLocal` returns without firing; clear -> the stock answer.
Every other caller of `QueryCollision` gets the stock value untouched. Bots fire through the same code (PC-equal).

R28's call chain (the disassembly):

    19066f4 ldr x0, [x0, #0x8]      000440f9     x0 = *(gs + 8) = CncaGame
    19066f8 ldr x8, [x0]            080040f9     the CncaGame's vtable pointer
    19066fc ldr x3, [x8, #0x278]    033d41f9     virtual +0x278
    1906700 br  x3                  60001fd6

`readelf` shows the vtable slot holding `CncaGame::StaticRaycastOccluded` (ELF 0x17f2614 = disassembly 0x18f2614; the function list names it
`NRadEngine::CncaGame::StaticRaycastOccluded`, size 464):

    :28821: 000000000362efa8 R_AARCH64_ABS64 00000000017f2614 _ZNK10NRadEngine8CncaGame21StaticRaycastOccludedERKNS_8C3VectorES3_ + 0
    (also :28822-28826, five more vtables)

and its body ends with the call into `bvh`'s R23 wrapper:

    18f27b0 add x0, x8, #0x148        (the CStaticRaycastCS's CBVHTree)
    18f27b4 add x1, sp, #0x10
    18f27b8 mov x2, sp
    18f27bc bl  0x01021a90            PLT stub: adrp x16,0x37e8000 ; ldr x17,[x16,#0x430] = slot 0x37e8430 = ELF 0x36e8430 = bvh R23

so the query ends in `CBVHTree::Occluded(tree, a', b')` where a', b' are the two points after the function's own arithmetic (`0x18f2740..0x18f2754`, not decoded here).

The extra check (ours, `GameIsPinned`): `*(gs + 8)` is the CncaGame, `*game` its vtable pointer, and `[vtable + 0x278]` must equal `base + 0x17f2614`. The three
words are read with `SafeRead`: the game's own `QueryCollision` never reads `this` (`0x19067a0 mov x0, sp` overwrites it in the prologue), so no dereference of the
game's backs them. An unreadable word, or a null pointer of the chain, is the stock answer (counter `skippedGame`). Nothing is remembered between shots. The request
is a plain load: it is the game's caller's stack frame (`0x2255660 add x1, sp, #0x30`).

Install (`InstallAt`): the legacy `wallfire.off` file is warned about; the switch is reported by the configuration step, one generated `combat_config_switch` record (the table row `wallfire`); off is one Info line
(`combat_wallfire_install` `off_config`); `bvh` installed; the word at the R27 site reads `0x3941b3e8` (`site_word` Error otherwise); the R28 slot holds
`base + 0x18066f4` (`occluded_slot` Error otherwise); then the manager hooks the R26 slot (it must hold `base + 0x1806784`: an unexpected word or a slot outside
the writable segments is refused and nothing is hooked). Lines: `combat_wallfire_install` (Info installed or off, Error refused, Warning for the bvh
dependency), `combat_wallfire_blocked` for each of the first 16 blocked shots and then at each power of two (with the segment), `combat_wallfire_skipped` (Warning,
once). Counters (`GetCounters`): `atSite`, `checked`, `blocked`, `skippedNan`, `skippedGame`. Per call nothing is logged beyond those.

Switch (config.yaml, `combat:`): `wallfire` (default true, as upstream, afk_wallfire.cpp:17,78). The plugin's `wallfire.off` is no longer read; one found is a
Warning (`combat_wallfire_legacy_switch`) that names the key.

**Scope (arena shares the APK).** The check acts only at FireGunLocal's site and only while `bvh::InCombatOrLobby()` (bvh.md section 4). Elsewhere the thunk is the game's
function, a load, a test and a return.

## 3. Evidence

- **R27 word**: the disassembly, `225566c ldrb w8, [sp, #0x6c]  e8b34139` (bytes `e8 b3 41 39` = word `0x3941b3e8`). The call before it:
  `225565c ldr x0, [x19, #0x80]` (gs = the gun component's +0x80), `2255660 add x1, sp, #0x30` (req = sp + 0x30),
  `2255664 mov w2, #0xffffffff`, `2255668 bl 0x01012aa0`; after it `2255670 mov w28, w0` and `225568c cbnz w28, 0x022556b8` (a non-zero result
  jumps to the function's epilogue: no shot). The PLT stub `0x1012aa0: adrp x16,0x37e0000; ldr x17,[x16,#0xc38]; add x16,x16,#0xc38; br x17` is slot 0x37e0c38
  = ELF 0x36e0c38, the readelf line.
- **Request layout (the segment is the first 24 bytes)**: the stores that fill it, the disassembly:
  `225563c str x10, [sp, #0x30]` (floats 0, 1), `2255648 str w11, [sp, #0x38]` (float 2), `225564c stur x9, [sp, #0x3c]` (floats 3, 4),
  `2255650 str w10, [sp, #0x44]` (float 5): six consecutive floats at sp + 0x30 .. 0x47, which is `req[0..5]`. That they are "player position" and
  "muzzle position" is the source's statement (afk_wallfire.cpp:2-4: `ComputeMuzzleXF`), not re-derived; either way the query is
  the same segment the stock physics query was given.
- **QueryCollision returns a bool**: `19067d0 ldr w8,[x19,#0x88]; cmn w8,#1; cset w0, ne` (the request's result word is -1 for "nothing"), so
  the wrapper's `bool` return is the same ABI.
- **R28 chain**: quoted above; ELF 0x17f2614 appears in `R0_readelf_r.txt` as the vtable slot (6 vtables).

## 4. Behaviour

A shot whose player-to-muzzle segment crosses static level geometry is refused, as on the PC. A shot that the stock query already blocked
(a physics body) is unchanged. No new shots are ever fired and no stock block is ever lifted: the wrapper only turns "nothing" into
"blocked". If anything about the build or the game's state is not as pinned, the stock behaviour stands.

## 5. Headset row

On a combat APK built at a tag after this unit (or a development APK repacked with the `combat` variant word, or `combat.force: true`), with `bvh` on: after the bvh
lines, `combat_wallfire_install` `result` `installed`. In a combat match, firing with the hand or muzzle inside or through a wall produces
`combat_wallfire_blocked` lines (`blocked`, `checked`, `segment`) with `checked >= blocked`; firing in the open produces none; the counters `skippedNan` and
`skippedGame` stay 0.

Falsifiers: `site_word` at install (R27's word is not what the pinned build has); `combat_wallfire_skipped` (CncaGame's virtual +0x278 is not
`CncaGame::StaticRaycastOccluded` at run time, or the `gs + 8` chain is null: the module then never blocks); shots fire through a wall while `checked` rises and
`blocked` stays 0 (the tree is missing for that level, or the transformed segment is wrong; check `combat_bvh_tree` and the bvh summary `occluded_blocked`); shots
are refused in the open; a crash in `OnQuery` or the stub; any `combat_wallfire_blocked` while an arena level is loaded.

## 6. Not built, unverified, open questions

- No word patch, no data file. No logged headset window shows a blocked shot.
- **Unverified**: the meaning of the two request points (player position, muzzle) beyond the reference's comment; that `FireGunLocal` is the only function that calls
  `QueryCollision` from this site (the return-address check makes the answer independent of that); that `gs` is a readable
  gamespace at that site (the game's `QueryCollision` never reads it, so the chain is read with `SafeRead`). The request is the caller's stack frame.
- **Differences from the reference**: `GameIsPinned` (above); the `combat:` key instead of a kill file; structured logs; the scope; the call goes to the pinned function by
  address (the reference resolves the same address with `dlsym` and checks it equals `base + 0x18066f4`).
- **The tree must exist**: bvh's Occluded hook answers only for a CBVHTree it built at Initialize. A level whose tree was initialised before the module installed
  takes the game's miss and wallfire never blocks that level.
- Open: `CncaGame::StaticRaycastOccluded` transforms the two points (fused multiply-add sequence `0x18f2740..0x18f2754`) before it calls `CBVHTree::Occluded`; that
  transform is not decoded. The PC runs the same function, so the same segment in level space is expected, and a headset run decides.

## 7. Cost (docs/adr/0038)

Measured on the development host (x86-64, g++ -O2), not on a headset (a tree of 147,000 random triangles; the stock query is a counter):

| Path | Per call |
|---|---|
| a shot at FireGunLocal's site on a combat map (the game's query, the three `SafeRead`s of the game chain, the check, the traversal) | about 3.2 us (1.7 us of it the three reads) |
| QueryCollision from another caller on a combat map (a true scope reads the clock to test its age) | 9.3 ns, against 1.7 ns for the function alone |
| QueryCollision from the site with no combat map or combat lobby loaded (arena) | 4.4 ns, against 1.7 ns for the function alone |

Rates: a shot at the site is a gun shot (a few a second per gun); the rate of QueryCollision calls from other callers, in arena and in combat, is not measured on a
headset. The three `SafeRead`s of the game chain (about 0.5 us each) are the same reads the plugin made, less the request, which is a plain load.

## 8. Tests and mutants

`just test-quest-combat wallfire` runs `wallfire_test`: a stock hit passes through, a clear segment is not blocked, a segment through each wall is, another call
site gets the stock answer, a null gamespace, a non-finite segment and every wrong game structure get the stock answer, the log volume, the refusals and what
they leave alone, the switch, the legacy file, an unreadable gamespace, game or vtable, and the scope (no combat map, no check; an arena level beside a combat map; a lobby arena players use).
Mutants: `src/quest/combat/mutants.d/wallfire.sh`, run with `src/quest/combat/mutants.sh wallfire`.

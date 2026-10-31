# Module `wallfire`: no shot through a wall (rows R26-R28)

Pinned build: libr15 build id b243509c08ce677aeb95fa348016949b3fc45230 (disassembly VA = ELF vaddr + 0x100000).
Source: `<source build>/the source build's wallfire` (read-only, never edited, never pushed).
Code: `src/quest/plugins/q15t/src/wallfire.h`, `src/quest/plugins/q15t/src/wallfire.cpp`. Test: `src/quest/tests/q15t/wallfire_test.cpp` (the real bvh module on a fake slot page, a synthetic room mesh).
**Depends on `bvh`** (`BvhTreeInstalled()`): without the static BVH answering `CBVHTree::Occluded` the segment query is never blocked, so
wallfire refuses to install (`bvh_not_installed`, WARNING).

## 1. Rows

| Row | What | Address | Stock | Action |
|---|---|---|---|---|
| R26 | `CncaGameSpace::QueryCollision(this, SPhQueryCollisionRequest&, CPhId)` | JUMP_SLOT ELF 0x36e0c38 (disassembly 0x37e0c38); function ELF 0x1806784 / disassembly 0x1906784 | 104-byte function (physics bodies only) | slot swap to `QueryWrapper` |
| R27 | `CR15NetGunCS::FireGunLocal`, the return site after `bl QueryCollision@plt` | ELF 0x215566c / disassembly 0x225566c, word `0x3941b3e8` (`ldrb w8, [sp, #0x6c]`) | not written | **read only**: the word is checked at install, and the wrapper acts only when it returns to this address |
| R28 | `CncaGameSpace::StaticRaycastOccluded(this, C3Vector*, C3Vector*)` | JUMP_SLOT ELF 0x36ce380; function ELF 0x18066f4 / disassembly 0x19066f4 | 16-byte virtual thunk | **called**, not swapped: the slot is checked to hold base + 0x18066f4 and the function is called directly |

`readelf -rW` (a source-build note, not in this tree):

    :123419: 00000000036e0c38 R_AARCH64_JUMP_SLOT 0000000001806784 _ZNK10NRadEngine13CncaGameSpace14QueryCollisionERNS_24SPhQueryCollisionRequestENS_5CPhIdE + 0
    :113924: 00000000036ce380 R_AARCH64_JUMP_SLOT 00000000018066f4 _ZNK10NRadEngine13CncaGameSpace21StaticRaycastOccludedERKNS_8C3VectorES3_ + 0

**Needs a word patch: none.** R26 is a data-page GOT swap; R27 is a read of a code word, R28 a call.

## 2. What the source does, and the mechanism here

PC and Quest run the same `CR15NetGunCS::FireGunLocal` (source the source build's wallfire:1-4). Before a shot it builds an
`SPhQueryCollisionRequest` on its stack for the segment player position -> muzzle position, calls `QueryCollision` and, on a hit, returns
without firing. On the PC the query sees the static level; on the Quest it reaches physics bodies only (the static level lives in
CBVHTree, compiled out on Android and served by `bvh`), so a hand or muzzle through a wall fires (the source build's wallfire:1-10).

`QueryWrapper(gs, req, id)`:
1. reads its return address (the slot's PLT stub is `adrp; ldr; add; br`, so it is the caller's), runs the stock `QueryCollision`;
2. returns the stock answer unless **all** hold: the return address is `base + 0x215566c`, the stock answer was "nothing", `gs != 0`;
3. reads the 24 bytes at `req` (`SafeRead`) as six floats, the segment start xyz and end xyz; any non-finite value or an unreadable
   request: the stock answer (counter `skipped_nan`);
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

The extra check (a hardening the source does not have, `GameIsPinned`): `SafeRead(gs + 8)` gives the CncaGame, `SafeRead(game)` its vtable
pointer, and `[vtable + 0x278]` must equal `base + 0x17f2614`; each read is a `SafeRead` (a null pointer fails the next one). Three reads
per shot attempt, nothing remembered. Without it a gamespace the stub cannot walk would fault inside the game.

Install (`InstallWallfireAt`): `wallfire.off` kill file (reason `off_file`); `bvh` installed; the word at the R27 site reads
`0x3941b3e8` (`site_word` ERROR otherwise); the R28 slot holds `base + 0x18066f4` (`occluded_slot` ERROR otherwise); then
`InstallImportHook` on the R26 slot with `expected_vaddr = 0x1806784` (an unexpected word, an executable page: refused, nothing swapped).
Logs: `wallfire on: ...` (INFO), `wallfire refused: nothing changed` / `wallfire hook refused` (ERROR; the bvh dependency is WARNING),
`wallfire blocked a shot` for each of the first 16 blocked shots and then at each power of two (with the segment), `wallfire skipped a shot check`
(WARNING, once). Counters (`WallfireGetCounters`): `at_site`, `checked`, `blocked`, `skipped_nan`, `skipped_game`. Per call nothing is logged beyond those.

## 3. Evidence

- **R27 word**: the disassembly, `225566c ldrb w8, [sp, #0x6c]  e8b34139` (bytes `e8 b3 41 39` = word `0x3941b3e8`). The call before it:
  `225565c ldr x0, [x19, #0x80]` (gs = the gun component's +0x80), `2255660 add x1, sp, #0x30` (req = sp + 0x30),
  `2255664 mov w2, #0xffffffff`, `2255668 bl 0x01012aa0`; after it `2255670 mov w28, w0` and `225568c cbnz w28, 0x022556b8` (a non-zero result
  jumps to the function's epilogue: no shot). The PLT stub `0x1012aa0: adrp x16,0x37e0000; ldr x17,[x16,#0xc38]; add x16,x16,#0xc38; br x17` is slot 0x37e0c38
  = ELF 0x36e0c38, the readelf line.
- **Request layout (the segment is the first 24 bytes)**: the stores that fill it, the disassembly:
  `225563c str x10, [sp, #0x30]` (floats 0, 1), `2255648 str w11, [sp, #0x38]` (float 2), `225564c stur x9, [sp, #0x3c]` (floats 3, 4),
  `2255650 str w10, [sp, #0x44]` (float 5): six consecutive floats at sp + 0x30 .. 0x47, which is `req[0..5]`. That they are "player position" and
  "muzzle position" is the source's statement (the source build's wallfire:2-4: `ComputeMuzzleXF`), not re-derived; either way the query is
  the same segment the stock physics query was given.
- **QueryCollision returns a bool**: `19067d0 ldr w8,[x19,#0x88]; cmn w8,#1; cset w0, ne` (the request's result word is -1 for "nothing"), so
  the wrapper's `bool` return is the same ABI.
- **R28 chain**: quoted above; ELF 0x17f2614 appears in `R0_readelf_r.txt` as the vtable slot (6 vtables).

## 4. Behaviour

A shot whose player-to-muzzle segment crosses static level geometry is refused, as on the PC. A shot that the stock query already blocked
(a physics body) is unchanged. No new shots are ever fired and no stock block is ever lifted: the wrapper only turns "nothing" into
"blocked". If anything about the build or the game's state is not as pinned, the stock behaviour stands.

## 5. Prediction (written before any headset run)

With `bvh` and `wallfire` on, in the plugin log: `wallfire on: ...` (INFO) after the bvh lines. In a combat level, firing with the hand or
muzzle inside or through a wall produces `wallfire blocked a shot blocked=1 checked=N segment=x y z -> x y z` lines (the first 16 shots, then
at 32, 64...) with `checked >= blocked`; firing in the open produces none; `skipped_nan` and `skipped_game` stay 0.

Falsifiers:
- `wallfire site word mismatch` / `site_word` at install: R27's word is not what the pinned build has (not this build).
- `wallfire skipped a shot check` appears: `CncaGame`'s virtual +0x278 is not `CncaGame::StaticRaycastOccluded` at runtime (another class), or the
  `gs + 8` chain is unreadable: the module then never blocks, and the layout must be re-read.
- Shots fire through a wall while `checked` rises and `blocked` stays 0: the static query answers "clear" (the bvh tree is missing for that
  level, or the transformed segment is wrong); check `bvh tree built` for the level and `bvh` summary `occluded_blocked`.
- Shots are refused in the open (`blocked` rises with no wall in the segment): the segment or the tree is wrong; turn it off with `wallfire.off`.
- A crash inside `QueryWrapper` or the stub (should not happen: every game read is `SafeRead` and the call target is the pinned function).
- A shot refused when the stock query would have fired on the PC for the same geometry (a PC-vs-Quest difference in the level data).

## 6. Not built, unverified, open questions

- Nothing is blocked; no word patch; no data file.
- **Unverified**: the meaning of the two request points (player position, muzzle) beyond the source's comment; that `FireGunLocal` is the only
  function that calls `QueryCollision` from this site (the return-address check makes the answer independent of that).
- **Deviation from the source**: `GameIsPinned` (above); `SafeRead` for the segment; kill file instead of a `setprop`; structured logs; the call goes
  to the pinned function by address (the source resolves the same address with `dlsym` and checks it equals `base + 0x18066f4`).
- **The tree must exist**: `bvh`'s R23 wrapper answers only for a CBVHTree it built at Initialize. If a level's tree was initialised before the plugin
  installed (not the case when the plugin loads at start), `Occluded` takes the stock miss and wallfire never blocks that level.
- Open: `CncaGame::StaticRaycastOccluded` transforms the two points (fused multiply-add sequence `0x18f2740..0x18f2754`, the disassembly) before
  it calls `CBVHTree::Occluded`; I did not decode that transform. The PC runs the same function, so I expect the same segment in level
  space, and a headset run (the falsifier "shots refused in the open" / "fire through a wall") decides.

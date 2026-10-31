# Module `bvh`: a real static raycast tree for CBVHTree (rows R20-R25)

Pinned build: libr15 build id b243509c08ce677aeb95fa348016949b3fc45230 (disassembly VA = ELF vaddr + 0x100000).
Source: `<source build>/the source build's bvh` and the source build's bvh_core (read-only, never edited, never pushed).
Code: `src/quest/plugins/q15t/src/bvh_core.h` (pure traversal and header parse), `src/quest/plugins/q15t/src/bvh.h`, `src/quest/plugins/q15t/src/bvh.cpp` (GOT wrappers). Tests: `src/quest/tests/q15t/bvh_core_test.cpp`
(synthetic meshes), `src/quest/tests/q15t/bvh_test.cpp` (the real installer on fake slot pages), `src/quest/tests/q15t/bvh_build.h` (test-only tree builder).
Depends on nothing. `wallfire` depends on this module.

## 1. Rows

| Row | What | JUMP_SLOT (ELF) | Stock function (ELF / disassembly) | Replaced by |
|---|---|---|---|---|
| R20 | `CBVHTree::Initialize(this, SBVHTreeData*)` | 0x36ec548 | 0x1a45a88 / 0x1b45a88, a bare `ret` (`c0035fd6`) | `InitializeWrapper`: stock, then parse and keep a `Tree*` in the 8-byte CBVHTree |
| R21 | `CBVHTree::Shutdown(this)` | 0x36eb648 | 0x1a45a8c / 0x1b45a8c, a bare `ret` | `ShutdownWrapper`: stock, then clear the word |
| R22 | `CBVHTree::Intersect(this, CRay*, SBVHHit*)` | 0x36d38f0 | 0x1a45ae4 / 0x1b45ae4, logs NotImplemented once, returns 0 | `IntersectWrapper`: nearest hit |
| R23 | `CBVHTree::Occluded(this, C3Vector*, C3Vector*)` | 0x36e8430 | 0x1a45b38 / 0x1b45b38, the same stub | `OccludedWrapper`: any hit on the segment |
| R24 | `CncaGameSpace::StaticRaycast` (the callers' gate) | 0x36c0288 | 0x18066e4 / 0x19066e4, a 4-word thunk | `GateWrapper`: marks the casts the tree answers |
| R25 | `CR15TouchInteractQuery::RayQueryStatic` | 0x36f37d8 | 0x20cf414 / 0x21cf414, 444 bytes | `TouchWrapper`: returns false, as the stock Quest's stub tree makes it |

All six slots are checked against `readelf -rW` (a source-build note, not in this tree):

    :129341: 00000000036ec548 R_AARCH64_JUMP_SLOT 0000000001a45a88 _ZN10NRadEngine8CBVHTree10InitializeERKNS_12SBVHTreeDataE + 0
    :128861: 00000000036eb648 R_AARCH64_JUMP_SLOT 0000000001a45a8c _ZN10NRadEngine8CBVHTree8ShutdownEv + 0
    :116658: 00000000036d38f0 R_AARCH64_JUMP_SLOT 0000000001a45ae4 _ZNK10NRadEngine8CBVHTree9IntersectERKNS_4CRayERNS_7SBVHHitE + 0
    :127258: 00000000036e8430 R_AARCH64_JUMP_SLOT 0000000001a45b38 _ZNK10NRadEngine8CBVHTree8OccludedERKNS_8C3VectorES3_ + 0
    :106725: 00000000036c0288 R_AARCH64_JUMP_SLOT 00000000018066e4 _ZNK10NRadEngine13CncaGameSpace13StaticRaycastERKNS_4CRayERNS_7SBVHHitERPNS_15CncaGameSpaceMTE + 0
    :133007: 00000000036f37d8 R_AARCH64_JUMP_SLOT 00000000020cf414 _ZN10NRadEngine8NRadGame22CR15TouchInteractQuery14RayQueryStaticEPKNS_15CncaGameSpaceMTERKNS_4CRayERNS0_8SMeshHitERf + 0

**Needs a word patch: none.** Every row is a data-page GOT swap (`got_swap.h`, the slot must hold base + the stock function; an
executable page is refused). No code word is written, so nothing needs `execmod`.

## 2. What the source does, and the mechanism here

The Quest build of libr15 has CBVHTree compiled out (R20/R21 are a bare `ret`, R22/R23 log "NotImplemented_(Embree)" once and
return 0; the disassembly, quoted in section 3), so every static raycast misses and every line of sight is clear. The levels still ship
their CBVHResource, so the source (the source build's bvh:1-12) implements the tree on that data: it swaps the four CBVHTree slots, parses
the resource's `SBVHTreeData` at Initialize and keeps a pointer in the 8-byte CBVHTree object (the source build's bvh:12-15).

Where the triangle data comes from: **nothing is stored in the plugin.** The game's loader attaches the CBVHResource in its own
memory and passes its `SBVHTreeData` to Initialize; the plugin reads the two memory blocks that header points at (triangles and
nodes) in place, at run time, and traverses them. No game-derived byte is written to a file, embedded in the plugin or copied
(the tree table keeps only the header's two pointers and sizes, never the geometry).

Per row:
- **R20** `InitializeWrapper(self, data)`: calls the stock function (kept for chaining), zeroes the 8-byte word, then `BuildTree`:
  `SafeRead` the 0x50-byte header; `bvh::Parse` (block sizes are multiples of 0xe0 / 0x40, bounded, non-zero, addresses do not wrap
  or sit in the null page; a block pointer of 0 means the block follows the header, as in the resource file); probe that every page
  of both blocks reads (`Readable`); take an entry of a 64-entry static table (the same data address reuses its entry; the 65th
  distinct tree is refused with an ERROR line) and store `&entry.tree` in the CBVHTree word. Refusals leave the word 0, so the
  stock miss stands, and log why (`bvh tree none` for no geometry, `bvh tree refused` for a layout or memory doubt).
- **R21** `ShutdownWrapper`: stock, then the word is cleared. A later cast finds no tree and takes the stock miss.
- **R22** `IntersectWrapper`: gate first (section 4); then `TreeOf(self)` reads the CBVHTree word with `SafeRead` and accepts it only
  if it is one of the table's entries (aligned, inside the table); the ray (6 floats) is read with `SafeRead`; `bvh::Trace`, nearest
  hit, `t in (0, inf)` in units of the direction (Embree 1.0); on a hit, writes SBVHHit `id0 +0`, `id1 +4`, `u +8`, `v +0xc`, `t +0x10`
  and the unit normal at `+0x38` and nothing else (the source build's bvh:19-22, `:178-179`); a miss writes nothing. Any doubt (no tree,
  foreign word, unreadable argument) calls the stock function.
- **R23** `OccludedWrapper`: the segment a..b is the ray `a + t (b - a)`, `t in (0, 1)`, first hit found. Not gated (the source build's bvh:56-57).
- **R24** `GateWrapper`: see section 4.
- **R25** `TouchWrapper`: returns false and writes nothing, as the stock Quest's stub tree makes `RayQueryStatic` behave (hit flag is
  Intersect's return; the source build's bvh:26-37). Without it a hand's touch ray (origin + dir x length, no `t <= 1` check by its reader)
  would hit a surface metres past the hand and the grab would pin the hand there. the disassembly shows `RayQueryStatic` itself calls the
  R24 stub (`0x21cf454 bl 0x00fd1740`, then `cbz w0, 0x021cf5c0`), so with the gate on its cast is already a miss (its return site
  is not in the allowed list); R25 matters when the gate is off.

Install (`InstallBvhAt`): kill files first (`bvh.off` installs nothing, reason `off_file`); then **all or none**: every one of the
4 (5 with the gate) slots is read and must hold base + its pinned function before any is swapped (reason `slot_mismatch`,
ERROR `bvh refused: nothing changed`); the gate goes in first, then Initialize, Shutdown, Intersect, Occluded, with a rollback of
the ones already swapped if one fails (e.g. an executable page); R25 last, and its refusal is not fatal. One structured line per
hook (`bvh hook installed` / `bvh hook refused`, with slot and real vaddr), one `bvh on: ...` line, and a 10 s summary
(`BvhTick`, only when a counter moved) with the counters: casts answered, hits, occluded, blocked, gated off, touch kept off,
calls that went to the stock function, microseconds per ray. Per call nothing is logged: counters only, plus one line for the
first hit and the first blocked segment.

Option files in the plugin's data directory (same directory as `weapon_arm`): `bvh.off`, `bvhgate.off` (every cast uses the tree:
the source's `debug.<app>.bvhgate 0`), `bvhtouch.keep` (R25 not swapped). Read at install.

## 3. Evidence: the structures read, each offset quoted and checked

Source file:line is the source build's; disassembly is of the Quest libr15 unless it says PC (echovr.exe, where the
engine's Embree traversal still exists and shows the layout). "Unverified" means not used by the code.

**SBVHTreeData (0x50 bytes)**: triangles `CMemBlock {ptr +0x00, size +0x08}`, nodes `CMemBlock {ptr +0x20, size +0x28}`, u32 root
reference `+0x40`.
- Source: the source build's bvh_core:6; root `+0x40` is *not* in the source (it assumes node 0, the source build's bvh_core:14; deviation 1 below).
- the disassembly, `NRadEngine::SBVHTreeData::Reset` (VA 0x1b54428, size 48): `str x30...; mov x1, xzr; mov w2, wzr; mov x19, x0; bl 0x0103e400` (CMemBlock reset at +0), `add x0, x19, #0x20 ... bl 0x0103e400`
  (CMemBlock reset at +0x20), `0x1b5444c str wzr, [x19, #0x40]` (u32 at +0x40). Two CMemBlocks at +0 and +0x20 and a u32 at +0x40: confirmed.
- the PC disassembly, the tree setup `FUN_14070f6e0(param_1, param_2)`: `uVar1 = *(undefined4 *)(param_2 + 0x40)`;
  `FUN_1400da1c0(param_2 + 0x20)` / `FUN_1400d5d30(param_2 + 0x20)` and the same on `param_2` (block pointer and size getters of the
  two CMemBlocks), the u32 stored to the traverser at `lVar2 + 0x20`. Root reference at +0x40: confirmed.
- CMemBlock `{ptr +0, size +8}`: the traversal reads the block pointer as the first qword (`*(longlong *)(... + 0x30)` of the
  traverser, set from `FUN_1400da1c0`) and the size as the second; the size/pointer order inside a CMemBlock is **not read
  directly** (no CMemBlock accessor was decompiled); it is consistent with the source (the source build's bvh_core:6) and with `Parse`
  failing safe: a wrong order makes sizes absurd, which `Parse` refuses (`block sizes`/`block address`) and the first headset
  line says so. Treated as source-asserted, checked only by that failure mode.
- Who passes it: `CStaticRaycastCS::Initialize` (VA 0x18121a8): `0x1812218 add x0, x19, #0x148` (this = the component system + 0x148),
  `0x181221c ldr x1, [x8, #0x70]` (the resource's data), `0x1812220 bl 0x01029cc0` (the Initialize PLT stub). `Shutdown`
  (0x1812748): `add x0, x0, #0x148; bl 0x01027ec0`. So **the CBVHTree is the 8-byte object at CStaticRaycastCS+0x148**, and the
  data pointer is the second argument.

**Triangle4 (0xe0 bytes)** `v0, e1, e2, Ng` as `x[4] y[4] z[4]` (48 floats), then `id0[4]`, `id1[4]`; an empty lane has `id0 = -1`.
- Source: the source build's bvh_core:9 and `static_assert(sizeof(Tri4) == 0xe0)` `:32`.
- the PC disassembly, Embree traversal `FUN_14135f9a0` (decompilation): the stride `lVar6 = uVar7 * 0xe0;` and `lVar6 = lVar6 + 0xe0;` per
  record; v0 read as `pfVar11[0..11]` (`fVar42 = *pfVar11 - fVar46; ... pfVar11[0xb] - fVar24`), e2 as `pfVar11[0x18..0x23]`,
  Ng as `pfVar11[0x24..0x2f]` (`pfVar11[0x28] * fVar13 + fVar41 * pfVar11[0x24] + pfVar11[0x2c] * fVar12`). That leaves
  `[0x0c..0x17]` for e1 and `[0x30..0x37]` (8 words) for the two id arrays: 0xe0 = 4 x 48 + 8 x 4 bytes. Confirmed for the stride and for
  v0, e2, Ng; e1 and the ids by elimination plus the source (the id position is also what the source's tests use).
  The source's definitions `v1 = v0 - e1`, `v2 = v0 + e2`, `Ng = e1 x e2` (the source build's bvh_core:10) are the Embree 1.0 Triangle4 form and agree with the
  traversal's use of Ng; **unverified** beyond that.

**Node (0x40 bytes)**: `x[4] y[4] z[4] = {left.lo, right.lo, left.hi, right.hi}`, `child[2]`, `pad[2]`; `child >= 0`: inner node at
byte offset `child * 8` (index `child >> 3`); `child < 0`: leaf, first record `(child & 0x7fffffff) >> 5`, count `child & 31`.
- Source: the source build's bvh_core:12-14, `:31`, leaf decode `:142`.
- the PC disassembly `FUN_14135f9a0`: `while ((int)uVar10 < 0) { uVar7 = (ulonglong)(longlong)(int)(uVar10 ^ 0x80000000) >> 5;
  uVar1 = ((uVar10 ^ 0x80000000) & 0x1f) + uVar7;`: the sign bit marks a leaf, `>> 5` is the first record, `& 0x1f` the count. Confirmed.
  The 0x40 node size, the lane order of the bounds and the `child * 8` byte offset are the source's and the Embree 1.0 BVH2 layout;
  the sizes are enforced by `Parse` (multiples of 0x40) and a wrong stride would mis-read every tree, which the first-hit line shows.
  **Partly unverified** (not read from the decompilation's node loads).

**CBVHTree = 8 bytes**: the source build's bvh:12; the object at CStaticRaycastCS+0x148 is the first (only) member of the engine's own
struct and the PC tree is a pointer to a traverser (`FUN_14070f6e0`: `*(undefined8 **)*param_1 = puVar8;`, `*param_1` is that pointer).

**Stock stubs (R20-R23), the disassembly**:

    1b45a88 ret                        c0035fd6        (Initialize, size 4)
    1b45a8c ret                        c0035fd6        (Shutdown, size 4)
    1b45ae4 str x30,[sp,#-0x10]!  ... adrp x8,0x389d000 ; ldrb w9,[x8,#0xa8c] ; tbnz w9,#0,0x01b45b2c ; ... mov w5,#0x87 ... bl 0x0104b960 ... mov w0,wzr ; ret
    1b45b38 ... ldrb w9,[x8,#0xa90] ... mov w5,#0xb9 ... bl 0x0104b960 ... mov w0,wzr ; ret

**CncaGameSpace::StaticRaycast thunk (R24), the disassembly**:

    19066e4 ldr x0,[x0,#8]    000440f9
    19066e8 ldr x8,[x0]       080040f9
    19066ec ldr x4,[x8,#0x270] 043941f9
    19066f0 br  x4            80001fd6

i.e. `CncaGame::StaticRaycast` through the vtable. The PLT stub is `0xfd1740: adrp x16,0x37c0000; ldr x17,[x16,#0x288]; add x16,x16,#0x288;
br x17`: slot 0x37c0288 = ELF 0x36c0288, matching the readelf line. The stub ends in `br`, so the wrapper's `__builtin_return_address`
is the caller's (this is what the gate relies on). The same was read for R23 (`0x1021a90 ... ldr x17,[x16,#0x430]` = slot 0x37e8430) and for
wallfire's R26 (`0x1012aa0`). The slot-to-stub link of R20, R21, R22 and R25 was not read (readelf only).

**The three allowed call sites** (the source build's bvh:103-105): return addresses ELF 0x12b2b4c, 0x12b2bc4, 0x12c0a54 (disassembly 0x13b2b4c, 0x13b2bc4,
0x13c0a54). the disassembly at the instruction before each:

    13b2b48 bl 0x00fd1740   fe7af097   in DualStaticRaycast@13b2b28
    13b2bc0 bl 0x00fd1740   e07af097   in DualStaticRaycast@13b2b28
    13c0a50 bl 0x00fd1740   3c43f097   in Update@13c096c

all three are `bl` to the R24 PLT stub, in `CR15NetRaycastUtils::DualStaticRaycast` (twice) and `SR15PatrolSM::Update` as the source names them.
The names come from the disassembly's function table; the source's comments for why these three (bullets, AI patrol) are its own analysis
(the source build's bvh:38-57), not re-derived here.

**SBVHHit (0x44 bytes)**: `id0 +0`, `id1 +4`, `u +8`, `v +0xc`, `t +0x10`, unit normal `+0x38`; `+0x14..+0x37` left as the caller set it.
Source the source build's bvh:19-22, `:178-179`. **Not independently verified in the disassembly** (the readers of `+0x38` named in the source were
not decompiled here); the writes are limited to these fields, and a wrong offset shows as a wrong normal, not a crash
(all inside the 0x44 bytes the callers zero).

## 4. The callers' gate (R24)

The tree answers only the casts it was added for (bullets, AI patrol); every other cast misses as on the stock Quest
(the source build's bvh:38-57: other callers take any hit as "within the segment" with `tmax = inf`, which grabs, hand reaches, cutters and
auto-target misread). Mechanism (`GateWrapper`): the wrapper reads its return address; if `ret - base` is one of the three allowed
values it sets a `thread_local` mark, calls the real `StaticRaycast`, and restores the mark that was there before. `IntersectWrapper`
answers from the tree only when the mark is set (gate on); any other Intersect is passed to the stock function (a miss).
Properties the tests check: a cast from an unlisted site never uses the tree; the mark does not leak past the cast; a nested unlisted
cast inside an allowed one is gated and the outer one still answers afterwards; a different thread is not marked; a direct
`CBVHTree::Intersect` with no marked cast is gated; Occluded is not gated.
The gate is installed first: until it is in no cast is marked, so a tree built a moment later answers none.

## 5. Behaviour

- Loading a level: `bvh tree built data=0x... triangle4=N nodes=M root_ref=0x...` per CBVHResource (INFO). A level without static
  geometry: `bvh tree none why=empty` (INFO). A header or block that cannot be read, or has bad sizes: `bvh tree refused ...` (WARNING).
  The 65th tree: ERROR `tree table full`. In every refusal the stock miss stands.
- In play: bullets (DualStaticRaycast forward/backward) and the AI patrol cast hit walls; damage, heal-bubble and sound lines of sight
  (Occluded) are blocked by walls. Grab/touch is unchanged from the stock Quest (physics bodies only).
- Off: `bvh.off`; no tree, gate or touch swap at all.

## 6. Deviations from the source (all deliberate; each is covered by a test)

1. **Root reference from the header (+0x40)**, not "node 0" (the source build's bvh_core:14). The PC builds its traverser from `param_2 + 0x40`
   (section 3). For a tree whose root is node 0 the result is identical; for any other it is the correct one. Logged as `root_ref`.
2. **A cycle budget**: the traversal stops after `2 * nodes + 2` pops and answers "miss". A well-formed tree pops each node and leaf
   once, so this never triggers on real data; a malformed tree with a loop would otherwise spin. Also: stack depth over 128, an index
   outside the node or triangle block, a leaf past the triangle block: "miss", never an out-of-bounds read.
3. **Non-finite or zero-direction rays hit nothing** (`Ray::ok`). The source walks the whole tree for them.
4. **Every game-memory read is `SafeRead`**: the header, both blocks (every page probed at Initialize), the CBVHTree word, the
   ray and segment arguments. A fault becomes the stock miss and a counter, never a crash.
5. **The CBVHTree word must be one of our 64 table entries**; any other value takes the stock path. The table is static memory with
   no constructors; a re-Initialize on the same data address reuses its entry.
6. **Kill files replace `setprop`** (no system properties on this build): `bvh.off`, `bvhgate.off`, `bvhtouch.keep`.
7. **All-or-none install with rollback**, a pre-check that every slot holds the pinned function before anything is swapped, and the
   gate installed first. The source swaps slot by slot.
8. **No `1e-20` clamp on a zero direction component**: IEEE division by zero gives +-inf and the slab test is exact for it (an origin on
   the slab's plane compares as NaN, which counts as inside). The host test covers axis-parallel rays and rays in a slab plane.
9. **Structured logging** (JSON through the plugin log) instead of the source build's text log; counters only on the per-call path.

## 7. Prediction (written before any headset run)

On the first headset run with this module on (gate on), in the plugin log, in this order:
1. `bvh hook installed` x5 (gate, Initialize, Shutdown, Intersect, Occluded) plus the touch hook, then `bvh on: ... gate=on allowed_casts=3`.
2. At level load, one `bvh tree built` per CBVHResource with `triangle4` and `nodes` both non-zero (I have not seen real numbers for this build), `root_ref=0x0` or another small multiple of 8.
3. Within the first shots in a combat level: `bvh first hit` with `mesh` >= 0, `tri` >= 0, `t` positive and of the order of metres, a
   `normal` of unit length; then 10 s summaries with `intersect` > 0, `intersect_hit` > 0, `gated_off` > 0 (physics-style casts from unlisted
   callers), `stock` = 0 or tiny.
4. Shots at a wall do not pass through it; damage through a wall is blocked (`occluded_blocked` > 0 in the summary).

Falsifiers (any one means the module or my reading is wrong, and the module should be turned off with `bvh.off`):
- No `bvh tree built` after a level load: Initialize is not called with the data pointer as its second argument, or the table hook never ran
  (check `bvh hook installed` first).
- `bvh tree refused` with `why=block sizes` / `block address` / `block unreadable` on every level: the SBVHTreeData layout (section 3) is
  wrong for this build, most likely the CMemBlock pointer/size order.
- `bvh first hit` never appears while `intersect` counts rise: the tree is built but every ray misses (wrong Node or Triangle4 layout); or
  `gated_off` is huge and `intersect` is 0 (the three allowed return addresses do not match this build).
- `bvh first hit` with `t` negative, NaN, or `mesh`/`tri` = -1.
- A crash in `IntersectWrapper`/`OccludedWrapper` (should be impossible: reads are bounds-checked and `SafeRead`).
- Grab or touch behaves differently from stock (R25 must keep it a miss).
- Frame time: `us_per_ray_avg` above a few microseconds or `us_per_ray_max` above ~0.5 ms.

## 8. Not built, unverified, open questions

- Nothing is blocked: no stage needs a word patch or a data file.
- **Unverified** (not used to make a decision): CMemBlock pointer/size order, Triangle4 e1 and id array positions, Node bound lane order and `child * 8`
  (all fail safe through `Parse` and the traversal's bounds checks), the SBVHHit offsets beyond `+0x38` normal and `+0x10` t, the reasons for the
  three allowed sites (source analysis).
- **R1 note correction**: a source-build note (not in this tree) says a `0xffd440` this-adjust thunk is a StaticRaycast thunk; disassembly
  shows 0xffd440 is a PLT stub (`adrp x16, 0x37d6000; ldr x17,[x16,#0x108]`), not a StaticRaycast thunk. No code depends on it.
- PC vs source differences: the PC's traverser is Embree's own packet code with SIMD (the PC disassembly `FUN_14135f9a0`); this module is the scalar port of
  the source's version. Hit order among coplanar triangles at the same `t` may differ from the PC (the first found wins here).
- A tree built before the plugin installs (an Initialize that ran earlier) has no table entry; the install is at plugin init, before any level loads.

## 9. Tests and mutants

`Q15T_MODULE=bvh bash src/quest/tests/q15t/run_host_tests.sh` runs `bvh_core_test` (the pure core on synthetic meshes: header parse and limits, ray validity,
hits and misses with exact `t`/`u`/`v`/normal, open range ends, a room at several leaf sizes, a random soup against two brute-force references,
degenerate and malformed trees, the stack and cycle limits, empty children, a node reference past the block) and `bvh_test` (the real
installer on fake slot pages; the wrappers called through their slots; the gate from two call sites and two threads; refusals and rollback;
the 64-entry table; unreadable and half-mapped blocks; option files; the 10 s summary; the pinned constants against the readelf lines).
Mutants: `src/quest/tests/q15t/mutants.d/bvh.sh` (rules of `bvh_core.h`) and `src/quest/tests/q15t/mutants.d/bvhwrap.sh` (rules of `bvh.cpp`, `bvh.h`), run as
`Q15T_MUTANTS_ONLY=bvh` and `Q15T_MUTANTS_ONLY=bvhwrap` (one list is too long for a 10 minute tool call). Rules that are redundant by
construction and so cannot be told apart by any test are named in the comment at the end of `bvh.sh` and not mutated.
Per-call cost: `Trace` uses two 128-entry stack arrays and no allocation; the tests use under 2 GiB.

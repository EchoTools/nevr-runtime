# Module `bvh`: a real static raycast tree for CBVHTree (hooks R20-R25 of the reference build)

Pinned build: libr15 build id b243509c08ce677aeb95fa348016949b3fc45230 (disassembly VA = ELF vaddr + 0x100000).
Origin: the reference build's runtime/native/afkxr/afk_bvh.cpp (slot table :93-99, allowed call sites :103-110, the gate :38-57, the default :121,365-367)
and afk_bvh_core.h (the format); the commit is in `docs/audits/2026-11-05-q15t-origin-citations.md`. The module is Ducky's (kotorvr's q15t-upstream): every
address below is in upstream's afk_bvh.cpp.
Code: `src/quest/combat/bvh_core.h` (pure traversal and header parse), `src/quest/combat/bvh.h`, `bvh.cpp` (the handlers and the install), `bvh_targets.h`
(the pins and the thunks). Tests: `src/quest/tests/bvh_core_test.cpp` (synthetic meshes), `src/quest/tests/bvh_test.cpp` (the real thunks and manager on an image
built in memory), `src/quest/tests/bvh_build.h` (test-only tree builder), `src/quest/tests/got_pinned_test.cpp` (the pins against the real libr15).
Depends on nothing. `wallfire` (`wallfire.md`) depends on this module.

## 1. Hooks

| Hook | What | JUMP_SLOT (ELF) | Stock function (ELF / disassembly) | Handler (`bvh.cpp`) |
|---|---|---|---|---|
| Initialize | `CBVHTree::Initialize(this, SBVHTreeData*)` | 0x36ec548 | 0x1a45a88 / 0x1b45a88, a bare `ret` (`c0035fd6`) | `OnInitialize`: the game's function, then parse and keep a `Tree*` in the 8-byte CBVHTree |
| Shutdown | `CBVHTree::Shutdown(this)` | 0x36eb648 | 0x1a45a8c / 0x1b45a8c, a bare `ret` | `OnShutdown`: the game's function, then clear the word |
| Intersect | `CBVHTree::Intersect(this, CRay*, SBVHHit*)` | 0x36d38f0 | 0x1a45ae4 / 0x1b45ae4, logs NotImplemented once, returns 0 | `OnIntersect`: nearest hit |
| Occluded | `CBVHTree::Occluded(this, C3Vector*, C3Vector*)` | 0x36e8430 | 0x1a45b38 / 0x1b45b38, the same stub | `OnOccluded`: any hit on the segment |
| Gate | `CncaGameSpace::StaticRaycast` (the callers' gate) | 0x36c0288 | 0x18066e4 / 0x19066e4, a 4-word thunk | `OnGate` (a `kCaller` thunk): marks the casts the tree answers |
| Touch | `CR15TouchInteractQuery::RayQueryStatic` | 0x36f37d8 | 0x20cf414 / 0x21cf414, 444 bytes | `OnTouch`: returns false, as the stock Quest's stub tree makes it |

All six slots are JUMP_SLOT relocations of the stock libr15 (sha256 `8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20`), each naming the symbol
above and defined at the stock function above (`readelf -rW` and `nm -D`; `got_pinned_test` checks the same against the store APK).

**Needs a word patch: none.** Every hook is a data-page GOT hook through the hook manager (`InstallThunk`, `bvh_targets.h`); no code word is written.

## 2. What it does, and the mechanism

The Quest build of libr15 has CBVHTree compiled out (Initialize and Shutdown are a bare `ret`, Intersect and Occluded log "NotImplemented_(Embree)" once and
return 0; the disassembly, quoted in section 3), so every static raycast misses and every line of sight is clear. The levels still ship
their CBVHResource, so the module implements the tree on that data: it hooks the four CBVHTree slots, parses the resource's `SBVHTreeData` at Initialize and
keeps a pointer in the 8-byte CBVHTree object.

Where the triangle data comes from: **nothing is stored.** The game's loader attaches the CBVHResource in its own memory and passes its `SBVHTreeData` to
Initialize; the module reads the two memory blocks that header points at (triangles and nodes) in place, at run time, and traverses them. No game-derived
byte is written to a file, embedded or copied (the tree table keeps only the header's two pointers and sizes, never the geometry).

Per hook:
- **Initialize** `OnInitialize(original, self, data)`: calls the game's function, zeroes the 8-byte word, then, when the level being loaded is a combat map or the combat lobby (see Scope below), `BuildTree`: `SafeRead` the 0x50-byte header;
  `Parse` (block sizes are multiples of 0xe0 / 0x40, bounded, non-zero, addresses do not wrap or sit in the null page; a block pointer of 0 means the block
  follows the header, as in the resource file); probe that every page of both blocks reads (`Readable`); take an entry of a 64-entry static table (the same
  data address reuses its entry; the 65th distinct tree is refused with an Error line) and store `&entry.tree` in the CBVHTree word. Refusals leave the word 0,
  so the stock miss stands, and log why (`combat_bvh_tree` `none` for no geometry, `refused` for a layout or memory doubt). Every `combat_bvh_tree` record names the
  level the scan found (`level`) and its class (`scope`: `combat`, `arena`, `other`, or `unknown` before the tick has shown the module a game, when it builds). A
  level that is not a combat map or the combat lobby is `skipped` (Info): the build's page probes (about a millisecond for a level with geometry on the host)
  are not paid for it, and its CBVHTree keeps the word 0. The scan reads the gamespaces still loading as well (`inFlight`), because Initialize runs while the
  level's resources are built, before the level is in the loaded tables; this is the same source the table-step modules use. That the loading level is
  already stored at gamespace `+0x58` when Initialize runs is read from the store libr15 (disassembly VAs): `CncaGameSpace::LoadLevel` (0x18e5484) stores it
  first (`str x22, [x19, #0x58]` at 0x18e54ec, `combat_level_targets.h`), and later in the same function calls `CncaGameSpace::CreateComponentSystems`
  (`bl 0x1050d40` at 0x18e5b6c, the thunk of 0x1900f0c; its one call site in libr15). That calls `FinalizeAfterLoad` (0x19017b4, thunk 0xfd5fe0, to
  0x18fde70), which calls `InitializeComponentSystems` (0x18fdec8, to 0x18fdf68), which calls each init callback registered at gamespace `+0x4a0` (count
  `ldr x8, [x0, #0x4d0]` at 0x18fdf7c, table `ldr x9, [x21, #0x4a0]` at 0x18fe0d4, the call `blr x8` at 0x18fe0f8). `CStaticRaycastCS::Initialize`
  (0x18121a8) is registered there: its address is loaded from the GOT word 0x38046a8 (`ldr x2, [x2, #0x6a8]` at 0x174524c) and passed to
  `CncaGameSpace::RegisterInit` (0x1905750, which appends to `+0x4a0`) at 0x1745264; and it makes the one `bl` to CBVHTree::Initialize (0x1812220, after
  `CResourceInstance::LoadAndInit`). The gamespace is in the init-fiber table for the whole of LoadLevel (`combat_level_targets.h`: the done bit is set after
  LoadLevel returns). Not shown: that nothing else calls the callbacks at `+0x4a0` outside LoadLevel, and the other branch of LoadLevel, where the game's
  virtual `+0x360` replaces CreateComponentSystems (not followed; it too runs after the store). Headset row 3 checks the case that would show it wrong.
- **Shutdown**: the game's function, then the word is cleared. A later cast finds no tree and takes the stock miss.
- **Intersect**: while no combat map or combat lobby is loaded (section 4) the game's function answers. Else the gate (section 4); then `TreeOf(self)` reads the CBVHTree word
  and accepts it only if it is one of the table's entries (aligned, inside the table); the ray (6 floats) is copied; `Trace`, nearest hit, `t in (0, inf)` in
  units of the direction (Embree 1.0); on a hit, writes SBVHHit `id0 +0`, `id1 +4`, `u +8`, `v +0xc`, `t +0x10` and the unit normal at `+0x38` and nothing
  else; a miss writes nothing. Any doubt (no tree, a word that is not a table entry) calls the game's function.
- **Occluded**: the segment a..b is the ray `a + t (b - a)`, `t in (0, 1)`, first hit found. Not gated.
- **Gate**: section 4.
- **Touch**: returns false and writes nothing, as the stock Quest's stub tree makes `RayQueryStatic` behave. Without it a hand's touch ray (origin + dir x
  length, no `t <= 1` check by its reader) would hit a surface metres past the hand and the grab would pin the hand there. `RayQueryStatic` itself calls the
  Gate's stub (`0x21cf454 bl 0x00fd1740`, then `cbz w0, 0x021cf5c0`), so with the gate on its cast is already a miss (its return site is not in the allowed
  list); Touch matters when the gate is off.

Install (`InstallAt`): the legacy switch files are warned about; the three switches are reported by the configuration step, one generated `combat_config_switch` record each (the table rows `bvh`, `bvh_gate`, `bvh_touch`); off is one Info line
(`combat_bvh_install` `off_config`) and nothing is touched. Otherwise **all or none**: the gate goes in first (until its hook is in, no cast is marked, so a tree
installed a moment later answers none), then Initialize, Shutdown, Intersect, Occluded; the first the manager refuses (a slot that does not hold the pinned
function, a different libr15, a slot outside the writable segments) removes the ones already hooked and is one Error line that names the hook and the
manager's status. Touch goes in last, and its refusal is an Error line but not fatal. One `combat_bvh_install` line states the result and the hook count.
The scope's refresh rides the combat tick: with no room in the tick table the module refuses (`tick_table_full`) rather than install and never answer.

Switches (config.yaml, `combat:`): `bvh` (default true, as upstream, afk_bvh.cpp:121 "ON BY DEFAULT", `:365-367` off only with `debug.echoquestcombat.bvh=0`),
`bvh_gate` (true, afk_bvh.cpp:278-279,300), `bvh_touch` (true, afk_bvh.cpp:307-327). The plugin's kill files `bvh.off`, `bvhgate.off` and `bvhtouch.keep` are no
longer read; one found is a Warning (`combat_bvh_legacy_switch`) that names the key.

## 3. Evidence: the structures read, each offset quoted and checked

Source file:line is the source build's; disassembly is of the Quest libr15 unless it says PC (echovr.exe, where the
engine's Embree traversal still exists and shows the layout). "Unverified" means not used by the code.

**SBVHTreeData (0x50 bytes)**: triangles `CMemBlock {ptr +0x00, size +0x08}`, nodes `CMemBlock {ptr +0x20, size +0x28}`, u32 root
reference `+0x40`.
- Source: afk_bvh_core.h:6; root `+0x40` is *not* in the source (it assumes node 0, afk_bvh_core.h:14; deviation 1 below).
- the disassembly, `NRadEngine::SBVHTreeData::Reset` (VA 0x1b54428, size 48): `str x30...; mov x1, xzr; mov w2, wzr; mov x19, x0; bl 0x0103e400` (CMemBlock reset at +0), `add x0, x19, #0x20 ... bl 0x0103e400`
  (CMemBlock reset at +0x20), `0x1b5444c str wzr, [x19, #0x40]` (u32 at +0x40). Two CMemBlocks at +0 and +0x20 and a u32 at +0x40: confirmed.
- the PC disassembly, the tree setup `FUN_14070f6e0(param_1, param_2)`: `uVar1 = *(undefined4 *)(param_2 + 0x40)`;
  `FUN_1400da1c0(param_2 + 0x20)` / `FUN_1400d5d30(param_2 + 0x20)` and the same on `param_2` (block pointer and size getters of the
  two CMemBlocks), the u32 stored to the traverser at `lVar2 + 0x20`. Root reference at +0x40: confirmed.
- CMemBlock `{ptr +0, size +8}`: the traversal reads the block pointer as the first qword (`*(longlong *)(... + 0x30)` of the
  traverser, set from `FUN_1400da1c0`) and the size as the second; the size/pointer order inside a CMemBlock is **not read
  directly** (no CMemBlock accessor was decompiled); it is consistent with the source (afk_bvh_core.h:6) and with `Parse`
  failing safe: a wrong order makes sizes absurd, which `Parse` refuses (`block sizes`/`block address`) and the first headset
  line says so. Treated as source-asserted, checked only by that failure mode.
- Who passes it: `CStaticRaycastCS::Initialize` (VA 0x18121a8): `0x1812218 add x0, x19, #0x148` (this = the component system + 0x148),
  `0x181221c ldr x1, [x8, #0x70]` (the resource's data), `0x1812220 bl 0x01029cc0` (the Initialize PLT stub). `Shutdown`
  (0x1812748): `add x0, x0, #0x148; bl 0x01027ec0`. So **the CBVHTree is the 8-byte object at CStaticRaycastCS+0x148**, and the
  data pointer is the second argument.

**Triangle4 (0xe0 bytes)** `v0, e1, e2, Ng` as `x[4] y[4] z[4]` (48 floats), then `id0[4]`, `id1[4]`; an empty lane has `id0 = -1`.
- Source: afk_bvh_core.h:9 and `static_assert(sizeof(Tri4) == 0xe0)` `:32`.
- the PC disassembly, Embree traversal `FUN_14135f9a0` (decompilation): the stride `lVar6 = uVar7 * 0xe0;` and `lVar6 = lVar6 + 0xe0;` per
  record; v0 read as `pfVar11[0..11]` (`fVar42 = *pfVar11 - fVar46; ... pfVar11[0xb] - fVar24`), e2 as `pfVar11[0x18..0x23]`,
  Ng as `pfVar11[0x24..0x2f]` (`pfVar11[0x28] * fVar13 + fVar41 * pfVar11[0x24] + pfVar11[0x2c] * fVar12`). That leaves
  `[0x0c..0x17]` for e1 and `[0x30..0x37]` (8 words) for the two id arrays: 0xe0 = 4 x 48 + 8 x 4 bytes. Confirmed for the stride and for
  v0, e2, Ng; e1 and the ids by elimination plus the source (the id position is also what the source's tests use).
  The source's definitions `v1 = v0 - e1`, `v2 = v0 + e2`, `Ng = e1 x e2` (afk_bvh_core.h:10) are the Embree 1.0 Triangle4 form and agree with the
  traversal's use of Ng; **unverified** beyond that.

**Node (0x40 bytes)**: `x[4] y[4] z[4] = {left.lo, right.lo, left.hi, right.hi}`, `child[2]`, `pad[2]`; `child >= 0`: inner node at
byte offset `child * 8` (index `child >> 3`); `child < 0`: leaf, first record `(child & 0x7fffffff) >> 5`, count `child & 31`.
- Source: afk_bvh_core.h:12-14, `:31`, leaf decode `:142`.
- the PC disassembly `FUN_14135f9a0`: `while ((int)uVar10 < 0) { uVar7 = (ulonglong)(longlong)(int)(uVar10 ^ 0x80000000) >> 5;
  uVar1 = ((uVar10 ^ 0x80000000) & 0x1f) + uVar7;`: the sign bit marks a leaf, `>> 5` is the first record, `& 0x1f` the count. Confirmed.
  The 0x40 node size, the lane order of the bounds and the `child * 8` byte offset are the source's and the Embree 1.0 BVH2 layout;
  the sizes are enforced by `Parse` (multiples of 0x40) and a wrong stride would mis-read every tree, which the first-hit line shows.
  **Partly unverified** (not read from the decompilation's node loads).

**CBVHTree = 8 bytes**: afk_bvh.cpp:12; the object at CStaticRaycastCS+0x148 is the first (only) member of the engine's own
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

**The three allowed call sites** (afk_bvh.cpp:103-105): return addresses ELF 0x12b2b4c, 0x12b2bc4, 0x12c0a54 (disassembly 0x13b2b4c, 0x13b2bc4,
0x13c0a54). the disassembly at the instruction before each:

    13b2b48 bl 0x00fd1740   fe7af097   in DualStaticRaycast@13b2b28
    13b2bc0 bl 0x00fd1740   e07af097   in DualStaticRaycast@13b2b28
    13c0a50 bl 0x00fd1740   3c43f097   in Update@13c096c

all three are `bl` to the R24 PLT stub, in `CR15NetRaycastUtils::DualStaticRaycast` (twice) and `SR15PatrolSM::Update` as the source names them.
The names come from the disassembly's function table; the source's comments for why these three (bullets, AI patrol) are its own analysis
(afk_bvh.cpp:38-57), not re-derived here.

**SBVHHit (0x44 bytes)**: `id0 +0`, `id1 +4`, `u +8`, `v +0xc`, `t +0x10`, unit normal `+0x38`; `+0x14..+0x37` left as the caller set it.
Source afk_bvh.cpp:19-22, `:178-179`. **Not independently verified in the disassembly** (the readers of `+0x38` named in the source were
not decompiled here); the writes are limited to these fields, and a wrong offset shows as a wrong normal, not a crash
(all inside the 0x44 bytes the callers zero).

## 4. The callers' gate (R24)

The tree answers only the casts it was added for (bullets, AI patrol); every other cast misses as on the stock Quest
(afk_bvh.cpp:38-57: other callers take any hit as "within the segment" with `tmax = inf`, which grabs, hand reaches, cutters and
auto-target misread). Mechanism (`OnGate`, a thunk that is told the game's return address): if `ret - base` is one of the three allowed
values it sets a per-thread mark (a `ThreadWord`, a pthread key made at install: the API 26 toolchain emulates `thread_local`, which allocates on a thread's
first use, and a hooked call must not), calls the real `StaticRaycast`, and restores the mark that was there before. `OnIntersect`
answers from the tree only when the mark is set (gate on); any other Intersect is passed to the stock function (a miss).
Properties the tests check: a cast from an unlisted site never uses the tree; the mark does not leak past the cast; a nested unlisted
cast inside an allowed one is gated and the outer one still answers afterwards; a different thread is not marked; a direct
`CBVHTree::Intersect` with no marked cast is gated; Occluded is not gated.
The gate is installed first: until it is in no cast is marked, so a tree built a moment later answers none.

**Scope (arena shares the APK).** Every query hook calls the game's function while `InCombatOrLobby()` is false. The value is a relaxed atomic the combat tick refreshes
four times a second from `combat_level.h` (`Classify` of the loaded levels with the combat lobby: a combat map or the combat lobby is loaded and no Echo Arena level
is; an arena level that is loaded wins; a lobby arena players use is no class). The word starts false, so nothing is combat before the first look, and it fails
closed: a look older than one second is not believed (`kStaleAfterSeconds`), so a tick that stopped leaves every hook calling the game's function; when the tick
comes back after such a gap with the scope open, it writes one Warning `combat_bvh_scope_stale` (`gap_s`, `stale_after_s`, `level`). The arena path
of a query is one relaxed load of a false value, a test and a tail call of the game's function; only a true value reads the clock. The combat lobby is in scope
because the q15t plugin answered in every level; the levels left out are the Echo Arena levels and the lobbies arena players use. KNOWN-WRONG #2168: this is the
module's own cached answer; combat_scope (F19, #2186) is the shared one, and this module has not switched to it yet.


## 5. Behaviour

- Loading a level: `combat_bvh_tree` `built` (`data`, `triangle4`, `nodes`, `root_ref`) per CBVHResource (Info). A level without static geometry: `none`, `why`
  `empty` (Info). A header or block that cannot be read, or has bad sizes: `refused` (Warning). The 65th tree: Error `tree table full`. In every refusal the
  stock miss stands.
- In a combat match: bullets (DualStaticRaycast forward/backward) and the AI patrol cast hit walls; damage, heal-bubble and sound lines of sight (Occluded)
  are blocked by walls. Grab/touch is unchanged from the stock Quest (physics bodies only).
- Arena, and every other level: no change. The hooks call the game's function.
- Off: `combat.bvh: false`; no hook at all.

## 6. Differences from the reference build

1. **Root reference from the header (+0x40)**, not "node 0" (afk_bvh_core.h:14). The PC builds its traverser from `param_2 + 0x40` (section 3). For a tree whose root
   is node 0 the result is identical; for any other it is the correct one. Logged as `root_ref`.
2. **A cycle budget**: the traversal stops after `2 * nodes + 2` pops and answers "miss". A well-formed tree pops each node and leaf once; a malformed tree with
   a loop would otherwise spin. Also: stack depth over 128, an index outside the node or triangle block, a leaf past the triangle block: "miss".
3. **Non-finite or zero-direction rays hit nothing** (`Ray::ok`).
4. **Reads.** The header and both blocks are probed with `process_vm_readv` once, at Initialize. The arguments of a cast are plain loads, each backed by a
   dereference the game makes of the same object (stock libr15 disassembly): Intersect's ray is read from the caller's CRay by `CncaGame::StaticRaycast`
   (0x18f23c4), which passes its own stack copy on (0x18f2510); `LocalStaticRaycast` passes it through unread and its one caller (`DetectStaticBox`) builds it on its
   stack. Occluded's two points are read by `CncaGame::StaticRaycastOccluded` (0x18f2660), which passes its own copies on (0x18f27b4); `LocalStaticRaycastOccluded`
   passes its caller's pointers through unread and has no static caller, so that path relies on the references being valid. The CBVHTree word is read from the
   object the game found that frame (the game does not read +0x148 itself). A system call per cast cost more than the traversal (section 10).
5. **The CBVHTree word must be one of our 64 table entries**; any other value takes the game's function. A re-Initialize on the same data address reuses its entry.
6. **Switches are `combat:` keys** (no system properties, no kill files).
7. **All-or-none install with rollback through the manager**, and the gate installed first.
8. **Scope: combat maps and the combat lobby only, with a fresh look** (section 4); a tree only for those levels.
9. **No `1e-20` clamp on a zero direction component**: IEEE division by zero gives +-inf and the slab test is exact for it.
10. **Structured logging** (`combat_bvh_*` records); counters only on the per-call path, plus one line for the first hit and the first blocked segment.

## 7. Defaults

`combat.bvh`, `bvh_gate` and `bvh_touch` default to true, as upstream has them (on in upstream's code). The headset logs of the integration sessions
show the module installed and its trees built, but no cast answered in any logged window (`bvh summary` `intersect=0 occluded=0` in all four
windows of the logged session, `touch_kept_off` 374 and 1176 per 10 s); the headset row below therefore requires the acting lines, not `installed`.

## 8. Headset row

On a combat APK built at a tag after this unit (or a development APK repacked with the `combat` variant word, or `combat.force: true`), in a combat match:
1. `combat_bvh_install` `result` `installed`, `hooks` 6, `allowed_casts` 3.
2. At level load one `combat_bvh_tree` `built` per CBVHResource with `triangle4` and `nodes` non-zero (the sizes seen in one logged session: 36864/24633, 4096/4097, 53815/31572; the log does not name their levels, so the first headset run reads `level` and `scope` in these records).
3. `combat_bvh_scope` `to` 1 at the combat map (and the combat lobby) and `to` 0 on leaving it; no `built` record with `scope` other than `combat`, and `skipped` records for the arena levels and the other lobbies.
   A combat map entered from a lobby arena players use (`mpl_lobby_b`, `mpl_lobby_b2`) logs `combat_bvh_tree` `built` with `scope` `combat` and the combat map's
   `level`: a `skipped` with `scope` `other` there means the loading level was not yet at gamespace `+0x58` when Initialize ran (section 2).
   `combat_bvh_scope_stale` is absent, or rare and tied to a hitch (a level load); a steady stream means the tick is starved and the answers are off.
4. The acting lines: `combat_bvh_first_hit` (`mesh` >= 0, `tri` >= 0, `t` positive, a unit `normal`) and `combat_bvh_summary` with `intersect` > 0, `intersect_hit` > 0,
   `gated_off` > 0, `stock` 0 or tiny; shots at a wall do not pass (`occluded_blocked` > 0).

Falsifiers: no `built` after a combat level load (Initialize is not called with the data pointer second, or the hook never ran); `refused` with `block sizes` /
`block address` / `block unreadable` on every level (the SBVHTreeData layout is wrong for this build); `first_hit` never appears while `intersect` counts rise (a
wrong Node or Triangle4 layout), or `gated_off` is huge and `intersect` is 0 (the three allowed return addresses do not match); `t` negative, NaN, or `mesh`/`tri`
-1; a crash in a handler; grab or touch behaves differently from stock; `us_per_cast_avg` above a few microseconds or `us_per_cast_max` above about 0.5 ms; any
`combat_bvh_summary` or answered cast while an arena level is loaded.

## 9. Not built, unverified, open questions

- **Unverified** (not used to make a decision): CMemBlock pointer/size order, Triangle4 e1 and id array positions, Node bound lane order and `child * 8` (all fail
  safe through `Parse` and the traversal's bounds checks), the SBVHHit offsets beyond `+0x38` normal and `+0x10` t, the reasons for the three allowed sites.
- Whether the AI patrol and bullet casts reach the tree in a real match is a headset question; no logged window answered a cast.
- A `0xffd440` this-adjust thunk named in an earlier note is a PLT stub (`adrp x16, 0x37d6000; ldr x17,[x16,#0x108]`), not a StaticRaycast thunk. No code depends on it.
- PC vs reference differences: the PC's traverser is Embree's own packet code with SIMD (`FUN_14135f9a0`); this module is the scalar port. Hit order among
  coplanar triangles at the same `t` may differ from the PC (the first found wins here).
- A tree built before the module installs (an Initialize that ran earlier) has no table entry; the install is at the normal step, before any level loads.

## 10. Cost (docs/adr/0038)

Measured on the development host (x86-64, g++ -O2, one thread), not on a headset; a Quest's arm64 cores are slower, so read these as ratios:

| Path | Per call | Rate |
|---|---|---|
| Intersect or Occluded, combat map, tree of 147,000 random triangles (the logged level had 36,864 Triangle4 records), with the clock reads and counters | about 2.9 us (Occluded), 2.8 us (Intersect) | 0 in all four logged windows (headset logs); upstream names the callers as bullets and the AI patrol |
| the same two with the tree-less path (arena, or no combat map) | the game's function plus 1.0 ns (2.8 ns against 1.8 ns for the function alone) | whatever the game makes of them: not measured on a headset |
| Touch, combat map | the scope test below, a counter and a return | 37 to 118 a second in the logged windows (374 and 1176 per 10 s) |
| the scope test every hook makes first (`InCombatOrLobby()`): a false cached answer (arena) / a true one, which also reads CLOCK_MONOTONIC (`LookIsFresh`) | 0.24 ns / 27.6 ns (50M calls, best of 5) | once per hooked call; the 27.6 ns is paid only on a combat map or the combat lobby, and is part of the 2.8-2.9 us cast rows above |
| one `process_vm_readv` of 24 bytes (what each argument read cost before) | 487 ns | the former code made 2 per Intersect and 3 per Occluded |

Per-frame cost in an arena match: the sum over calls of about 1 ns (a relaxed load, a test and a tail call), so a thousand calls a frame at 90 Hz would be
about 1 us a frame. Unmeasured: the real call rate in arena (the headset logs carry none). The tick adds a clock read per frame and, four times a second in every
level, arena included, a scan of the game's level tables: 4 + 3 x N `process_vm_readv` calls for N gamespaces, about 5 us a scan with one gamespace on the host,
so about 20 us a second of game-thread time. A level load that is not a combat level pays one scan and no page probes. For a combat level the page probes of the
build cost about 1.2 ms (the 36,864-record tree) to 1.7 ms (the 53,815-record tree) on the host.

## 11. Tests and mutants

`just test-quest-combat bvh` runs `bvh_core_test` (the pure core on synthetic meshes: header parse and limits, ray validity, hits and misses with exact
`t`/`u`/`v`/normal, open range ends, a room at several leaf sizes, a random soup against two brute-force references, degenerate and malformed trees, the stack
and cycle limits, empty children, a node reference past the block) and `bvh_test` (the real thunks through the real manager into an image built in memory; the
hooks called through their slots; the gate from two call sites and two threads; refusals and rollback; the 64-entry table; unreadable and half-mapped blocks;
the switches; the legacy files; the scope: every hook calls the game's function with no combat map, with an arena level beside a combat map, with a lobby arena
players use; the combat lobby is in scope; a stalled tick fails closed (the clock moves with no tick) and says so when it is back (`combat_bvh_scope_stale`, not
for a look on time and not when the scope was shut); a level that is not a combat level gets no tree, a loading combat map does; the arena paths make no read,
no scan and no clock read; the cached answer starts false; the 10 s summary; the pinned constants).
Mutants: `src/quest/combat/mutants.d/bvh_core.sh` (rules of `bvh_core.h`, target `bvh_core_test`) and `src/quest/combat/mutants.d/bvh.sh` (rules of `bvh.cpp`,
`bvh.h`, `bvh_targets.h`), run with `src/quest/combat/mutants.sh bvh` and `bvh_core`.

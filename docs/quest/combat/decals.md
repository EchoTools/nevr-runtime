# Module `decals`: the level decals of the combat levels

Files: `src/quest/combat/decals.{h,cpp}`, `src/quest/combat/decals_core.h` (the table, the level rule and the state machine, game-free), `src/quest/combat/decals_targets.h`
(the pins), `src/quest/tests/decals_test.cpp`, `src/quest/tests/got_pinned_test.cpp`, `src/quest/combat/mutants.d/decals.sh`. The module is a step of the sentinel's normal
combat step (`src/quest/combat/combat_install.cpp`), registered after `impactsnd`: it runs in the `q15t` and `combat` APKs and in no other.

Switches (`config.yaml`, `combat:`): `decals` (default true: it draws nothing and installs no hook without a `decals.bin`), `decals_flip_u` and `decals_flip_v`
(default false; mirror the uv inside each quad). The plugin's `decals.off`, `decalflipu.on` and `decalflipv.on` are no longer read: one found in the plugin's data
directory is a Warning (`combat_decals_legacy_switch`) that names the key. `decals.bin` is a data file and is read where it was.

Origin: the reference build's static level decals (the reference build's `afk_fx` source, the `fxdecal` block `:4540-4606`: `decals.bin`, the `ADC1` table, the dynamic-mesh nodes, the bullet
system's UpdateMovement and Shutdown as the host), by way of the q15t plugin's decals module, which this moves. The slots `0x370dbc8`, `0x36933e8` and the function
`0x2124290` are in that file (upstream's numbering is the ELF vaddr used here). The level scope below is ours.

## What it does

The Quest renderer draws none of the static decals of the combat levels (no Quest shader reads the decal clusters). The content pack
carries them as a table, `decals.bin`, baked offline: world-space quads, each group with the level it belongs to, a material and a blend.
The module draws each group as one dynamic-mesh node (`CDynamicMeshNode`, up to 160 quads, both windings of every quad so exactly one
survives culling from any side) in the game space of that level.

Which node a game space gets: every group whose level is the game space's level, plus the groups of the war room (the drop ships) when
the game space is a combat arena (`mpl_combat_dyson`, `_combustion`, `_fission`, `_gauss`: `combat_level.h` `IsCombatMap`, the one list of combat maps). When a game space whose level is the war
room exists, the arena's copy of the war room's groups is hidden, so the ships are drawn once.

## The table

Searched in this order, the first readable file wins (the `combat_decals_install` line `reason: no_table` is written when none is readable):

1. the plugin's old data directory, `decals.bin`, being `/sdcard/Android/data/<package>/files/q15t`;
2. `/sdcard/Android/data/<package>/files/afk/decals.bin`;
3. `/sdcard/Android/media/<package>/files/afk/decals.bin`.

Format (little endian), written by the pack's bake:

```
char magic[4] = "ADC1"; u32 groups
per group: u64 level (CSymbol64 of the level name), u64 material (CSymbol64 of the shader set / material pair), u32 quads, u32 flags,
           then quads x 4 corners x {f32 x, y, z, u, v}, world space, corners (-,-) (+,-) (+,+) (-,+)
```

Flags: the low byte is the blend written into the node's render state: 1, 6, 7, 8 or 11 (`BlendOf`; any other value draws as 1).
The parser refuses the whole table, with one ERROR line naming the reason (`bad_table`, `LoadResultName`), for: a file under 8 bytes, another
magic, more than 256 groups, a group with more than 4096 quads, a group whose header or quads run past the end of the file, a NaN or an
infinity in any coordinate or uv. A file that exists and cannot be read, or is over 32 MiB, is refused before the parser (ERROR, reason
`table_unreadable`, with `why`). A group of more than 160 quads draws its first 160 (the install line counts those
groups, `groups_cut_to_160_quads`); trailing bytes after the last group are counted (`trailing_bytes`) and ignored. A table with no
groups is refused (`empty_table`).

`combat.decals_flip_u` and `combat.decals_flip_v` mirror the uv inside each quad (the orientation of the bake's uv is
inferred, not read): u swaps corners 0 and 1, 3 and 2; v swaps 0 and 3, 1 and 2.

## Mechanism (no code word written)

Two handlers, through the hook manager, on the bullet component system `CR15NetBulletCS`, which every game space has:

| Hook | Slot (ELF vaddr) | Holds | Wrapper |
| --- | --- | --- | --- |
| `UpdateMovement` | `0x370dbc8`, R_AARCH64_GLOB_DAT | `0x2124290`, `CR15NetBulletCS::UpdateMovement` (5208 bytes, exported) | calls the real function, then creates and rewrites the game space's nodes |
| `Shutdown` | `0x36933e8`, R_AARCH64_ABS64 (vtable word) | `0x21226f0`, `CR15NetBulletCS::Shutdown` (348 bytes) | destroys the system's nodes, then calls on |

The update slot is the word `R15NetRegisterComponentSystemUpdates` reads (one `ldr` at ELF `0x21e68a8`, in the exported function at
`0x21e4b74`) and hands to `CncaGameSpace::RegisterUpdate` as the member-function pointer of `UpdateMovement`, when a game space creates
its component systems at level load. The combat step runs before any level loads, so the handler is what gets registered. The callback is
`void()` on the component system (`this` in x0).

`Shutdown` is also hooked by `impactsnd` (the vtable word `0x36933e8`). The `decals` row runs after the `impactsnd` row, and its Shutdown
handler joins that word's chain above impactsnd's (`got_hook.h`, the first handler on a slot and the chain): it calls impactsnd's, which calls the game's. A
word that holds neither the game's function nor the chain's newest hook is refused (`original_mismatch`). The Shutdown wrapper goes in first; if it is refused nothing else is swapped. If the update wrapper is
refused the Shutdown wrapper stays and only passes through.

Per bullet system, on its update (the update thread of its game space; one mutex serialises the module's state):

1. read the system's game space (`[system+0x80]`) and its level (`[gamespace+0x58]`, a CSymbol64; `-1` until `LoadLevel` stores it, which
   reads as "no level yet"); every read is a `SafeRead`;
2. first sight of a system: one `decals: game space` line (system, level, groups, quads). A system whose level changes under it has its
   nodes destroyed and is gated again (`decals: level changed under a bullet system`);
3. create the missing nodes of the level, at most 3 tries a system, 5 s apart (the graphics must be up: `CGS::initialized` non-zero; a
   node whose `Initialize` fails is destroyed and the group stays off). At most 8 systems and 24 nodes at a time; the first system past the
   cap is logged once;
4. every update, for every node of the system: map the vertices, write the group's quads (position, uv, both colours white, normal and
   tangent zero), unmap, set the drawn index count to `12 x quads`, show the node.

Creating a node (the engine's own sequence for a dynamic mesh): make the system's allocator the current memory context; allocate `0x2048`
bytes from it (allocator vtable `+0x10`); `CDynamicMeshNode::CDynamicMeshNode`; `LinkToParent(nullptr)` (node vtable `+0x58`); hide
(`+0x60`, `SetVisible`); `SDynamicMeshVtx::VertexFormat()` (a 0x128-byte value returned in x8, so declared as a return value);
`CGDynamicMesh::Initialize(mesh = node + 0xa8, format, init)` with `SGDynamicMeshInit` = `{material, 4 x quads, 12 x quads, index type
2 (u16), 1, 0, 0, context[0], context[1], indices, null}`; then `[node+0x98] = 4 x quads` (vertices) and `[node+0xa0] = 0` (indices
drawn); restore the memory context. Destroying one: hide, `CDynamicMeshNode::Shutdown`, the system's allocator current, the node's
destructor (vtable `+0x0`, which takes it out of the scene graph), the allocator's free (`+0x38`), the memory context restored.

The blend: `Initialize` copies its render pool's state into the mesh's draw data (`mesh + 0x320`), and for material type 15 (the transparent
type the pack's decal materials have) that state's third word, at `node + 0xa8 + 0x320 + 0x708` (`0xad0`), is 0, which draws every atlas
cell as an opaque square. The group's blend is written there, over a 0 only: any other value is left alone and the node line says
`blend=0`.

Frequency: the update runs once a frame for each game space that has a bullet system. A system whose level the table does not cover costs
two `SafeRead`s (two `process_vm_readv` calls) and a clock read a frame; a system whose level it covers adds the vertex rewrite of each of
its nodes (at most 160 quads x 4 vertices x 44 bytes a node, 24 nodes in all) and one map/unmap pair a node. Not measured on a headset.

## Engine facts the module rests on

Read in the pinned library (GNU build id `b243509c08ce677aeb95fa348016949b3fc45230`, ELF vaddrs; the disassembly of `libr15.so`, VA = ELF +
`0x100000`):

- `CComponentSystem::CComponentSystem` (ELF `0x135e46c`): `[cs+0x80]` = the game space; `[cs+0x30]` = `[gamespace+0]`, a `CAllocator*`
  (`CncaGameSpace::LoadLevel` passes it to `CMemoryContext::SetCurrent`).
- `CncaGameSpace::LoadLevel` (ELF `0x17e5484`): `str x22, [x19, #88]` stores the level symbol at `gamespace+0x58`; the constructor leaves -1.
- `CDynamicMeshNode` is `0x2048` bytes: the embedded `CGDynamicMesh` starts at `+0xa8` and ends at `0x1fa0`; the game's own users
  (`CR15PointerMeshCS::InitCI`, `CR15NetFrisbeeTrailCS::InitCI`) allocate `0x2048`, call the constructor, `Initialize` the mesh at `+0xa8`
  and treat a non-zero result as failure. Vtable: `+0x0` `D2`, `+0x58` `CNode3D::LinkToParent`, `+0x60` `CNode3D::SetVisible(u32)`.
- `CDynamicMeshNode::Render` draws nothing when `[node+0x98]` or `[node+0xa0]` is 0 and passes both to `CGDynamicMesh::Render`.
- `SGDynamicMeshInit` is `0x48` bytes (`CGDynamicMesh::Initialize` reads the fields above); `SDynamicMeshVtx` is 44 bytes (the game's own
  writer uses stride `0x2c`: float x, y, z at +0, uv at +28).
- `CMemoryContext::Current` and `SetCurrent` read and write a per-thread slot (`CSysThread::GetTLS` / `SetTLS`), so the module's switch of the
  memory context on an update thread does not reach another thread's.
- `CGDynamicMesh::MapVertices` tail-calls `CGBuffer::Map`; `UnmapVertices` is a bare `ret` in this build.
- Exported from `libr15.so`: `CDynamicMeshNode::CDynamicMeshNode(), Shutdown()`, `SDynamicMeshVtx::VertexFormat()`,
  `CGDynamicMesh::Initialize, MapVertices, UnmapVertices`, `CMemoryContext::Current, SetCurrent, context`, `CGS::initialized`
  (a 4-byte int, 1 after `CGS::InitCommon`), `CR15NetBulletCS::UpdateMovement`.
- The module resolves these by name with `dlsym` on `libr15.so`; one missing refuses the module (`engine_export_missing`), and the
  exported address of `UpdateMovement` must equal the pinned one (`update_symbol_mismatch`).

## Logging (AGENTS.md)

Info `combat_decals_table` (path, groups, quads, groups cut to 160 quads, trailing bytes, `flip_u`, `flip_v`), one `combat_decals_hook` per hook (`hook`:
`update` or `shutdown`, `status`) and one `combat_decals_install` (`result`: `installed`, `off` or `refused`; `status`; `reason`); Error when a refusal leaves
the module out (`bad_table`, `empty_table`, `table_unreadable`, `engine_export_missing`, `module_not_loaded`, a hook refused) with the reason; Info `combat_decals_game_space`
once a system (`level`, `acts`, groups, quads); `combat_decals_node` (`made`, Warning with `rc`, the engine's answer, when not made) for the first 64 nodes of
a run; `combat_decals_window` every 10 s of drawing (frames, nodes, quads); `combat_decals_level_changed` and `combat_decals_system_shutdown` with the system and
the count; Warning `combat_decals_legacy_switch` for each plugin switch file found. The per-frame path writes no line.

## Switches and the level scope

`combat.decals`, `combat.decals_flip_u`, `combat.decals_flip_v` (above). The module acts on a game space only when that space's own level is a combat map
(`combat_level.h` `IsCombatMap`) or the war room (`mpl_combat_war_room`, the drop ship, which is no arena level). A table group that names any other level, an
arena level included, is never drawn, and a game space in an arena level, an arena lobby or the combat lobby creates no node. The SDK's `LevelGate`
(`sdk/include/nevr/level_gate.h`) is the PC plugin API's gate over the host's main level; the q15t plugin never used it for decals, so nothing moves off it and
it stays for plugins.

### Arena cost (ADR 0038)

`UpdateMovement` runs once per rendered frame for each bullet system (one per game space), so in an arena match with the module on and a `decals.bin` present each
call is the game's function, a mutex, a lookup of the system's record and a compare of the clock. The record of a game space that does not act reads its level
(two `process_vm_readv` calls) once a second (`kInertRecheckMs`), not on every update; a combat game space reads it on every update, as the plugin did. With
`combat.decals` false, or no `decals.bin`, nothing is hooked and an arena match pays nothing. The measured host numbers are in the pull request that moved the
module.

## Prediction (written before any headset run)

On the pinned build with the content pack's `decals.bin` and its material pairs in place, in a combat match: the log shows
`combat_decals_table` (`groups`, `quads`) at start, `combat_decals_hook` `update` and `shutdown` with `status: ok`; at the
first update of each combat game space `combat_decals_game_space` (`acts: 1`, `level` 0x43e2da7914642604 (dyson; combustion `0x42670f2bed45703c`,
fission `0xdf5ca7b7dfa383d4`, gauss `0x43e2da7a0c623a19`) with the groups of that level, then one `combat_decals_node` (`made: 1`, `rc: 0`, `blend` 1, 7 or 11)
a group within about 5 s of the graphics coming up, then a `combat_decals_window` line every 10 s. The decals are visible on the walls as
additive or lit stencils. At the end of the match `combat_decals_system_shutdown` (`nodes_destroyed` = the groups) and
no crash. A group whose material pair is missing from the pack logs `combat_decals_node` `made: 0` with `rc` three times, 5 s apart, and stays off.

## Falsifiers

- `combat_decals_install` `bad_table` or `engine_export_missing` on the pinned build: the table or the export list is not what the module
  was written for; nothing is swapped.
- `combat_decals_hook` `shutdown` with `status: original_mismatch`: another library wrapped the vtable word.
- `combat_decals_node` `made: 1`, `rc: 0` for every group and nothing on the walls: the vertices are written but the node does not draw (the index count,
  the visibility, the blend or the camera culling); `blend: 0` on the line says the blend word was not the pool's 0.
- The decals are mirrored: `combat.decals_flip_u` / `combat.decals_flip_v`.
- A crash in `CDynamicMeshNode` code at the end of a match or a level change: a node outlived its system or its allocator.
- A hitch or a drop in frame rate in a combat arena: the per-update vertex rewrite (160 quads x 4 vertices a node at most).

## Not established

- That the update thread of a game space is not the thread that renders it: the module writes the mapped vertex buffer from the update
  and the engine's own users of a dynamic mesh (`CR15PointerMeshCS`, the frisbee trail) do the same, so it is the engine's pattern.
- That `[node+0xa8+0x320+0x708]` is the render state's blend and not another word of it: nothing in the library names it; it is the third
  word of the 16-byte state the pool table holds, which is 0 for pool 23 in both Quest passes that write it.
- That `CGBuffer::Map` returns a buffer that is drawn without an `Unmap` having an effect: `UnmapVertices` is a bare `ret` in this build.
- That the normal and tangent can be zero: the game's own mesh writer sets their w to `0x7fff`; the decal shaderset reads neither.
- Which two allocators `CMemoryContext::context[0]` and `[1]` are; the module passes them on as the engine's own users do.
- That no engine call the module makes under its lock (node destructor, `CDynamicMeshNode::Shutdown`, the allocator) waits for a thread that is
  blocked on the module's lock; the engine's own shutdown of a dynamic-mesh system makes the same calls.
- A node whose system's allocator cannot be read at shutdown is not freed (a WARNING says so); the engine keeps it.
- The scheduler's call of the registered `UpdateMovement` (only the registration was read).
- The decals on a headset: no run yet (the five headset logs read `decals off: no readable decals.bin`); the prediction above is the test. Whether the combat
  APK's content pack carries a `decals.bin`: nothing in this repository names one, so without it the module installs nothing and draws nothing.

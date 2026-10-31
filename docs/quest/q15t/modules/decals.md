# Module `decals` (issue #1619)

Files: `src/quest/plugins/q15t/src/decals.{h,cpp}`, `src/quest/plugins/q15t/src/decals_core.h`, `src/quest/tests/q15t/decals_test.cpp`, `src/quest/tests/q15t/mod/decals.sh`,
`src/quest/tests/q15t/mutants.d/decals.sh`; the chained hook it needs is `InstallChainedImportHook` in `src/quest/plugins/q15t/src/import_hook.{h,cpp}`.

Option macro: `Q15T_DECALS`, default **ON**; the module is inert (one INFO line, reason `off`) when no `decals.bin` is found, so a build
or an install without the content pack is unchanged. Kill file: `decals.off` in the plugin data directory (`module_switch.h`).
API: `InstallResult InstallDecals(std::uintptr_t base)`; seam `InstallDecalsAt(base, update_slot, update_real, shutdown_slot,
shutdown_real, table_path, api)`. No tick of the game-tick table: the module rides the bullet system's own update.

## What it does

The Quest renderer draws none of the static decals of the combat levels (no Quest shader reads the decal clusters). The content pack
carries them as a table, `decals.bin`, baked offline: world-space quads, each group with the level it belongs to, a material and a blend.
The module draws each group as one dynamic-mesh node (`CDynamicMeshNode`, up to 160 quads, both windings of every quad so exactly one
survives culling from any side) in the game space of that level.

Which node a game space gets: every group whose level is the game space's level, plus the groups of the war room (the drop ships) when
the game space is a combat arena (`mpl_combat_dyson`, `_combustion`, `_fission`, `_gauss`). When a game space whose level is the war
room exists, the arena's copy of the war room's groups is hidden, so the ships are drawn once.

## The table

Searched in this order, the first readable file wins (the line `decals off: no readable decals.bin` names every path searched):

1. `<plugin data dir>/decals.bin`, the data directory being `/sdcard/Android/data/<package>/files/q15t`;
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

`decalflipu.on` and `decalflipv.on` in the plugin data directory mirror the uv inside each quad (the orientation of the bake's uv is
inferred, not read): u swaps corners 0 and 1, 3 and 2; v swaps 0 and 3, 1 and 2.

## Mechanism (no code word written)

Two data-slot wrappers on the bullet component system `CR15NetBulletCS`, which every game space has:

| Hook | Slot (ELF vaddr) | Holds | Wrapper |
| --- | --- | --- | --- |
| `UpdateMovement` | `0x370dbc8`, R_AARCH64_GLOB_DAT | `0x2124290`, `CR15NetBulletCS::UpdateMovement` (5208 bytes, exported) | calls the real function, then creates and rewrites the game space's nodes |
| `Shutdown` | `0x36933e8`, R_AARCH64_ABS64 (vtable word) | `0x21226f0`, `CR15NetBulletCS::Shutdown` (348 bytes) | destroys the system's nodes, then calls on |

The update slot is the word `R15NetRegisterComponentSystemUpdates` reads (one `ldr` at ELF `0x21e68a8`, in the exported function at
`0x21e4b74`) and hands to `CncaGameSpace::RegisterUpdate` as the member-function pointer of `UpdateMovement`, when a game space creates
its component systems at level load. The plugin loads before any level does, so the wrapper is what gets registered. The callback is
`void()` on the component system (`this` in x0).

`Shutdown` is also wrapped by `impactsnd` (the vtable word `0x36933e8`). `InstallDecals` runs after `InstallImpactSnd`, and its Shutdown
wrapper is installed with `InstallChainedImportHook`: the word must be the game's own function or an address in this plugin library's
executable mapping (the earlier wrapper), and whichever it is becomes the function the decals wrapper calls. A word anywhere else is
refused (`slot_not_stock_or_own`). The Shutdown wrapper goes in first; if it is refused nothing else is swapped. If the update wrapper is
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

Read in the pinned library (GNU build id `b243509c08ce677aeb95fa348016949b3fc45230`, ELF vaddrs; ReVault binary `libr15.so`, VA = ELF +
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

INFO `decals on` (path, groups, quads, groups cut to 160 quads, trailing bytes, flips) and one line a wrapper (`decals update wrapper`,
`decals shutdown wrapper`: slot, status, reason); ERROR when a refusal leaves the module out (`bad_table`, `engine_export_missing`,
`update_symbol_mismatch`, a wrapper refused) with the reason; `decals: game space` once a system; `decals: node made` / `decals: node
not made` (WARNING, with `rc`, the engine's answer) for the first 64 nodes of a run; `decals: window` every 10 s of drawing (frames,
nodes, quads); `decals: level changed ...` and `decals: bullet system shutdown, its nodes are destroyed` with the system and the count.
The per-frame path writes no line.

## Switches

`decals.off` (module out), `decalflipu.on`, `decalflipv.on` (uv mirror), all in the plugin data directory.

## Prediction (written before any headset run)

On the pinned build with the content pack's `decals.bin` and its material pairs in place, in a combat match: the log shows
`decals on ... groups=<n> quads=<m>` at start, `decals update wrapper ... status=ok`, `decals shutdown wrapper ... status=ok`; at the
first update of each combat game space `decals: game space ... level=0x43e2da7914642604` (dyson; combustion `0x42670f2bed45703c`,
fission `0xdf5ca7b7dfa383d4`, gauss `0x43e2da7a0c623a19`) with the groups of that level, then one `decals: node made ... rc=0 blend=<1|7|11>`
a group within about 5 s of the graphics coming up, then a `decals: window` line every 10 s. The decals are visible on the walls as
additive or lit stencils. At the end of the match `decals: bullet system shutdown, its nodes are destroyed nodes_destroyed=<groups>` and
no crash. A group whose material pair is missing from the pack logs `node not made ... rc=<n>` three times, 5 s apart, and stays off.

## Falsifiers

- `decals refused ... bad_table` or `engine_export_missing` on the pinned build: the table or the export list is not what the module
  was written for; nothing is swapped.
- `decals shutdown wrapper ... status=unexpected_original reason=slot_not_stock_or_own`: another library wrapped the vtable word.
- `node made ... rc=0` for every group and nothing on the walls: the vertices are written but the node does not draw (the index count,
  the visibility, the blend or the camera culling); `blend=0` on the line says the blend word was not the pool's 0.
- The decals are mirrored: `decalflipu.on` / `decalflipv.on`.
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
- The decals on a headset: no run yet; the prediction above is the test.

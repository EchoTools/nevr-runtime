/* Level decals (#2183, #1619, docs/adr/0046; docs/quest/combat/decals.md): draws the combat levels' static decals, which the Quest renderer cannot
 * draw on its own (no Quest shader reads the decal clusters), as world-space quads in dynamic-mesh nodes.
 *
 * The table is decals.bin (decals_core.h: groups of a level, a material and quads). A group's material is a shader set / material pair the content
 * pack carries under one symbol; a pair that is not there fails CGDynamicMesh::Initialize and that group stays off.
 *
 * Mechanism, no code word written: two handlers on the bullet component system (CR15NetBulletCS), the host the engine gives every game space and that
 * updates after physics each frame:
 *   - its UpdateMovement (the GLOB_DAT word R15NetRegisterComponentSystemUpdates reads when a level loads): the game's function runs, then the module
 *     creates the nodes of the game space's level (3 tries, 5 s apart) and rewrites their vertices;
 *   - its Shutdown (the vtable word impactsnd also hooks, so this handler is the second on that word's chain, above impactsnd's): the system's nodes are
 *     destroyed before the system is.
 *
 * Owner (ADR 0039). This file owns UpdateMovement's GLOB_DAT word; the Shutdown word is shared with impactsnd (it installs first, in the table's
 * order). The kill file `decals.off` and the two flip files `decalflipu.on` and `decalflipv.on`
 * are no longer read: one found in the data directory is a Warning (`combat_decals_legacy_switch`) that names the key that replaces it.
 * `decals.bin` is a data file, not a switch, and is read where it was.
 *
 * The level, and why LevelGate is not used. The module acts on a game space only when that space's own level is a combat map (combat_level.h
 * `IsCombatMap`) or the war room, and a group is drawn in the game space of the level it names (the war room's also in a combat map's). So a table that
 * names an arena level, or a bullet system of an arena match, draws nothing and creates nothing. This is the one answer to "is this combat?" that the
 * other combat modules use; there is no second level list here. The SDK's `LevelGate` (sdk/include/nevr/level_gate.h) is the PC plugin API's
 * per-context gate over the host's main level; this module's gate is its own table, not that gate, which
 * stays for plugins. The combat level answer (combat_scope.h, the loaded levels as a whole) does not answer this module's question, by decision:
 * the question is about one game space's level, which the handler reads from the system it was called with.
 *
 * Which APKs (ADR 0046 rule 5). The combat variants only (the normal step of combat_install.h), and only with `combat.decals` true and a
 * readable decals.bin: with `combat.decals` false or no table the module installs nothing and an arena match pays nothing.
 *
 * Frequency (ADR 0038). UpdateMovement runs once per rendered frame per bullet system (one per game space, each on its own update thread); Shutdown once
 * per teardown. In an arena match with the module on, each call is the game's function, a mutex, a lookup of the system's record and a compare of the
 * clock; the level is read again (two process_vm_readv calls) once a second per system. In a combat level it also draws up to 24 nodes of up to 160 quads
 * each. Numbers: docs/quest/combat/decals.md, "Arena cost".
 *
 * Origin. The reference build's level decals (`afk_*` decal drawing, docs/audits/2026-11-05-q15t-origin-citations.md) by way of the combat plugin's
 * `decals.cpp` (imported in #1591). The table format and the draw path are that plugin's.
 *
 * Lines (sentinel log):
 *   install   {"event":"combat_decals_install","result":"installed|off|refused","status":"<token>","reason":"..","decals_flip_u":0|1,"decals_flip_v":0|1,"msg":".."}
 *             `decals_flip_u` and `decals_flip_v` are the `combat.decals_flip_u` and `combat.decals_flip_v` values the step was handed, on every path
 *   table     {"event":"combat_decals_table","path":"..","groups":<n>,"quads":<n>,"groups_cut_to_160_quads":<n>,"trailing_bytes":<n>,"flip_u":0|1,"flip_v":0|1}
 *   hook      {"event":"combat_decals_hook","hook":"shutdown|update","status":"<GotStatus>"}
 *   node      {"event":"combat_decals_node","made":0|1,"system":..,"node":..,"material":"0x..","quads":<n>,"rc":<n>,"blend":<n>}   the first 64
 *   run       combat_decals_game_space, _level_changed, _window, _system_shutdown, _systems_full, _node_destroy_failed
 *   legacy    {"event":"combat_decals_legacy_switch","file":"..","replaced_by":".."}   Warning
 * and the manager's own `got_hook` line for each slot.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/decals_core.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::decals {

// CGVertexFormat, 0x128 bytes, returned by value (an AArch64 indirect result: the caller's buffer comes in x8, so it is declared as a return value and
// never as an out pointer) and passed to Initialize by reference.
struct VertexFormat {
  alignas(8) std::uint8_t raw[0x128];
};

// SGDynamicMeshInit, 0x48 bytes.
struct MeshInit {
  std::uint64_t material, max_vertices, max_indices;
  std::uint32_t index_type, one, extra, sort_key;
  void* allocator0;
  void* allocator1;
  const std::uint16_t* indices;
  const void* vertices;
};
static_assert(sizeof(MeshInit) == 0x48, "SGDynamicMeshInit");

// The engine calls the module draws with, resolved by name from the game library (all exported).
struct Api {
  void (*ctor)(void* node) = nullptr;                                          // CDynamicMeshNode::CDynamicMeshNode()
  VertexFormat (*vertex_format)() = nullptr;                                   // SDynamicMeshVtx::VertexFormat()
  int (*init)(void* mesh, const VertexFormat* fmt, const MeshInit* init) = nullptr;  // CGDynamicMesh::Initialize
  void* (*map)(void* mesh) = nullptr;                                          // CGDynamicMesh::MapVertices
  void (*unmap)(void* mesh) = nullptr;                                         // CGDynamicMesh::UnmapVertices
  void (*shutdown)(void* node) = nullptr;                                      // CDynamicMeshNode::Shutdown
  void* (*mem_current)() = nullptr;                                            // CMemoryContext::Current
  void (*mem_set)(void* allocator) = nullptr;                                  // CMemoryContext::SetCurrent
  void*** mem_context = nullptr;                                               // &CMemoryContext::context
  const std::uint32_t* gs_initialized = nullptr;                               // &CGS::initialized
};

// The three keys: `combat.decals`, `combat.decals_flip_u`, `combat.decals_flip_v` (flip: mirror the uv inside each quad; the orientation of the bake is
// inferred, not read).
struct Switches {
  bool on = false;
  bool flipU = false;
  bool flipV = false;
};

// Searched for decals.bin, in order: <the data directory>/decals.bin, /sdcard/Android/data/<pkg>/files/afk/decals.bin,
// /sdcard/Android/media/<pkg>/files/afk/decals.bin. No file: the module stays out with one Info line (reason "no_table").
ModuleResult Install(const Switches& switches) noexcept;

// The same with the lookup and the table given (tests). `api` null resolves it from libr15.so by name.
ModuleResult InstallAt(const Switches& switches, sentinel::ImageLookup lookup, const char* tablePath, const Api* api) noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

const Counters& CountersForTest() noexcept;
// Forgets the hooks, the thunks, the table and the state.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::decals

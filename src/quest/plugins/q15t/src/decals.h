// decals (issue #1619; default ON, option Q15T_DECALS, on at run time only when a decals.bin is found): draws the combat levels' static
// decals, which the Quest renderer cannot draw on its own (no Quest shader reads the decal clusters), as world-space quads in
// dynamic-mesh nodes.
//
// The table is decals.bin (decals_core.h: groups of a level, a material and quads). A group's material is a shader set / material pair
// the content pack carries under one symbol; a pair that is not there fails CGDynamicMesh::Initialize and that group stays off.
//
// Mechanism, no code word written: two data-slot wrappers on the bullet component system (CR15NetBulletCS), the host the engine gives
// every game space and that updates after physics each frame:
//   - its UpdateMovement (the GLOB_DAT word R15NetRegisterComponentSystemUpdates reads when a level loads): the real function runs,
//     then the module creates the nodes of the game space's level (3 tries, 5 s apart) and rewrites their vertices;
//   - its Shutdown (the vtable word impactsnd also wraps, so this wrapper chains on the one already there): the system's nodes are
//     destroyed before the system is.
// Details, the engine facts it rests on and the prediction: docs/quest/q15t/modules/decals.md.
#pragma once

#include <cstdint>

#include "decals_core.h"
#include "import_hook.h"

namespace q15t {

// ELF vaddrs of the pinned build (docs/quest/q15t/modules/decals.md).
inline constexpr std::uintptr_t kDecalsUpdateSlot = 0x370dbc8;    // GLOB_DAT: &CR15NetBulletCS::UpdateMovement
inline constexpr std::uintptr_t kDecalsUpdateReal = 0x2124290;    // CR15NetBulletCS::UpdateMovement (5208 bytes), also exported by name
inline constexpr std::uintptr_t kDecalsShutdownSlot = 0x36933e8;  // vtable word: &CR15NetBulletCS::Shutdown
inline constexpr std::uintptr_t kDecalsShutdownReal = 0x21226f0;

// CGVertexFormat, 0x128 bytes, returned by value (an AArch64 indirect result: the caller's buffer comes in x8, so it is declared as a
// return value and never as an out pointer) and passed to Initialize by reference.
struct DecalsVertexFormat {
  alignas(8) std::uint8_t raw[0x128];
};

// SGDynamicMeshInit, 0x48 bytes.
struct DecalsMeshInit {
  std::uint64_t material, max_vertices, max_indices;
  std::uint32_t index_type, one, extra, sort_key;
  void* allocator0;
  void* allocator1;
  const std::uint16_t* indices;
  const void* vertices;
};
static_assert(sizeof(DecalsMeshInit) == 0x48, "SGDynamicMeshInit");

// The engine calls the module draws with, resolved by name from the game library (all exported).
struct DecalsApi {
  void (*ctor)(void* node) = nullptr;                                       // CDynamicMeshNode::CDynamicMeshNode()
  DecalsVertexFormat (*vertex_format)() = nullptr;                          // SDynamicMeshVtx::VertexFormat()
  int (*init)(void* mesh, const DecalsVertexFormat* fmt, const DecalsMeshInit* init) = nullptr;  // CGDynamicMesh::Initialize
  void* (*map)(void* mesh) = nullptr;                                       // CGDynamicMesh::MapVertices
  void (*unmap)(void* mesh) = nullptr;                                      // CGDynamicMesh::UnmapVertices
  void (*shutdown)(void* node) = nullptr;                                   // CDynamicMeshNode::Shutdown
  void* (*mem_current)() = nullptr;                                         // CMemoryContext::Current
  void (*mem_set)(void* allocator) = nullptr;                               // CMemoryContext::SetCurrent
  void*** mem_context = nullptr;                                            // &CMemoryContext::context
  const std::uint32_t* gs_initialized = nullptr;                            // &CGS::initialized
};

// Switch files in the plugin data directory (module_switch.h): decals.off keeps the module out; decalflipu.on / decalflipv.on mirror the
// uv inside each quad (the uv orientation of the bake is inferred, not read).
// Searched for decals.bin, in order: <data dir>/decals.bin, /sdcard/Android/data/<pkg>/files/afk/decals.bin,
// /sdcard/Android/media/<pkg>/files/afk/decals.bin. No file: the module stays out with one INFO line (reason "off").
InstallResult InstallDecals(std::uintptr_t base);

// The same with the slots and the table given (tests). `api` null resolves it from libr15.so by name.
InstallResult InstallDecalsAt(std::uintptr_t base, std::uintptr_t update_slot, std::uintptr_t update_real, std::uintptr_t shutdown_slot,
                              std::uintptr_t shutdown_real, const char* table_path, const DecalsApi* api);

// Test seams.
const decals::Counters& DecalsCountersForTest();
void ResetDecalsForTest();

}  // namespace q15t

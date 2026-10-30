// outline (issue #1737 rows G6A-025, G6A-026; on by default, kill file outline.off): player outlines draw on skinned meshes, and a
// teammate's body is not given the team outline.
//
// The game's CGScene::SetOutlineMode(scene, mode) walks every mesh of a scene and every draw data set of each mesh and calls
// CGDrawDataSet::SetOutlineMode(dds, mode, format, prim), which builds the outline pipeline for that draw. For a compute-skinned mesh
// it passes the scene resource's vertex format, which lacks the skinned stream-1 elements the main pass binds; the outline pipeline
// then does not match the skinned vertex buffer and the outline is not drawn. This module replaces the scene function (its JUMP_SLOT,
// a data word) with the same walk, done here, with two changes:
//   1. a skinned mesh's draw sets get the merged format the main pass uses (CGComputeVertexBuffer::Bind's rule: every element of the
//      scene format is kept, except a stream-1 element whose usage is position, normal, tangent or 0xb, which is taken from the compute
//      buffer's own format), rebuilt with the game's own CGVertexFormat functions;
//   2. mode 7 (the team outline, drawn through walls) on a skinned mesh is passed as mode 0 (no outline), as on the PC. Mode 12, the
//      threat scanner's outline, is passed unchanged. Kill file outline_noteam.off keeps mode 7.
// The per-draw function is never hooked: the walk is the per-object level above it. The witness of the renderer's outline switch
// (G6A-026) is one INFO line at the first scene call. Details, evidence, frequency: docs/quest/q15t/modules/outline.md.
#pragma once

#include <cstddef>
#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

namespace q15t {

struct OutlinePins {
  std::uintptr_t scene_slot;   // CGScene::SetOutlineMode JUMP_SLOT (ELF vaddr): swapped
  std::uintptr_t scene_fn;     // CGScene::SetOutlineMode (what that slot holds)
  std::uintptr_t dds_slot;     // CGDrawDataSet::SetOutlineMode JUMP_SLOT: checked, not swapped
  std::uintptr_t dds_fn;       // CGDrawDataSet::SetOutlineMode: called by the walk
  std::uintptr_t render_switch;  // u32 the renderer's outline switch is copied to (G6A-026 witness; read only)
};

// ELF vaddrs of the pinned build (ReVault libr15 VA - 0x100000).
inline constexpr OutlinePins kOutlinePinned = {0x36fb718, 0x18c5140, 0x36e3bc0, 0x183ab9c, 0x3753260};

// The scene walk's instruction words: the offsets the walk below reads are the ones these instructions read.
inline constexpr WordPin kOutlineWalkWords[] = {
    {0x18c5158, 0xf9403016},  // ldr x22, [x0, #0x60]     mesh count
    {0x18c5170, 0x52804d1b},  // mov w27, #0x268           mesh stride
    {0x18c5174, 0xf9401a68},  // ldr x8, [x19, #0x30]      mesh array
    {0x18c5180, 0xf9401f9d},  // ldr x29, [x28, #0x38]     draw data set count of a mesh
    {0x18c51a8, 0xf9401268},  // ldr x8, [x19, #0x20]      scene resource
    {0x18c51b8, 0xf9403909},  // ldr x9, [x8, #0x70]       resource mesh rows (stride 0x98)
    {0x18c51bc, 0xf940710a},  // ldr x10, [x8, #0xe0]      vertex formats (stride 0x150)
    {0x18c51c0, 0xf9405508},  // ldr x8, [x8, #0xa8]       prim rows (stride 0x70, prim at +0x40)
    {0x18c51cc, 0xb940192b},  // ldr w11, [x9, #0x18]      row: format index
    {0x18c51d0, 0xb9403529},  // ldr w9, [x9, #0x34]       row: first prim index
    {0x18c51e8, 0xb8470723},  // ldr w3, [x25], #0x70     prim of a draw data set, prim row stride
    {0x18c51ec, 0xf9400788},  // ldr x8, [x28, #0x8]       draw data sets of a mesh
    {0x18c5204, 0x911de318},  // add x24, x24, #0x778      draw data set stride
};

// Layout the walk reads (CGScene, its resource, CGMesh, CGDrawDataSet).
inline constexpr std::uintptr_t kSceneRes = 0x20, kSceneMeshes = 0x30, kSceneMeshCount = 0x60, kMeshStride = 0x268;
inline constexpr std::uintptr_t kMeshDds = 0x8, kMeshDdsCount = 0x38, kDdsStride = 0x778;
inline constexpr std::uintptr_t kResRows = 0x70, kResPrims = 0xa8, kResFormats = 0xe0;
inline constexpr std::uintptr_t kRowStride = 0x98, kRowFormat = 0x18, kRowPrim = 0x34, kFormatStride = 0x150;
inline constexpr std::uintptr_t kPrimStride = 0x70, kPrimType = 0x40;
inline constexpr std::uintptr_t kMeshSkinned = 0xd8;     // u32: non-zero for a compute-skinned mesh
inline constexpr std::uintptr_t kMeshComputeFmt = 0xf0;  // the compute vertex buffer's own format (buffer at +0x40, format at +0xb0)
inline constexpr std::uint32_t kModeNone = 0, kModeTeam = 7;

// The game's CGVertexFormat functions the merge uses (resolved by name in libr15; a test passes fakes).
struct OutlineFns {
  void (*fmt_ctor)(void*);                               // CGVertexFormat::CGVertexFormat()
  std::uint64_t (*count_used)(const void*);              // CountUsed() const
  void (*get_index)(const void*, std::uint64_t, void*);  // GetElement(u64 index, SVertexElement&) const
  void (*get_usage)(const void*, std::uint32_t, void*);  // GetElement(EUsage, SVertexElement&) const
  void (*append)(void*, const void*);                    // AppendElement(SVertexElement const&)
};

// CGVertexFormat: 36 eight-byte elements and the count at +0x120; Bind copies 0x128 bytes of it.
inline constexpr std::size_t kFormatBytes = 0x128;
inline constexpr std::uint64_t kFormatElements = 36;
inline constexpr std::size_t kMergedMax = 256;  // interned merged formats (never freed: a pipeline may keep the pointer)

// Resolves OutlineFns from the loaded game library and checks four of them against their pinned vaddrs. False (nothing set) when any
// is missing or not where the pinned build has it.
bool ResolveOutlineFns(std::uintptr_t base, OutlineFns* out);

// Kill files: outline.off installs nothing (reason "off_file"); outline_noteam.off keeps mode 7.
InstallResult InstallOutline(std::uintptr_t base);
InstallResult InstallOutlineAt(std::uintptr_t base, const OutlinePins& pins, const WordPin* words, std::size_t n_words,
                               const OutlineFns& fns);

struct OutlineCounters {
  std::uint64_t scene_calls;     // scene walks done by the wrapper
  std::uint64_t skinned_meshes;  // skinned meshes seen
  std::uint64_t merged;          // ... given a merged format
  std::uint64_t merge_refused;   // ... given the scene format (element count past 36, interning full, allocation failed)
  std::uint64_t team_dropped;    // draw data sets whose mode 7 was passed as 0
};
OutlineCounters OutlineGetCounters();

// Test seams.
void ResetOutlineForTest();
std::uintptr_t OutlineWrapperForTest();

}  // namespace q15t

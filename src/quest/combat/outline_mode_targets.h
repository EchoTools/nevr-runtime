/* The pinned facts of the outline-mode module (#1640 and #1656; the module itself is not in this tree): the two SetOutlineMode JUMP_SLOTs, the
 * scene walk the module replaces, the field that says whether a mesh's draw sets were built with a merged vertex format, the merge rule
 * the game applies there, and the five CGVertexFormat functions the module calls to build that format. Behaviour is in the upstream reference
 * (kotorvr/echoquest-combat, runtime/native/afkxr/afk_fx.cpp: fxoutlinefix2 and olnoteam).
 *
 * Every address is an ELF virtual address in the store libr15 (its build ID is kLibR15StockBuildId in pinned_targets.h). The
 * disassembly's address is the ELF address plus 0x100000 (ReVault project echovr, binary libr15.so). Every word below was read from the
 * store file with llvm-objdump; src/quest/tests/outline_mode_pinned_test.cpp checks each one, each relocation, each .dynsym value and each
 * decode named here against the real file (`just test-quest-hooks-pinned`).
 *
 * Sources. What is stated as fact below is carried by a pinned instruction word, relocation, .dynsym entry or data word read from the store
 * file, or by a byte-checked reconstruction (ReVault project echovr, binary libr15.so), named where it is used: CGVertexFormat C1 (VA
 * 0x2909098: 36 elements of 8 bytes with usage 0xc, the count at +0x120 zeroed, 0x128 bytes), GetElement(EUsage) (VA 0x29094bc),
 * AppendElement(const SVertexElement&) (VA 0x290a4a8: no bound check), CGMesh::SetOutlineMode (VA 0x19641e4: sets at +0x8, count at +0x38,
 * stride 0x778, prim at +0x40 of 0x70-byte entries) and CModelCS::SetOutlineMode (VA 0x16b3dd8: the CGScene it calls is at owner+0xa0, the
 * call is a tail call to CGScene::SetOutlineMode). These functions have no byte-checked reconstruction yet: CGScene::SetOutlineMode (ELF
 * 0x18c5140), CGDrawDataSet::SetOutlineMode (0x183ab9c), CountUsed (0x2809438), GetElement(u64) (0x28094a8), CGMesh::Initialize (0x1857ab8),
 * CGMesh::InitializeComputeSkinning (0x18631c0), CGComputeVertexBuffer::Bind (0x25bdbac), CGDrawDataSet::InitOutlineDraw (0x18394fc),
 * CGRenderer::OutlineRenderPool (0x183b21c) and CModelSetOutlineModeNode::Enter (0x2647ff8). Of each, only what pinned words carry is
 * stated; anything else about them is written "not established (the function is not in the proven reconstruction tree)".
 *
 * 1. The slots.
 *   CGScene::SetOutlineMode(EMeshOutlineMode)                                          JUMP_SLOT 0x36fb718 -> 0x18c5140  (size 236)
 *   CGDrawDataSet::SetOutlineMode(EMeshOutlineMode, CGVertexFormat const&, EPrimType)  JUMP_SLOT 0x36e3bc0 -> 0x183ab9c  (size 292)
 * Each function is the .dynsym value of the symbol the slot's relocation names.
 *
 * 2. The scene walk, CGScene::SetOutlineMode (ELF 0x18c5140..0x18c5228; x19 = the scene, w20 = the mode). For each mesh i below
 * u64[scene+0x60] (0x18c5158), the mesh is [scene+0x30] + i*0x268 (0x18c5174, 0x18c5170, 0x18c517c); a mesh whose draw-set count
 * u64[mesh+0x38] is 0 is skipped (0x18c5180, 0x18c5184). For the others, with d = [scene+0x20] (0x18c51a8):
 *   record     = [d+0x70] + i*0x98                      (0x18c51b8, 0x18c51ac, 0x18c51c8; the mesh index i also indexes the records)
 *   format     = [d+0xe0] + u32[record+0x18] * 0x150     (0x18c51bc, 0x18c51cc, 0x18c51b0, 0x18c51d8; the CGVertexFormat passed, by reference)
 *   prim of k  = u32[[d+0xa8] + (u32[record+0x34] + k)*0x70 + 0x40]   (0x18c51c0, 0x18c51d0, 0x18c516c, 0x18c51e4, 0x18c51e8)
 *   draw set k = [mesh+0x8] + k*0x778                    (0x18c51ec, 0x18c5204), for k below the count
 * and it calls CGDrawDataSet::SetOutlineMode(draw set k, mode, format, prim of k) through the PLT stub at ELF 0xf189b0 (the `bl` at
 * 0x18c51fc), whose `adrp`/`ldr` read the JUMP_SLOT 0x36e3bc0. Within one mesh the same format is passed for every draw set; the
 * format is looked up again for each mesh (`madd x21,x11,x12,x10` at 0x18c51d8 sits in the per-mesh block, after the `cbnz x29` at
 * 0x18c5184, and is the only write to x21 before the return).
 * The 0x150 stride is the stride of the array at d+0xe0; that the CGVertexFormat sits at offset 0 of each 0x150-byte entry is what the
 * walk passes (the entry's address), not a size of CGVertexFormat.
 * The mesh is a CGMesh. CGMesh::SetOutlineMode (the reconstruction above) walks the same two fields (count [this+0x38], sets [this+0x8],
 * stride 0x778). Pinned words in CGMesh::Initialize (ELF 0x1857ab8; x23 = this: `mov x23,x0` at 0x1857ae0, saved by `str x23,[sp,#0x78]` at
 * 0x1857e98 and reloaded by `ldr x23,[sp,#0x78]` at 0x1858044, with no other write of x23 between 0x1858044 and 0x1858300 in the listing):
 * `add x0,x23,#0x8` (0x18582dc) is the address of the draw-set table field, `mov w9,#0x778` (0x1858670) is a stride of 0x778, and the call
 * at 0x185868c goes to CGDrawDataSet::Initialize. That the loop around these words builds the draw sets at [this+0x8] + k*0x778 is not
 * established (the function is not in the proven reconstruction tree). The scene's mesh stride 0x268 is above the store `str wzr,[x23,#0x260]` (0x18582f8); what else
 * Initialize writes at or above that offset is not established (the function is not in the proven reconstruction tree).
 *
 * 3. The merged format, in CGMesh::Initialize (x23 = this, above). The words at ELF 0x1858300..0x1858324:
 *   ldr w8,[x23,#0xd8]; cbz w8 -> 0x1858338                (0x1858300, 0x1858308: a zero word branches past the Bind call)
 *   add x0,x23,#0xf0; bl CountUsed; cbz x0 -> 0x1858338     (0x185830c, 0x1858310, 0x1858314: an empty format at +0xf0 branches past it)
 *   add x0,x23,#0x40; add x8,sp,#0x98; bl Bind              (0x1858318, 0x185831c, 0x1858324: this+0x40 is Bind's `this`, its indirect
 *                                                            result is the buffer at sp+0x98)
 * and the draw-set Initialize call at 0x185868c has x1 = sp+0x98 (`add x1,sp,#0x98` at 0x1858674, no write to x1 between it and the call
 * in the listing). So u32[this+0xd8] != 0 and a non-empty format at this+0xf0 are the two conditions of the Bind call. Not established
 * (the function is not in the proven reconstruction tree): what else writes sp+0x98 on the path that skips Bind, and so whether every draw set of a mesh that passes both
 * conditions is initialized from Bind's result. Upstream's module applies the same two reads, in the same order, as its test for a mesh whose
 * draw sets were built with a merged format ("skinned" in upstream's word).
 * The compute vertex buffer is at mesh+0x40 and its format at +0xb0 in it (`add x23,x23,#0xb0` at 0x25bdce0, the format Bind reads), so
 * mesh+0xf0 = 0x40 + 0xb0.
 * Not established (the function is not in the proven reconstruction tree): which function writes u32[mesh+0xd8] (u32 at +0x98 of the compute vertex buffer), and so what
 * its nonzero values mean beyond the gate above; what CGMesh::InitializeComputeSkinning (ELF 0x18631c0) does with the compute vertex buffer.
 *
 * 4. The merge rule, CGComputeVertexBuffer::Bind(CGVertexBuffer const&) (ELF 0x25bdbac, size 668). `mov x23,x0` (0x25bdbcc) is `this`,
 * `mov x22,x1` (0x25bdbd8) the input buffer (its CGVertexFormat at offset 0: GetElement(u64) is called on it), `mov x19,x8` (0x25bdbdc)
 * the indirect result; `add x23,x23,#0xb0` (0x25bdce0) makes x23 the compute format at this+0xb0. Each pass reads element x24 of the input
 * into the 8 bytes at sp+0x10 (`mov x0,x22`, `mov x1,x24`, `add x2,sp,#0x10`, then the `bl` at 0x25bdd28 to GetElement(u64, SVertexElement&)
 * through its stub). Then:
 *   - stream (element byte 6, `ldrb w8,[sp,#0x16]` at 0x25bdd2c) != 1 (`b.ne` at 0x25bdd34), or usage (byte 0, `ldrb w8,[sp,#0x10]` at
 *     0x25bdd38) > 11 (`b.hi` at 0x25bdd40): both branch to 0x25bdcfc, which is `add x0,sp,#0x18; add x1,sp,#0x10; bl` to
 *     AppendElement(const SVertexElement&) through its stub (0x25bdcfc..0x25bdd04): the element as read is appended to the format at sp+0x18;
 *   - else `adr x9,#-0x58` (the case base 0x25bdcec, 0x25bdd44), the table byte for the usage (`ldrb w10,[x26,x8]`, 0x25bdd48; x26 = the
 *     table, `adrp` 0x25bdcd8 + `add` 0x25bdce4), `add x9,x9,x10,lsl #2` and `br x9` (0x25bdd4c, 0x25bdd50) pick a case from the jump table
 *     at ELF 0x31f19f8 (12 bytes, one per usage 0..11): usages 0, 2, 3 and 11 go to a case that sets w1 to the usage (the `mov w1` at
 *     0x25bdd5c, 0x25bdd68, 0x25bdd74, 0x25bdcf0) and ends in the call at 0x25bdcf8 to GetElement(EUsage, SVertexElement&) through its stub,
 *     after `mov x0,x23` (0x25bdcf4); the case bodies' other words (their `add x2,sp,#0x10`, usage 0's own `mov x0,x23`, the `b` that
 *     joins 0x25bdcf4 or 0x25bdcf8) are not pinned. The next instruction after that call, the append at 0x25bdcfc, does not read w0, so
 *     the call's return is not tested. GetElement(EUsage) (the reconstruction above) overwrites the element with the compute format's
 *     first element of that usage and returns 1, and returns 0 with the element untouched when the compute format has none; the element
 *     is then appended. Usages 1 and 4..10 go to 0x25bdcfc and are appended as read.
 * The merged format at sp+0x18 is then copied whole, 0x128 bytes (`add x1,sp,#0x18` 0x25bdd94, `mov w2,#0x128` 0x25bdd98, `mov x0,x19`
 * 0x25bdd9c, `bl` to memcpy through its stub 0x25bdda0), to offset 0 of the result.
 * Not established (the function is not in the proven reconstruction tree): how the format at sp+0x18 is initialized before the loop (the call at 0x25bdcc8 is to CGVertexFormat
 * C1 in the listing; its arguments are not pinned), the loop's index (x24: its start and step) and its bound and end (CountUsed(input) in the listing, 0x25bdd08..0x25bdd18),
 * and what Bind writes to the rest of the result.
 *
 * 5. CGVertexFormat. 36 elements of 8 bytes from offset 0, then the u64 element count at +0x120: sizeof 0x128. The constructor writes
 * usage 0xc into each of the 36 elements (the last at +0x118, 0x2809254) and zeroes the count (`str xzr,[x0,#0x120]` at 0x2809260);
 * CountUsed returns [this+0x120]; GetElement(u64) copies element i (`ldr x8,[x0,x1,lsl #3]`); GetElement(EUsage) finds the first element
 * whose byte 0 is the usage and copies it (returns 1), else returns 0; AppendElement(const SVertexElement&) stores at the count and
 * increments it with no bound check (0x280a4a8..0x280a4bc): a caller that appends to a full format writes past +0x120.
 * The five functions the module calls, by .dynsym name (kHelpers); not their near namesakes GetElement(EUsage, u64, SVertexElement&)
 * at 0x28094f4 and AppendElement(EUsage, EType, u64, u64, u64, u32) at 0x280a4c8 (kNotHelpers, pinned so a swap is caught).
 *
 * 6. The modes.
 * CGDrawDataSet::InitOutlineDraw (ELF 0x18394fc): `ldr w21,[x1,#0x68]` (0x1839514) reads a u32 from its second argument, `cmp w21,#0xd`
 * (0x1839518) compares it with 13, and `b.lo` (0x183951c) branches when it is lower. Not established (the function is not in the proven reconstruction tree): that the u32
 * is the EMeshOutlineMode, what the function does on either side of the compare (a refusal of a mode >= 13, a message), and so how many
 * modes there are.
 * CGRenderer::OutlineRenderPool(EMeshOutlineMode) (ELF 0x183b21c, size 16): its four words are pinned, `adrp x8,0x2d6e000`, `add x8,x8,#0x59c`,
 * `ldr w0,[x8,w0,uxtw #2]`, `ret`: it returns the u32 at ELF 0x2d6e59c + 4 * its argument. The table's words at index 7 and 12 are pinned
 * data: 0x2d and 0x32.
 * CModelSetOutlineModeNode::Enter (ELF 0x2647ff8): `mov x19,x1` (0x2648034) keeps its second argument, the node data, and `ldr w19,[x19,#0x20]`
 * (0x26480b0) reads the u32 at data+0x20; `mov w2,w19` (0x26480c4) and the `bl` at 0x26480c8, through the stub at ELF 0xf1e490 whose
 * JUMP_SLOT names CModelCS::SetOutlineMode, call it with that u32 as the mode. The path through those words is the one that does not take
 * the `cbz w9` at 0x26480b8; what the taken path does, and where the node's data comes from (the level's script data), are not established
 * (the function is not in the proven reconstruction tree). Which mode values the game uses is not established: that 7 is the teammate outline and 12 the scanner outline are
 * upstream's labels (afk_fx.cpp), not read from the binary.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/pinned_words.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::outline_mode::targets {

inline constexpr const char* kSceneSymbol = "_ZN10NRadEngine7CGScene14SetOutlineModeENS_16EMeshOutlineModeE";
inline constexpr const char* kDrawSetSymbol =
    "_ZN10NRadEngine13CGDrawDataSet14SetOutlineModeENS_16EMeshOutlineModeERKNS_14CGVertexFormatENS_9EPrimTypeE";

// The slots, and the function each slot's symbol resolves to in the store libr15.
struct Pins {
  std::uint64_t sceneSlot, sceneFn;
  std::uint64_t drawSetSlot, drawSetFn;
};
inline constexpr Pins kPins = {0x36fb718ULL, 0x18c5140ULL, 0x36e3bc0ULL, 0x183ab9cULL};

// The two JUMP_SLOT targets, each with its slot pinned. A target carries no expected original: the slot's value at run time is
// the install's check, not this header's.
inline sentinel::GotTarget SceneTarget(const Pins& pins = kPins) {
  return sentinel::GotTarget(sentinel::pinned::kLibR15, kSceneSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), pins.sceneSlot);
}
inline sentinel::GotTarget DrawSetTarget(const Pins& pins = kPins) {
  return sentinel::GotTarget(sentinel::pinned::kLibR15, kDrawSetSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), pins.drawSetSlot);
}

// --- The scene walk (section 2) ---------------------------------------------------------------------------------------------------

inline constexpr pinned::Word kSceneMeshCount{"ldr x22,[x0,#0x60] (scene mesh count)", 0x18c5158ULL, 0xf9403016U};
inline constexpr pinned::Word kScenePrimStride{"mov w26,#0x70 (prim entry stride)", 0x18c516cULL, 0x52800e1aU};
inline constexpr pinned::Word kSceneMeshStride{"mov w27,#0x268 (mesh stride)", 0x18c5170ULL, 0x52804d1bU};
inline constexpr pinned::Word kSceneMeshes{"ldr x8,[x19,#0x30] (scene mesh array)", 0x18c5174ULL, 0xf9401a68U};
inline constexpr pinned::Word kSceneDrawSetCount{"ldr x29,[x28,#0x38] (mesh draw-set count)", 0x18c5180ULL, 0xf9401f9dU};
inline constexpr pinned::Word kSceneSkipEmpty{"cbnz x29 (a mesh with no draw sets is skipped)", 0x18c5184ULL, 0xb500013dU};
inline constexpr pinned::Word kSceneData{"ldr x8,[x19,#0x20] (scene mesh data)", 0x18c51a8ULL, 0xf9401268U};
inline constexpr pinned::Word kSceneRecordStride{"mov w11,#0x98 (record stride)", 0x18c51acULL, 0x5280130bU};
inline constexpr pinned::Word kSceneFormatStride{"mov w12,#0x150 (format entry stride)", 0x18c51b0ULL, 0x52802a0cU};
inline constexpr pinned::Word kSceneRecords{"ldr x9,[x8,#0x70] (record array)", 0x18c51b8ULL, 0xf9403909U};
inline constexpr pinned::Word kSceneFormats{"ldr x10,[x8,#0xe0] (format array)", 0x18c51bcULL, 0xf940710aU};
inline constexpr pinned::Word kScenePrims{"ldr x8,[x8,#0xa8] (prim array)", 0x18c51c0ULL, 0xf9405508U};
inline constexpr pinned::Word kSceneRecordMadd{"madd x9,x23,x11,x9 (record of mesh i)", 0x18c51c8ULL, 0x9b0b26e9U};
inline constexpr pinned::Word kSceneFormatIndex{"ldr w11,[x9,#0x18] (record format index)", 0x18c51ccULL, 0xb940192bU};
inline constexpr pinned::Word kSceneFirstPrim{"ldr w9,[x9,#0x34] (record first prim index)", 0x18c51d0ULL, 0xb9403529U};
inline constexpr pinned::Word kSceneFormatMadd{"madd x21,x11,x12,x10 (the format passed)", 0x18c51d8ULL, 0x9b0c2975U};
inline constexpr pinned::Word kScenePrimField{"add x25,x8,#0x40 (prim field)", 0x18c51e4ULL, 0x91010119U};
inline constexpr pinned::Word kScenePrimRead{"ldr w3,[x25],#0x70 (prim of draw set k)", 0x18c51e8ULL, 0xb8470723U};
inline constexpr pinned::Word kSceneDrawSets{"ldr x8,[x28,#0x8] (mesh draw-set array)", 0x18c51ecULL, 0xf9400788U};
inline constexpr pinned::Word kSceneDrawSetCall{"bl 0xf189b0 (CGDrawDataSet::SetOutlineMode stub)", 0x18c51fcULL, 0x97d94dedU};
inline constexpr pinned::Word kSceneDrawSetStride{"add x24,x24,#0x778 (draw-set stride)", 0x18c5204ULL, 0x911de318U};
// The words that carry the rest of section 2: the scene and the mode the walk keeps, the mesh index i and its loop, the mesh address
// (the madd that makes [scene+0x30] + i*0x268), the draw-set index k and its loop, the prim index term and the three call arguments.
inline constexpr pinned::Word kSceneSceneReg{"mov x19,x0 (x19 = the scene)", 0x18c5160ULL, 0xaa0003f3U};
inline constexpr pinned::Word kSceneModeReg{"mov w20,w1 (w20 = the mode)", 0x18c5164ULL, 0x2a0103f4U};
inline constexpr pinned::Word kSceneMeshIndexZero{"mov x23,xzr (mesh index i starts at 0)", 0x18c5168ULL, 0xaa1f03f7U};
inline constexpr pinned::Word kSceneMeshAddr{"madd x28,x23,x27,x8 (mesh i = [scene+0x30] + i*0x268)", 0x18c517cULL, 0x9b1b22fcU};
inline constexpr pinned::Word kSceneMeshNext{"add x23,x23,#0x1 (next mesh)", 0x18c5188ULL, 0x910006f7U};
inline constexpr pinned::Word kSceneMeshCompare{"cmp x23,x22 (against the scene's mesh count)", 0x18c518cULL, 0xeb1602ffU};
inline constexpr pinned::Word kSceneMeshLoopEnd{"b.eq (every mesh done)", 0x18c5190ULL, 0x54000400U};
inline constexpr pinned::Word kSceneDrawSetIndexZero{"mov x24,xzr (draw set k starts at 0)", 0x18c51b4ULL, 0xaa1f03f8U};
inline constexpr pinned::Word kScenePrimIndexScale{"mul x9,x9,x26 (first prim index * 0x70)", 0x18c51dcULL, 0x9b1a7d29U};
inline constexpr pinned::Word kScenePrimIndexAdd{"add x8,x8,x9 (the prim array + that offset)", 0x18c51e0ULL, 0x8b090108U};
inline constexpr pinned::Word kSceneModeArg{"mov w1,w20 (arg 2: the mode)", 0x18c51f0ULL, 0x2a1403e1U};
inline constexpr pinned::Word kSceneFormatArg{"mov x2,x21 (arg 3: the format)", 0x18c51f4ULL, 0xaa1503e2U};
inline constexpr pinned::Word kSceneDrawSetArg{"add x0,x8,x24 (arg 1: draw set k)", 0x18c51f8ULL, 0x8b180100U};
inline constexpr pinned::Word kSceneDrawSetCountDown{"subs x29,x29,#0x1 (draw sets left in the mesh)", 0x18c5200ULL, 0xf10007bdU};
inline constexpr pinned::Word kSceneDrawSetLoop{"b.ne (back to the draw-set call while draw sets are left)", 0x18c5208ULL, 0x54ffff01U};
inline constexpr pinned::Word kSceneSkipEmptyBranch{"cbz x29 (a mesh whose draw-set count is 0: to the next mesh)", 0x18c51a4ULL, 0xb4ffff3dU};
inline constexpr pinned::Word kSceneDrawSetLoopExit{"b (the draw-set loop is done: to the next mesh)", 0x18c520cULL, 0x17ffffdfU};

inline constexpr std::uintptr_t kSceneMeshCountOffset = pinned::LdrXOffset(kSceneMeshCount.word);
inline constexpr std::uintptr_t kSceneMeshesOffset = pinned::LdrXOffset(kSceneMeshes.word);
inline constexpr std::uintptr_t kSceneDataOffset = pinned::LdrXOffset(kSceneData.word);
inline constexpr std::uintptr_t kMeshStride = pinned::MovImmediate(kSceneMeshStride.word);
inline constexpr std::uintptr_t kMeshDrawSetsOffset = pinned::LdrXOffset(kSceneDrawSets.word);
inline constexpr std::uintptr_t kMeshDrawSetCountOffset = pinned::LdrXOffset(kSceneDrawSetCount.word);
inline constexpr std::uintptr_t kDrawSetStride = pinned::AddImmediate(kSceneDrawSetStride.word);
inline constexpr std::uintptr_t kDataRecordsOffset = pinned::LdrXOffset(kSceneRecords.word);
inline constexpr std::uintptr_t kRecordStride = pinned::MovImmediate(kSceneRecordStride.word);
inline constexpr std::uintptr_t kRecordFormatIndexOffset = pinned::LdrWOffset(kSceneFormatIndex.word);
inline constexpr std::uintptr_t kRecordFirstPrimOffset = pinned::LdrWOffset(kSceneFirstPrim.word);
inline constexpr std::uintptr_t kDataFormatsOffset = pinned::LdrXOffset(kSceneFormats.word);
inline constexpr std::uintptr_t kFormatEntryStride = pinned::MovImmediate(kSceneFormatStride.word);
inline constexpr std::uintptr_t kDataPrimsOffset = pinned::LdrXOffset(kScenePrims.word);
inline constexpr std::uintptr_t kPrimStride = pinned::MovImmediate(kScenePrimStride.word);
inline constexpr std::uintptr_t kPrimFieldOffset = pinned::AddImmediate(kScenePrimField.word);
static_assert(kSceneMeshCountOffset == 0x60 && kSceneMeshesOffset == 0x30 && kSceneDataOffset == 0x20);
static_assert(kMeshStride == 0x268 && kMeshDrawSetsOffset == 0x8 && kMeshDrawSetCountOffset == 0x38 && kDrawSetStride == 0x778);
static_assert(kDataRecordsOffset == 0x70 && kRecordStride == 0x98 && kRecordFormatIndexOffset == 0x18 && kRecordFirstPrimOffset == 0x34);
static_assert(kDataFormatsOffset == 0xe0 && kFormatEntryStride == 0x150 && kDataPrimsOffset == 0xa8);
static_assert(kPrimStride == 0x70 && kPrimFieldOffset == 0x40);
// The post-index of `ldr w3,[x25],#0x70` (imm9 in bits 12-20, the field SturOffset reads) is the prim stride: one entry per draw set.
static_assert(pinned::SturOffset(kScenePrimRead.word) == kPrimStride);

// The CGDrawDataSet::SetOutlineMode PLT stub the walk calls (its slot is kPins.drawSetSlot).
inline constexpr std::uint64_t kDrawSetStubVaddr = 0xf189b0ULL;
inline constexpr pinned::Word kDrawSetStubAdrp{"stub: adrp x16,0x36e3000", kDrawSetStubVaddr, 0xf0013e50U};
inline constexpr pinned::Word kDrawSetStubLdr{"stub: ldr x17,[x16,#0xbc0]", 0xf189b4ULL, 0xf945e211U};

// --- The merged format in CGMesh::Initialize (section 3) --------------------------------------------------------------------------

inline constexpr const char* kMeshInitializeSymbol = "_ZN10NRadEngine6CGMesh10InitializeERKNS_10SGMeshInitE";
inline constexpr std::uint64_t kMeshInitializeVaddr = 0x1857ab8ULL;
inline constexpr pinned::Word kMeshSkinnedRead{"ldr w8,[x23,#0xd8] (mesh compute word)", 0x1858300ULL, 0xb940dae8U};
inline constexpr std::uint64_t kMeshMergeSkipVaddr = 0x1858338ULL;  // where both `cbz` branch to (the test decodes them): past the Bind call
inline constexpr pinned::Word kMeshSkinnedTest{"cbz w8 (zero: branch past the Bind call)", 0x1858308ULL, 0x34000188U};
inline constexpr pinned::Word kMeshComputeFormatArg{"add x0,x23,#0xf0 (mesh compute format)", 0x185830cULL, 0x9103c2e0U};
inline constexpr pinned::Word kMeshCountUsedCall{"bl 0xf41210 (CountUsed stub)", 0x1858310ULL, 0x97dba3c0U};
inline constexpr pinned::Word kMeshCountUsedTest{"cbz x0 (empty compute format: branch past the Bind call)", 0x1858314ULL, 0xb4000120U};
inline constexpr pinned::Word kMeshComputeVbArg{"add x0,x23,#0x40 (mesh compute vertex buffer)", 0x1858318ULL, 0x910102e0U};
inline constexpr pinned::Word kMeshBindCall{"bl 0xf08d30 (CGComputeVertexBuffer::Bind stub)", 0x1858324ULL, 0x97dac283U};
inline constexpr pinned::Word kMeshDrawSetsArg{"add x0,x23,#0x8 (draw-set table, kept at sp+0x60)", 0x18582dcULL, 0x910022e0U};
inline constexpr pinned::Word kMeshStoreAt260{"str wzr,[x23,#0x260] (a store inside the scene's mesh stride)", 0x18582f8ULL, 0xb90262ffU};
inline constexpr pinned::Word kMeshThisMove{"mov x23,x0 (this)", 0x1857ae0ULL, 0xaa0003f7U};
inline constexpr pinned::Word kMeshThisSave{"str x23,[sp,#0x78] (this kept)", 0x1857e98ULL, 0xf9003ff7U};
inline constexpr pinned::Word kMeshThisReload{"ldr x23,[sp,#0x78] (this reloaded)", 0x1858044ULL, 0xf9403ff7U};
inline constexpr pinned::Word kMeshBindResultArg{"add x8,sp,#0x98 (Bind's indirect result)", 0x185831cULL, 0x910263e8U};
inline constexpr pinned::Word kMeshDrawSetInitBufArg{"add x1,sp,#0x98 (CGDrawDataSet::Initialize's x1)", 0x1858674ULL, 0x910263e1U};
inline constexpr pinned::Word kMeshInitDrawSetStride{"mov w9,#0x778 (draw-set stride)", 0x1858670ULL, 0x5280ef09U};
inline constexpr pinned::Word kMeshDrawSetInitCall{"bl 0xf4bbf0 (CGDrawDataSet::Initialize stub, x1 = the bound buffer)", 0x185868cULL, 0x97dbcd59U};

inline constexpr std::uintptr_t kMeshSkinnedOffset = pinned::LdrWOffset(kMeshSkinnedRead.word);
inline constexpr std::uintptr_t kMeshComputeFormatOffset = pinned::AddImmediate(kMeshComputeFormatArg.word);
inline constexpr std::uintptr_t kMeshComputeVbOffset = pinned::AddImmediate(kMeshComputeVbArg.word);
static_assert(kMeshSkinnedOffset == 0xd8 && kMeshComputeFormatOffset == 0xf0 && kMeshComputeVbOffset == 0x40);
static_assert(pinned::AddImmediate(kMeshDrawSetsArg.word) == kMeshDrawSetsOffset);
static_assert(pinned::MovImmediate(kMeshInitDrawSetStride.word) == kDrawSetStride);
static_assert(pinned::LdrWOffset(kMeshStoreAt260.word) + 4 <= kMeshStride);
static_assert(pinned::AddImmediate(kMeshBindResultArg.word) == 0x98 && pinned::AddImmediate(kMeshDrawSetInitBufArg.word) == 0x98);
// The buffer Bind writes its result to and the buffer the draw-set Initialize call is given are the same offset in the frame.

// The PLT stubs CGMesh::Initialize calls, each with the JUMP_SLOT it reads and the symbol that slot's relocation names.
struct Stub {
  const char* symbol;
  std::uint64_t vaddr;      // the stub's `adrp x16` (its first instruction, the `bl` target)
  std::uint32_t adrpWord;
  std::uint32_t ldrWord;    // `ldr x17,[x16,#off]` at vaddr + 4
  std::uint64_t slotVaddr;  // the JUMP_SLOT word the two instructions name
};
inline constexpr Stub kStubs[] = {
    {"_ZNK10NRadEngine14CGVertexFormat9CountUsedEv", 0xf41210ULL, 0xd0013db0U, 0xf947fa11U, 0x36f7ff0ULL},
    {"_ZNK10NRadEngine21CGComputeVertexBuffer4BindERKNS_14CGVertexBufferE", 0xf08d30ULL, 0xf0013e90U, 0xf946c211U, 0x36dbd80ULL},
    {"_ZN10NRadEngine13CGDrawDataSet10InitializeERKNS_14SGDrawDataInitERKNS0_18SGDrawDataSettingsE", 0xf4bbf0ULL, 0xd0013d90U,
     0xf9427211U, 0x36fd4e0ULL},
    {kDrawSetSymbol, kDrawSetStubVaddr, kDrawSetStubAdrp.word, kDrawSetStubLdr.word, 0x36e3bc0ULL},
    {"_ZNK10NRadEngine14CGVertexFormat10GetElementEyRNS0_14SVertexElementE", 0xee12a0ULL, 0xf0013f30U, 0xf9401e11U, 0x36c8038ULL},
    {"_ZNK10NRadEngine14CGVertexFormat10GetElementENS0_6EUsageERNS0_14SVertexElementE", 0xf1f910ULL, 0x90013e50U, 0xf941ba11U, 0x36e7370ULL},
    {"_ZN10NRadEngine14CGVertexFormat13AppendElementERKNS0_14SVertexElementE", 0xf153b0ULL, 0xb0013e70U, 0xf9406211U, 0x36e20c0ULL},
    {"memcpy", 0xf476e0ULL, 0x90013db0U, 0xf9412e11U, 0x36fb258ULL},
    {"_ZN10NRadEngine8CModelCS14SetOutlineModeENS_18SCompactPoolHandleENS_16EMeshOutlineModeE", 0xf1e490ULL, 0x90013e50U, 0xf9449a11U,
     0x36e6930ULL},
};
// The index of each stub in kStubs.
inline constexpr std::size_t kStubCountUsed = 0, kStubBind = 1, kStubDrawSetInit = 2, kStubDrawSetSetMode = 3, kStubGetElementIndex = 4,
                             kStubGetElementUsage = 5, kStubAppend = 6, kStubMemcpy = 7, kStubModelSetMode = 8;

// A pinned `bl` and the stub (an index of kStubs) it must reach.
struct Call {
  pinned::Word site;
  std::size_t stub;
};

// --- The merge rule, CGComputeVertexBuffer::Bind (section 4) ----------------------------------------------------------------------

inline constexpr const char* kBindSymbol = "_ZNK10NRadEngine21CGComputeVertexBuffer4BindERKNS_14CGVertexBufferE";
inline constexpr std::uint64_t kBindVaddr = 0x25bdbacULL;
inline constexpr pinned::Word kBindComputeFormat{"add x23,x23,#0xb0 (compute format)", 0x25bdce0ULL, 0x9102c2f7U};
inline constexpr pinned::Word kBindThisMove{"mov x23,x0 (this)", 0x25bdbccULL, 0xaa0003f7U};
inline constexpr pinned::Word kBindInputMove{"mov x22,x1 (the input buffer)", 0x25bdbd8ULL, 0xaa0103f6U};
inline constexpr pinned::Word kBindResultMove{"mov x19,x8 (the indirect result)", 0x25bdbdcULL, 0xaa0803f3U};
inline constexpr pinned::Word kBindElementThis{"mov x0,x22 (GetElement(u64) on the input)", 0x25bdd20ULL, 0xaa1603e0U};
inline constexpr pinned::Word kBindElementIndex{"mov x1,x24 (the element index)", 0x25bdd24ULL, 0xaa1803e1U};
inline constexpr pinned::Word kBindElementArg{"add x2,sp,#0x10 (the element buffer)", 0x25bdd1cULL, 0x910043e2U};
inline constexpr pinned::Word kBindStreamRead{"ldrb w8,[sp,#0x16] (element stream, byte 6 of sp+0x10)", 0x25bdd2cULL, 0x39405be8U};
inline constexpr pinned::Word kBindStreamTest{"cmp w8,#0x1 (stream 1)", 0x25bdd30ULL, 0x7100051fU};
inline constexpr pinned::Word kBindStreamBranch{"b.ne (other stream: append as is)", 0x25bdd34ULL, 0x54fffe41U};
inline constexpr pinned::Word kBindUsageRead{"ldrb w8,[sp,#0x10] (element usage, byte 0)", 0x25bdd38ULL, 0x394043e8U};
inline constexpr pinned::Word kBindUsageTest{"cmp w8,#0xb (usage above 11: append as is)", 0x25bdd3cULL, 0x71002d1fU};
inline constexpr pinned::Word kBindUsageBranch{"b.hi (usage above 11: append as is)", 0x25bdd40ULL, 0x54fffde8U};
inline constexpr pinned::Word kBindTableLoad{"ldrb w10,[x26,x8] (the table byte of the usage)", 0x25bdd48ULL, 0x38686b4aU};
inline constexpr pinned::Word kBindCaseAdd{"add x9,x9,x10,lsl #2 (the case)", 0x25bdd4cULL, 0x8b0a0929U};
inline constexpr pinned::Word kBindCaseBranch{"br x9 (the case)", 0x25bdd50ULL, 0xd61f0120U};
inline constexpr pinned::Word kBindCaseFormatArg{"mov x0,x23 (GetElement(EUsage) on the compute format)", 0x25bdcf4ULL, 0xaa1703e0U};
inline constexpr pinned::Word kBindAppendDst{"add x0,sp,#0x18 (AppendElement on the merged format)", 0x25bdcfcULL, 0x910063e0U};
inline constexpr pinned::Word kBindAppendSrc{"add x1,sp,#0x10 (the element appended)", 0x25bdd00ULL, 0x910043e1U};
inline constexpr pinned::Word kBindCopySrc{"add x1,sp,#0x18 (the merged format copied)", 0x25bdd94ULL, 0x910063e1U};
inline constexpr pinned::Word kBindCopyDst{"mov x0,x19 (copied to the result)", 0x25bdd9cULL, 0xaa1303e0U};
inline constexpr Call kBindCalls[] = {
    {{"bl GetElement(u64) stub", 0x25bdd28ULL, 0x97a48d5eU}, kStubGetElementIndex},
    {{"bl GetElement(EUsage) stub", 0x25bdcf8ULL, 0x97a58706U}, kStubGetElementUsage},
    {{"bl AppendElement stub", 0x25bdd04ULL, 0x97a55dabU}, kStubAppend},
    {{"bl memcpy stub (the merged format copied whole)", 0x25bdda0ULL, 0x97a62650U}, kStubMemcpy},
};
inline constexpr pinned::Word kBindCaseBaseAdr{"adr x9,#-0x58 (case base 0x25bdcec)", 0x25bdd44ULL, 0x10fffd49U};
inline constexpr pinned::Word kBindTablePage{"adrp x26,0x31f1000 (jump table page)", 0x25bdcd8ULL, 0x900061baU};
inline constexpr pinned::Word kBindTableOffset{"add x26,x26,#0x9f8 (jump table)", 0x25bdce4ULL, 0x9127e35aU};
inline constexpr pinned::Word kBindCopySize{"mov w2,#0x128 (merged format copied whole)", 0x25bdd98ULL, 0x52802502U};
inline constexpr std::uint64_t kBindCaseBase = 0x25bdcecULL;
inline constexpr std::uint64_t kBindAppendAsIs = kBindAppendDst.vaddr;  // `add x0,sp,#0x18`, then AppendElement of the element as read
inline constexpr std::uint64_t kBindJumpTable = 0x31f19f8ULL;
inline constexpr std::size_t kBindJumpTableSize = 12;  // one byte per usage 0..11
// The table's 12 bytes as file data: bytes 0-7 (a Quad, little-endian) and 8-11 (a Word).
inline constexpr pinned::Quad kBindJumpTableLow{"Bind jump table bytes 0-7 (usages 0..7)", kBindJumpTable, 0x04040404211e041aULL};
inline constexpr pinned::Word kBindJumpTableHigh{"Bind jump table bytes 8-11 (usages 8..11)", kBindJumpTable + 8, 0x00040404U};

static_assert(pinned::AddImmediate(kBindComputeFormat.word) + kMeshComputeVbOffset == kMeshComputeFormatOffset);
// The element is the 8 bytes at sp+0x10 (the buffer GetElement(u64) fills and AppendElement reads); its stream and usage bytes are the
// loads' offsets from it.
inline constexpr std::uintptr_t kElementUsageByte = pinned::LdrbOffset(kBindUsageRead.word) - pinned::AddImmediate(kBindElementArg.word);
inline constexpr std::uintptr_t kElementStreamByte = pinned::LdrbOffset(kBindStreamRead.word) - pinned::AddImmediate(kBindElementArg.word);
static_assert(kElementUsageByte == 0 && kElementStreamByte == 6);
static_assert(pinned::AddImmediate(kBindAppendSrc.word) == pinned::AddImmediate(kBindElementArg.word));  // the element appended is the one read
static_assert(pinned::AddImmediate(kBindCopySrc.word) == pinned::AddImmediate(kBindAppendDst.word));    // the format copied is the one appended to
static_assert(pinned::AddImmediate(kBindTableOffset.word) == (kBindJumpTable & 0xfff));

// The usages the merge replaces with the compute format's element: the case the table sends each one to, and the `mov w1` there
// that names the usage GetElement looks up.
struct MergeCase {
  std::uint8_t usage;
  std::uint64_t caseVaddr;
  pinned::Word usageArg;
};
inline constexpr MergeCase kMergeCases[] = {
    {0, 0x25bdd54ULL, {"mov w1,wzr (usage 0)", 0x25bdd5cULL, 0x2a1f03e1U}},
    {2, 0x25bdd64ULL, {"mov w1,#0x2 (usage 2)", 0x25bdd68ULL, 0x52800041U}},
    {3, 0x25bdd70ULL, {"mov w1,#0x3 (usage 3)", 0x25bdd74ULL, 0x52800061U}},
    {11, 0x25bdcecULL, {"mov w1,#0xb (usage 11)", 0x25bdcf0ULL, 0x52800161U}},
};
static_assert(pinned::MovImmediate(kMergeCases[1].usageArg.word) == 2 && pinned::MovImmediate(kMergeCases[2].usageArg.word) == 3 &&
              pinned::MovImmediate(kMergeCases[3].usageArg.word) == 11);
inline constexpr std::uint8_t kMergeStream = 1;
inline constexpr std::uint8_t kMaxCaseUsage = 11;
static_assert(pinned::AddImmediate(kBindStreamTest.word) == kMergeStream && pinned::AddImmediate(kBindUsageTest.word) == kMaxCaseUsage);

// --- CGVertexFormat (section 5) ---------------------------------------------------------------------------------------------------

inline constexpr std::size_t kVertexFormatSize = pinned::MovImmediate(kBindCopySize.word);
inline constexpr pinned::Word kCountUsedRead{"CountUsed: ldr x0,[x0,#0x120]", 0x2809438ULL, 0xf9409000U};
inline constexpr pinned::Word kCtorUnusedUsage{"C1: mov w8,#0xc (usage of an empty element)", 0x2809098ULL, 0x52800188U};
inline constexpr pinned::Word kCtorLastElement{"C1: strb w8,[x0,#0x118] (element 35)", 0x2809254ULL, 0x39046008U};
inline constexpr pinned::Word kCtorCountZero{"C1: str xzr,[x0,#0x120]", 0x2809260ULL, 0xf900901fU};
inline constexpr pinned::Word kGetElementIndex{"GetElement(u64): ldr x8,[x0,x1,lsl #3]", 0x28094a8ULL, 0xf8617808U};
inline constexpr pinned::Word kGetElementUsageCount{"GetElement(EUsage): ldr x8,[x0,#0x120]", 0x28094bcULL, 0xf9409008U};
inline constexpr pinned::Word kGetElementUsageByte{"GetElement(EUsage): ldrb w9,[x0] (usage byte 0)", 0x28094c4ULL, 0x39400009U};
inline constexpr pinned::Word kAppendCount{"AppendElement: ldr x8,[x0,#0x120]", 0x280a4a8ULL, 0xf9409008U};
inline constexpr pinned::Word kAppendStore{"AppendElement: str x9,[x0,x8,lsl #3] (no bound check)", 0x280a4b0ULL, 0xf8287809U};
inline constexpr pinned::Word kAppendCountStore{"AppendElement: str x9,[x0,#0x120]", 0x280a4bcULL, 0xf9009009U};

inline constexpr std::uintptr_t kVertexFormatCountOffset = pinned::LdrXOffset(kCountUsedRead.word);
inline constexpr std::size_t kVertexElementSize = 8;
inline constexpr std::size_t kVertexFormatCapacity = pinned::LdrbOffset(kCtorLastElement.word) / kVertexElementSize + 1;
inline constexpr std::uint8_t kUnusedUsage = static_cast<std::uint8_t>(pinned::MovImmediate(kCtorUnusedUsage.word));
static_assert(kVertexFormatSize == 0x128 && kVertexFormatCountOffset == 0x120);
static_assert(kVertexFormatCountOffset + 8 == kVertexFormatSize);
static_assert(kVertexFormatCapacity == 36 && kVertexFormatCapacity * kVertexElementSize == kVertexFormatCountOffset);
static_assert(pinned::LdrXOffset(kCtorCountZero.word) == kVertexFormatCountOffset &&
              pinned::LdrXOffset(kGetElementUsageCount.word) == kVertexFormatCountOffset &&
              pinned::LdrXOffset(kAppendCount.word) == kVertexFormatCountOffset &&
              pinned::LdrXOffset(kAppendCountStore.word) == kVertexFormatCountOffset);
static_assert(kUnusedUsage == 0xc);
static_assert(pinned::LdrbOffset(kGetElementUsageByte.word) == kElementUsageByte);  // GetElement(EUsage) tests the same usage byte

// The five functions the module calls, and the two near namesakes it must not.
inline constexpr pinned::Symbol kHelpers[] = {
    {"_ZN10NRadEngine14CGVertexFormatC1Ev", 0x2809098ULL},
    {"_ZNK10NRadEngine14CGVertexFormat9CountUsedEv", 0x2809438ULL},
    {"_ZNK10NRadEngine14CGVertexFormat10GetElementEyRNS0_14SVertexElementE", 0x28094a8ULL},
    {"_ZNK10NRadEngine14CGVertexFormat10GetElementENS0_6EUsageERNS0_14SVertexElementE", 0x28094bcULL},
    {"_ZN10NRadEngine14CGVertexFormat13AppendElementERKNS0_14SVertexElementE", 0x280a4a8ULL},
};
inline constexpr pinned::Symbol kNotHelpers[] = {
    {"_ZNK10NRadEngine14CGVertexFormat10GetElementENS0_6EUsageEyRNS0_14SVertexElementE", 0x28094f4ULL},
    {"_ZN10NRadEngine14CGVertexFormat13AppendElementENS0_6EUsageENS0_5ETypeEyyyj", 0x280a4c8ULL},
};

// --- The modes (section 6) --------------------------------------------------------------------------------------------------------

inline constexpr std::uint32_t kModeTeamUpstream = 7;      // upstream's label; the binary shows only that 7 is below the compare's 13
inline constexpr std::uint32_t kModeScannerUpstream = 12;  // upstream's label; the binary shows only that 12 is below the compare's 13
inline constexpr pinned::Word kInitOutlineModeRead{"InitOutlineDraw: ldr w21,[x1,#0x68] (a u32 of the second argument)", 0x1839514ULL, 0xb9406835U};
inline constexpr pinned::Word kInitOutlineModeTest{"InitOutlineDraw: cmp w21,#0xd (the bound 13)", 0x1839518ULL, 0x710036bfU};
inline constexpr pinned::Word kInitOutlineModeBranch{"InitOutlineDraw: b.lo (taken when the u32 is below 13)", 0x183951cULL, 0x54000243U};
inline constexpr pinned::Word kOutlinePoolAdrp{"OutlineRenderPool: adrp x8,0x2d6e000", 0x183b21cULL, 0xf000a988U};
inline constexpr pinned::Word kOutlinePoolTable{"OutlineRenderPool: add x8,x8,#0x59c (pool table)", 0x183b220ULL, 0x91167108U};
inline constexpr pinned::Word kOutlinePoolLoad{"OutlineRenderPool: ldr w0,[x8,w0,uxtw #2] (table[mode])", 0x183b224ULL, 0xb8605900U};
inline constexpr pinned::Word kOutlinePoolRet{"OutlineRenderPool: ret", 0x183b228ULL, 0xd65f03c0U};
inline constexpr std::uint64_t kOutlineRenderPoolSize = 16;  // its .dynsym size: the four words above and nothing else
inline constexpr std::uint64_t kOutlinePoolTableVaddr = 0x2d6e59cULL;
inline constexpr pinned::Word kOutlinePoolMode7{"OutlineRenderPool table[7] = pool 0x2d (data)", kOutlinePoolTableVaddr + 4 * 7, 0x2dU};
inline constexpr pinned::Word kOutlinePoolMode12{"OutlineRenderPool table[12] = pool 0x32 (data)", kOutlinePoolTableVaddr + 4 * 12, 0x32U};
inline constexpr pinned::Word kNodeDataMove{"CModelSetOutlineModeNode::Enter: mov x19,x1 (the node data)", 0x2648034ULL, 0xaa0103f3U};
inline constexpr pinned::Word kNodeModeRead{"CModelSetOutlineModeNode::Enter: ldr w19,[x19,#0x20] (u32 at node data +0x20)", 0x26480b0ULL,
                                            0xb9402273U};
inline constexpr pinned::Word kNodeModeArg{"CModelSetOutlineModeNode::Enter: mov w2,w19 (the mode argument)", 0x26480c4ULL, 0x2a1303e2U};
inline constexpr Call kNodeSetModeCall = {{"CModelSetOutlineModeNode::Enter: bl CModelCS::SetOutlineMode stub", 0x26480c8ULL, 0x97a358f2U},
                                          kStubModelSetMode};
inline constexpr std::uintptr_t kNodeModeOffset = pinned::LdrWOffset(kNodeModeRead.word);
static_assert(kNodeModeOffset == 0x20);
// The bound InitOutlineDraw compares the u32 at +0x68 with (not established to be a count of modes).
inline constexpr std::uint32_t kOutlineModeBound = static_cast<std::uint32_t>(pinned::AddImmediate(kInitOutlineModeTest.word));
static_assert(kOutlineModeBound == 13 && kModeTeamUpstream < kOutlineModeBound && kModeScannerUpstream < kOutlineModeBound);
static_assert(pinned::AddImmediate(kOutlinePoolTable.word) == (kOutlinePoolTableVaddr & 0xfff));

inline constexpr const char* kInitOutlineDrawSymbol =
    "_ZN10NRadEngine13CGDrawDataSet15InitOutlineDrawERKNS_14SGDrawDataInitERKNS0_18SGDrawDataSettingsERNS_10CGDrawDataERh";
inline constexpr const char* kOutlineRenderPoolSymbol = "_ZN10NRadEngine10CGRenderer17OutlineRenderPoolENS_16EMeshOutlineModeE";
inline constexpr const char* kNodeEnterSymbol = "_ZN10NRadEngine24CModelSetOutlineModeNode5EnterERKNS_28SModelSetOutlineModeNodeDataE";

// The functions the facts above are read in, by .dynsym name.
inline constexpr pinned::Symbol kFunctions[] = {
    {kMeshInitializeSymbol, kMeshInitializeVaddr},
    {"_ZN10NRadEngine6CGMesh25InitializeComputeSkinningEjRKNS_14CGVertexFormatEPyPKNS_8CGBufferEPKNS_11CGBoneCacheEj", 0x18631c0ULL},
    {kBindSymbol, kBindVaddr},
    {"_ZN10NRadEngine6CGMesh14SetOutlineModeENS_16EMeshOutlineModeERKNS_17SGMeshRuntimeDataE", 0x18641e4ULL},
    {kInitOutlineDrawSymbol, 0x18394fcULL},
    {kOutlineRenderPoolSymbol, 0x183b21cULL},
    {kNodeEnterSymbol, 0x2647ff8ULL},
};

// Every word the test checks against the file (the merge-case `mov`s are checked from kMergeCases, the stubs from kStubs, the calls from
// kBindCalls and kNodeSetModeCall).
inline constexpr pinned::Word kWords[] = {
    kSceneMeshCount, kScenePrimStride, kSceneMeshStride, kSceneMeshes, kSceneDrawSetCount, kSceneSkipEmpty, kSceneData,
    kSceneRecordStride, kSceneFormatStride, kSceneRecords, kSceneFormats, kScenePrims, kSceneRecordMadd, kSceneFormatIndex,
    kSceneFirstPrim, kSceneFormatMadd, kScenePrimField, kScenePrimRead, kSceneDrawSets, kSceneDrawSetCall, kSceneDrawSetStride,
    kSceneSceneReg, kSceneModeReg, kSceneMeshIndexZero, kSceneMeshAddr, kSceneMeshNext, kSceneMeshCompare, kSceneMeshLoopEnd,
    kSceneDrawSetIndexZero, kScenePrimIndexScale, kScenePrimIndexAdd, kSceneModeArg, kSceneFormatArg, kSceneDrawSetArg,
    kSceneDrawSetCountDown, kSceneDrawSetLoop, kSceneSkipEmptyBranch, kSceneDrawSetLoopExit,
    kMeshSkinnedRead, kMeshSkinnedTest, kMeshComputeFormatArg, kMeshCountUsedCall, kMeshCountUsedTest, kMeshComputeVbArg,
    kMeshBindCall, kMeshDrawSetsArg, kMeshStoreAt260, kMeshInitDrawSetStride, kMeshDrawSetInitCall, kMeshThisMove, kMeshThisSave,
    kMeshThisReload, kMeshBindResultArg, kMeshDrawSetInitBufArg,
    kBindComputeFormat, kBindStreamRead, kBindStreamTest, kBindStreamBranch, kBindUsageRead, kBindUsageTest, kBindCaseBaseAdr,
    kBindTablePage, kBindTableOffset, kBindCopySize, kBindJumpTableHigh, kBindThisMove, kBindInputMove, kBindResultMove,
    kBindElementThis, kBindElementIndex, kBindElementArg, kBindUsageBranch, kBindTableLoad, kBindCaseAdd, kBindCaseBranch,
    kBindCaseFormatArg, kBindAppendDst, kBindAppendSrc, kBindCopySrc, kBindCopyDst,
    kCountUsedRead, kCtorUnusedUsage, kCtorLastElement, kCtorCountZero, kGetElementIndex, kGetElementUsageCount,
    kGetElementUsageByte, kAppendCount, kAppendStore, kAppendCountStore,
    kInitOutlineModeRead, kInitOutlineModeTest, kInitOutlineModeBranch, kOutlinePoolAdrp, kOutlinePoolTable, kOutlinePoolLoad,
    kOutlinePoolRet, kOutlinePoolMode7, kOutlinePoolMode12, kNodeDataMove, kNodeModeRead, kNodeModeArg,
};
inline constexpr pinned::Quad kQuads[] = {kBindJumpTableLow};

}  // namespace nevr_quest::combat::outline_mode::targets

/* The pinned facts of the outline-mode module (#1640 and #1656; the module itself is not in the tree yet): the two SetOutlineMode JUMP_SLOTs, the
 * scene walk the module replaces, the field that says whether a mesh's draw sets were built with a merged vertex format, the merge rule
 * the game applies there, and the five CGVertexFormat functions the module calls to build that format. Behaviour is in the upstream reference
 * (kotorvr/echoquest-combat at c5a67e8, runtime/native/afkxr/afk_fx.cpp: fxoutlinefix2 and olnoteam, around lines 5267-5345 and 8152-8202).
 *
 * Every address is an ELF virtual address in the store libr15 (its build ID is kLibR15StockBuildId in pinned_targets.h). The
 * disassembly's address is the ELF address plus 0x100000 (ReVault project echovr, binary libr15.so). Every word below was read from the
 * store file with llvm-objdump; src/quest/tests/outline_mode_pinned_test.cpp checks each one, each relocation, each .dynsym value and each
 * decode named here against the real file (`just test-quest-hooks-pinned`).
 *
 * Sources. The facts below are instruction words, relocations and .dynsym entries read from the store file. The byte-checked
 * reconstructions (ReVault project echovr, binary libr15.so) agree where they exist: CGVertexFormat C1 (VA 0x2909098: 36 elements of
 * 8 bytes with usage 0xc, the count at +0x120 zeroed, 0x128 bytes), GetElement(EUsage) (VA 0x29094bc), AppendElement(const
 * SVertexElement&) (VA 0x290a4a8: no bound check), CGMesh::SetOutlineMode (VA 0x19641e4: sets at +0x8, count at +0x38, stride 0x778,
 * prim at +0x40 of 0x70-byte entries) and CModelCS::SetOutlineMode (VA 0x16b3dd8: the CGScene it calls is at owner+0xa0, the call is a
 * tail call to CGScene::SetOutlineMode). CGScene::SetOutlineMode, CGDrawDataSet::SetOutlineMode, CountUsed, GetElement(u64),
 * CGMesh::Initialize and CGComputeVertexBuffer::Bind have no byte-checked reconstruction yet; what is said of them here is read
 * from their instruction words only.
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
 * 0x18c51fc), whose `adrp`/`ldr` read the JUMP_SLOT 0x36e3bc0. The same format is passed for every draw set of every mesh.
 * The 0x150 stride is the stride of the array at d+0xe0; that the CGVertexFormat sits at offset 0 of each 0x150-byte entry is what the
 * walk passes (the entry's address), not a size of CGVertexFormat.
 * The mesh is a CGMesh: CGMesh::Initialize (ELF 0x1857ab8) builds its draw sets at [this+0x8] + k*0x778 (`str x0,[sp,#0x60]` with
 * x0 = this+0x8 at 0x18582e0, `mov w9,#0x778` at 0x1858670), and CGMesh::SetOutlineMode (ELF 0x18641e4) walks the same two fields
 * (count [this+0x38], sets [this+0x8], stride 0x778); the scene's stride 0x268 is larger than the last field Initialize writes
 * (`str wzr,[x23,#0x260]` at 0x18582f8).
 *
 * 3. The merged format. CGMesh::Initialize (x23 = this) runs, at ELF 0x1858300..0x1858324:
 *   if (u32[this+0xd8] != 0 && CGVertexFormat::CountUsed(this+0xf0) != 0)
 *       vb = CGComputeVertexBuffer::Bind(this+0x40, <the mesh's vertex buffer>);   // the result replaces the buffer copy
 * and the buffer copy (sp+0x98) is what each draw set is initialized with (`bl` at 0x185868c, through the stub at 0xf4bbf0 to
 * CGDrawDataSet::Initialize, x1 = sp+0x98). So a mesh with u32[+0xd8] != 0 and a non-empty format at +0xf0 has draw sets built with the
 * merged format, and the scene walk (2) passes them the unmerged one. This is the test the module applies per mesh ("skinned" in
 * upstream's word): the same two reads, in the same order.
 * The compute vertex buffer is at mesh+0x40 and its format at +0xb0 in it (`add x23,x23,#0xb0` at 0x25bdce0, the format Bind reads),
 * so mesh+0xf0 = 0x40 + 0xb0. CGMesh::InitializeComputeSkinning (ELF 0x18631c0) hands this+0x40 to SComputeSkinningEntry::Initialize as
 * its compute vertex buffer (`add x6,x20,#0x40` at 0x1863268).
 * Not established: which function writes u32[mesh+0xd8] (u32 at +0x98 of the compute vertex buffer), and so what its nonzero values
 * mean beyond the gate above. Read: the store at offset 0xd8 is not in CGMesh::Initialize or InitializeComputeSkinning.
 *
 * 4. The merge rule, CGComputeVertexBuffer::Bind(CGVertexBuffer const&) (ELF 0x25bdbac, size 668; x22 = the input buffer, whose
 * CGVertexFormat is at offset 0; x23 = this + 0xb0, the compute format). It constructs an empty format (CGVertexFormat C1 through the
 * stub 0xf2ad40), then for each element i below CountUsed(input) (GetElement(u64, SVertexElement&) at 0x25bdd28):
 *   - stream (element byte 6, `ldrb w8,[sp,#0x16]` at 0x25bdd2c, the element at sp+0x10) != 1, or usage (byte 0, 0x25bdd38) > 11:
 *     the element is appended as it is (AppendElement(const SVertexElement&) at 0x25bdd04);
 *   - else the jump table at ELF 0x31f19f8 (12 bytes, one per usage 0..11, case = 0x25bdcec + 4*byte, `adr` at 0x25bdd44) sends
 *     usages 0, 2, 3 and 11 to GetElement(EUsage, SVertexElement&) on the compute format with that usage (the `mov w1` at 0x25bdd5c,
 *     0x25bdd68, 0x25bdd74, 0x25bdcf0), which overwrites the element only when the compute format has that usage (its return is not
 *     tested), then appends; usages 1 and 4..10 are appended as they are.
 * The merged format is then copied whole, 0x128 bytes (`mov w2,#0x128` at 0x25bdd98, memcpy), to offset 0 of the result.
 *
 * 5. CGVertexFormat. 36 elements of 8 bytes from offset 0, then the u64 element count at +0x120: sizeof 0x128. The constructor writes
 * usage 0xc into each of the 36 elements (the last at +0x118, 0x2809254) and zeroes the count (`str xzr,[x0,#0x120]` at 0x2809260);
 * CountUsed returns [this+0x120]; GetElement(u64) copies element i (`ldr x8,[x0,x1,lsl #3]`); GetElement(EUsage) finds the first element
 * whose byte 0 is the usage and copies it (returns 1), else returns 0; AppendElement(const SVertexElement&) stores at the count and
 * increments it with no bound check (0x280a4a8..0x280a4bc): a caller that appends to a full format writes past +0x120.
 * The five functions the module calls, by .dynsym name (kHelpers); not their near namesakes GetElement(EUsage, u64, SVertexElement&)
 * at 0x28094f4 and AppendElement(EUsage, EType, u64, u64, u64, u32) at 0x280a4c8 (kNotHelpers, pinned so a swap is caught).
 *
 * 6. The modes. EMeshOutlineMode has 13 values: CGDrawDataSet::InitOutlineDraw (ELF 0x18394fc) refuses a mode >= 13 with
 * "Invalid outline mode %u" (`cmp w21,#0xd; b.lo` at 0x1839518), and CGRenderer::OutlineRenderPool (ELF 0x183b21c) maps a mode through
 * the u32 table at ELF 0x2d6e59c (mode 7 -> pool 0x2d, mode 12 -> pool 0x32). So 7 and 12 are valid modes.
 * Not established from the binary: that 7 is the teammate outline and 12 the scanner outline. Those labels are upstream's (afk_fx.cpp).
 * Read: CModelSetOutlineModeNode::Enter (ELF 0x2647ff8) takes the mode from its node data, u32[data+0x20] (`ldr w19,[x19,#0x20]` at
 * 0x26480b0), and calls CModelCS::SetOutlineMode with it; the script libraries that use the node (MODELSETOUTLINEMODENODE in six Quest
 * script libraries, e.g. lib037a773d9ce35387.so execute_node_333e361e, ReVault VA 0x10d11c) copy that data from the script's data
 * block (memcpy of 0x18 bytes from [[thread]+0x68]+0x98), so the value is in the level's script data, not in code. No literal 7 or 12
 * reaches the node in libr15 or in the script code read.
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
inline constexpr pinned::Word kMeshSkinnedTest{"cbz w8 (zero: no merge)", 0x1858308ULL, 0x34000188U};
inline constexpr pinned::Word kMeshComputeFormatArg{"add x0,x23,#0xf0 (mesh compute format)", 0x185830cULL, 0x9103c2e0U};
inline constexpr pinned::Word kMeshCountUsedCall{"bl 0xf41210 (CountUsed stub)", 0x1858310ULL, 0x97dba3c0U};
inline constexpr pinned::Word kMeshCountUsedTest{"cbz x0 (empty compute format: no merge)", 0x1858314ULL, 0xb4000120U};
inline constexpr pinned::Word kMeshComputeVbArg{"add x0,x23,#0x40 (mesh compute vertex buffer)", 0x1858318ULL, 0x910102e0U};
inline constexpr pinned::Word kMeshBindCall{"bl 0xf08d30 (CGComputeVertexBuffer::Bind stub)", 0x1858324ULL, 0x97dac283U};
inline constexpr pinned::Word kMeshDrawSetsArg{"add x0,x23,#0x8 (draw-set table, kept at sp+0x60)", 0x18582dcULL, 0x910022e0U};
inline constexpr pinned::Word kMeshLastField{"str wzr,[x23,#0x260]", 0x18582f8ULL, 0xb90262ffU};
inline constexpr pinned::Word kMeshInitDrawSetStride{"mov w9,#0x778 (draw-set stride)", 0x1858670ULL, 0x5280ef09U};
inline constexpr pinned::Word kMeshDrawSetInitCall{"bl 0xf4bbf0 (CGDrawDataSet::Initialize stub, x1 = the bound buffer)", 0x185868cULL, 0x97dbcd59U};

inline constexpr std::uintptr_t kMeshSkinnedOffset = pinned::LdrWOffset(kMeshSkinnedRead.word);
inline constexpr std::uintptr_t kMeshComputeFormatOffset = pinned::AddImmediate(kMeshComputeFormatArg.word);
inline constexpr std::uintptr_t kMeshComputeVbOffset = pinned::AddImmediate(kMeshComputeVbArg.word);
static_assert(kMeshSkinnedOffset == 0xd8 && kMeshComputeFormatOffset == 0xf0 && kMeshComputeVbOffset == 0x40);
static_assert(pinned::AddImmediate(kMeshDrawSetsArg.word) == kMeshDrawSetsOffset);
static_assert(pinned::MovImmediate(kMeshInitDrawSetStride.word) == kDrawSetStride);
static_assert(pinned::LdrWOffset(kMeshLastField.word) + 4 <= kMeshStride);

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
};

// --- The merge rule, CGComputeVertexBuffer::Bind (section 4) ----------------------------------------------------------------------

inline constexpr const char* kBindSymbol = "_ZNK10NRadEngine21CGComputeVertexBuffer4BindERKNS_14CGVertexBufferE";
inline constexpr std::uint64_t kBindVaddr = 0x25bdbacULL;
inline constexpr pinned::Word kBindComputeFormat{"add x23,x23,#0xb0 (compute format)", 0x25bdce0ULL, 0x9102c2f7U};
inline constexpr pinned::Word kBindStreamRead{"ldrb w8,[sp,#0x16] (element stream, byte 6 of sp+0x10)", 0x25bdd2cULL, 0x39405be8U};
inline constexpr pinned::Word kBindStreamTest{"cmp w8,#0x1 (stream 1)", 0x25bdd30ULL, 0x7100051fU};
inline constexpr pinned::Word kBindStreamBranch{"b.ne (other stream: append as is)", 0x25bdd34ULL, 0x54fffe41U};
inline constexpr pinned::Word kBindUsageRead{"ldrb w8,[sp,#0x10] (element usage, byte 0)", 0x25bdd38ULL, 0x394043e8U};
inline constexpr pinned::Word kBindUsageTest{"cmp w8,#0xb (usage above 11: append as is)", 0x25bdd3cULL, 0x71002d1fU};
inline constexpr pinned::Word kBindCaseBaseAdr{"adr x9,#-0x58 (case base 0x25bdcec)", 0x25bdd44ULL, 0x10fffd49U};
inline constexpr pinned::Word kBindTablePage{"adrp x26,0x31f1000 (jump table page)", 0x25bdcd8ULL, 0x900061baU};
inline constexpr pinned::Word kBindTableOffset{"add x26,x26,#0x9f8 (jump table)", 0x25bdce4ULL, 0x9127e35aU};
inline constexpr pinned::Word kBindCopySize{"mov w2,#0x128 (merged format copied whole)", 0x25bdd98ULL, 0x52802502U};
inline constexpr std::uint64_t kBindCaseBase = 0x25bdcecULL;
inline constexpr std::uint64_t kBindAppendAsIs = 0x25bdcfcULL;  // `add x0,sp,#0x18`, then AppendElement of the element as read
inline constexpr std::uint64_t kBindJumpTable = 0x31f19f8ULL;
inline constexpr std::size_t kBindJumpTableSize = 12;  // one byte per usage 0..11
// The table's 12 bytes as file data: bytes 0-7 (a Quad, little-endian) and 8-11 (a Word).
inline constexpr pinned::Quad kBindJumpTableLow{"Bind jump table bytes 0-7 (usages 0..7)", kBindJumpTable, 0x04040404211e041aULL};
inline constexpr pinned::Word kBindJumpTableHigh{"Bind jump table bytes 8-11 (usages 8..11)", kBindJumpTable + 8, 0x00040404U};

static_assert(pinned::AddImmediate(kBindComputeFormat.word) + kMeshComputeVbOffset == kMeshComputeFormatOffset);
static_assert(pinned::LdrbOffset(kBindStreamRead.word) - pinned::LdrbOffset(kBindUsageRead.word) == 6);
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
inline constexpr std::size_t kElementUsageByte = 0;
inline constexpr std::size_t kElementStreamByte = 6;
static_assert(kVertexFormatSize == 0x128 && kVertexFormatCountOffset == 0x120);
static_assert(kVertexFormatCountOffset + 8 == kVertexFormatSize);
static_assert(kVertexFormatCapacity == 36 && kVertexFormatCapacity * kVertexElementSize == kVertexFormatCountOffset);
static_assert(pinned::LdrXOffset(kCtorCountZero.word) == kVertexFormatCountOffset &&
              pinned::LdrXOffset(kGetElementUsageCount.word) == kVertexFormatCountOffset &&
              pinned::LdrXOffset(kAppendCount.word) == kVertexFormatCountOffset &&
              pinned::LdrXOffset(kAppendCountStore.word) == kVertexFormatCountOffset);
static_assert(kUnusedUsage == 0xc);

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

inline constexpr std::uint32_t kModeTeamUpstream = 7;      // upstream's label; the binary shows only that 7 is a valid mode
inline constexpr std::uint32_t kModeScannerUpstream = 12;  // upstream's label; the binary shows only that 12 is a valid mode
inline constexpr pinned::Word kInitOutlineModeRead{"InitOutlineDraw: ldr w21,[x1,#0x68] (mode)", 0x1839514ULL, 0xb9406835U};
inline constexpr pinned::Word kInitOutlineModeTest{"InitOutlineDraw: cmp w21,#0xd (13 modes)", 0x1839518ULL, 0x710036bfU};
inline constexpr pinned::Word kInitOutlineModeBranch{"InitOutlineDraw: b.lo (valid mode)", 0x183951cULL, 0x54000243U};
inline constexpr pinned::Word kOutlinePoolTable{"OutlineRenderPool: add x8,x8,#0x59c (pool table)", 0x183b220ULL, 0x91167108U};
inline constexpr std::uint64_t kOutlinePoolTableVaddr = 0x2d6e59cULL;
inline constexpr pinned::Word kOutlinePoolMode7{"OutlineRenderPool table[7] = pool 0x2d (data)", kOutlinePoolTableVaddr + 4 * 7, 0x2dU};
inline constexpr pinned::Word kOutlinePoolMode12{"OutlineRenderPool table[12] = pool 0x32 (data)", kOutlinePoolTableVaddr + 4 * 12, 0x32U};
inline constexpr pinned::Word kNodeModeRead{"CModelSetOutlineModeNode::Enter: ldr w19,[x19,#0x20] (mode from node data)", 0x26480b0ULL,
                                            0xb9402273U};
inline constexpr std::uint32_t kOutlineModeCount = static_cast<std::uint32_t>(pinned::AddImmediate(kInitOutlineModeTest.word));
static_assert(kOutlineModeCount == 13 && kModeTeamUpstream < kOutlineModeCount && kModeScannerUpstream < kOutlineModeCount);
static_assert(pinned::AddImmediate(kOutlinePoolTable.word) == (kOutlinePoolTableVaddr & 0xfff));

// The functions the facts above are read in, by .dynsym name.
inline constexpr pinned::Symbol kFunctions[] = {
    {kMeshInitializeSymbol, kMeshInitializeVaddr},
    {kBindSymbol, kBindVaddr},
    {"_ZN10NRadEngine6CGMesh14SetOutlineModeENS_16EMeshOutlineModeERKNS_17SGMeshRuntimeDataE", 0x18641e4ULL},
    {"_ZN10NRadEngine13CGDrawDataSet15InitOutlineDrawERKNS_14SGDrawDataInitERKNS0_18SGDrawDataSettingsERNS_10CGDrawDataERh", 0x18394fcULL},
    {"_ZN10NRadEngine10CGRenderer17OutlineRenderPoolENS_16EMeshOutlineModeE", 0x183b21cULL},
    {"_ZN10NRadEngine24CModelSetOutlineModeNode5EnterERKNS_28SModelSetOutlineModeNodeDataE", 0x2647ff8ULL},
};

// Every word the test checks against the file (the merge-case `mov`s are checked from kMergeCases, the stubs from kStubs).
inline constexpr pinned::Word kWords[] = {
    kSceneMeshCount, kScenePrimStride, kSceneMeshStride, kSceneMeshes, kSceneDrawSetCount, kSceneSkipEmpty, kSceneData,
    kSceneRecordStride, kSceneFormatStride, kSceneRecords, kSceneFormats, kScenePrims, kSceneRecordMadd, kSceneFormatIndex,
    kSceneFirstPrim, kSceneFormatMadd, kScenePrimField, kScenePrimRead, kSceneDrawSets, kSceneDrawSetCall, kSceneDrawSetStride,
    kMeshSkinnedRead, kMeshSkinnedTest, kMeshComputeFormatArg, kMeshCountUsedCall, kMeshCountUsedTest, kMeshComputeVbArg,
    kMeshBindCall, kMeshDrawSetsArg, kMeshLastField, kMeshInitDrawSetStride, kMeshDrawSetInitCall,
    kBindComputeFormat, kBindStreamRead, kBindStreamTest, kBindStreamBranch, kBindUsageRead, kBindUsageTest, kBindCaseBaseAdr,
    kBindTablePage, kBindTableOffset, kBindCopySize, kBindJumpTableHigh,
    kCountUsedRead, kCtorUnusedUsage, kCtorLastElement, kCtorCountZero, kGetElementIndex, kGetElementUsageCount,
    kGetElementUsageByte, kAppendCount, kAppendStore, kAppendCountStore,
    kInitOutlineModeRead, kInitOutlineModeTest, kInitOutlineModeBranch, kOutlinePoolTable, kOutlinePoolMode7, kOutlinePoolMode12,
    kNodeModeRead,
};
inline constexpr pinned::Quad kQuads[] = {kBindJumpTableLow};

}  // namespace nevr_quest::combat::outline_mode::targets

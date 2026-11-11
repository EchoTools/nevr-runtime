/* The pinned table words of the texel-buffer guard (the module that U5 adds): the Vulkan function table that libr15 calls through,
 * and the fill stores that write its slots. Checked against the store libr15 by src/quest/tests/texel_buffer_guard_pinned_test.cpp
 * (the hooks-pinned group, which needs the store APK).
 *
 * Addresses. Every address is an ELF virtual address in the store libr15 (build id in pinned_targets.h, `kLibR15StockBuildId`).
 * The disassembly and ReVault (project echovr, binary libr15.so, base 0x100000) use ELF + 0x100000; each read below gives its VA.
 *
 * The table. The word at ELF 0x370f360 (VA 0x380f360) is an R_AARCH64_GLOB_DAT relocation for NRadEngine::gContext, which is a
 * 0xb80-byte object in .bss (ELF 0x3775488). The file holds zero there: every slot is filled at run time, by CGS::Initialize
 * (ELF 0x18dc9d4, VA 0x19dc9d4, 14884 bytes). The fill reads the table pointer from that word (`adrp x29,0x370f000` at ELF
 * 0x18dca84, `ldr x29,[x29,#0x360]` at ELF 0x18dca88, VA 0x19dca84 and 0x19dca88) and, for each entry point, does
 *   ldr x8,[x29,#0x88] (vkGetDeviceProcAddr); ldr x0,[x29,#0xb50] (the device); adrp/add the name; blr x8; str x0,[x29,#slot]
 * The name of each slot is the string its fill loads (the adrp/add before the blr).
 *
 * The slots this guard uses (slot = byte offset in the table; the fill store is the one instruction that writes it):
 *   0x290 vkCreateDescriptorSetLayout  fill store ELF 0x18de8b4 (VA 0x19de8b4), word 0xf9014ba0
 *   0x2b8 vkAllocateDescriptorSets     fill store ELF 0x18de940 (VA 0x19de940), word 0xf9015fa0
 *   0x2c8 vkUpdateDescriptorSets       fill store ELF 0x18de978 (VA 0x19de978), word 0xf90167a0
 * The table's other slots that the guard may name are in the same fill: 0x258 vkCreateGraphicsPipelines (VA 0x19de7f0), 0x2e0
 * vkCreateRenderPass (VA 0x19de9cc), 0x390 vkCmdBindDescriptorSets (VA 0x19dec34), 0x398 vkCmdBindIndexBuffer (VA 0x19dec50),
 * 0x480 vkCmdBeginRenderPass (VA 0x19def7c), 0x1e0 vkCreateBuffer (VA 0x19de64c).
 *
 * When the table is filled. The normal step (src/quest/combat/README.md, "When a module installs") runs at the first non-null dlopen
 * the game makes. INFERENCE: that is before android_main runs, because android_app_entry (VA 0x1079994) calls android_main
 * (VA 0x10784c4) only after the library is loaded. So the table is not yet filled when the normal step runs. CGS::Initialize
 * (VA 0x19dc9d4) is called from Initialize (VA 0x2bbfd00). Not established: the caller of Initialize at VA 0x2bbfd00 (ReVault's
 * measured call graph has no caller edge for it, indirect or inferred), so the step that reaches the fill is open. The fill needs
 * the device at gctx+0xb50. A guard that uses a slot installs after the fill has run; the trigger is U5's design.
 *
 * Manager operation. The table words are .bss words written at run time: a checked data-slot write (F06) is the operation for them,
 * and this header pins them (the address lies in .bss, and the store instruction that writes each one is named with its encoding).
 *
 * Drift from the upstream reference (kotorvr/echoquest-combat at c5a67e8), recorded here, not carried as fact. Upstream's comments
 * name its libr15 build "4911ef7f"; the store file this header is pinned to has build id b243509c08ce677aeb95fa348016949b3fc45230
 * (sha256 8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20, which ReVault's libr15.so also has). The ELF offsets
 * upstream gives for the table slots match this file.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/pinned_words.h"

namespace nevr_quest::combat::texel_buffer_guard::targets {

inline constexpr const char* kGctxSymbol = "_ZN10NRadEngine8gContextE";
// The GOT word whose relocation is R_AARCH64_GLOB_DAT for kGctxSymbol (readelf -rW: 0000000000370f360 R_AARCH64_GLOB_DAT 0x3775488).
inline constexpr std::uint64_t kGctxWordVaddr = 0x370f360ULL;
// The object the word names: st_value and st_size of kGctxSymbol in the store file. It lies in .bss (.bss 0x3761180, size 0x713b8).
inline constexpr std::uint64_t kGctxObjectVaddr = 0x3775488ULL;
inline constexpr std::uint64_t kGctxObjectSize = 0xb80ULL;

// Slot offsets in the table, each the byte offset from the gctx object (the device word, the function pointer word and the slot word
// are all inside the object: 0xb50 < 0xb80).
inline constexpr std::uintptr_t kSlotVkCreateDescriptorSetLayout = 0x290;
inline constexpr std::uintptr_t kSlotVkAllocateDescriptorSets = 0x2b8;
inline constexpr std::uintptr_t kSlotVkUpdateDescriptorSets = 0x2c8;
inline constexpr std::uintptr_t kSlotDevice = 0xb50;

// The instruction that reads the gctx word into x29 (CGS::Initialize, ELF 0x18dca88; the adrp is at 0x18dca84).
inline constexpr pinned::Word kLoadGctxBase{"ldr x29,[x29,#0x360] (CGS::Initialize reads the gctx word)", 0x18dca88ULL, 0xf941b3bdU};

// The fill stores, one per slot: the instruction that writes the slot (`str x0,[x29,#slot]`), named with its encoding.
inline constexpr pinned::Word kFillCreateDescriptorSetLayout{"str x0,[x29,#0x290] (vkCreateDescriptorSetLayout)", 0x18de8b4ULL, 0xf9014ba0U};
inline constexpr pinned::Word kFillAllocateDescriptorSets{"str x0,[x29,#0x2b8] (vkAllocateDescriptorSets)", 0x18de940ULL, 0xf9015fa0U};
inline constexpr pinned::Word kFillUpdateDescriptorSets{"str x0,[x29,#0x2c8] (vkUpdateDescriptorSets)", 0x18de978ULL, 0xf90167a0U};

// Every word the test checks against the file.
inline constexpr pinned::Word kWords[] = {kLoadGctxBase, kFillCreateDescriptorSetLayout, kFillAllocateDescriptorSets,
                                          kFillUpdateDescriptorSets};

}  // namespace nevr_quest::combat::texel_buffer_guard::targets

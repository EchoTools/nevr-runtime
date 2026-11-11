/* The pinned table words of the texel-buffer guard (texel_buffer_guard.h): the Vulkan function table that libr15 calls through, the fill
 * stores that write its slots, and the lookup chain that fills them. Checked against the store libr15 by src/quest/tests/texel_buffer_guard_pinned_test.cpp
 * (the hooks-pinned group, which needs the store APK).
 *
 * Addresses. Every address is an ELF virtual address in the store libr15 (build id in pinned_targets.h, `kLibR15StockBuildId`).
 * The disassembly and ReVault (project echovr, binary libr15.so, base 0x100000) use ELF + 0x100000; each read below gives its VA.
 *
 * The table. The word at ELF 0x370f360 (VA 0x380f360) is an R_AARCH64_GLOB_DAT relocation for NRadEngine::gContext, which is a
 * 0xb80-byte object in .bss (ELF 0x3775488). The file holds zero there. The three slots this guard uses are written at run time by the fill stores below, all in
 * CGS::Initialize (ELF 0x18dc9d4, VA 0x19dc9d4, 14884 bytes); that no other instruction writes them is not shown (a store through another register
 * that holds the table pointer would not be found by reading these stores). The fill reads the table pointer from that word (`adrp x29,0x370f000` at ELF
 * 0x18dca84, `ldr x29,[x29,#0x360]` at ELF 0x18dca88, VA 0x19dca84 and 0x19dca88) and, for each entry point, does
 *   ldr x8,[x29,#0x88] (vkGetDeviceProcAddr); ldr x0,[x29,#0xb50] (the device); adrp/add the name; blr x8; str x0,[x29,#slot]
 * The name of each slot is the string its fill loads (the adrp/add before the blr); the test decodes the pairs of the two slots the guard wraps and
 * reads their strings (below); the others are read from the file by hand.
 * Not established: the light-UV texel binding of shader 407e0930 (set 0, bindings 3 and 4); nothing here reads that shader.
 *
 * The slots this guard uses (slot = byte offset in the table; the fill store is the one instruction that writes it):
 *   0x290 vkCreateDescriptorSetLayout  fill store ELF 0x18de8b4 (VA 0x19de8b4), word 0xf9014ba0
 *   0x2b8 vkAllocateDescriptorSets     fill store ELF 0x18de940 (VA 0x19de940), word 0xf9015fa0
 *   0x2c8 vkUpdateDescriptorSets       fill store ELF 0x18de978 (VA 0x19de978), word 0xf90167a0
 * The table's other slots that the guard may name are in the same fill: 0x258 vkCreateGraphicsPipelines (VA 0x19de7f0), 0x2e0
 * vkCreateRenderPass (VA 0x19de9cc), 0x390 vkCmdBindDescriptorSets (VA 0x19dec34), 0x398 vkCmdBindIndexBuffer (VA 0x19dec50),
 * 0x480 vkCmdBeginRenderPass (VA 0x19def7c), 0x1e0 vkCreateBuffer (VA 0x19de64c).
 *
 * How the guard reaches the slots. The fill takes every entry point from the game's own vkGetDeviceProcAddr, which it took from
 * vkGetInstanceProcAddr, which it took from CSysModule::FindMethod; the words below pin that chain:
 *   ELF 0x18dca9c adrp x1 / 0x18dcaa0 add x1  -> the string "vkGetInstanceProcAddr" at ELF 0x2c6009d
 *   ELF 0x18dcaa4 bl 0xf08620                 -> the PLT stub whose adrp x16 (0xf08620) and ldr x17,[x16,#0x9f8] (0xf08624) read the
 *                                                JUMP_SLOT 0x36db9f8, CSysModule::FindMethod's: the slot vk_ext_window hooks
 *                                                (pinned_targets.h, LibR15FindMethod)
 *   ELF 0x18dcaa8 str x0,[x29]                -> the table's word 0 holds what FindMethod answered (vkGetInstanceProcAddr)
 *   ELF 0x18dd1bc ldr x8,[x29]; 0x18dd1c4 adrp x1 / 0x18dd1c8 add x1 -> "vkGetDeviceProcAddr" at ELF 0x2c62455; blr x8 at 0x18dd1cc (x0 is
 *                                                [x29,#0x508], the instance)
 *   ELF 0x18dd1d0 str x0,[x29,#0x88]          -> the table's word 0x88 holds what vkGetInstanceProcAddr answered (vkGetDeviceProcAddr)
 *   ELF 0x18de8a0 ldr x8,[x29,#0x88]; 0x18de8a8 adrp x1 / 0x18de8ac add x1 -> "vkCreateDescriptorSetLayout" at ELF 0x2c63647; then the
 *                                                fill store of slot 0x290 above
 *   ELF 0x18de92c ldr x8,[x29,#0x88]; 0x18de934 adrp x1 / 0x18de938 add x1 -> "vkAllocateDescriptorSets" at ELF 0x2c63783; then the
 *                                                fill store of slot 0x2b8 above
 * So whatever vk_ext_window's FindMethod handler answers for vkGetInstanceProcAddr is what the fill calls for vkGetDeviceProcAddr, and
 * whatever that answers for the two names is what the game stores in the two slots: the guard is an input to vk_ext_window
 * (RequestDeviceFunction), which answers vkGetDeviceProcAddr with its own wrapper when a request is registered. No table word is written
 * by the sentinel. The request is registered in the combat area's early step, before install_vk_ext_window and before the game runs
 * CGS::Initialize, so when the table is filled does not matter: the game stores whatever its lookup returns. Not established: that
 * nothing else writes these slots after the fill (CGS::Initialize and InitCGS are not reconstructed).
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

// The two instructions that read the gctx word into x29 (CGS::Initialize): the adrp at ELF 0x18dca84 names the page 0x370f000, the ldr at
// 0x18dca88 adds 0x360; the test decodes both and checks that they name kGctxWordVaddr.
inline constexpr pinned::Word kLoadGctxPage{"adrp x29,0x370f000 (CGS::Initialize, the gctx word's page)", 0x18dca84ULL, 0xf000f19dU};
inline constexpr pinned::Word kLoadGctxBase{"ldr x29,[x29,#0x360] (CGS::Initialize reads the gctx word)", 0x18dca88ULL, 0xf941b3bdU};

// The fill stores, one per slot: the instruction that writes the slot (`str x0,[x29,#slot]`), named with its encoding.
inline constexpr pinned::Word kFillCreateDescriptorSetLayout{"str x0,[x29,#0x290] (vkCreateDescriptorSetLayout)", 0x18de8b4ULL, 0xf9014ba0U};
inline constexpr pinned::Word kFillAllocateDescriptorSets{"str x0,[x29,#0x2b8] (vkAllocateDescriptorSets)", 0x18de940ULL, 0xf9015fa0U};
inline constexpr pinned::Word kFillUpdateDescriptorSets{"str x0,[x29,#0x2c8] (vkUpdateDescriptorSets)", 0x18de978ULL, 0xf90167a0U};

// The lookup chain (the header's "How the guard reaches the slots"). Word 0 of the table is vkGetInstanceProcAddr, word 0x88
// vkGetDeviceProcAddr.
inline constexpr std::uintptr_t kSlotGetInstanceProcAddr = 0x0;
inline constexpr std::uintptr_t kSlotGetDeviceProcAddr = 0x88;
// The JUMP_SLOT the FindMethod call goes through: vk_ext_window's slot (pinned_targets.h, LibR15FindMethod); the test compares the two.
inline constexpr std::uint64_t kFindMethodSlotVaddr = 0x36db9f8ULL;
inline constexpr std::uint64_t kFindMethodStubVaddr = 0xf08620ULL;

inline constexpr pinned::Word kGipaNamePage{"adrp x1,0x2c60000 (CGS::Initialize, the name FindMethod is asked for)", 0x18dca9cULL, 0x90009c21U};
inline constexpr pinned::Word kGipaNameOffset{"add x1,x1,#0x9d (\"vkGetInstanceProcAddr\")", 0x18dcaa0ULL, 0x91027421U};
inline constexpr pinned::Word kFindMethodCall{"bl 0xf08620 (CSysModule::FindMethod's PLT stub)", 0x18dcaa4ULL, 0x97d8aedfU};
inline constexpr pinned::Word kStoreGipa{"str x0,[x29] (the table's vkGetInstanceProcAddr)", 0x18dcaa8ULL, 0xf90003a0U};
inline constexpr pinned::Word kFindMethodStubPage{"adrp x16,0x36db000 (FindMethod's PLT stub)", 0xf08620ULL, 0xf0013e90U};
inline constexpr pinned::Word kFindMethodStubLoad{"ldr x17,[x16,#0x9f8] (FindMethod's PLT stub reads its JUMP_SLOT)", 0xf08624ULL, 0xf944fe11U};
inline constexpr pinned::Word kLoadGipa{"ldr x8,[x29] (the fill calls the table's vkGetInstanceProcAddr)", 0x18dd1bcULL, 0xf94003a8U};
inline constexpr pinned::Word kGdpaNamePage{"adrp x1,0x2c62000 (the name vkGetInstanceProcAddr is asked for)", 0x18dd1c4ULL, 0xb0009c21U};
inline constexpr pinned::Word kGdpaNameOffset{"add x1,x1,#0x455 (\"vkGetDeviceProcAddr\")", 0x18dd1c8ULL, 0x91115421U};
inline constexpr pinned::Word kStoreGdpa{"str x0,[x29,#0x88] (the table's vkGetDeviceProcAddr)", 0x18dd1d0ULL, 0xf90047a0U};
inline constexpr pinned::Word kLoadGdpaForCreateLayout{"ldr x8,[x29,#0x88] (the vkCreateDescriptorSetLayout fill)", 0x18de8a0ULL, 0xf94047a8U};
inline constexpr pinned::Word kCreateLayoutNamePage{"adrp x1,0x2c63000 (the vkCreateDescriptorSetLayout fill)", 0x18de8a8ULL, 0xb0009c21U};
inline constexpr pinned::Word kCreateLayoutNameOffset{"add x1,x1,#0x647 (\"vkCreateDescriptorSetLayout\")", 0x18de8acULL, 0x91191c21U};
inline constexpr pinned::Word kLoadGdpaForAllocate{"ldr x8,[x29,#0x88] (the vkAllocateDescriptorSets fill)", 0x18de92cULL, 0xf94047a8U};
inline constexpr pinned::Word kAllocateNamePage{"adrp x1,0x2c63000 (the vkAllocateDescriptorSets fill)", 0x18de934ULL, 0xb0009c21U};
inline constexpr pinned::Word kAllocateNameOffset{"add x1,x1,#0x783 (\"vkAllocateDescriptorSets\")", 0x18de938ULL, 0x911e0c21U};

// The names the chain loads, each a NUL-terminated string of the file's .rodata at its ELF address.
struct Name {
  const char* text;
  std::uint64_t vaddr;
};
inline constexpr Name kNameGetInstanceProcAddr{"vkGetInstanceProcAddr", 0x2c6009dULL};
inline constexpr Name kNameGetDeviceProcAddr{"vkGetDeviceProcAddr", 0x2c62455ULL};
inline constexpr Name kNameCreateDescriptorSetLayout{"vkCreateDescriptorSetLayout", 0x2c63647ULL};
inline constexpr Name kNameAllocateDescriptorSets{"vkAllocateDescriptorSets", 0x2c63783ULL};
inline constexpr Name kNames[] = {kNameGetInstanceProcAddr, kNameGetDeviceProcAddr, kNameCreateDescriptorSetLayout, kNameAllocateDescriptorSets};

// Every word the test checks against the file.
inline constexpr pinned::Word kWords[] = {kLoadGctxPage, kLoadGctxBase, kFillCreateDescriptorSetLayout, kFillAllocateDescriptorSets,
                                          kFillUpdateDescriptorSets, kGipaNamePage, kGipaNameOffset, kFindMethodCall, kStoreGipa,
                                          kFindMethodStubPage, kFindMethodStubLoad, kLoadGipa, kGdpaNamePage, kGdpaNameOffset, kStoreGdpa,
                                          kLoadGdpaForCreateLayout, kCreateLayoutNamePage, kCreateLayoutNameOffset, kLoadGdpaForAllocate,
                                          kAllocateNamePage, kAllocateNameOffset};

}  // namespace nevr_quest::combat::texel_buffer_guard::targets

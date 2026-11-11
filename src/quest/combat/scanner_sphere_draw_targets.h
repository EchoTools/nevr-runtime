/* The pinned draw site of the scanner sphere (the module that U6 adds): the place in the store libr15 where the eye pass draws its
 * pool-11 entries, the call that U6 follows with a second draw of pool 18, and the words that call goes through. Checked against the
 * store libr15 by src/quest/tests/scanner_sphere_draw_pinned_test.cpp (the hooks-pinned group, which needs the store APK).
 *
 * Addresses. Every address is an ELF virtual address in the store libr15 (build id in pinned_targets.h, `kLibR15StockBuildId`).
 * The disassembly and ReVault (project echovr, binary libr15.so, base 0x100000) use ELF + 0x100000; each read below gives its VA.
 *
 * The draw site. NRadEngine::CGRenderPassMainAndroid::Render (ELF 0x189e838, VA 0x199e838) calls
 * NRadEngine::CGRenderer::RenderDrawData<CGDrawData> through a PLT stub, once per pool. The pool is an immediate in the call's
 * argument setup; the pool-11 call is the one U6 follows:
 *   ELF 0x189f0e8 (VA 0x199f0e8)  mov w2,#0xb            the pool argument (ERenderPool 11)
 *   ELF 0x189f0f8 (VA 0x199f0f8)  bl 0xf46d40            the call (the thunk below)
 *   ELF 0x189f0fc (VA 0x199f0fc)  add x23,x22,#0x378     the instruction the call returns to
 * The argument registers at the call: x0 = x22 (the renderer), x1 = x20 (the SGThreadContext&), w2 = pool, x3 = x21 (the u64 the
 * frame passes in; Render's own fourth argument, x4 at entry).
 *
 * The stub. ELF 0xf46d40 (VA 0x1046d40): `adrp x16,0x36fa000` (ELF 0xf46d40, VA 0x1046d40) then `ldr x17,[x16,#0xd88]` (ELF 0xf46d44,
 * VA 0x1046d44), which reads the JUMP_SLOT word at ELF 0x36fad88 (VA 0x37fad88), then `br x17`. The relocation of that word is
 * R_AARCH64_JUMP_SLOT for kRenderDrawDataSymbol, whose value is ELF 0x1898b78 (the function, defined in this library).
 *
 * The function. ELF 0x1898b78 (VA 0x1998b78), size 320: its first instruction is `stp x25,x24,[sp,#-0x40]!`.
 *
 * The blend constants. The pool-18 draw sets the blend constants first, through the table at gctx+0x368 (vkCmdSetBlendConstants).
 * That slot is filled by CGS::Initialize (ELF 0x18deba8, VA 0x19deba8), which stores `str x0,[x29,#0x368]` (word 0xf901b7a0).
 * The gctx word and its object are the ones in texel_buffer_guard_targets.h (ELF 0x370f360, VA 0x380f360; object ELF 0x3775488).
 *
 * The draw of pool 18 itself is the eye-pass change U6 makes; it is not pinned here.
 *
 * Drift from the upstream reference (kotorvr/echoquest-combat at c5a67e8), recorded here, not carried as fact:
 *   1. The label of the word at ELF 0x3753258 (VA 0x3853258). Upstream (afk_fx.cpp:645) calls it "u32 frame slot MainAndroid indexes
 *      pools with (x 39 pools)". Measured: Render indexes its pool table with its own u64 argument, `mul x24,x4,#0x27` at VA
 *      0x199e864, not with a memory word. The word is read as s1 with s0 (`ldp w10,w9,[x29]`, x29 = 0x3853254) in the mesh loops at
 *      VA 0x199eab8 to 0x199f090, and it has 47 read xrefs and one write (VA 0x198bed4). The label is wrong for the pool-11 path.
 *   2. The kP18Frame index. Upstream afk_fx.cpp:669 reads that word as the frame index `fi` and takes the pool-18 entry at
 *      `fi * 39 + 18`, for its stat line. Because the word is s1, that stat reads the wrong pool entry. The frame key that the
 *      pool-11 call passes is the u64 in x3 at the call (x21 in Render), and a pool-18 draw made at the same site should pass the same.
 *   3. The build name. Upstream names its libr15 build "4911ef7f"; the file these pins are measured on has build id
 *      b243509c08ce677aeb95fa348016949b3fc45230 (see texel_buffer_guard_targets.h).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/pinned_words.h"

namespace nevr_quest::combat::scanner_sphere_draw::targets {

// The enclosing function: CGRenderPassMainAndroid::Render (ELF 0x189e838, VA 0x199e838), first instruction `sub sp,sp,#0xb0`.
inline constexpr std::uint64_t kRenderVaddr = 0x189e838ULL;
inline constexpr std::uint32_t kRenderFirstWord = 0xd102c3ffU;

// The pool-11 call: its argument and the call itself (see the header comment for their VAs).
inline constexpr std::uint64_t kPool11ArgVaddr = 0x189f0e8ULL;
inline constexpr std::uint32_t kPool11ArgWord = 0x52800162U;  // mov w2,#0xb
inline constexpr std::uint64_t kPool11CallVaddr = 0x189f0f8ULL;
inline constexpr std::uint32_t kPool11CallWord = 0x97da9f12U;  // bl 0xf46d40
inline constexpr std::uint64_t kPool11ReturnVaddr = 0x189f0fcULL;  // the call's return address: kPool11CallVaddr + 4
inline constexpr std::uint32_t kPool11ReturnWord = 0x910de2d7U;   // add x23,x22,#0x378
inline constexpr std::uint32_t kPool11Pool = 11;                 // ERenderPool value passed in w2

// The PLT stub the call goes through, and its JUMP_SLOT word.
inline constexpr std::uint64_t kThunkVaddr = 0xf46d40ULL;
inline constexpr std::uint32_t kThunkAdrpWord = 0x90013db0U;  // adrp x16,0x36fa000
inline constexpr std::uint64_t kThunkLdrVaddr = 0xf46d44ULL;
inline constexpr std::uint32_t kThunkLdrWord = 0xf946c611U;  // ldr x17,[x16,#0xd88]
inline constexpr std::uint64_t kRenderDrawDataSlotVaddr = 0x36fad88ULL;
inline constexpr const char* kRenderDrawDataSymbol =
    "_ZNK10NRadEngine10CGRenderer14RenderDrawDataINS_10CGDrawDataEEEvRNS_15SGThreadContextENS_12CGRenderPool11ERenderPoolEy";

// The function the JUMP_SLOT resolves to, and its first instruction (`stp x25,x24,[sp,#-0x40]!`).
inline constexpr std::uint64_t kRenderDrawDataVaddr = 0x1898b78ULL;
inline constexpr std::uint32_t kRenderDrawDataFirstWord = 0xa9bc63f9U;

// The gctx GOT word and the object it names (the same pins as texel_buffer_guard_targets.h, repeated so this header stands alone).
inline constexpr const char* kGctxSymbol = "_ZN10NRadEngine8gContextE";
inline constexpr std::uint64_t kGctxWordVaddr = 0x370f360ULL;
inline constexpr std::uint64_t kGctxObjectVaddr = 0x3775488ULL;
inline constexpr std::uint64_t kGctxObjectSize = 0xb80ULL;

// The blend-constants slot (gctx+0x368) and the instruction that fills it.
inline constexpr std::uintptr_t kSlotVkCmdSetBlendConstants = 0x368;
inline constexpr pinned::Word kFillBlendConstants{"str x0,[x29,#0x368] (vkCmdSetBlendConstants)", 0x18deba8ULL, 0xf901b7a0U};

// Every word the test checks against the file.
inline constexpr pinned::Word kWords[] = {
    {"Render entry: sub sp,sp,#0xb0", kRenderVaddr, kRenderFirstWord},
    {"mov w2,#0xb (pool 11)", kPool11ArgVaddr, kPool11ArgWord},
    {"bl the RenderDrawData stub", kPool11CallVaddr, kPool11CallWord},
    {"return site: add x23,x22,#0x378", kPool11ReturnVaddr, kPool11ReturnWord},
    {"stub: adrp x16,0x36fa000", kThunkVaddr, kThunkAdrpWord},
    {"stub: ldr x17,[x16,#0xd88]", kThunkLdrVaddr, kThunkLdrWord},
    {"RenderDrawData entry: stp x25,x24,[sp,#-0x40]!", kRenderDrawDataVaddr, kRenderDrawDataFirstWord},
    kFillBlendConstants,
};

}  // namespace nevr_quest::combat::scanner_sphere_draw::targets

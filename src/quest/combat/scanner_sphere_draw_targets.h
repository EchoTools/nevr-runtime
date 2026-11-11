/* The pinned draw site of the scanner sphere (the scanner-sphere module, not in the tree yet): the place in the store libr15 where the eye pass draws its
 * pool-11 entries, the call the module follows with a second draw of pool 18, and the words that call goes through; and the facts of the
 * game's own pool-18 draw that the module copies: the low-res transparents pass, the command buffer it records into, its blend constants, its
 * frame key, and how the game makes the pipelines a draw binds. Checked against the store libr15 by
 * src/quest/tests/scanner_sphere_draw_pinned_test.cpp (the hooks-pinned group, which needs the store APK).
 *
 * Addresses. Every address is an ELF virtual address in the store libr15 (build id in pinned_targets.h, `kLibR15StockBuildId`).
 * The disassembly and ReVault (project echovr, binary libr15.so, base 0x100000) use ELF + 0x100000; each read below gives its VA.
 *
 * The draw site. NRadEngine::CGRenderPassMainAndroid::Render (ELF 0x189e838, VA 0x199e838) calls
 * NRadEngine::CGRenderer::RenderDrawData<CGDrawData> through a PLT stub, once per pool. The pool is an immediate in the call's
 * argument setup; the pool-11 call is the one the module follows:
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
 * The function. ELF 0x1898b78 (VA 0x1998b78), size 320, the .dynsym value of kRenderDrawDataSymbol (the test reads it): its first instruction is `stp x25,x24,[sp,#-0x40]!`.
 *
 * The blend constants. Upstream's pool-18 draw sets the blend constants before it draws, through the table at gctx+0x368
 * (vkCmdSetBlendConstants); that is upstream's behaviour, not measured in this tree. Measured here: CGS::Initialize stores
 * `str x0,[x29,#0x368]` (ELF 0x18deba8, VA 0x19deba8, word 0xf901b7a0), the store whose offset is that slot.
 * The gctx word and its object are the ones in texel_buffer_guard_targets.h (ELF 0x370f360, VA 0x380f360; object ELF 0x3775488).
 *
 * The draw of pool 18 itself is the eye-pass change the module makes; it is not pinned here. What the game does when it draws pool 18 is:
 *
 * (c) The low-res transparents pass, the game's only pool-18 draw. NRadEngine::CGRenderPassLowresTransparents::Render (ELF 0x189c854,
 * VA 0x199c854, size 852, vtable slot ELF 0x365bb00) takes (this, const CGRenderer&, SGThreadContext&, CGSComputeContext*, u64) in x0..x4
 * and keeps the renderer in x24 (`mov x24,x1` ELF 0x189c87c), the thread context in x19 (`mov x19,x2` 0x189c878) and the u64 in x22
 * (`mov x22,x4` 0x189c88c). In order it calls DownscaleDepth, CGRenderTargetState::Begin, then:
 *   ELF 0x189c8c4  fmov v0.4s,#1.0              the four blend constants, each 1.0
 *   ELF 0x189c8c8  str q0,[sp,#0x10]            ... to a stack array
 *   ELF 0x189c8cc  ldrb w8,[x19,#0x260]         the thread context's command-buffer index (b, below)
 *   ELF 0x189c8d0  adrp x9,0x370f000 / 0x189c8d4 ldr x9,[x9,#0x360]   the gctx GOT word (kGctxWordVaddr)
 *   ELF 0x189c8d8  add x1,sp,#0x10              x1 = the constants
 *   ELF 0x189c8dc  add x8,x19,x8,lsl #3 / 0x189c8e0 ldr x0,[x8,#0x18]  x0 = the command buffer
 *   ELF 0x189c8e4  ldr x9,[x9,#0x368] / 0x189c8e8 blr x9               vkCmdSetBlendConstants(cmd, {1,1,1,1})
 *   ELF 0x189c8ec  mov w2,#0x12                 pool 18
 *   ELF 0x189c8f0  mov x0,x24 / 0x189c8f4 mov x1,x19 / 0x189c8f8 mov x3,x22
 *   ELF 0x189c8fc  bl 0xf46d40                  RenderDrawData<CGDrawData>(renderer, ctx, 18, key): the same stub as the eye pass's pool-11 call
 *   ELF 0x189c900  ldr x8,[x24,#0x1200]         the return site
 * So the frame key the pool-18 draw passes is the pass's own u64 argument, passed through unchanged, as the eye pass passes its own (x21,
 * its x4) to pool 11: a pool-18 draw made after the pool-11 call can pass x3 of that call. The constants are set on the command buffer
 * before the draw and are not put back after it (the pass goes on to draw pools 0x1d, 0x1f, 0x20, 0x1c, 0x1e through
 * CGDrawData::Render, then ends the render-target state). CGRenderPassMainAndroid::Render (ELF 0x189e838, size 2444) makes no load from
 * gctx+0x368 and no `fmov v0.4s,#1.0` in its 2444 bytes (scanned in the objdump listing): the eye pass never sets blend constants itself.
 *
 * Whether the pass runs on Quest. The pass's vtable (ELF 0x365ba90) is named by one GLOB_DAT word, ELF 0x3726128, and the .text loads that
 * word at three places only: its constructor (ELF 0x189b644, 0x189b60c + 0x38), D2 (0x189bfa0) and D0 (0x189c048). The constructor
 * (ELF 0x189b60c, size 2444) has no `b`/`bl` to it in .text and no relocation names it; its address occurs in the file only in .dynsym
 * (the C1 and C2 entries). The test makes the .text scans. Of the 1,041 binaries of ReVault project echovr only libr15.so carries the
 * constructor's name (a string search). So no code in the store libr15 constructs the pass, and pool 18 is never drawn on Quest unless the
 * module draws it (an inference from the scans; another library calling the exported constructor by address is ruled out only as far as
 * the name search goes).
 *
 * (b) The command buffer. NRadEngine::SGThreadContext::CmdBuffer() (ELF 0x1885a18, .dynsym, size 16) is
 * `ldrb w8,[x0,#0x260]; add x8,x0,x8,lsl #3; ldr x0,[x8,#0x18]; ret`: the current command buffer is the u64 at
 * ctx + 0x18 + 8 * u8[ctx + 0x260]. The low-res pass inlines the same three instructions (above). Upstream's field matches.
 *
 * (a) How a pool-18 pipeline is told apart: at pipeline creation it is not, by anything about the pool. The game makes every draw pipeline at
 * one place: the unnamed function at ELF 0x18da00c (first word `stp x28,x22,[sp,#-0x30]!`), called only from
 * CGStateCache::GetDrawPipelineStateIdx (`bl` at ELF 0x183c8b4) and CGStateCache::RegenerateDrawPSOs (`bl` at ELF 0x18da5ac); the .text has
 * no other `bl`/`b` to it. Its argument is an SGDrawPSODesc (GetDrawPipelineStateIdx hashes 0x340 bytes of it), and it calls
 * vkCreateGraphicsPipelines through the table, once per pipeline:
 *   ELF 0x18da3c0  adrp x15,0x370f000 / 0x18da3c4 ldr x15,[x15,#0x360]   the gctx word
 *   ELF 0x18da418  ldr x22,[x15,#0x258]          vkCreateGraphicsPipelines
 *   ELF 0x18da4ac  mov w2,#0x1                   createInfoCount 1
 *   ELF 0x18da4b8  blr x22
 * A scan of the listing for a load of gctx+0x258 from a register just loaded from the gctx word finds this one site in libr15 (the
 * game's only vkCreateGraphicsPipelines call; the scan is a pattern over the listing, kept in the unit's scratch, not in the test).
 * The colour-blend state of the pipeline is one of the game's blend states, chosen by the u32 at desc+0x2d8:
 *   ELF 0x18da034  ldr w10,[x0,#0x2d8] / 0x18da038 cmp w10,#0x19 (b.gt: no pipeline)   the blend mode, 0..25
 *   ELF 0x18da314  adrp x2,0x3718000 / 0x18da318 ldr x2,[x2,#0xc70]   GLOB_DAT CGRawInterface::blendstates
 *   ELF 0x18da324  mov w17,#0x558 / 0x18da334 madd x10,x10,x17,x2     &blendstates[mode], 0x558 bytes each
 * CGRawInterface::blendstates (ELF 0x3782b88, .bss, 35568 bytes = 26 * 0x558) is filled at run time by CGRawInterface::CreateBlendState
 * (ELF 0x188194c), which makes each entry a VkPipelineColorBlendStateCreateInfo (sType 0x1a, `mov w9,#0x1a` 0x18819bc; logicOp 0 and
 * attachmentCount 8, `mov x10,#0x800000000` 0x18819c0; pAttachments = entry + 0x158, `add x8,x1,#0x158` 0x18819b8) and fills eight
 * attachments, at a stride of 0x80 in the entry (as read; the stride of the attachments past the first is not checked against what the
 * driver reads, and the pipeline's attachmentCount comes from the subpass, not from 8). It translates the engine's SGBlendStateDesc per
 * render target (0x20 bytes from desc+8: enable, colour op, alpha op, src colour, dst colour, src alpha, dst alpha, write mask) through two
 * tables: factors at ELF 0x2d6dd38 (`add x11,x11,#0xd38` 0x1881960; engine factor n -> VkBlendFactor kBlendFactorTable[n]) and ops at
 * ELF 0x2d6dd84 (`add x12,x12,#0xd84` 0x1881964; engine op n -> VkBlendOp kBlendOpTable[n]). So the game's additive blend, engine factors
 * 1/1 with op 0, is VK_BLEND_FACTOR_ONE/ONE, VK_BLEND_OP_ADD.
 * What this means for the module: a pipeline is the game's per desc (blend mode, render pass, shaders, ...), made on demand the first time
 * a draw asks for it, and nothing in its create info names a pool. Upstream's create-info predicate (an opaque, depth-tested, not
 * depth-writing, multisampled, one-attachment pipeline becomes additive) is a guess at the pool-18 pipelines from their shape; the game
 * gives no field that confirms it.
 *
 * (d) The two table fills in CGS::Initialize, each through the game's vkGetDeviceProcAddr (`ldr x8,[x29,#0x88]`) with the device:
 *   vkCreateGraphicsPipelines (gctx+0x258): `ldr x8,[x29,#0x88]` ELF 0x18de7dc, `adrp x1,0x2c63000` 0x18de7e4, `add x1,x1,#0x4a8` 0x18de7e8
 *     (the string at ELF 0x2c634a8), `blr x8` 0x18de7ec, `str x0,[x29,#0x258]` 0x18de7f0.
 *   vkCmdSetBlendConstants (gctx+0x368): `ldr x8,[x29,#0x88]` ELF 0x18deb94, `adrp x1,0x2c63000` 0x18deb9c, `add x1,x1,#0xc8a` 0x18deba0
 *     (the string at ELF 0x2c63c8a), `blr x8` 0x18deba4, `str x0,[x29,#0x368]` 0x18deba8.
 *
 * Not established (each waits on a reconstruction listed in the port2 NEEDED list; nothing here was taken from a decompilation):
 *   1. Which engine blend modes (0..25) are additive: the mode -> SGBlendStateDesc map is CGMaterial::ConvertToBlendState (ELF 0x18623cc).
 *   2. Which blend mode, render pass and shaders the pool-18 draws' descs carry: the route from a draw entry to its pipeline is
 *      CGDrawData::Render (ELF 0x1836d34) and GetDrawPipelineStateIdx (ELF 0x183c794); the descs' blend mode is material data, not code.
 *   3. Which desc fields set depth test, depth write and the sample count of the pipeline (the rest of the create function, ELF 0x18da00c),
 *      which a shape predicate like upstream's would read.
 *   4. Whether a pool-18 draw recorded inside the eye pass binds pipelines made for the eye pass's render pass (the desc's render-pass
 *      fields, desc+0x2e4 and desc+0x330, as read in the create function, are filled by the caller: point 2).
 *
 *
 * Drift from the upstream reference (kotorvr/echoquest-combat), recorded here, not carried as fact:
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

// (c) The low-res transparents pass and its pool-18 call.
inline constexpr const char* kLowresRenderSymbol =
    "_ZN10NRadEngine30CGRenderPassLowresTransparents6RenderERKNS_10CGRendererERNS_15SGThreadContextEPNS_17CGSComputeContextEy";
inline constexpr std::uint64_t kLowresRenderVaddr = 0x189c854ULL;
inline constexpr std::uint64_t kLowresRenderSize = 852;
inline constexpr std::uint64_t kLowresVtableSlotVaddr = 0x365bb00ULL;  // R_AARCH64_ABS64 to kLowresRenderSymbol
inline constexpr std::uint64_t kLowresBlendFmovVaddr = 0x189c8c4ULL;
inline constexpr std::uint32_t kLowresBlendFmovWord = 0x4f03f600U;  // fmov v0.4s,#1.0
inline constexpr std::uint64_t kLowresGctxAdrpVaddr = 0x189c8d0ULL;
inline constexpr std::uint32_t kLowresGctxAdrpWord = 0xf000f389U;  // adrp x9,0x370f000
inline constexpr std::uint32_t kLowresGctxLdrWord = 0xf941b129U;   // ldr x9,[x9,#0x360]
inline constexpr std::uint32_t kLowresCmdIndexWord = 0x39498268U;  // ldrb w8,[x19,#0x260]
inline constexpr std::uint32_t kLowresCmdLoadWord = 0xf9400d00U;   // ldr x0,[x8,#0x18]
inline constexpr std::uint32_t kLowresBlendSlotWord = 0xf941b529U;  // ldr x9,[x9,#0x368]
inline constexpr std::uint64_t kPool18ArgVaddr = 0x189c8ecULL;
inline constexpr std::uint32_t kPool18ArgWord = 0x52800242U;  // mov w2,#0x12
inline constexpr std::uint32_t kPool18Pool = 18;
inline constexpr std::uint64_t kPool18CallVaddr = 0x189c8fcULL;
inline constexpr std::uint32_t kPool18CallWord = 0x97daa911U;  // bl 0xf46d40
inline constexpr std::uint64_t kPool18ReturnVaddr = 0x189c900ULL;
inline constexpr pinned::Word kLowresWords[] = {
    {"Lowres Render entry: sub sp,sp,#0x80", kLowresRenderVaddr, 0xd10203ffU},
    {"Lowres: mov x19,x2 (the thread context)", 0x189c878ULL, 0xaa0203f3U},
    {"Lowres: mov x24,x1 (the renderer)", 0x189c87cULL, 0xaa0103f8U},
    {"Lowres: mov x22,x4 (the frame key)", 0x189c88cULL, 0xaa0403f6U},
    {"Lowres: fmov v0.4s,#1.0", kLowresBlendFmovVaddr, kLowresBlendFmovWord},
    {"Lowres: str q0,[sp,#0x10]", 0x189c8c8ULL, 0x3d8007e0U},
    {"Lowres: ldrb w8,[x19,#0x260]", 0x189c8ccULL, kLowresCmdIndexWord},
    {"Lowres: adrp x9,0x370f000", kLowresGctxAdrpVaddr, kLowresGctxAdrpWord},
    {"Lowres: ldr x9,[x9,#0x360]", 0x189c8d4ULL, kLowresGctxLdrWord},
    {"Lowres: add x1,sp,#0x10", 0x189c8d8ULL, 0x910043e1U},
    {"Lowres: add x8,x19,x8,lsl #3", 0x189c8dcULL, 0x8b080e68U},
    {"Lowres: ldr x0,[x8,#0x18]", 0x189c8e0ULL, kLowresCmdLoadWord},
    {"Lowres: ldr x9,[x9,#0x368]", 0x189c8e4ULL, kLowresBlendSlotWord},
    {"Lowres: blr x9 (vkCmdSetBlendConstants)", 0x189c8e8ULL, 0xd63f0120U},
    {"Lowres: mov w2,#0x12 (pool 18)", kPool18ArgVaddr, kPool18ArgWord},
    {"Lowres: mov x0,x24", 0x189c8f0ULL, 0xaa1803e0U},
    {"Lowres: mov x1,x19", 0x189c8f4ULL, 0xaa1303e1U},
    {"Lowres: mov x3,x22", 0x189c8f8ULL, 0xaa1603e3U},
    {"Lowres: bl the RenderDrawData stub (pool 18)", kPool18CallVaddr, kPool18CallWord},
    {"Lowres: return site ldr x8,[x24,#0x1200]", kPool18ReturnVaddr, 0xf9490308U},
};

// Whether the pass is built: its vtable's GOT word and the only three loads of it, and its constructor.
inline constexpr const char* kLowresVtableSymbol = "_ZTVN10NRadEngine30CGRenderPassLowresTransparentsE";
inline constexpr std::uint64_t kLowresVtableVaddr = 0x365ba90ULL;
inline constexpr std::uint64_t kLowresVtableGotVaddr = 0x3726128ULL;
inline constexpr const char* kLowresCtorSymbol = "_ZN10NRadEngine30CGRenderPassLowresTransparentsC1ERKNS_13CPresenceInitE";
inline constexpr std::uint64_t kLowresCtorVaddr = 0x189b60cULL;
inline constexpr std::uint64_t kLowresVtableLoads[] = {0x189b644ULL, 0x189bfa0ULL, 0x189c048ULL};  // constructor, D2, D0 (each an adrp)

// (b) The command buffer.
inline constexpr const char* kCmdBufferSymbol = "_ZN10NRadEngine15SGThreadContext9CmdBufferEv";
inline constexpr std::uint64_t kCmdBufferVaddr = 0x1885a18ULL;
inline constexpr std::uintptr_t kCtxCmdIndexOffset = 0x260;  // u8: which command buffer
inline constexpr std::uintptr_t kCtxCmdBuffersOffset = 0x18;  // u64[]: the command buffers
inline constexpr pinned::Word kCmdBufferWords[] = {
    {"CmdBuffer: ldrb w8,[x0,#0x260]", kCmdBufferVaddr, 0x39498008U},
    {"CmdBuffer: add x8,x0,x8,lsl #3", 0x1885a1cULL, 0x8b080c08U},
    {"CmdBuffer: ldr x0,[x8,#0x18]", 0x1885a20ULL, 0xf9400d00U},
    {"CmdBuffer: ret", 0x1885a24ULL, 0xd65f03c0U},
};

// (a) The game's one draw-pipeline create, and the blend state it takes.
inline constexpr std::uintptr_t kSlotVkCreateGraphicsPipelines = 0x258;
inline constexpr std::uint64_t kPsoCreateVaddr = 0x18da00cULL;
inline constexpr std::uint64_t kPsoCreateCallers[] = {0x183c8b4ULL, 0x18da5acULL};  // GetDrawPipelineStateIdx, RegenerateDrawPSOs
inline constexpr std::uint32_t kDescBlendModeLdrWord = 0xb942d80aU;  // ldr w10,[x0,#0x2d8]
inline constexpr std::uintptr_t kDescBlendModeOffset = 0x2d8;
inline constexpr std::uint32_t kBlendModeCmpWord = 0x7100655fU;  // cmp w10,#0x19
inline constexpr std::uint32_t kBlendModeMax = 0x19;
inline constexpr std::uint64_t kBlendStatesAdrpVaddr = 0x18da314ULL;
inline constexpr std::uint32_t kBlendStatesAdrpWord = 0xd000f1e2U;  // adrp x2,0x3718000
inline constexpr std::uint32_t kBlendStatesLdrWord = 0xf9463842U;   // ldr x2,[x2,#0xc70]
inline constexpr std::uint64_t kBlendStatesGotVaddr = 0x3718c70ULL;
inline constexpr std::uint32_t kBlendStateStrideWord = 0x5280ab11U;  // mov w17,#0x558
inline constexpr std::uint64_t kBlendStateSize = 0x558;
inline constexpr const char* kBlendStatesSymbol = "_ZN10NRadEngine14CGRawInterface11blendstatesE";
inline constexpr std::uint64_t kBlendStatesVaddr = 0x3782b88ULL;
inline constexpr std::uint32_t kPsoGctxAdrpWord = 0xb000f1afU;    // adrp x15,0x370f000
inline constexpr std::uint32_t kPsoGctxLdrWord = 0xf941b1efU;     // ldr x15,[x15,#0x360]
inline constexpr std::uint32_t kPsoCreateSlotWord = 0xf9412df6U;  // ldr x22,[x15,#0x258]
inline constexpr pinned::Word kPsoWords[] = {
    {"PSO create entry: stp x28,x22,[sp,#-0x30]!", kPsoCreateVaddr, 0xa9bd5bfcU},
    {"PSO create: ldr w10,[x0,#0x2d8] (blend mode)", 0x18da034ULL, kDescBlendModeLdrWord},
    {"PSO create: cmp w10,#0x19", 0x18da038ULL, kBlendModeCmpWord},
    {"PSO create: adrp x2,0x3718000", kBlendStatesAdrpVaddr, kBlendStatesAdrpWord},
    {"PSO create: ldr x2,[x2,#0xc70] (blendstates)", 0x18da318ULL, kBlendStatesLdrWord},
    {"PSO create: mov w17,#0x558", 0x18da324ULL, kBlendStateStrideWord},
    {"PSO create: madd x10,x10,x17,x2", 0x18da334ULL, 0x9b11094aU},
    {"PSO create: adrp x15,0x370f000", 0x18da3c0ULL, kPsoGctxAdrpWord},
    {"PSO create: ldr x15,[x15,#0x360]", 0x18da3c4ULL, kPsoGctxLdrWord},
    {"PSO create: ldr x22,[x15,#0x258] (vkCreateGraphicsPipelines)", 0x18da418ULL, kPsoCreateSlotWord},
    {"PSO create: mov w2,#0x1 (createInfoCount)", 0x18da4acULL, 0x52800022U},
    {"PSO create: blr x22", 0x18da4b8ULL, 0xd63f02c0U},
    {"GetDrawPipelineStateIdx: bl PSO create", 0x183c8b4ULL, 0x940275d6U},
    {"RegenerateDrawPSOs: bl PSO create", 0x18da5acULL, 0x97fffe98U},
};

// CreateBlendState and its two translation tables (engine value n -> Vulkan value table[n]).
inline constexpr const char* kCreateBlendStateSymbol =
    "_ZN10NRadEngine14CGRawInterface16CreateBlendStateERKNS_16SGBlendStateDescERNS_12SGBlendStateE";
inline constexpr std::uint64_t kCreateBlendStateVaddr = 0x188194cULL;
inline constexpr std::uint64_t kBlendFactorTableVaddr = 0x2d6dd38ULL;
inline constexpr std::uint32_t kBlendFactorTable[] = {0, 1, 2, 3, 6, 7, 8, 9, 4, 5, 14, 10, 11, 12, 13, 15, 16, 17, 18};
inline constexpr std::uint64_t kBlendOpTableVaddr = 0x2d6dd84ULL;
inline constexpr std::uint32_t kBlendOpTable[] = {0, 3, 4, 1, 2};
inline constexpr std::uintptr_t kBlendStateAttachmentsOffset = 0x158;
inline constexpr pinned::Word kCreateBlendStateWords[] = {
    {"CreateBlendState: adrp x11,0x2d6d000", kCreateBlendStateVaddr, 0x9000a76bU},
    {"CreateBlendState: add x11,x11,#0xd38 (factors)", 0x1881960ULL, 0x9134e16bU},
    {"CreateBlendState: add x12,x12,#0xd84 (ops)", 0x1881964ULL, 0x9136118cU},
    {"CreateBlendState: add x8,x1,#0x158 (pAttachments)", 0x18819b8ULL, 0x91056028U},
    {"CreateBlendState: mov w9,#0x1a (sType)", 0x18819bcULL, 0x52800349U},
    {"CreateBlendState: mov x10,#0x800000000 (attachmentCount 8)", 0x18819c0ULL, 0xd2c0010aU},
};

// (d) The two table fills in CGS::Initialize (the vkCmdSetBlendConstants store is kFillBlendConstants).
inline constexpr std::uint64_t kCreatePipelinesNameVaddr = 0x2c634a8ULL;    // "vkCreateGraphicsPipelines"
inline constexpr std::uint64_t kSetBlendConstantsNameVaddr = 0x2c63c8aULL;  // "vkCmdSetBlendConstants"
inline constexpr pinned::Word kFillCreateGraphicsPipelines{"str x0,[x29,#0x258] (vkCreateGraphicsPipelines)", 0x18de7f0ULL, 0xf9012fa0U};
inline constexpr std::uint32_t kGdpaLoadWord = 0xf94047a8U;  // ldr x8,[x29,#0x88]
inline constexpr std::uint32_t kNameAdrpWord = 0xb0009c21U;  // adrp x1,0x2c63000
inline constexpr pinned::Word kFillWords[] = {
    {"fill 0x258: ldr x8,[x29,#0x88]", 0x18de7dcULL, kGdpaLoadWord},
    {"fill 0x258: adrp x1,0x2c63000", 0x18de7e4ULL, kNameAdrpWord},
    {"fill 0x258: add x1,x1,#0x4a8", 0x18de7e8ULL, 0x9112a021U},
    {"fill 0x258: blr x8", 0x18de7ecULL, 0xd63f0100U},
    kFillCreateGraphicsPipelines,
    {"fill 0x368: ldr x8,[x29,#0x88]", 0x18deb94ULL, kGdpaLoadWord},
    {"fill 0x368: adrp x1,0x2c63000", 0x18deb9cULL, kNameAdrpWord},
    {"fill 0x368: add x1,x1,#0xc8a", 0x18deba0ULL, 0x91322821U},
    {"fill 0x368: blr x8", 0x18deba4ULL, 0xd63f0100U},
    kFillBlendConstants,
};

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

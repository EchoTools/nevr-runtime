/* The pinned words of the live eye-buffer change's deadlock (U15a under #1737): the upload lock and the upload count that a
 * CEngine::Resize waits on, the functions that take and give back an upload slot, and the call path from CEngine::Resize to the
 * wait. Nothing here is hooked or written; a later look-driven Resize reads the two words first (texture_quest2, not in the tree
 * yet). Checked against the store libr15 by src/quest/tests/texture_quality_pinned_test.cpp (the hooks-pinned group, which needs
 * the store APK). The reading is written up in docs/quest/combat/texture_quest2.md.
 *
 * Addresses. Every address is an ELF virtual address in the store libr15 (build id in pinned_targets.h). The disassembly and
 * ReVault (project echovr, binary libr15.so) use ELF + 0x100000; each comment gives the ELF address.
 *
 * What is established. Every word, symbol, PLT stub and JUMP_SLOT below is read from the store file and checked by the test. The
 * deferred-destroy body (0x1888a98), CEngine::Resize, CCanvasDisplay::Resize and CCanvas::BeforeResize are read whole from their
 * byte-checked reconstructions. The other functions named here (FlushGPU, CGDisplay::Resize, the reaper, the slot allocator, both
 * CGUploader::Upload and both End) have no reconstruction yet and are queued for one: what this comment says each of them does
 * beyond its pinned words is INFERRED from those words, and the deadlock below is an inference until they are read whole.
 *
 * The two words. Both lie in .bss (.bss 0x3761180, size 0x713b8), so the file holds zero and the game writes them at run time.
 *   0x379b780  the upload lock: a 32-bit spin lock (ldar spin, then ldaxr/stlxr 0 -> 1; released by stlr wzr).
 *   0x379b400  the upload count: a 64-bit count of the upload ring's slots (32 slots, 0x38 bytes each) that are taken.
 * Taking a slot: CGUploader::Upload (VkBuffer 0x18ebef4, VkImage 0x18e7134) takes the lock, reaps without waiting, calls the slot
 * allocator 0x18ebd38 (count + 1, stored at 0x18ebe28; the slot's submitted word, ring + 0x8a0, set to 0) and gives the lock back.
 * The caller then records its copy, and the matching End (SGUploadBufferContext::End 0x18eb89c, SGUploadImageContext::End
 * 0x18e7260) takes the lock again, submits the command buffer with the slot's fence and stores 1 into the slot's submitted word
 * before it gives the lock back.
 * Giving a slot back: only the reaper 0x18ebb48 decrements the count (sub at 0x18ebcb0, store at 0x18ebcb4). It walks the ring
 * from the oldest slot and returns at once, decrementing nothing, when that slot's submitted word is 0 (cbz at 0x18ebbc8).
 * The wait: the immediate path of the deferred-destroy body 0x1888a98 (taken when CGS::currframe equals the GPU frame, gctx +
 * 0xb70) takes the upload lock and calls the reaper until the count reads 0, holding the lock throughout (0x1888ad4..0x1888b50);
 * CGS::FlushGPU 0x18e08cc does the same (0x18e08f4..0x18e093c) and then stores currframe into gctx + 0xb70 (0x18e0988).
 * So a Resize that reaches either wait while a slot is taken and not yet submitted (between an Upload and its End, on another
 * thread) spins with the lock held, and that End waits for the lock: neither returns.
 *
 * The path from CEngine::Resize (each call is a bl; a call through the PLT names the stub, whose JUMP_SLOT names the callee):
 *   CEngine::Resize 0x2ac093c: bl at 0x2ac09c0 to stub 0xf3a4d0 (slot 0x36f4950) -> CCanvasDisplay::Resize 0x25edfb4
 *   CCanvasDisplay::Resize: bl at 0x25edfd0 to stub 0xf394b0 (slot 0x36f4140) -> CCanvas::BeforeResize 0x25edc0c
 *     CCanvas::BeforeResize: bl at 0x25edc14 to stub 0xef36c0 (slot 0x36d1248) -> CGS::FlushGPU 0x18e08cc   (first wait)
 *   CCanvasDisplay::Resize: bl at 0x25edff0 to stub 0xf402a0 (slot 0x36f7838) -> CGDisplay::Resize 0x1834748
 *     CGDisplay::Resize: four direct bl to 0x1888a98 (kind 0xe) at 0x1834884, 0x18348b0, 0x18348d4, 0x18348f8   (four more waits;
 *     FlushGPU has just made currframe equal the GPU frame, so each takes the immediate path)
 *
 * The second lock path. The deferred-destroy body's other branch (0x1888b04, currframe not the GPU frame) takes a different lock,
 * the one the pointer word 0x379b478 (.bss) names, for a constant-time append to the deferred queue (a CMemBlock resize when it is
 * full) and gives it back at 0x1888be0. It never reads the upload count, so it cannot wait on an upload. Who writes the pointer
 * word is not shown (ReVault lists two reads, 0x18848d4 and 0x1888b08, and no write).
 *
 * Not shown: which thread runs CGUploader::Upload during a level load (its callers are the CGBuffer initializers that upload
 * initial data, through 0x182bdf0), so that the deadlock needs two threads is read from the code, not observed; and that reading
 * the two words before a Resize closes the window (an upload can take a slot after the read and before FlushGPU's lock).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/pinned_words.h"

namespace nevr_quest::texture_quality::targets {

// The two words a Resize waits on, and the deferred queue's lock pointer (all in .bss).
inline constexpr std::uint64_t kUploadLockVaddr = 0x379b780ULL;   // 32-bit spin lock
inline constexpr std::uint64_t kUploadCountVaddr = 0x379b400ULL;  // 64-bit count of taken upload slots
inline constexpr std::uint64_t kDeferredQueueLockPointerVaddr = 0x379b478ULL;
inline constexpr std::uint64_t kUploadLockBytes = 4;
inline constexpr std::uint64_t kUploadCountBytes = 8;
// The upload ring: 32 slots of 0x38 bytes; a slot's submitted word is at ring + 0x8a0 (the reaper's ldr, End's str).
inline constexpr std::uintptr_t kSlotSubmittedOffset = 0x8a0;

// Functions that have no .dynsym name (local to libr15), by address.
inline constexpr std::uint64_t kDeferredDestroyBodyVaddr = 0x1888a98ULL;  // DeferredDestroy(VkBuffer_T*) at 0x1888a8c forwards (9, h)
inline constexpr std::uint64_t kReapUploadsVaddr = 0x18ebb48ULL;          // the only writer that decrements the count
inline constexpr std::uint64_t kTakeUploadSlotVaddr = 0x18ebd38ULL;       // the only writer that increments it

// Functions with a .dynsym name: the test checks each name's st_value.
inline constexpr nevr_quest::combat::pinned::Symbol kSymbols[] = {
    {"_ZN10NRadEngine7CEngine6ResizeEjjNS_11EWindowModeE", 0x2ac093cULL},
    {"_ZN10NRadEngine14CCanvasDisplay6ResizeERjS1_NS_11EWindowModeE", 0x25edfb4ULL},
    {"_ZN10NRadEngine7CCanvas12BeforeResizeEv", 0x25edc0cULL},
    {"_ZN10NRadEngine3CGS8FlushGPUEv", 0x18e08ccULL},
    {"_ZN10NRadEngine9CGDisplay6ResizeERjS1_NS_11EWindowModeE", 0x1834748ULL},
    {"_ZN10NRadEngine16CGRawInterfaceVK15DeferredDestroyEP10VkBuffer_T", 0x1888a8cULL},
    {"_ZN10NRadEngine10CGUploader6UploadEP10VkBuffer_Tmj", 0x18ebef4ULL},
    {"_ZN10NRadEngine10CGUploader6UploadEP9VkImage_Tmmj", 0x18e7134ULL},
    {"_ZN10NRadEngine21SGUploadBufferContext3EndEP17VkCommandBuffer_Tjmm", 0x18eb89cULL},
    {"_ZN10NRadEngine20SGUploadImageContext3EndEP17VkCommandBuffer_T13VkImageLayoutS3_j23VkImageSubresourceRange", 0x18e7260ULL},
};

// One call of the path from CEngine::Resize: the bl at `site` reaches the PLT stub `stub`, whose JUMP_SLOT `slot` names `callee`
// (resolved to `calleeVaddr`).
struct PltCall {
  const char* what;
  std::uint64_t site;
  std::uint32_t word;
  std::uint64_t stub;
  std::uint64_t slot;
  const char* callee;
  std::uint64_t calleeVaddr;
};
inline constexpr PltCall kResizePath[] = {
    {"CEngine::Resize -> CCanvasDisplay::Resize", 0x2ac09c0ULL, 0x9791e6c4U, 0xf3a4d0ULL, 0x36f4950ULL,
     "_ZN10NRadEngine14CCanvasDisplay6ResizeERjS1_NS_11EWindowModeE", 0x25edfb4ULL},
    {"CCanvasDisplay::Resize -> CCanvas::BeforeResize", 0x25edfd0ULL, 0x97a52d38U, 0xf394b0ULL, 0x36f4140ULL,
     "_ZN10NRadEngine7CCanvas12BeforeResizeEv", 0x25edc0cULL},
    {"CCanvas::BeforeResize -> CGS::FlushGPU", 0x25edc14ULL, 0x97a416abU, 0xef36c0ULL, 0x36d1248ULL, "_ZN10NRadEngine3CGS8FlushGPUEv",
     0x18e08ccULL},
    {"CCanvasDisplay::Resize -> CGDisplay::Resize", 0x25edff0ULL, 0x97a548acU, 0xf402a0ULL, 0x36f7838ULL,
     "_ZN10NRadEngine9CGDisplay6ResizeERjS1_NS_11EWindowModeE", 0x1834748ULL},
};

// CGDisplay::Resize's four direct calls of the deferred-destroy body (kind 0xe): each bl's target is kDeferredDestroyBodyVaddr.
inline constexpr nevr_quest::combat::pinned::Word kDisplayDestroyCalls[] = {
    {"bl 0x1888a98 (CGDisplay::Resize, first view)", 0x1834884ULL, 0x94015085U},
    {"bl 0x1888a98 (CGDisplay::Resize, second view)", 0x18348b0ULL, 0x9401507aU},
    {"bl 0x1888a98 (CGDisplay::Resize, third view)", 0x18348d4ULL, 0x94015071U},
    {"bl 0x1888a98 (CGDisplay::Resize, fourth view)", 0x18348f8ULL, 0x94015068U},
};

// The deferred-destroy body: the frame test, the upload lock taken, the wait on the count, the lock given back.
inline constexpr nevr_quest::combat::pinned::Word kGpuFrameLoad{"ldr x10,[x8,#0xb70] (the GPU frame)", 0x1888ac8ULL, 0xf945b90aU};
inline constexpr nevr_quest::combat::pinned::Word kFrameBranch{"b.ne 0x1888b04 (frames differ: the queue path)", 0x1888ad0ULL, 0x540001a1U};
inline constexpr nevr_quest::combat::pinned::Word kLockPage{"adrp x21,0x379b000 (the upload lock's page)", 0x1888ad4ULL, 0xf000f895U};
inline constexpr nevr_quest::combat::pinned::Word kLockAdd{"add x21,x21,#0x780 (the upload lock)", 0x1888ad8ULL, 0x911e02b5U};
inline constexpr nevr_quest::combat::pinned::Word kLockExclusive{"ldaxr w9,[x21] (take the upload lock)", 0x1888ae8ULL, 0x885ffea9U};
inline constexpr nevr_quest::combat::pinned::Word kLockStore{"stlxr w9,w8,[x21] (store 1 into the upload lock)", 0x1888af0ULL, 0x8809fea8U};
inline constexpr nevr_quest::combat::pinned::Word kCountPage{"adrp x22,0x379b000 (the upload count's page)", 0x1888b34ULL, 0xf000f896U};
inline constexpr nevr_quest::combat::pinned::Word kCountLoad{"ldr x8,[x22,#0x400] (the upload count)", 0x1888b38ULL, 0xf94202c8U};
inline constexpr nevr_quest::combat::pinned::Word kReapCall{"bl 0x18ebb48 (reap, waiting on at most 32 slots)", 0x1888b44ULL, 0x94018c01U};
inline constexpr nevr_quest::combat::pinned::Word kCountReload{"ldr x8,[x22,#0x400] (the count again)", 0x1888b48ULL, 0xf94202c8U};
inline constexpr nevr_quest::combat::pinned::Word kCountLoop{"cbnz x8,0x1888b40 (wait while the count is not 0)", 0x1888b4cULL, 0xb5ffffa8U};
inline constexpr nevr_quest::combat::pinned::Word kLockRelease{"stlr wzr,[x21] (give the upload lock back)", 0x1888b50ULL, 0x889ffebfU};
// The second lock path: the deferred queue's lock, named by the pointer word 0x379b478.
inline constexpr nevr_quest::combat::pinned::Word kQueueLockPage{"adrp x9,0x379b000 (the queue lock pointer's page)", 0x1888b04ULL, 0xf000f889U};
inline constexpr nevr_quest::combat::pinned::Word kQueueLockPointer{"ldr x22,[x9,#0x478] (the queue lock pointer)", 0x1888b08ULL, 0xf9423d36U};
inline constexpr nevr_quest::combat::pinned::Word kQueueLockExclusive{"ldaxr w10,[x22] (take the queue lock)", 0x1888b18ULL, 0x885ffecaU};
inline constexpr nevr_quest::combat::pinned::Word kQueueLockRelease{"stlr wzr,[x22] (give the queue lock back)", 0x1888be0ULL, 0x889ffedfU};

// The reaper: the count's address, the early return on an unsubmitted slot, the decrement.
inline constexpr nevr_quest::combat::pinned::Word kReapCountPage{"adrp x21,0x379b000 (reaper: the count's page)", 0x18ebb64ULL, 0x9000f595U};
inline constexpr nevr_quest::combat::pinned::Word kReapCountAdd{"add x21,x21,#0x400 (reaper: the count)", 0x18ebb68ULL, 0x911002b5U};
inline constexpr nevr_quest::combat::pinned::Word kReapSubmittedLoad{"ldr x8,[x8,#0x8a0] (reaper: the oldest slot's submitted word)", 0x18ebbc4ULL,
                                                                     0xf9445108U};
inline constexpr nevr_quest::combat::pinned::Word kReapUnsubmittedReturn{"cbz x8,0x18ebd18 (reaper: return on an unsubmitted slot)", 0x18ebbc8ULL,
                                                                         0xb4000a88U};
inline constexpr nevr_quest::combat::pinned::Word kReapDecrement{"sub x9,x9,#0x1 (reaper: count - 1)", 0x18ebcb0ULL, 0xd1000529U};
inline constexpr nevr_quest::combat::pinned::Word kReapStore{"str x9,[x21] (reaper: store the count)", 0x18ebcb4ULL, 0xf90002a9U};

// The slot allocator: the count read, incremented and stored.
inline constexpr nevr_quest::combat::pinned::Word kTakeCountPage{"adrp x8,0x379b000 (allocator: the count's page)", 0x18ebd44ULL, 0x9000f588U};
inline constexpr nevr_quest::combat::pinned::Word kTakeCountLoad{"ldr x9,[x8,#0x400] (allocator: the count)", 0x18ebd48ULL, 0xf9420109U};
inline constexpr nevr_quest::combat::pinned::Word kTakeIncrement{"add x13,x9,#0x1 (allocator: count + 1)", 0x18ebe04ULL, 0x9100052dU};
inline constexpr nevr_quest::combat::pinned::Word kTakeStorePage{"adrp x14,0x379b000 (allocator: the count's page)", 0x18ebe0cULL, 0x9000f58eU};
inline constexpr nevr_quest::combat::pinned::Word kTakeStoreAdd{"add x14,x14,#0x400 (allocator: the count)", 0x18ebe20ULL, 0x911001ceU};
inline constexpr nevr_quest::combat::pinned::Word kTakeStore{"str x13,[x14] (allocator: store the count)", 0x18ebe28ULL, 0xf90001cdU};

// CGUploader::Upload (VkBuffer): the lock taken around the slot allocation and given back.
inline constexpr nevr_quest::combat::pinned::Word kUploadLockPage{"adrp x21,0x379b000 (Upload: the upload lock's page)", 0x18ebf40ULL, 0x9000f595U};
inline constexpr nevr_quest::combat::pinned::Word kUploadLockAdd{"add x21,x21,#0x780 (Upload: the upload lock)", 0x18ebf44ULL, 0x911e02b5U};
inline constexpr nevr_quest::combat::pinned::Word kUploadLockExclusive{"ldaxr w9,[x21] (Upload: take the lock)", 0x18ebf60ULL, 0x885ffea9U};
inline constexpr nevr_quest::combat::pinned::Word kUploadTakeCall{"bl 0x18ebd38 (Upload: take a slot)", 0x18ebf80ULL, 0x97ffff6eU};
inline constexpr nevr_quest::combat::pinned::Word kUploadLockRelease{"stlr wzr,[x21] (Upload: give the lock back)", 0x18ebf90ULL, 0x889ffebfU};
// CGUploader::Upload (VkImage): the same lock around its slot allocation.
inline constexpr nevr_quest::combat::pinned::Word kImageUploadLockPage{"adrp x21,0x379b000 (image Upload: the lock's page)", 0x18e7180ULL, 0x9000f5b5U};
inline constexpr nevr_quest::combat::pinned::Word kImageUploadLockAdd{"add x21,x21,#0x780 (image Upload: the lock)", 0x18e7184ULL, 0x911e02b5U};
inline constexpr nevr_quest::combat::pinned::Word kImageUploadTakeCall{"bl 0x18ebd38 (image Upload: take a slot)", 0x18e71c0ULL, 0x940012deU};

// SGUploadBufferContext::End and SGUploadImageContext::End: the lock taken, the slot marked submitted, the lock given back.
inline constexpr nevr_quest::combat::pinned::Word kEndLockPage{"adrp x8,0x379b000 (buffer End: the lock's page)", 0x18eba8cULL, 0x9000f588U};
inline constexpr nevr_quest::combat::pinned::Word kEndLockAdd{"add x8,x8,#0x780 (buffer End: the lock)", 0x18eba90ULL, 0x911e0108U};
inline constexpr nevr_quest::combat::pinned::Word kEndLockExclusive{"ldaxr w10,[x8] (buffer End: take the lock)", 0x18ebaa0ULL, 0x885ffd0aU};
inline constexpr nevr_quest::combat::pinned::Word kEndSubmitted{"str x19,[x20,#0x8a0] (buffer End: the slot is submitted)", 0x18ebb20ULL, 0xf9045293U};
inline constexpr nevr_quest::combat::pinned::Word kEndLockRelease{"stlr wzr,[x8] (buffer End: give the lock back)", 0x18ebb24ULL, 0x889ffd1fU};
inline constexpr nevr_quest::combat::pinned::Word kImageEndLockPage{"adrp x8,0x379b000 (image End: the lock's page)", 0x18e750cULL, 0x9000f5a8U};
inline constexpr nevr_quest::combat::pinned::Word kImageEndLockAdd{"add x8,x8,#0x780 (image End: the lock)", 0x18e7510ULL, 0x911e0108U};
inline constexpr nevr_quest::combat::pinned::Word kImageEndSubmitted{"str x19,[x20,#0x8a0] (image End: the slot is submitted)", 0x18e759cULL,
                                                                     0xf9045293U};

// CGS::FlushGPU: the same lock and the same wait, then the GPU frame set to currframe.
inline constexpr nevr_quest::combat::pinned::Word kFlushLockPage{"adrp x20,0x379b000 (FlushGPU: the lock's page)", 0x18e08f4ULL, 0xf000f5d4U};
inline constexpr nevr_quest::combat::pinned::Word kFlushLockAdd{"add x20,x20,#0x780 (FlushGPU: the lock)", 0x18e08f8ULL, 0x911e0294U};
inline constexpr nevr_quest::combat::pinned::Word kFlushLockExclusive{"ldaxr w9,[x20] (FlushGPU: take the lock)", 0x18e0910ULL, 0x885ffe89U};
inline constexpr nevr_quest::combat::pinned::Word kFlushCountPage{"adrp x21,0x379b000 (FlushGPU: the count's page)", 0x18e0920ULL, 0xf000f5d5U};
inline constexpr nevr_quest::combat::pinned::Word kFlushCountLoad{"ldr x8,[x21,#0x400] (FlushGPU: the count)", 0x18e0924ULL, 0xf94202a8U};
inline constexpr nevr_quest::combat::pinned::Word kFlushLockRelease{"stlr wzr,[x20] (FlushGPU: give the lock back)", 0x18e093cULL, 0x889ffe9fU};
inline constexpr nevr_quest::combat::pinned::Word kFlushGpuFrameStore{"str x8,[x19,#0xb70] (FlushGPU: GPU frame = currframe)", 0x18e0988ULL, 0xf905ba68U};

// Every word the test checks against the file (the path's call words are in kResizePath and kDisplayDestroyCalls).
inline constexpr nevr_quest::combat::pinned::Word kWords[] = {
    kGpuFrameLoad,      kFrameBranch,        kLockPage,          kLockAdd,           kLockExclusive,       kLockStore,
    kCountPage,         kCountLoad,          kReapCall,          kCountReload,       kCountLoop,           kLockRelease,
    kQueueLockPage,     kQueueLockPointer,   kQueueLockExclusive, kQueueLockRelease, kReapCountPage,       kReapCountAdd,
    kReapSubmittedLoad, kReapUnsubmittedReturn, kReapDecrement,  kReapStore,         kTakeCountPage,       kTakeCountLoad,
    kTakeIncrement,     kTakeStorePage,      kTakeStoreAdd,      kTakeStore,         kUploadLockPage,      kUploadLockAdd,
    kUploadLockExclusive, kUploadTakeCall,   kUploadLockRelease, kImageUploadLockPage, kImageUploadLockAdd, kImageUploadTakeCall,
    kEndLockPage,       kEndLockAdd,         kEndLockExclusive,  kEndSubmitted,      kEndLockRelease,      kImageEndLockPage,
    kImageEndLockAdd,   kImageEndSubmitted,  kFlushLockPage,     kFlushLockAdd,      kFlushLockExclusive,  kFlushCountPage,
    kFlushCountLoad,    kFlushLockRelease,   kFlushGpuFrameStore,
};

// An adrp and the add or ldr that completes it name one data word; the test decodes each pair and checks the word it names.
struct AddressPair {
  nevr_quest::combat::pinned::Word page;
  nevr_quest::combat::pinned::Word low;
  bool lowIsLdrX;  // false: `add x, x, #imm`; true: `ldr x, [x, #imm]` (imm scaled by 8)
  std::uint64_t names;
};
inline constexpr AddressPair kAddressPairs[] = {
    {kLockPage, kLockAdd, false, kUploadLockVaddr},
    {kCountPage, kCountLoad, true, kUploadCountVaddr},
    {kCountPage, kCountReload, true, kUploadCountVaddr},
    {kQueueLockPage, kQueueLockPointer, true, kDeferredQueueLockPointerVaddr},
    {kReapCountPage, kReapCountAdd, false, kUploadCountVaddr},
    {kTakeCountPage, kTakeCountLoad, true, kUploadCountVaddr},
    {kTakeStorePage, kTakeStoreAdd, false, kUploadCountVaddr},
    {kUploadLockPage, kUploadLockAdd, false, kUploadLockVaddr},
    {kImageUploadLockPage, kImageUploadLockAdd, false, kUploadLockVaddr},
    {kEndLockPage, kEndLockAdd, false, kUploadLockVaddr},
    {kImageEndLockPage, kImageEndLockAdd, false, kUploadLockVaddr},
    {kFlushLockPage, kFlushLockAdd, false, kUploadLockVaddr},
    {kFlushCountPage, kFlushCountLoad, true, kUploadCountVaddr},
};

// A direct bl and the function it reaches.
struct Branch {
  nevr_quest::combat::pinned::Word call;
  std::uint64_t target;
};
inline constexpr Branch kBranches[] = {
    {kReapCall, kReapUploadsVaddr},          {kUploadTakeCall, kTakeUploadSlotVaddr},
    {kImageUploadTakeCall, kTakeUploadSlotVaddr}, {kDisplayDestroyCalls[0], kDeferredDestroyBodyVaddr},
    {kDisplayDestroyCalls[1], kDeferredDestroyBodyVaddr}, {kDisplayDestroyCalls[2], kDeferredDestroyBodyVaddr},
    {kDisplayDestroyCalls[3], kDeferredDestroyBodyVaddr},
};

}  // namespace nevr_quest::texture_quality::targets

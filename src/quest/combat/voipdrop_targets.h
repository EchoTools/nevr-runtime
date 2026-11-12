/* The game words voipdrop reads and hooks (#2178), as ELF vaddrs of the store libr15 (build ID b243509c...; the disassembly lists each 0x100000
 * higher). got_pinned_test.cpp checks the relocation, the symbol's definition and each code word below against the store file.
 *
 *   CR15NetVoipReceiverCS::ReceiveVoip(SCompactPoolHandle, const SR15NetUserVoipEvent&, const void*, unsigned)
 *       defined at 0x21c3238 (1192 bytes), JUMP_SLOT 0x36c5d80 (`readelf -rW`: R_AARCH64_JUMP_SLOT, symbol value 0x21c3238)
 *
 * The code, by the disassembly: ReceiveVoip passes the receiver's voice hash table (`add x0,x21,#0x168`, 0x21c3698) to FindOrCreate, whose
 * AllocPage (0x23164d0..0x231650c) takes the table's allocator block (`add x22,x0,#0x18`), calls CMemBlock::Allocator, CMemoryContext::CurrentPtr,
 * SetCurrent and Current, loads the page size (`ldr w1,[x19,#0x64]`) and the request through the allocator's vtable (`ldr x8,[x8,#0x10]`, `blr x8`),
 * and writes through the answer without testing it; the table's free head is stored at `str w9,[x19,#0x50]` (0x23165a8). DestroyContext frees through
 * the same vtable at +0x38 (`ldr x8,[x8,#0x38]`, 0x124943c).
 *
 * The four engine functions the probe calls are libr15 exports found with dlsym (voipdrop.h); their addresses are not pinned here.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/pinned_words.h"
#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::voipdrop {

inline constexpr const char* kReceiveVoipSymbol =
    "_ZN10NRadEngine8NRadGame21CR15NetVoipReceiverCS11ReceiveVoipENS_18SCompactPoolHandleERKNS0_20SR15NetUserVoipEventEPKvj";
inline constexpr std::uint64_t kReceiveVoipVaddr = 0x21c3238ULL;
inline constexpr std::uint64_t kReceiveVoipSlotVaddr = 0x36c5d80ULL;

// The engine exports the probe makes, as the loaded libr15 exports them (dlsym).
inline constexpr const char* kBlockAllocatorSymbol = "_ZNK10NRadEngine9CMemBlock9AllocatorEv";
inline constexpr const char* kCurrentPtrSymbol = "_ZN10NRadEngine14CMemoryContext10CurrentPtrEv";
inline constexpr const char* kSetCurrentSymbol = "_ZN10NRadEngine14CMemoryContext10SetCurrentEPNS_10CAllocatorE";
inline constexpr const char* kCurrentSymbol = "_ZN10NRadEngine14CMemoryContext7CurrentEv";

// CR15NetVoipReceiverCS: the voice hash table at +0x168; its free head (table+0x50, int32), page size (table+0x64, u32) and allocator block (table+0x18).
inline constexpr std::uintptr_t kTable = 0x168, kFreeHead = kTable + 0x50, kPageBytes = kTable + 0x64, kPageBlock = kTable + 0x18;
// CAllocator vtable: the page request AllocPage makes, and DestroyContext's free.
inline constexpr std::uintptr_t kVtAlloc = 0x10, kVtFree = 0x38;

// void ReceiveVoip(CS* x0, SCompactPoolHandle x1, const SR15NetUserVoipEvent& x2, const void* x3, unsigned w4).
using ReceiveVoipSig = void(void* self, std::uint64_t handle, const void* event, const void* data, std::uint32_t bytes);
struct ReceiveVoipTag {};
using ReceiveVoipThunk = sentinel::CallbackThunk<ReceiveVoipTag, ReceiveVoipSig>;

// The code words the guard was derived from: ReceiveVoip's entry, the table address and the FindOrCreate call, the PLT stub, and AllocPage's block,
// page-size and request loads and the free-head store.
inline constexpr pinned::Word kWords[] = {
    {"ReceiveVoip: sub sp, sp, #0x80", 0x21c3238, 0xd10203ff},
    {"ReceiveVoip: add x0, x21, #0x168", 0x21c3698, 0x9105a2a0},
    {"ReceiveVoip: bl <target>", 0x21c36b4, 0x97b5dab7},  // bl FindOrCreate
    {"FindOrCreate PLT stub: adrp x16", 0xedcd30, 0xb0013f50},
    {"FindOrCreate PLT stub: ldr x17, [x16, #0xd80]", 0xedcd34, 0xf946c211},   // ldr x17,[x16,#...]
    {"FindOrCreate PLT stub: add x16, x16, #0xd80", 0xedcd38, 0x91360210},   // add x16,x16,#...
    {"AllocPage: add x22, x0, #0x18", 0x23164d8, 0x91006016},
    {"AllocPage: ldr w1, [x19, #0x64]", 0x2316504, 0xb9406661},
    {"AllocPage: ldr x8, [x8, #0x10]", 0x2316508, 0xf9400908},
    {"AllocPage: str w9, [x19, #0x50]", 0x23165a8, 0xb9005269},
};
inline constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
inline constexpr pinned::Pins kWordPins = {kWords, kWordCount, nullptr, 0};
// The words carry the offsets the guard reads: a changed offset in this file fails the build, a different libr15 fails the install.
static_assert(pinned::AddImmediate(kWords[1].word) == kTable, "the pinned address of the voice table does not carry kTable");
static_assert(pinned::AddImmediate(kWords[6].word) == kPageBlock - kTable, "the pinned allocator block is not at kPageBlock");
static_assert(pinned::LdrWOffset(kWords[7].word) == kPageBytes - kTable, "the pinned load of the page size is not at kPageBytes");
static_assert(pinned::LdrXOffset(kWords[8].word) == kVtAlloc, "the pinned request is not vtable slot kVtAlloc");
static_assert(pinned::LdrWOffset(kWords[9].word) == kFreeHead - kTable, "the pinned store of the free head is not at kFreeHead");

// The slot as a manager target on a libr15 loaded at `base`: it must hold ReceiveVoip itself (base + its address), so a word something else already
// replaced is refused, not chained over.
inline sentinel::GotTarget ReceiveVoipTarget(std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kReceiveVoipSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), kReceiveVoipSlotVaddr);
  target.expectedOriginal = reinterpret_cast<const void*>(base + kReceiveVoipVaddr);
  return target.Owned("combat/voipdrop");
}

}  // namespace nevr_quest::combat::voipdrop

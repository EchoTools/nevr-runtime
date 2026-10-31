#include "sendguard.h"

#include <atomic>
#include <cstdint>

#include "module_switch.h"
#include "pbhud_log.h"
#include "plugin_log.h"
#include "word_pins.h"

namespace q15t {
namespace {

// ELF vaddrs of the pinned build (disassembly VA = ELF + 0x100000).
//
// The stock function, disassembly libr15 0x2825a80 (NRadEngine::CSendEventNode::Enter, 476 bytes; raw decompilation and disassembly
// read this session). Reconstructed as C++ (x0 = the node's execution context, x1 = the node data):
//   void Enter(Ctx* ctx, SSendEventNodeData* d) {
//     space = ctx->frame->owner->spaceHolder->space;                    // 0x2825a90..0x2825ab0: ldr x8,[x0]; [x8]; [+0x30]; [..]; ldr x19,[x9,#0x80]
//     if (d->level != -1) space = CncaGameSpace::GameSpaceMT(space, d->level);   // 0x2825aa4 cmn x1,#1; 0x2825abc bl 0xff5650
//     if (!SComponentNodeUtils::Actor(ctx, <the first space>, d->actor, ...)) { ScriptActorError(...); return; }   // 0x2825af4
//     ... event = { d->event, space->[0x58], ... };                      // 0x2825b78 ldr x9,[x19,#0x58]: the fault when space == 0
//     CncaGameSpace::SendComponentEvent(space, &event);                  // 0x2825b90
//   }
// The Actor lookup uses the first space, never the looked-up one, so it passes while the named level is absent and the function goes
// on to read the null space. The engine's own callers of GameSpaceMT test its result (PlayAnimation 0x146c5c4, CanPlayAnimation
// 0x146e2b4, UpdateBindings 0x1490b6c); this function is the one that does not.
//
// The module does not reimplement any of that. The wrapper repeats the stock function's own first reads (the chain to the current space
// and the level key), asks GameSpaceMT the same question, and, when the answer is null, returns without sending. In every other case
// it calls the stock function, which asks again (a lookup, no side effect: the engine calls it from many per-frame paths).
constexpr std::uintptr_t kEnter = 0x2725a80;
constexpr std::uintptr_t kGameSpaceMtThunk = 0xef5650;  // the PLT stub of CncaGameSpace::GameSpaceMT; called, so a lazy bind is resolved by the loader

// Code pins: the words the wrapper's reads and the fault were derived from.
constexpr WordPin kPins[] = {
    {0x2725a80, 0xd101c3ff},  // sub sp,sp,#0x70 (Enter's first word)
    {0x2725a90, 0xf9400008},  // ldr x8,[x0]
    {0x2725a98, 0xf9400021},  // ldr x1,[x1]   (the level key)
    {0x2725aa0, 0xf9400109},  // ldr x9,[x8]
    {0x2725aa8, 0xf9401929},  // ldr x9,[x9,#0x30]
    {0x2725aac, 0xf9400129},  // ldr x9,[x9]
    {0x2725ab0, 0xf9404133},  // ldr x19,[x9,#0x80]   (the current space)
    {0x2725abc, 0x979f3ee5},  // bl GameSpaceMT
    {0x2725b78, 0xf9402e69},  // ldr x9,[x19,#0x58]   (the faulting load)
    {0xef5650, 0xb0013ef0},   // the GameSpaceMT stub: adrp x16
    {0xef5654, 0xf9410a11},   //   ldr x17,[x16,#0x210]
    {0xef5658, 0x91084210},   //   add x16,x16,#0x210
    {0xef565c, 0xd61f0220},   //   br x17
};
constexpr std::size_t kPinCount = sizeof kPins / sizeof kPins[0];

// The data words that hold Enter's address (disassembly xrefs to 0x2825a80: [data] 0x366a6b0 and 0x37f7928, the second the PLT stub's
// GOT slot), as ELF vaddrs. Each is swapped on its own, checked against base + kEnter, so a slot that holds something else is refused.
// The third xref, 0x33e7924, is 4-aligned and cannot hold a pointer word; it is not swapped.
constexpr int kSlotCount = 2;
constexpr std::uintptr_t kSlots[kSlotCount] = {0x356a6b0, 0x36f7928};

constexpr std::uint64_t kNoLevel = ~0ull;  // d->level == -1: send within the current space
constexpr std::size_t kSpaceChain = 0x30, kSpaceOffset = 0x80;
constexpr unsigned kLogFirstDrops = 8;

using EnterFn = void (*)(void* ctx, void* data);
using GameSpaceFn = void* (*)(void* space, std::uint64_t level);

// Each slot has its own stored original. InstallImportHook clears the word it stored when a slot is refused, so a shared word would
// unhook the slots that installed before it.
std::atomic<std::uintptr_t> g_next[kSlotCount]{};
std::atomic<std::uintptr_t> g_lookup{0};
std::atomic<unsigned> g_dropped{0}, g_passed{0};

// True when the stock function would read a null game space: the node names a level and the lookup finds none. Every pointer is the
// one the stock function reads next, so a null on the way means the stock function is left to do what it does.
const std::uint8_t* Load(const std::uint8_t* from, std::size_t offset) {
  return from == nullptr ? nullptr : *reinterpret_cast<const std::uint8_t* const*>(from + offset);
}

bool NamedLevelAbsent(void* ctx, const std::uint64_t* data) {
  const auto lookup = reinterpret_cast<GameSpaceFn>(g_lookup.load(std::memory_order_acquire));
  if (lookup == nullptr || ctx == nullptr || data == nullptr || data[0] == kNoLevel) return false;
  // 0x2825a90 ldr x8,[x0]; 0x2825aa0 ldr x9,[x8]; 0x2825aa8 ldr x9,[x9,#0x30]; 0x2825aac ldr x9,[x9]; 0x2825ab0 ldr x19,[x9,#0x80]
  const std::uint8_t* x9 = Load(Load(static_cast<const std::uint8_t*>(ctx), 0), 0);
  x9 = Load(Load(x9, kSpaceChain), 0);
  if (x9 == nullptr) return false;
  void* space = const_cast<std::uint8_t*>(Load(x9, kSpaceOffset));
  return lookup(space, data[0]) == nullptr;
}

void Wrap(int i, void* ctx, void* data) {
  // Stored before the slot was swapped (InstallImportHook), so the original is set whenever this runs.
  const auto next = reinterpret_cast<EnterFn>(g_next[i].load(std::memory_order_acquire));
  const auto* d = static_cast<const std::uint64_t*>(data);
  if (NamedLevelAbsent(ctx, d)) {
    const unsigned n = g_dropped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kLogFirstDrops) {
      Rec().U("n", n).X("slot", static_cast<unsigned long long>(i)).X("level_key", d[0]).X("event_key", d[1]).X("actor_key", d[2])
          .Emit(NEVR_LOG_INFO, "sendguard: send dropped, the game space the node names is not loaded (the stock function would fault)");
    }
    return;
  }
  g_passed.fetch_add(1, std::memory_order_relaxed);
  next(ctx, data);
}

template <int I>
void Wrapper(void* ctx, void* data) {
  Wrap(I, ctx, data);
}

using WrapperFn = void (*)(void*, void*);
constexpr WrapperFn kWrappers[kSlotCount] = {&Wrapper<0>, &Wrapper<1>};

}  // namespace

SendGuardStats GetSendGuardStats() { return {g_dropped.load(), g_passed.load()}; }

void ResetSendGuardForTest() {
  for (auto& n : g_next) n = 0;
  g_lookup = 0;
  g_dropped = 0;
  g_passed = 0;
}

InstallResult InstallSendGuard(std::uintptr_t base) {
  InstallResult result;
  if (base == 0) {
    result.reason = "no_base";
    Log(NEVR_LOG_ERROR, "sendguard not installed: no game base");
    return result;
  }
  if (ModuleSwitchPresent("sendguard.off")) {
    result.status = SwapStatus::kOk;
    result.reason = "sendguard_off";
    Log(NEVR_LOG_INFO, "sendguard off: sendguard.off present, nothing changed");
    return result;
  }
  if (!CheckWordPins("sendguard", base, kPins, kPinCount)) {
    result.status = SwapStatus::kUnexpectedOriginal;
    result.reason = "word_pin_mismatch";
    return result;
  }
  g_lookup.store(base + kGameSpaceMtThunk, std::memory_order_release);

  int installed = 0;
  SwapStatus first_failure = SwapStatus::kOk;
  const char* first_reason = "";
  for (int i = 0; i < kSlotCount; ++i) {
    const InstallResult r = InstallImportHook("sendguard", base, kSlots[i], reinterpret_cast<std::uintptr_t>(kWrappers[i]),
                                              &g_next[i], kEnter);
    Rec rec;
    rec.X("slot_elf_vaddr", kSlots[i]).X("fn_elf_vaddr", kEnter).S("status", StatusName(r.status)).S("reason", r.reason);
    if (r.status == SwapStatus::kOk) {
      ++installed;
      rec.Emit(NEVR_LOG_INFO, "sendguard: slot swapped");
    } else if (r.status == SwapStatus::kRestoreFailed) {
      ++installed;  // the word was written; only the page's protection could not be put back (the caller logged it)
      rec.Emit(NEVR_LOG_ERROR, "sendguard: slot swapped, but the page's old protection could not be restored");
    } else {
      if (first_failure == SwapStatus::kOk) {
        first_failure = r.status;
        first_reason = r.reason;
      }
      rec.Emit(NEVR_LOG_WARNING, "sendguard: slot refused, its calls stay stock");
    }
  }

  if (installed == 0) {
    result.status = first_failure;
    result.reason = first_reason;
    Log(NEVR_LOG_ERROR, "sendguard: OFF; no slot installed, nothing changed");
    return result;
  }
  Rec().U("slots", static_cast<unsigned long long>(installed)).U("of", static_cast<unsigned long long>(kSlotCount))
      .Emit(NEVR_LOG_INFO, "sendguard: on (a script send to a game space that is not loaded is dropped, not faulted)");
  result.status = SwapStatus::kOk;
  return result;
}

}  // namespace q15t

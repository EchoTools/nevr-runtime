// Runs the headset gates' two arm64 entries (src/quest/combat/headset_gates_entry.inc, the very text headset_gates.cpp builds) under qemu-aarch64.
// Freestanding: no libc, no sentinel code; only the entries, and recording stand-ins for the two functions they continue in. Built and run by
// src/quest/combat/check/headset_gates.sh (NDK r26d clang++ -static -nostdlib).
//
// What is checked, by running it:
//   - nevr_combat_headset_tramp hands its continuation x0 = the caller's sp at the call and x1 = the caller's return address, and the continuation's
//     return lands in the caller (x30 and sp were untouched by the entry): the result comes back, and no callee-saved register of the caller changed;
//   - nevr_combat_party_tramp hands x0 and x1 on as the caller set them and x2 = the caller's return address, with the same return;
//   - the return address is exact: it is the instruction after the caller's blr, not the entry's own.
#include <cstdint>

extern "C" {
static std::uintptr_t g_headSp, g_headRet;
static int g_headCalls;
static void* g_partySelf;
static std::uint32_t g_partyIndex;
static std::uintptr_t g_partyRet;
static int g_partyCalls;
static const std::int32_t kHeadResult = 0x4455;
static const std::int32_t kPartyResult = 0x6677;

__attribute__((visibility("hidden"), used, noinline)) std::int32_t nevr_combat_headset_dispatch(std::uintptr_t bindSp, std::uintptr_t ret) {
  ++g_headCalls;
  g_headSp = bindSp;
  g_headRet = ret;
  return kHeadResult;
}
__attribute__((visibility("hidden"), used, noinline)) std::int32_t nevr_combat_party_dispatch(void* self, std::uint32_t index, std::uintptr_t ret) {
  ++g_partyCalls;
  g_partySelf = self;
  g_partyIndex = index;
  g_partyRet = ret;
  return kPartyResult;
}
}

#include "quest/combat/headset_gates_entry.inc"

extern "C" {
__attribute__((visibility("hidden"))) void nevr_combat_headset_tramp();
__attribute__((visibility("hidden"))) void nevr_combat_party_tramp();
}

// out[0] = sp at the call, out[1] = the return address of the call (the label after blr), out[2], out[3] = x20, x21 after it
extern "C" std::int64_t hgq_call(std::uintptr_t entry, std::uintptr_t arg0, std::uintptr_t arg1, std::uint64_t* out);
asm(R"(
.text
.global hgq_call
.type hgq_call,%function
hgq_call:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    str x21, [sp, #32]
    mov x16, x0
    mov x19, x3
    mov x0, x1
    mov x1, x2
    mov x20, #0xa5a5
    mov x21, #0x5a5a
    adr x17, 1f
    str x17, [x19, #8]
    mov x6, sp
    str x6, [x19]
    blr x16
1:
    str x20, [x19, #16]
    str x21, [x19, #24]
    ldr x21, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #48
    ret
)");

static void Write(const char* s) {
  std::uint64_t n = 0;
  while (s[n]) ++n;
  register std::uint64_t x0 asm("x0") = 1;
  register const char* x1 asm("x1") = s;
  register std::uint64_t x2 asm("x2") = n;
  register std::uint64_t x8 asm("x8") = 64;  // write
  asm volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
}
static int g_fail = 0;
static void Check(bool ok, const char* what) {
  if (ok) return;
  ++g_fail;
  Write("FAIL: ");
  Write(what);
  Write("\n");
}
[[noreturn]] static void Exit(int code) {
  register std::uint64_t x0 asm("x0") = static_cast<std::uint64_t>(code);
  register std::uint64_t x8 asm("x8") = 93;  // exit
  asm volatile("svc 0" : : "r"(x0), "r"(x8));
  for (;;) {
  }
}

extern "C" [[noreturn]] void _start() {
  std::uint64_t out[4];

  // 1. CVR::HeadsetType(): the continuation gets the caller's sp and return address, and its result reaches the caller.
  std::int64_t r = hgq_call(reinterpret_cast<std::uintptr_t>(&nevr_combat_headset_tramp), 0, 0, out);
  Check(r == kHeadResult, "headset: the continuation's result did not reach the caller");
  Check(g_headCalls == 1, "headset: the continuation was not called once");
  Check(g_headSp == out[0], "headset: x0 is not the caller's sp at the call");
  Check(g_headRet == out[1], "headset: x1 is not the caller's return address (the instruction after the blr)");
  Check(out[2] == 0xa5a5 && out[3] == 0x5a5a, "headset: a callee-saved register of the caller changed");

  // 2. PartyMemberHeadsetType(this, index): the arguments arrive as the caller set them, and x2 is the caller's return address.
  r = hgq_call(reinterpret_cast<std::uintptr_t>(&nevr_combat_party_tramp), 0x1234560, 7, out);
  Check(r == kPartyResult, "party: the continuation's result did not reach the caller");
  Check(g_partyCalls == 1, "party: the continuation was not called once");
  Check(g_partySelf == reinterpret_cast<void*>(0x1234560) && g_partyIndex == 7, "party: x0 / x1 changed on the way to the continuation");
  Check(g_partyRet == out[1], "party: x2 is not the caller's return address (the instruction after the blr)");
  Check(out[2] == 0xa5a5 && out[3] == 0x5a5a, "party: a callee-saved register of the caller changed");
  Check(g_headCalls == 1, "party: the headset continuation was called");

  if (g_fail == 0) Write("headset_gates_qemu: all checks passed\n");
  Exit(g_fail == 0 ? 0 : 1);
}

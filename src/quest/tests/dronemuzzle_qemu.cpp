// Runs dronemuzzle's two arm64 entries (src/quest/combat/dronemuzzle_entry.inc, the very text dronemuzzle.cpp builds) under qemu-aarch64.
// Freestanding: no libc, no sentinel code; only the entries, their four words, and recording stand-ins for the two continuations and the two
// next functions. Built and run by src/quest/combat/check/dronemuzzle.sh (NDK r26d clang++ -static -nostdlib).
//
// What is checked, by running it:
//   - a call whose return address is the entry's call-site word reaches the continuation with x0..x3 (teleport) or x0..x6 and s0 (fire) as the
//     caller set them, and the continuation's result comes back to the caller;
//   - any other call reaches the next word's function with the same arguments, not the continuation, and its result comes back;
//   - either way the caller's callee-saved registers and its return are intact (x30 and sp untouched by the entry).
#include <cstdint>

extern "C" {
// 8-byte aligned words, as the sentinel's are (an ldar of a word needs its natural alignment)
__attribute__((used, aligned(8))) static std::uintptr_t nevr_combat_dm_tp_ret = 0;
__attribute__((used, aligned(8))) static void* nevr_combat_dm_tp_next = nullptr;
__attribute__((used, aligned(8))) static std::uintptr_t nevr_combat_dm_fire_ret = 0;
__attribute__((used, aligned(8))) static void* nevr_combat_dm_fire_next = nullptr;

static int g_cont = 0, g_next = 0;
static std::uint64_t g_a[7];
static float g_s0 = 0;
static const std::uint64_t kContResult = 0x4455, kNextResult = 0x6677;

__attribute__((used, noinline)) std::uint64_t nevr_combat_dm_teleport(void* a, std::uint64_t b, const std::uint8_t* c, std::uint64_t d) noexcept {
  ++g_cont;
  g_a[0] = reinterpret_cast<std::uint64_t>(a), g_a[1] = b, g_a[2] = reinterpret_cast<std::uint64_t>(c), g_a[3] = d;
  return kContResult;
}
__attribute__((used, noinline)) std::uint64_t nevr_combat_dm_fire(std::uint8_t* a, std::uint64_t b, const void* c, float s0, const void* d, const void* e,
                                                                 std::uint64_t f, std::uint64_t g) noexcept {
  ++g_cont;
  g_a[0] = reinterpret_cast<std::uint64_t>(a), g_a[1] = b, g_a[2] = reinterpret_cast<std::uint64_t>(c), g_a[3] = reinterpret_cast<std::uint64_t>(d);
  g_a[4] = reinterpret_cast<std::uint64_t>(e), g_a[5] = f, g_a[6] = g;
  g_s0 = s0;
  return kContResult;
}
__attribute__((noinline)) std::uint64_t NextTp(void* a, std::uint64_t b, const std::uint8_t* c, std::uint64_t d) {
  ++g_next;
  g_a[0] = reinterpret_cast<std::uint64_t>(a), g_a[1] = b, g_a[2] = reinterpret_cast<std::uint64_t>(c), g_a[3] = d;
  return kNextResult;
}
__attribute__((noinline)) std::uint64_t NextFire(std::uint8_t* a, std::uint64_t b, const void* c, float s0, const void* d, const void* e, std::uint64_t f,
                                                 std::uint64_t g) {
  ++g_next;
  g_a[0] = reinterpret_cast<std::uint64_t>(a), g_a[1] = b, g_a[2] = reinterpret_cast<std::uint64_t>(c), g_a[3] = reinterpret_cast<std::uint64_t>(d);
  g_a[4] = reinterpret_cast<std::uint64_t>(e), g_a[5] = f, g_a[6] = g;
  g_s0 = s0;
  return kNextResult;
}
}

#include "quest/combat/dronemuzzle_entry.inc"

extern "C" {
__attribute__((visibility("hidden"))) void nevr_combat_dm_tp_entry();
__attribute__((visibility("hidden"))) void nevr_combat_dm_fire_entry();
}

// dmq_call(entry, word, out): when `word` is not null, the call's own return address is stored there first (the call site the entry acts on);
// then the entry is called with x0..x6 = 0x100..0x106 and s0 = 2.5. out[0] = the result, out[1], out[2] = x20, x21 after the call.
extern "C" void dmq_call(std::uintptr_t entry, std::uintptr_t* word, std::uint64_t* out);
asm(R"(
.text
.global dmq_call
.type dmq_call,%function
dmq_call:
    stp x29, x30, [sp, #-48]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    str x21, [sp, #32]
    mov x16, x0
    mov x19, x2
    adr x17, 1f
    cbz x1, 2f
    str x17, [x1]
2:
    mov x20, #0xa5a5
    mov x21, #0x5a5a
    mov x0, #0x100
    mov x1, #0x101
    mov x2, #0x102
    mov x3, #0x103
    mov x4, #0x104
    mov x5, #0x105
    mov x6, #0x106
    fmov s0, #2.5
    blr x16
1:
    str x0, [x19]
    str x20, [x19, #8]
    str x21, [x19, #16]
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
static bool Args(int n) {
  for (int i = 0; i < n; ++i)
    if (g_a[i] != 0x100u + static_cast<unsigned>(i)) return false;
  return true;
}

extern "C" [[noreturn]] void _start() {
  std::uint64_t out[3];
  nevr_combat_dm_tp_next = reinterpret_cast<void*>(&NextTp);
  nevr_combat_dm_fire_next = reinterpret_cast<void*>(&NextFire);

  // WorldTeleport from the call site the entry acts on: the continuation, x0..x3 as the caller set them
  dmq_call(reinterpret_cast<std::uintptr_t>(&nevr_combat_dm_tp_entry), &nevr_combat_dm_tp_ret, out);
  Check(out[0] == kContResult && g_cont == 1 && g_next == 0, "teleport: the acting call did not reach the continuation");
  Check(Args(4), "teleport: x0..x3 changed on the way to the continuation");
  Check(out[1] == 0xa5a5 && out[2] == 0x5a5a, "teleport: a callee-saved register of the caller changed");
  // from any other call site: the next word, untouched
  nevr_combat_dm_tp_ret = 0x10;
  dmq_call(reinterpret_cast<std::uintptr_t>(&nevr_combat_dm_tp_entry), nullptr, out);
  Check(out[0] == kNextResult && g_cont == 1 && g_next == 1 && Args(4), "teleport: another caller did not go straight to the next word");
  Check(out[1] == 0xa5a5 && out[2] == 0x5a5a, "teleport pass-through: a callee-saved register of the caller changed");

  // FireBullet: the same, with x0..x6 and s0
  dmq_call(reinterpret_cast<std::uintptr_t>(&nevr_combat_dm_fire_entry), &nevr_combat_dm_fire_ret, out);
  Check(out[0] == kContResult && g_cont == 2 && g_next == 1, "fire: the acting call did not reach the continuation");
  Check(Args(7) && g_s0 == 2.5f, "fire: x0..x6 or s0 changed on the way to the continuation");
  g_s0 = 0;
  nevr_combat_dm_fire_ret = 0x10;
  dmq_call(reinterpret_cast<std::uintptr_t>(&nevr_combat_dm_fire_entry), nullptr, out);
  Check(out[0] == kNextResult && g_cont == 2 && g_next == 2 && Args(7) && g_s0 == 2.5f, "fire: another caller did not go straight to the next word");
  Check(out[1] == 0xa5a5 && out[2] == 0x5a5a, "fire: a callee-saved register of the caller changed");

  if (g_fail == 0) Write("dronemuzzle_qemu: all checks passed\n");
  Exit(g_fail == 0 ? 0 : 1);
}

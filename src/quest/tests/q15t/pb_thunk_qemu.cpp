// Runs the plugin's one naked arm64 function (src/quest/plugins/q15t/src/powerboost_thunk.inc, the very text src/quest/plugins/q15t/src/powerboost.cpp includes) under
// qemu-aarch64. Freestanding: no libc, no plugin code; only the thunk, the two words it reads and a recording stand-in for
// q15t_pb_cone_at. Build: NDK r26d clang++ -static -nostdlib (src/quest/tests/q15t/mod/powerboost.sh).
//
// What is checked, by running it:
//   - a call whose return address is not q15t_pb_cone_ret reaches the real function with x0, x1, x2 and x30 as the caller
//     left them (the real function returns straight to the caller), q15t_pb_cone_at is not called, and no callee-saved
//     register (x19, x20, x24, x25) and no stack word of the caller is disturbed;
//   - the call whose return address equals q15t_pb_cone_ret goes to q15t_pb_cone_at with x0 = the target, x1 = the caller's
//     sp at the call, x2 = the caller's x24, returns what it returns, and the real function is not called;
//   - the compare is exact: a return address one instruction off is an ordinary call.
#include <atomic>
#include <cstdint>

extern "C" {
__attribute__((visibility("hidden"))) std::atomic<std::uintptr_t> q15t_pb_cone_ret{0};
__attribute__((visibility("hidden"))) std::atomic<std::uintptr_t> q15t_pb_resolve_real{0};

static std::uintptr_t g_cone_target, g_cone_frame, g_cone_dcs;
static int g_cone_calls;
static std::uintptr_t g_real_x0, g_real_x1, g_real_x2, g_real_lr;
static int g_real_calls;
static const std::uintptr_t kConeResult = 0xc0de0001;

__attribute__((visibility("hidden"), used, noinline)) const std::uint8_t* q15t_pb_cone_at(const void* target, const std::uint8_t* frame,
                                                                                        std::uintptr_t dcs) {
  ++g_cone_calls;
  g_cone_target = reinterpret_cast<std::uintptr_t>(target);
  g_cone_frame = reinterpret_cast<std::uintptr_t>(frame);
  g_cone_dcs = dcs;
  return reinterpret_cast<const std::uint8_t*>(kConeResult);
}

#include "../../plugins/q15t/src/powerboost_thunk.inc"

__attribute__((noinline)) std::uintptr_t real_resolve(std::uintptr_t x0, std::uintptr_t x1, std::uintptr_t x2) {
  ++g_real_calls;
  g_real_x0 = x0;
  g_real_x1 = x1;
  g_real_x2 = x2;
  g_real_lr = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  return x0 + 0x1000;
}
}

// out[0] = sp at the call, out[1] = the return address of the call (the label after blr), out[2..5] = x24, x20, x25, x19 after it
extern "C" std::uintptr_t pbt_call(std::uintptr_t thunk, std::uintptr_t target, std::uintptr_t x24, std::uint64_t* out, std::uintptr_t arm);
asm(R"(
.text
.global pbt_call
.type pbt_call,%function
pbt_call:
    stp x29, x30, [sp, #-64]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x24, x25, [sp, #32]
    mov x16, x0
    mov x19, x3
    mov x0, x1
    mov x24, x2
    mov x1, #0x1111
    mov x2, #0x2222
    mov x20, #0xa5a5
    mov x25, #0x5a5a
    adr x17, 1f
    str x17, [x19, #8]
    cbz x4, 2f
    adrp x5, q15t_pb_cone_ret
    add x5, x5, :lo12:q15t_pb_cone_ret
    str x17, [x5]
2:
    mov x6, sp
    str x6, [x19]
    blr x16
1:
    str x24, [x19, #16]
    str x20, [x19, #24]
    str x25, [x19, #32]
    str x19, [x19, #40]
    ldp x24, x25, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #64
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
  std::uint64_t out[6];
  const std::uintptr_t thunk = reinterpret_cast<std::uintptr_t>(&q15t_pb_resolve_thunk);
  const std::uintptr_t kTarget = 0x1234560, kDcs = 0xd0d0d0d0;
  q15t_pb_resolve_real.store(reinterpret_cast<std::uintptr_t>(&real_resolve));

  // 1. an ordinary caller (cone return address not armed): straight on to the real function
  q15t_pb_cone_ret.store(0);
  std::uintptr_t r = pbt_call(thunk, kTarget, kDcs, out, 0);
  const std::uintptr_t label = out[1];
  Check(r == kTarget + 0x1000, "ordinary call: the real function's result is not returned");
  Check(g_real_calls == 1 && g_cone_calls == 0, "ordinary call: real not called once / cone_at called");
  Check(g_real_x0 == kTarget && g_real_x1 == 0x1111 && g_real_x2 == 0x2222, "ordinary call: x0 / x1 / x2 changed on the way to the real function");
  Check(g_real_lr == label, "ordinary call: the real function did not see the caller's return address (x30 changed)");
  Check(out[2] == kDcs && out[3] == 0xa5a5 && out[4] == 0x5a5a && out[5] == reinterpret_cast<std::uintptr_t>(out), "ordinary call: x24 / x20 / x25 / x19 changed");

  // 2. the armed return address: the cone function runs, the real one does not
  g_real_calls = 0;
  r = pbt_call(thunk, kTarget, kDcs, out, 1);
  Check(q15t_pb_cone_ret.load() == label, "the test did not arm the return address it meant to");
  Check(g_cone_calls == 1 && g_real_calls == 0, "cone call: cone_at not called once / real called");
  Check(r == kConeResult, "cone call: cone_at's result is not returned to the caller");
  Check(g_cone_target == kTarget, "cone call: x0 (the target) not handed to cone_at");
  Check(g_cone_frame == out[0], "cone call: x1 is not the caller's sp at the call");
  Check(g_cone_dcs == kDcs, "cone call: x2 is not the caller's x24");
  Check(out[2] == kDcs && out[3] == 0xa5a5 && out[4] == 0x5a5a && out[5] == reinterpret_cast<std::uintptr_t>(out), "cone call: x24 / x20 / x25 / x19 changed");

  // 3. the compare is exact: one instruction off in either direction is an ordinary call
  for (int delta = -4; delta <= 4; delta += 8) {
    q15t_pb_cone_ret.store(label + static_cast<std::uintptr_t>(static_cast<long>(delta)));
    g_cone_calls = g_real_calls = 0;
    r = pbt_call(thunk, kTarget, kDcs, out, 0);
    Check(r == kTarget + 0x1000 && g_real_calls == 1 && g_cone_calls == 0, "a return address one instruction off was treated as the cone call");
  }

  if (g_fail == 0) Write("pb_thunk_qemu: all checks passed\n");
  Exit(g_fail == 0 ? 0 : 1);
}

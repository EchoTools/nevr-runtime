// Built -fno-exceptions. The four assembly entries of the q15t plugin (dronemuzzle's two thunks, powerboost's resolve thunk, the
// headset trampoline), under the frame sensor's assembly-entry check (tests/quest frames_sensor_test.go). Never run.
//
//   frames_asm_probe_real_naked        the entries as the plugin writes them: three __attribute__((naked)) functions and a
//                                      top-level trampoline, none with CFI: the sensor MUST fail them
//   frames_asm_probe_real_moved        the same four as top-level asm() blocks with CFI, every published word read with ldar:
//                                      the sensor MUST pass them
//   frames_asm_probe_real_moved_pbldr  the same, with powerboost's two words left as a plain ldr (#2216): the sensor MUST fail it,
//                                      and only that entry
#include <cstdint>

#include "hook_install.h"

namespace probe {
// Defined in frames_probe_helper_ok.cpp (exceptions enabled), annotated NEVR_OUTSIDE_GAME_CALL.
int LiveHelper(int value) noexcept;
}  // namespace probe

extern "C" {
void* q15t_dm_tp_ret = nullptr;
void* q15t_dm_tp_next = nullptr;
void* q15t_dm_fire_ret = nullptr;
void* q15t_dm_fire_next = nullptr;
void* q15t_pb_cone_ret = nullptr;
void* q15t_pb_resolve_real = nullptr;
void* q15t_headset_next = nullptr;  // the trampoline has no pass-through word; the install is given this one

std::uint64_t q15t_dm_teleport(std::uint64_t a) noexcept { return static_cast<std::uint64_t>(probe::LiveHelper(static_cast<int>(a))); }
std::uint64_t q15t_dm_fire(std::uint64_t a) noexcept { return static_cast<std::uint64_t>(probe::LiveHelper(static_cast<int>(a))); }
const std::uint8_t* q15t_pb_cone_at(const void*, const std::uint8_t* f, std::uintptr_t) noexcept { return f + probe::LiveHelper(0); }
std::int32_t q15t_headset_dispatch(std::uintptr_t sp, std::uintptr_t ret) noexcept { return probe::LiveHelper(static_cast<int>(sp + ret)); }

void q15t_dm_tp_thunk();
void q15t_dm_fire_thunk();
void q15t_pb_resolve_thunk();
void q15t_headset_tramp();
}

#if defined(PROBE_REAL_NAKED)
#define PROBE_CFI(directive) ""
#else
#define PROBE_CFI(directive) directive "\n"
#endif

#define PROBE_LOAD_ACQUIRE(word) "adrp x16, " word "\nadd x16, x16, :lo12:" word "\nldar x16, [x16]\n"
#define PROBE_LOAD_PLAIN(word) "adrp x16, " word "\nldr x16, [x16, :lo12:" word "]\n"
#if defined(PROBE_REAL_PLAIN_LDR)
#define PROBE_LOAD_PB(word) PROBE_LOAD_PLAIN(word)
#else
#define PROBE_LOAD_PB(word) PROBE_LOAD_ACQUIRE(word)
#endif

// One acting entry: act when x30 is the call site, else jump to the next word; a frame, the arguments the thunk sets, a bl.
#define PROBE_THUNK_BODY(load, ret, next, setup, call)                                                          \
  "hint #34\n" load(ret) "cmp x30, x16\nb.eq 1f\n" load(next) "br x16\n"                                         \
  "1:\nstp x29, x30, [sp, #-16]!\n" PROBE_CFI(".cfi_def_cfa_offset 16") PROBE_CFI(".cfi_offset 29, -16")        \
  PROBE_CFI(".cfi_offset 30, -8") "mov x29, sp\n" setup "bl " call "\nldp x29, x30, [sp], #16\n"                \
  PROBE_CFI(".cfi_restore 29") PROBE_CFI(".cfi_restore 30") PROBE_CFI(".cfi_def_cfa_offset 0") "ret\n"

#if defined(PROBE_REAL_NAKED)
#define PROBE_THUNK(name, body) \
  extern "C" __attribute__((naked, visibility("hidden"))) void name() { asm volatile(body); }
#else
#define PROBE_THUNK(name, body)                                                                                      \
  asm(".text\n.p2align 2\n.hidden " #name "\n.type " #name ",%function\n" #name ":\n" PROBE_CFI(".cfi_startproc") body \
          PROBE_CFI(".cfi_endproc") ".size " #name ", .-" #name "\n");
#endif

PROBE_THUNK(q15t_dm_tp_thunk,
            PROBE_THUNK_BODY(PROBE_LOAD_ACQUIRE, "q15t_dm_tp_ret", "q15t_dm_tp_next", "", "q15t_dm_teleport"))
PROBE_THUNK(q15t_dm_fire_thunk,
            PROBE_THUNK_BODY(PROBE_LOAD_ACQUIRE, "q15t_dm_fire_ret", "q15t_dm_fire_next", "", "q15t_dm_fire"))
PROBE_THUNK(q15t_pb_resolve_thunk,
            PROBE_THUNK_BODY(PROBE_LOAD_PB, "q15t_pb_cone_ret", "q15t_pb_resolve_real", "add x1, sp, #16\nmov x2, x24\n",
                             "q15t_pb_cone_at"))

// The headset trampoline is a top-level block in the plugin in every variant: no frame, a tail branch with x30 and sp untouched.
asm(".text\n.p2align 2\n.hidden q15t_headset_tramp\n.type q15t_headset_tramp,%function\nq15t_headset_tramp:\n" PROBE_CFI(".cfi_startproc")
    "hint #34\nmov x0, sp\nmov x1, x30\nb q15t_headset_dispatch\n" PROBE_CFI(".cfi_endproc") ".size q15t_headset_tramp, .-q15t_headset_tramp\n");

namespace {
NEVR_ASM_ENTRY_RECORD(kTp, q15t_dm_tp_thunk, q15t_dm_teleport);
NEVR_ASM_ENTRY_RECORD(kFire, q15t_dm_fire_thunk, q15t_dm_fire);
NEVR_ASM_ENTRY_RECORD(kPb, q15t_pb_resolve_thunk, q15t_pb_cone_at);
NEVR_ASM_ENTRY_RECORD(kHs, q15t_headset_tramp, q15t_headset_dispatch);
}  // namespace

int main() {
  sentinel::GotHook tp, fire, pb, hs;
  const sentinel::GotTarget target{"libprobe.so", "probe", sentinel::RelocKind::kJumpSlot};
  bool ok = sentinel::InstallAsmEntry(tp, target, kTp, &q15t_dm_tp_next) == sentinel::GotStatus::kOk;
  ok = ok && sentinel::InstallAsmEntry(fire, target, kFire, &q15t_dm_fire_next) == sentinel::GotStatus::kOk;
  ok = ok && sentinel::InstallAsmEntry(pb, target, kPb, &q15t_pb_resolve_real) == sentinel::GotStatus::kOk;
  ok = ok && sentinel::InstallAsmEntry(hs, target, kHs, &q15t_headset_next) == sentinel::GotStatus::kOk;
  return ok ? 0 : 1;
}

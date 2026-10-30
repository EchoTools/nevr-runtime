// Host test of the return-address-gated headset wrappers: a call that returns into the gated range is answered
// "not a Quest"; every other caller gets the real answer. Real mprotect, real /proc/self/maps, a fake slot page.
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "headset_gates.h"

extern "C" char __start_gate_in[], __stop_gate_in[];
extern "C" char __start_gate_link[], __stop_gate_link[];
extern "C" char __start_gate_zz[], __stop_gate_zz[];

static int g_fail = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      ++g_fail;                                           \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
      std::printf(__VA_ARGS__);                           \
      std::printf("\n");                                  \
    }                                                     \
  } while (0)

extern "C" __attribute__((noinline)) int32_t PartyReal(void*, uint32_t) { return 3; }
extern "C" __attribute__((noinline)) int32_t HeadsetReal() { return 3; }

using PartyFn = int32_t (*)(void*, uint32_t);
using HeadFn = int32_t (*)();

__attribute__((noinline, section("gate_in"))) int32_t PartyCallerIn(PartyFn f) {
  volatile int32_t r = f(nullptr, 0);
  return r;
}
__attribute__((noinline)) int32_t PartyCallerOut(PartyFn f) {
  volatile int32_t r = f(nullptr, 0);
  return r;
}
__attribute__((noinline, section("gate_in"))) int32_t HeadCallerIn(HeadFn f) {
  volatile int32_t r = f();
  return r;
}
__attribute__((noinline, section("gate_link"))) int32_t HeadCallerLink(HeadFn f) {
  volatile int32_t r = f();
  return r;
}
__attribute__((noinline, section("gate_zz"))) int32_t HeadCallerAfter(HeadFn f) {
  volatile int32_t r = f();
  return r;
}
__attribute__((noinline)) int32_t HeadCallerOut(HeadFn f) {
  volatile int32_t r = f();
  return r;
}

int main() {
  void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) std::abort();
  auto* w = static_cast<uintptr_t*>(p);
  const uintptr_t base = reinterpret_cast<uintptr_t>(p);
  w[4] = reinterpret_cast<uintptr_t>(&PartyReal);  // slot at 0x20
  w[5] = reinterpret_cast<uintptr_t>(&HeadsetReal);  // slot at 0x28
  mprotect(p, 4096, PROT_READ);

  const q15t::CallerRange range{reinterpret_cast<uintptr_t>(__start_gate_in) - base,
                                reinterpret_cast<uintptr_t>(__stop_gate_in) - base};
  const auto r1 = q15t::InstallPartyHeadsetGate(base, 0x20, range, reinterpret_cast<uintptr_t>(&PartyReal) - base);
  const q15t::CallerRange link_range{reinterpret_cast<uintptr_t>(__start_gate_link) - base,
                                     reinterpret_cast<uintptr_t>(__stop_gate_link) - base};
  const auto r2 = q15t::InstallHeadsetGate(base, 0x28, range, reinterpret_cast<uintptr_t>(&HeadsetReal) - base, link_range,
                                           q15t::kNoHeadsetFlags);
  CHECK(r1.status == q15t::SwapStatus::kOk, "party install: %s", q15t::StatusName(r1.status));
  CHECK(r2.status == q15t::SwapStatus::kOk, "headset install: %s", q15t::StatusName(r2.status));

  const auto party = reinterpret_cast<PartyFn>(w[4]);
  const auto head = reinterpret_cast<HeadFn>(w[5]);
  CHECK(party != &PartyReal && head != &HeadsetReal, "slots not replaced");
  CHECK(PartyCallerIn(party) == q15t::kNotQuest, "party: a call from the gated range must answer not-a-Quest");
  CHECK(PartyCallerOut(party) == 3, "party: any other caller must get the real answer (3)");
  CHECK(HeadCallerIn(head) == q15t::kNotQuest, "headset: a call from the gated range must answer not-a-Quest");
  CHECK(HeadCallerOut(head) == 3, "headset: any other caller must get the real answer (3)");
  // the second range (the headset-type expression's Bind) gets its own answer, one the Bind has no case for
  CHECK(q15t::kNoHeadsetFlags > 4, "the answer must be above the Bind's last case (4)");
  CHECK(HeadCallerLink(head) == q15t::kNoHeadsetFlags, "headset: a call from the link range must get the no-flags answer");
  CHECK(HeadCallerIn(head) == q15t::kNotQuest, "headset: the bots range keeps its own answer next to the link range");
  CHECK(HeadCallerOut(head) == 3, "headset: any other caller still gets the real answer next to the link range");
  // callers on both sides of the link range keep the real answer: the range has an upper edge as well as a lower one
  {
    const auto after = reinterpret_cast<uintptr_t>(__start_gate_zz);
    const auto link_hi = reinterpret_cast<uintptr_t>(__stop_gate_link);
    const auto link_lo = reinterpret_cast<uintptr_t>(__start_gate_link);
    CHECK(after >= link_hi || after < link_lo, "layout: the 'after' caller sits inside the link range");
    CHECK(HeadCallerAfter(head) == 3, "headset: a caller outside the link range (above or below) must get the real answer");
    CHECK(after >= link_hi, "layout: the linker did not place the 'after' section above the link section (%p vs %p)", (void*)after, (void*)link_hi);
  }
  const q15t::GateStats s = q15t::GetGateStats();
  CHECK(s.party_calls == 2 && s.party_gated == 1 && s.headset_calls == 6 && s.headset_gated == 2 && s.link_gated == 1,
        "counters %u/%u %u/%u link %u", s.party_calls, s.party_gated, s.headset_calls, s.headset_gated, s.link_gated);

  // the library policy: a caller from the link range that cannot be named (the host has no Bind frame) is answered only by "all"
  q15t::SetLinkGateLibs("none");
  CHECK(HeadCallerLink(head) == 3, "policy none: the link range must get the real answer (got %d)", HeadCallerLink(head));
  q15t::SetLinkGateLibs("lib555db38933fef859.so,lib1da85b18cbad6297.so");
  CHECK(HeadCallerLink(head) == 3, "policy list: a library that cannot be named is not in the list (got %d)", HeadCallerLink(head));
  q15t::SetLinkGateLibs("all");
  CHECK(HeadCallerLink(head) == q15t::kNoHeadsetFlags, "policy all: the link range must be answered again");

  // link gate alone: the other range empty must gate nothing, not everything
  {
    void* q = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    auto* v = static_cast<uintptr_t*>(q);
    const uintptr_t base3 = reinterpret_cast<uintptr_t>(q);
    v[5] = reinterpret_cast<uintptr_t>(&HeadsetReal);
    mprotect(q, 4096, PROT_READ);
    const q15t::CallerRange link3{reinterpret_cast<uintptr_t>(__start_gate_link) - base3, reinterpret_cast<uintptr_t>(__stop_gate_link) - base3};
    const auto r3 = q15t::InstallHeadsetGate(base3, 0x28, q15t::CallerRange{0, 0}, reinterpret_cast<uintptr_t>(&HeadsetReal) - base3, link3,
                                             q15t::kNoHeadsetFlags);
    CHECK(r3.status == q15t::SwapStatus::kOk, "link-only install: %s", q15t::StatusName(r3.status));
    const auto h3 = reinterpret_cast<HeadFn>(v[5]);
    CHECK(HeadCallerLink(h3) == q15t::kNoHeadsetFlags, "link only: the link range must answer");
    CHECK(HeadCallerIn(h3) == 3 && HeadCallerOut(h3) == 3, "link only: an empty other range must gate nothing (got %d / %d)", HeadCallerIn(h3), HeadCallerOut(h3));
  }

  // a slot that does not hold the pinned function is refused and left alone
  w = static_cast<uintptr_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  w[4] = 0x4242420;
  mprotect(w, 4096, PROT_READ);
  const uintptr_t base2 = reinterpret_cast<uintptr_t>(w);
  const auto bad = q15t::InstallPartyHeadsetGate(base2, 0x20, range, reinterpret_cast<uintptr_t>(&PartyReal) - base2);
  CHECK(bad.status == q15t::SwapStatus::kUnexpectedOriginal && w[4] == 0x4242420, "foreign slot word: %s", q15t::StatusName(bad.status));

  if (g_fail == 0) std::printf("gates_test: all checks pass\n");
  else std::printf("gates_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}

// Host test of the `ecfx` module (src/quest/plugins/q15t/src/ecfx.cpp, src/quest/plugins/q15t/src/ecfx_core.h): the NEUTRON queue rules, and attach / tick / gate against a
// stand-in wall lib that is really loaded (soname libac4e9374a146d3a4.so, src/quest/tests/q15t/ecfx_wall_stub.cpp).
// usage: ecfx_test full|missing <path to the stand-in lib>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ec.h"
#include "ec_core.h"
#include "ecfx.h"
#include "ecfx_core.h"
#include "nevr/plugin_interface.h"
#include "plugin_log.h"

using namespace q15t;
using namespace q15t::ecfx;

static int g_fail = 0;
#define CHECK(cond, ...)                                \
  do {                                                  \
    if (!(cond)) {                                      \
      ++g_fail;                                         \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
    }                                                   \
  } while (0)

struct Line {
  uint32_t level;
  std::string text;
};
static std::vector<Line> g_lines;
static void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = std::string("plugin=") + id + " " + msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  g_lines.push_back({level, t});
}
static bool HasLine(const char* needle, int level = -1) {
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) return true;
  return false;
}
static int CountLines(const char* needle) {
  int n = 0;
  for (const auto& l : g_lines) n += l.text.find(needle) != std::string::npos;
  return n;
}

// ---------- the pure queue rules ----------
alignas(16) static uint64_t g_q[2 * kSlots];
static void Put(int slot, uint64_t ctx, uint64_t word) {
  g_q[2 * slot] = ctx;
  g_q[2 * slot + 1] = word;
}
static uint64_t WallFake(uint64_t ctx) { return ctx == 0x40 ? 0 : 0x9000 + ctx; }
static const uint64_t kE0 = 0x10000, kE1 = 0x10010, kE2 = 0x10020, kE3 = 0x10030;
static Entries Ents() {
  Entries e;
  e.entry[0] = kE0;
  e.entry[1] = kE1;
  e.entry[2] = kE2;
  e.entry[3] = kE3;
  return e;
}

static void RuleTests() {
  Run run[kSlots];
  Counts c;
  Drain d;
  std::memset(g_q, 0, sizeof g_q);
  CHECK(d.Plan(g_q, Ents(), 1000, &WallFake, run, &c) == 0, "empty queue");

  // lowest kind first, one entry per ctx; the other slot of that ctx stays for the next drain
  Put(0, 0x100, kE1 | 1);
  Put(1, 0x100, kE0 | 0);
  c = Counts();
  int n = d.Plan(g_q, Ents(), 1010, &WallFake, run, &c);
  CHECK(n == 1 && run[0].kind == kCharge && run[0].slot == 1 && run[0].fn == kE0 && run[0].ctx == 0x100 && c.run == 1, "lowest kind first: n %d kind %d", n, n ? run[0].kind : -1);
  CHECK(g_q[0] == 0x100 && g_q[1] == (kE1 | 1) && g_q[2] == 0x100, "planned runs must not be freed by the planner (Q-1)");
  Free(g_q, run[0].slot);
  CHECK(g_q[2] == 0 && g_q[3] == 0, "Free clears word and ctx");
  c = Counts();
  n = d.Plan(g_q, Ents(), 1020, &WallFake, run, &c);
  CHECK(n == 1 && run[0].kind == kBurst, "the other kind of that ctx runs on the next drain");
  Free(g_q, run[0].slot);

  // two ctxs: both run in one drain
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE0);
  c = Counts();
  n = d.Plan(g_q, Ents(), 1030, &WallFake, run, &c);
  CHECK(n == 2, "one entry per ctx: %d", n);
  Free(g_q, 0);
  Free(g_q, 1);

  // BOOM: freed, never run, counted
  Put(0, 0x100, kE2 | 2);
  c = Counts();
  n = d.Plan(g_q, Ents(), 1040, &WallFake, run, &c);
  CHECK(n == 0 && c.boom == 1 && g_q[0] == 0 && g_q[1] == 0, "boom must be freed and not run");
  // an entry that is not the lib's exact one for its kind: refused and freed
  Put(0, 0x100, 0x20000 | 0);
  c = Counts();
  n = d.Plan(g_q, Ents(), 1050, &WallFake, run, &c);
  CHECK(n == 0 && c.refused == 1 && g_q[0] == 0, "a foreign entry address must be refused");
  Put(0, 0x100, kE1 | 0);  // right address, wrong kind
  c = Counts();
  CHECK(d.Plan(g_q, Ents(), 1060, &WallFake, run, &c) == 0 && c.refused == 1, "an entry of another kind must be refused");
  Put(0, 0x100, kE0 | 4);  // flag bits above the kind
  c = Counts();
  CHECK(d.Plan(g_q, Ents(), 1070, &WallFake, run, &c) == 0 && c.refused == 1, "bits 2-3 of the word must be zero");
  Entries none;
  Put(0, 0x100, kE0);
  c = Counts();
  CHECK(d.Plan(g_q, none, 1080, &WallFake, run, &c) == 0 && c.refused == 1, "an unknown entry (0) is never run");
  Put(0, 0x100, 7);  // fn == 0 after the mask
  c = Counts();
  CHECK(d.Plan(g_q, none, 1090, &WallFake, run, &c) == 0 && c.refused == 1, "no function pointer");
  Put(0, 0x100, 3);  // fn == 0, kind sound, clean flag bits, and the entry for that kind unknown (0): equal, still refused
  c = Counts();
  CHECK(d.Plan(g_q, none, 1095, &WallFake, run, &c) == 0 && c.refused == 1 && c.sound == 0, "a null function pointer must be refused");

  // in flight: ctx set, word not yet: left alone
  Put(0, 0x100, 0);
  c = Counts();
  n = d.Plan(g_q, Ents(), 1100, &WallFake, run, &c);
  CHECK(n == 0 && c.inflight == 1 && g_q[0] == 0x100, "an in-flight slot must be left alone");
  g_q[1] = kE0;  // the lib completes it
  c = Counts();
  CHECK(d.Plan(g_q, Ents(), 1110, &WallFake, run, &c) == 1, "a completed slot runs");
  Free(g_q, 0);

  // a slot of the same ctx that is still in flight (word 0) is not a candidate for "lowest kind"
  Put(0, 0x100, kE1 | 1);
  Put(1, 0x100, 0);
  c = Counts();
  n = d.Plan(g_q, Ents(), 1115, &WallFake, run, &c);
  CHECK(n == 1 && run[0].kind == kBurst && run[0].slot == 0 && c.refused == 0, "an in-flight slot of the same ctx was chosen: n %d kind %d slot %d", n, n ? run[0].kind : -1, n ? run[0].slot : -1);
  Free(g_q, 0);
  Free(g_q, 1);

  // sound: unreadable wall refused; once per wall per 3 s; 8 walls remembered, the oldest replaced
  Put(0, 0x40, kE3 | 3);
  c = Counts();
  CHECK(d.Plan(g_q, Ents(), 1120, &WallFake, run, &c) == 0 && c.sound == 1 && g_q[0] == 0, "a sound with an unreadable wall must be refused");
  Put(0, 0x100, kE3 | 3);
  c = Counts();
  CHECK(d.Plan(g_q, Ents(), 1130, &WallFake, run, &c) == 1 && run[0].kind == kSound, "first sound runs");
  Free(g_q, 0);
  Put(0, 0x100, kE3 | 3);
  c = Counts();
  CHECK(d.Plan(g_q, Ents(), 1140, &WallFake, run, &c) == 0 && c.sound == 1, "a repeated sound within 3 s must be refused");
  for (uint64_t t = 1140 + 400; t < 1130 + 3000 - 1; t += 400) {  // idle drains: a gap over 500 ms would make the queue stale
    c = Counts();
    d.Plan(g_q, Ents(), t, &WallFake, run, &c);
  }
  c = Counts();
  d.Plan(g_q, Ents(), 1130 + 3000 - 2, &WallFake, run, &c);
  Put(0, 0x100, kE3 | 3);
  c = Counts();
  CHECK(d.Plan(g_q, Ents(), 1130 + 3000 - 1, &WallFake, run, &c) == 0 && c.sound == 1, "a sound 2999 ms after the last must be refused");
  Put(0, 0x100, kE3 | 3);
  c = Counts();
  CHECK(d.Plan(g_q, Ents(), 1130 + 3000, &WallFake, run, &c) == 1, "a sound runs again after exactly 3000 ms");
  Free(g_q, 0);
  Sounds s;
  for (uint64_t w = 1; w <= 8; ++w) s.Again(w, 100 + w);
  CHECK(!s.Again(9, 200), "ninth wall is new");
  CHECK(!s.Again(1, 201), "the oldest wall was forgotten (replaced by the ninth)");
  CHECK(s.Again(9, 202), "the ninth is remembered");

  // stale: more than 500 ms since the last drain frees every complete slot, runs nothing
  Drain d2;
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE1 | 1);
  Put(2, 0x300, 0);  // in flight: stays
  c = Counts();
  n = d2.Plan(g_q, Ents(), 5000, &WallFake, run, &c);  // first drain: not stale (no last)
  CHECK(n == 2 && c.stale == 0, "the first drain is never stale: n %d stale %d", n, c.stale);
  Free(g_q, 0);
  Free(g_q, 1);
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE1 | 1);
  c = Counts();
  n = d2.Plan(g_q, Ents(), 5000 + 500, &WallFake, run, &c);
  CHECK(n == 2, "exactly 500 ms is not stale: %d", n);
  Free(g_q, 0);
  Free(g_q, 1);
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE1 | 1);
  c = Counts();
  n = d2.Plan(g_q, Ents(), 5000 + 500 + 500 + 1, &WallFake, run, &c);
  CHECK(n == 0 && c.stale == 2 && g_q[0] == 0 && g_q[2] == 0 && g_q[4] == 0x300, "a stale queue must be freed unrun, in-flight left: n %d stale %d", n, c.stale);
  std::memset(g_q, 0, sizeof g_q);
}

// ---------- attach / tick / gate against the loaded stand-in ----------
static void Dummy() {}
alignas(16) static uint8_t g_ctxobj[2][0x80];
static std::vector<std::string> g_hook_notes;
static uint64_t g_now = 10000;
static int g_hook_slot_ctx_nonzero = 0, g_hook_runs = 0;
static int g_reenter = 0;
static int* g_calls = nullptr;
static void Hook(void* tp, int kind) {
  ++g_hook_runs;
  uint64_t* q = reinterpret_cast<uint64_t*>(EcPage() + kQueueOff);
  // Q-1: while the entry runs, its slot is still claimed
  for (int i = 0; i < kSlots; ++i)
    if (q[2 * i] == reinterpret_cast<uint64_t>(tp) && q[2 * i + 1] != 0) ++g_hook_slot_ctx_nonzero;
  if (g_reenter) {
    const int before = g_calls[0] + g_calls[1] + g_calls[2] + g_calls[3];
    EcfxTickForTest(g_now + 100);  // a re-entry from inside an entry
    CHECK(g_calls[0] + g_calls[1] + g_calls[2] + g_calls[3] == before, "a re-entry ran an entry");
  }
  (void)kind;
}
alignas(16) static void LocalEntry(void*) {}

static uint64_t* Q() { return reinterpret_cast<uint64_t*>(EcPage() + kQueueOff); }
static void QPut(int slot, uint64_t ctx, uint64_t word) {
  Q()[2 * slot] = ctx;
  Q()[2 * slot + 1] = word;
}
static void Tick(uint64_t advance_ms = 10) {
  g_now += advance_ms;
  EcfxTickForTest(g_now);
}

static void FullTests(const char* path) {
  EcResetForTest();
  EcfxResetForTest();
  SetEcNoThreadForTest(true);
  SetEcfxNoThreadForTest(true);
  for (int i = 0; i < 2; ++i) {
    std::memset(g_ctxobj[i], 0, sizeof g_ctxobj[i]);
    *reinterpret_cast<uint64_t*>(g_ctxobj[i] + 0x68) = 0x7000 + static_cast<uint64_t>(i);  // the wall's script data
  }
  // [ctx] = a: ctx is the address of a word holding a pointer to g_ctxobj
  static uint64_t holder[2];
  holder[0] = reinterpret_cast<uint64_t>(g_ctxobj[0]);
  holder[1] = reinterpret_cast<uint64_t>(g_ctxobj[1]);
  const uint64_t ctx0 = reinterpret_cast<uint64_t>(&holder[0]), ctx1 = reinterpret_cast<uint64_t>(&holder[1]);

  // the lib is not loaded: waiting, once, gate stays 0, no tick slot used
  CHECK(EcfxAttach() == EcfxStatus::kWaiting && EcfxAttach() == EcfxStatus::kWaiting, "not loaded: must wait");
  CHECK(CountLines("neutronfx: waiting") == 1, "the waiting line must be logged once: %d", CountLines("neutronfx: waiting"));
  CHECK(!EcfxGateOpenForTest(), "gate open without the lib");

  void* lib = dlopen(path, RTLD_NOW);
  CHECK(lib != nullptr, "cannot load the stand-in lib: %s", dlerror());
  if (lib == nullptr) return;
  auto* calls = static_cast<int*>(dlsym(lib, "re33_stub_calls"));
  g_calls = calls;
  auto** last_tp = static_cast<void**>(dlsym(lib, "re33_stub_last_tp"));
  auto* hook = static_cast<void (**)(void*, int)>(dlsym(lib, "re33_stub_hook"));
  *hook = &Hook;
  const uint64_t e0 = reinterpret_cast<uint64_t>(dlsym(lib, "re33_ecent_ac4e93_17900"));
  const uint64_t e1 = reinterpret_cast<uint64_t>(dlsym(lib, "re33_ecent_ac4e93_17700"));
  const uint64_t e2 = reinterpret_cast<uint64_t>(dlsym(lib, "re33_ecent_ac4e93_17b00"));
  const uint64_t e3 = reinterpret_cast<uint64_t>(dlsym(lib, "re33_ecent_ac4e93_17b60"));
  const uint64_t not_entry = reinterpret_cast<uint64_t>(dlsym(lib, "re33_not_an_entry"));
  CHECK(e0 && e1 && e2 && e3 && not_entry && !(e0 & 15) && !(e1 & 15) && !(e2 & 15) && !(e3 & 15), "stand-in entries");

  CHECK(EcfxAttach() == EcfxStatus::kOn, "a loaded lib with all four entries must attach");
  CHECK(HasLine("neutronfx: on", 1), "no INFO for attach");
  CHECK(!EcfxGateOpenForTest(), "the gate must not open at attach: only after a drain ran on the tick");
  // ecfx took one of the four net tick slots
  int free_slots = 0;
  while (EcOnNetTick(&Dummy)) ++free_slots;
  CHECK(free_slots == 3, "ecfx must hold exactly one net tick slot: %d free", free_slots);
  CHECK(EcfxAttach() == EcfxStatus::kOn, "attach is idempotent");

  // first tick: nothing queued, the gate opens
  Tick();
  CHECK(EcfxGateOpenForTest() && HasLine("neutronfx: gate +e20 = 1", 1), "the gate must open at the first drain");
  CHECK(*reinterpret_cast<uint32_t*>(EcPage() + kGateOff) == 1, "the gate word on the page must be 1");

  // a charge entry runs once with its ctx; the slot is freed after it returned (Q-1)
  QPut(0, ctx0, e0 | kCharge);
  g_hook_slot_ctx_nonzero = 0;
  g_hook_runs = 0;
  Tick();
  CHECK(calls[0] == 1 && last_tp[0] == reinterpret_cast<void*>(ctx0), "charge not run once with its ctx: %d", calls[0]);
  CHECK(g_hook_slot_ctx_nonzero == 1, "the slot must still be claimed while its entry runs");
  CHECK(Q()[0] == 0 && Q()[1] == 0, "the slot was not freed after the entry returned");
  CHECK(HasLine("neutronfx: ran") && HasLine("kind=charge"), "no ran record");

  // cadence: a tick within 8 ms of the last drain does nothing
  QPut(0, ctx0, e0 | kCharge);
  Tick(7);
  CHECK(calls[0] == 1, "ticked within 8 ms");
  Tick(1);
  CHECK(calls[0] == 2, "no drain after 8 ms");

  // BOOM freed, never run
  QPut(0, ctx0, e2 | kBoom);
  Tick();
  CHECK(calls[2] == 0 && Q()[0] == 0 && HasLine("neutronfx: drain") && HasLine("boom_freed=1"), "boom was run or not reported");
  // a foreign entry
  QPut(0, ctx0, not_entry | kCharge);
  Tick();
  CHECK(calls[0] == 2 && Q()[0] == 0, "a foreign entry was run");
  // charge and burst of one ctx: one per drain
  QPut(0, ctx0, e1 | kBurst);
  QPut(1, ctx0, e0 | kCharge);
  Tick();
  CHECK(calls[0] == 3 && calls[1] == 0, "one entry per ctx per drain, lowest kind first");
  Tick();
  CHECK(calls[1] == 1, "the second entry of that ctx runs on the next drain");
  // sound: once per wall per 3 s
  QPut(0, ctx1, e3 | kSound);
  Tick();
  CHECK(calls[3] == 1, "first sound");
  QPut(0, ctx1, e3 | kSound);
  Tick();
  CHECK(calls[3] == 1 && HasLine("sound_repeats=1"), "a repeated sound was run");
  for (int i = 0; i < 7; ++i) Tick(400);  // idle drains (a gap over 500 ms is a stale queue)
  QPut(0, ctx1, e3 | kSound);
  Tick(210);
  CHECK(calls[3] == 2, "a sound after 3 s");
  // a ctx that cannot be read: its sound is refused
  QPut(0, 0x40, e3 | kSound);
  Tick();
  CHECK(calls[3] == 2 && Q()[0] == 0, "a sound with an unreadable ctx ran");
  // stale: no drain for > 500 ms frees what is queued, runs nothing
  QPut(0, ctx0, e0 | kCharge);
  Tick(600);
  CHECK(calls[0] == 3 && Q()[0] == 0 && HasLine("stale_freed=1"), "a stale queue was run");
  // the entry is not the lib's any more (dladdr): not run, freed, WARNING
  EcfxSetEntryForTest(kBurst, reinterpret_cast<uint64_t>(&LocalEntry));
  QPut(0, ctx0, reinterpret_cast<uint64_t>(&LocalEntry) | kBurst);
  Tick();
  CHECK(calls[1] == 1 && Q()[0] == 0 && HasLine("entry is no longer the wall lib's", 2), "an address that is not the named entry was called");
  EcfxSetEntryForTest(kBurst, e1);
  // an address inside an entry, not its start: not the named entry (exact start), so not run
  EcfxSetEntryForTest(kCharge, e0 + 16);
  QPut(0, ctx0, (e0 + 16) | kCharge);
  Tick();
  CHECK(calls[0] == 3 && Q()[0] == 0 && HasLine("entry is no longer the wall lib's", 2), "an address inside the entry was called: %d", calls[0]);
  EcfxSetEntryForTest(kCharge, e0);
  // re-entry from inside an entry returns at once
  g_reenter = 1;
  QPut(0, ctx0, e0 | kCharge);
  Tick();
  g_reenter = 0;
  CHECK(calls[0] == 4, "the entry did not run for the re-entry test");
  *hook = nullptr;

  // the net tick registered by ecfx drives it: find it through a recvfrom-free path is covered in ec_test; here the tick function
  // is the registered one, so draining through EcfxTickForTest is the same code
  (void)free_slots;
}

static void MissingTests(const char* path) {
  EcResetForTest();
  EcfxResetForTest();
  SetEcNoThreadForTest(true);
  void* lib = dlopen(path, RTLD_NOW);
  CHECK(lib != nullptr, "cannot load the stand-in lib: %s", dlerror());
  if (lib == nullptr) return;
  CHECK(EcfxAttach() == EcfxStatus::kNoEntry, "a lib without the burst entry must not attach");
  CHECK(HasLine("neutronfx: OFF: the wall lib has no such entry", 3) && HasLine("entry=re33_ecent_ac4e93_17700"), "no ERROR for a missing entry");
  CHECK(!EcfxGateOpenForTest(), "the gate must stay 0 when an entry is missing");
  int free_slots = 0;
  while (EcOnNetTick(&Dummy)) ++free_slots;
  CHECK(free_slots == 4, "no net tick slot may be taken by a failed attach: %d free", free_slots);
  EcResetForTest();
  // a failed attach is not "waiting": it is reported again on the next try (the lib is still loaded and still wrong)
  CHECK(EcfxAttach() == EcfxStatus::kNoEntry, "still no entry");
}

static void SlotsFullTest(const char* path) {
  EcResetForTest();
  EcfxResetForTest();
  void* lib = dlopen(path, RTLD_NOW);
  if (lib == nullptr) return;
  for (int i = 0; i < 4; ++i) EcOnNetTick(&Dummy);
  CHECK(EcfxAttach() == EcfxStatus::kNoTickSlot, "no free net tick slot: must stay off");
  CHECK(HasLine("neutronfx: OFF: no free net tick slot", 3) && !EcfxGateOpenForTest(), "no ERROR / the gate opened");
}

#ifndef Q15T_ECFX_DEFAULT_ON
#define Q15T_ECFX_DEFAULT_ON 0
#endif

static void SwitchTest() {
  char tmpl[] = "/tmp/q15t-ecfx-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) std::exit(2);
  EcResetForTest();
  SetEcfxNoThreadForTest(true);
  SetEcDirForTest(tmpl);
  InstallResult r = InstallEcfx(0);
#if Q15T_ECFX_DEFAULT_ON
  CHECK(r.status == SwapStatus::kOk && std::string(r.reason) == "waiting", "default-on, no file: must wait for the lib, not be off: %s", r.reason);
#else
  CHECK(std::string(r.reason) == "off", "no neutronfx.on: must be off");
#endif
  const std::string f = std::string(tmpl) + "/neutronfx.on";
  FILE* fp = std::fopen(f.c_str(), "w");
  std::fclose(fp);
  r = InstallEcfx(0);
  CHECK(r.status == SwapStatus::kOk && std::string(r.reason) == "waiting", "neutronfx.on: must wait for the lib: %s", r.reason);
  unlink(f.c_str());
  rmdir(tmpl);
  SetEcDirForTest("");
}

int main(int argc, char** argv) {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  SetLogContext(&ctx);
  if (argc < 3) {
    std::printf("usage: ecfx_test full|missing <lib>\n");
    return 2;
  }
  if (!std::strcmp(argv[1], "full")) {
    RuleTests();
    SwitchTest();
    FullTests(argv[2]);
    SlotsFullTest(argv[2]);
  } else {
    MissingTests(argv[2]);
  }
  if (g_fail == 0) std::printf("ecfx_test %s: all checks passed\n", argv[1]);
  return g_fail == 0 ? 0 : 1;
}

// Host test of NEUTRON effects (src/quest/combat/ecfx.cpp, ecfx_core.h, and the drain rules they share with PC, src/runtime/patch/ecfx_rules.h;
// #2187, #2349): the queue rules, and attach / tick / gate against a stand-in
// wall library that is really loaded (soname libac4e9374a146d3a4.so, src/quest/tests/ecfx_wall_stub.cpp). Moved from the q15t plugin's ecfx_test:
// the assertions are the same; the lines are sentinel records (`combat_ecfx_*`), and the switch is `combat.ecfx` (the plugin's neutronfx.on is
// warned about and changes nothing), with the refusal when ec is not installed.
// usage: ecfx_test full|missing <path to the stand-in lib>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/ec.h"
#include "quest/combat/ecfx.h"
#include "quest/combat/ecfx_core.h"
#include "quest/combat/legacy_switch.h"
#include "quest/sentinel/ec_page.h"
#include "quest/sentinel/hook_log.h"

using namespace nevr_quest::combat;
using namespace nevr_quest::combat::ecfx;
using namespace nevr_quest::combat::ecfx::core;
using sentinel::LogLevel;

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
  LogLevel level;
  std::string text;
};
static std::vector<Line> g_lines;
static void Capture(LogLevel level, const char* line) { g_lines.push_back({level, line}); }
// level -1: any, 1 Info, 2 Warning, 3 Error (the plugin test's numbers)
static bool HasLine(const char* needle, int level = -1) {
  const LogLevel want = level == 1 ? LogLevel::kInfo : level == 2 ? LogLevel::kWarn : LogLevel::kError;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || l.level == want)) return true;
  return false;
}
static uint8_t* EcPage() { return sentinel::EcPage(); }
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

// One drain of the shared rules (runtime/patch/ecfx_rules.h, #2349) with the entries known: each entry the drain runs is recorded in out,
// in the order run, and g_held says whether every slot was still claimed while its entry ran (the drain frees it after it returned, Q-1).
static bool g_held = true;
static int PlanInto(Drain& d, const Entries& e, uint64_t now, Run* out, Counts* c) {
  Env env;
  env.entries = &e;
  env.wallOf = &WallFake;
  int k = 0;
  return d.Plan(g_q, env, now, [&](const Run& r) {
    g_held = g_held && g_q[2 * r.slot] == r.ctx && g_q[2 * r.slot + 1] != 0;
    out[k++] = r;
  }, c);
}

static void RuleTests() {
  Run run[kSlots];
  Counts c;
  Drain d;
  std::memset(g_q, 0, sizeof g_q);
  CHECK(PlanInto(d, Ents(), 1000, run, &c) == 0, "empty queue");

  // lowest kind first, one entry per ctx; the other slot of that ctx stays for the next drain
  Put(0, 0x100, kE1 | 1);
  Put(1, 0x100, kE0 | 0);
  c = Counts();
  int n = PlanInto(d, Ents(), 1010, run, &c);
  CHECK(n == 1 && run[0].kind == kCharge && run[0].slot == 1 && run[0].fn == kE0 && run[0].ctx == 0x100 && c.run == 1, "lowest kind first: n %d kind %d", n, n ? run[0].kind : -1);
  CHECK(g_held, "a slot must still be claimed while its entry runs (Q-1)");
  CHECK(g_q[0] == 0x100 && g_q[1] == (kE1 | 1) && g_q[2] == 0 && g_q[3] == 0, "the run slot is freed after its entry returned, the other kept");
  c = Counts();
  n = PlanInto(d, Ents(), 1020, run, &c);
  CHECK(n == 1 && run[0].kind == kBurst, "the other kind of that ctx runs on the next drain");
  CHECK(g_q[0] == 0 && g_q[1] == 0, "the drain frees the slot it ran");

  // two ctxs: both run in one drain
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE0);
  c = Counts();
  n = PlanInto(d, Ents(), 1030, run, &c);
  CHECK(n == 2, "one entry per ctx: %d", n);
  CHECK(g_q[0] == 0 && g_q[1] == 0 && g_q[2] == 0 && g_q[3] == 0, "the drain frees the slot it ran");

  // BOOM: freed, never run, counted
  Put(0, 0x100, kE2 | 2);
  c = Counts();
  n = PlanInto(d, Ents(), 1040, run, &c);
  CHECK(n == 0 && c.boom == 1 && g_q[0] == 0 && g_q[1] == 0, "boom must be freed and not run");
  // an entry that is not the lib's exact one for its kind: refused and freed
  Put(0, 0x100, 0x20000 | 0);
  c = Counts();
  n = PlanInto(d, Ents(), 1050, run, &c);
  CHECK(n == 0 && c.refused == 1 && g_q[0] == 0, "a foreign entry address must be refused");
  Put(0, 0x100, kE1 | 0);  // right address, wrong kind
  c = Counts();
  CHECK(PlanInto(d, Ents(), 1060, run, &c) == 0 && c.refused == 1, "an entry of another kind must be refused");
  Put(0, 0x100, kE0 | 4);  // flag bits above the kind
  c = Counts();
  CHECK(PlanInto(d, Ents(), 1070, run, &c) == 0 && c.refused == 1, "bits 2-3 of the word must be zero");
  Entries none;
  Put(0, 0x100, kE0);
  c = Counts();
  CHECK(PlanInto(d, none, 1080, run, &c) == 0 && c.refused == 1, "an unknown entry (0) is never run");
  Put(0, 0x100, 7);  // fn == 0 after the mask
  c = Counts();
  CHECK(PlanInto(d, none, 1090, run, &c) == 0 && c.refused == 1, "no function pointer");
  Put(0, 0x100, 3);  // fn == 0, kind sound, clean flag bits, and the entry for that kind unknown (0): equal, still refused
  c = Counts();
  CHECK(PlanInto(d, none, 1095, run, &c) == 0 && c.refused == 1 && c.sound == 0, "a null function pointer must be refused");

  // in flight: ctx set, word not yet: left alone
  Put(0, 0x100, 0);
  c = Counts();
  n = PlanInto(d, Ents(), 1100, run, &c);
  CHECK(n == 0 && c.inflight == 1 && g_q[0] == 0x100, "an in-flight slot must be left alone");
  g_q[1] = kE0;  // the lib completes it
  c = Counts();
  CHECK(PlanInto(d, Ents(), 1110, run, &c) == 1, "a completed slot runs");
  CHECK(g_q[0] == 0 && g_q[1] == 0, "the drain frees the slot it ran");

  // a slot of the same ctx that is still in flight (word 0) is not a candidate for "lowest kind"
  Put(0, 0x100, kE1 | 1);
  Put(1, 0x100, 0);
  c = Counts();
  n = PlanInto(d, Ents(), 1115, run, &c);
  CHECK(n == 1 && run[0].kind == kBurst && run[0].slot == 0 && c.refused == 0, "an in-flight slot of the same ctx was chosen: n %d kind %d slot %d", n, n ? run[0].kind : -1, n ? run[0].slot : -1);
  CHECK(g_q[0] == 0 && g_q[1] == 0, "the drain frees the slot it ran");
  Free(g_q, 1);

  // sound: unreadable wall refused; once per wall per 3 s; 8 walls remembered, the oldest replaced
  Put(0, 0x40, kE3 | 3);
  c = Counts();
  CHECK(PlanInto(d, Ents(), 1120, run, &c) == 0 && c.sound == 1 && g_q[0] == 0, "a sound with an unreadable wall must be refused");
  Put(0, 0x100, kE3 | 3);
  c = Counts();
  CHECK(PlanInto(d, Ents(), 1130, run, &c) == 1 && run[0].kind == kSound, "first sound runs");
  CHECK(g_q[0] == 0 && g_q[1] == 0, "the drain frees the slot it ran");
  Put(0, 0x100, kE3 | 3);
  c = Counts();
  CHECK(PlanInto(d, Ents(), 1140, run, &c) == 0 && c.sound == 1, "a repeated sound within 3 s must be refused");
  for (uint64_t t = 1140 + 400; t < 1130 + 3000 - 1; t += 400) {  // idle drains: a gap over 500 ms would make the queue stale
    c = Counts();
    PlanInto(d, Ents(), t, run, &c);
  }
  c = Counts();
  PlanInto(d, Ents(), 1130 + 3000 - 2, run, &c);
  Put(0, 0x100, kE3 | 3);
  c = Counts();
  CHECK(PlanInto(d, Ents(), 1130 + 3000 - 1, run, &c) == 0 && c.sound == 1, "a sound 2999 ms after the last must be refused");
  Put(0, 0x100, kE3 | 3);
  c = Counts();
  CHECK(PlanInto(d, Ents(), 1130 + 3000, run, &c) == 1, "a sound runs again after exactly 3000 ms");
  CHECK(g_q[0] == 0 && g_q[1] == 0, "the drain frees the slot it ran");
  Sounds s;
  for (uint64_t w = 1; w <= 8; ++w) s.Again(w, 100 + w);
  CHECK(!s.Again(9, 200), "ninth wall is new");
  CHECK(!s.Again(1, 201), "the oldest wall was forgotten (replaced by the ninth)");
  CHECK(s.Again(9, 202), "the ninth is remembered");

  CHECK(!Sounds().Again(0, 1000), "an empty remembered slot matches no wall (#2349 D4)");

  // EC's order (#2349 D5): an entry runs as the drain reaches its slot, so an effect a running entry queues into a later slot, for a node
  // not yet run, runs this drain; one queued into a slot already passed, or for a node that already ran, waits for the next drain
  {
    std::memset(g_q, 0, sizeof g_q);
    Drain d5;
    Env env;
    const Entries ents = Ents();
    env.entries = &ents;
    env.wallOf = &WallFake;
    int queued = 0;
    uint64_t order[kSlots] = {};
    int k = 0;
    Put(1, 0x100, kE0);
    c = Counts();
    n = d5.Plan(g_q, env, 9000, [&](const Run& r) {
      order[k++] = r.ctx;
      if (r.ctx != 0x100 || queued++) return;
      Put(3, 0x200, kE1 | 1);  // a later slot, another node
      Put(0, 0x300, kE0);      // a slot already passed
      Put(4, 0x100, kE1 | 1);  // a node that already ran
    }, &c);
    CHECK(n == 2 && order[1] == 0x200 && g_q[6] == 0 && g_q[0] == 0x300 && g_q[8] == 0x100, "an effect queued during the drain: n %d", n);
    c = Counts();
    k = 0;
    CHECK(d5.Plan(g_q, env, 9010, [&](const Run& r) { order[k++] = r.ctx; }, &c) == 2, "the waiting effects run on the next drain");
    std::memset(g_q, 0, sizeof g_q);
  }

  // stale: more than 500 ms since the last drain frees every complete slot, runs nothing
  Drain d2;
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE1 | 1);
  Put(2, 0x300, 0);  // in flight: stays
  c = Counts();
  n = PlanInto(d2, Ents(), 5000, run, &c);  // first drain: not stale (no last)
  CHECK(n == 2 && c.stale == 0, "the first drain is never stale: n %d stale %d", n, c.stale);
  CHECK(g_q[0] == 0 && g_q[1] == 0 && g_q[2] == 0 && g_q[3] == 0, "the drain frees the slot it ran");
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE1 | 1);
  c = Counts();
  n = PlanInto(d2, Ents(), 5000 + 500, run, &c);
  CHECK(n == 2, "exactly 500 ms is not stale: %d", n);
  CHECK(g_q[0] == 0 && g_q[1] == 0 && g_q[2] == 0 && g_q[3] == 0, "the drain frees the slot it ran");
  Put(0, 0x100, kE0);
  Put(1, 0x200, kE1 | 1);
  c = Counts();
  n = PlanInto(d2, Ents(), 5000 + 500 + 500 + 1, run, &c);
  CHECK(n == 0 && c.stale == 2 && g_q[0] == 0 && g_q[2] == 0 && g_q[4] == 0x300, "a stale queue must be freed unrun, in-flight left: n %d stale %d", n, c.stale);
  std::memset(g_q, 0, sizeof g_q);
}

// ---------- attach / tick / gate against the loaded stand-in ----------
static void Dummy() noexcept {}
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
    TickForTest(g_now + 100);  // a re-entry from inside an entry
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
  TickForTest(g_now);
}

static void FullTests(const char* path) {
  ec::ResetForTest();
  ecfx::ResetForTest();
  ec::SetNoThreadForTest(true);
  ecfx::SetNoThreadForTest(true);
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
  CHECK(Attach() == Status::kWaiting && Attach() == Status::kWaiting, "not loaded: must wait");
  CHECK(CountLines("\"event\":\"combat_ecfx_waiting\"") == 1, "the waiting line must be logged once: %d", CountLines("\"event\":\"combat_ecfx_waiting\""));
  CHECK(!GateOpenForTest(), "gate open without the lib");

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

  CHECK(Attach() == Status::kOn, "a loaded lib with all four entries must attach");
  CHECK(HasLine("\"event\":\"combat_ecfx_attach\",\"status\":\"on\"", 1), "no INFO for attach");
  CHECK(!GateOpenForTest(), "the gate must not open at attach: only after a drain ran on the tick");
  // ecfx took one of the four net tick slots
  int free_slots = 0;
  while (ec::OnNetTick(&Dummy)) ++free_slots;
  CHECK(free_slots == 3, "ecfx must hold exactly one net tick slot: %d free", free_slots);
  CHECK(Attach() == Status::kOn, "attach is idempotent");

  // first tick: nothing queued, the gate opens
  Tick();
  CHECK(GateOpenForTest() && HasLine("\"event\":\"combat_ecfx_gate\"", 1), "the gate must open at the first drain");
  CHECK(*reinterpret_cast<uint32_t*>(EcPage() + kGateOff) == 1, "the gate word on the page must be 1");

  // a charge entry runs once with its ctx; the slot is freed after it returned (Q-1)
  QPut(0, ctx0, e0 | kCharge);
  g_hook_slot_ctx_nonzero = 0;
  g_hook_runs = 0;
  Tick();
  CHECK(calls[0] == 1 && last_tp[0] == reinterpret_cast<void*>(ctx0), "charge not run once with its ctx: %d", calls[0]);
  CHECK(g_hook_slot_ctx_nonzero == 1, "the slot must still be claimed while its entry runs");
  CHECK(Q()[0] == 0 && Q()[1] == 0, "the slot was not freed after the entry returned");
  CHECK(HasLine("\"event\":\"combat_ecfx_ran\"") && HasLine("\"kind\":\"charge\""), "no ran record");

  // cadence: a tick within 8 ms of the last drain does nothing
  QPut(0, ctx0, e0 | kCharge);
  Tick(7);
  CHECK(calls[0] == 1, "ticked within 8 ms");
  Tick(1);
  CHECK(calls[0] == 2, "no drain after 8 ms");

  // BOOM freed, never run
  QPut(0, ctx0, e2 | kBoom);
  Tick();
  CHECK(calls[2] == 0 && Q()[0] == 0 && HasLine("\"event\":\"combat_ecfx_drain\"") && HasLine("\"boom_freed\":1"), "boom was run or not reported");
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
  CHECK(calls[3] == 1 && HasLine("\"sound_repeats\":1"), "a repeated sound was run");
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
  CHECK(calls[0] == 3 && Q()[0] == 0 && HasLine("\"stale_freed\":1"), "a stale queue was run");
  // the entry is not the lib's any more (dladdr): not run, freed, WARNING
  SetEntryForTest(kBurst, reinterpret_cast<uint64_t>(&LocalEntry));
  QPut(0, ctx0, reinterpret_cast<uint64_t>(&LocalEntry) | kBurst);
  Tick();
  CHECK(calls[1] == 1 && Q()[0] == 0 && HasLine("no longer the wall library's", 2), "an address that is not the named entry was called");
  SetEntryForTest(kBurst, e1);
  // an address inside an entry, not its start: not the named entry (exact start), so not run
  SetEntryForTest(kCharge, e0 + 16);
  QPut(0, ctx0, (e0 + 16) | kCharge);
  Tick();
  CHECK(calls[0] == 3 && Q()[0] == 0 && HasLine("no longer the wall library's", 2), "an address inside the entry was called: %d", calls[0]);
  SetEntryForTest(kCharge, e0);
  // re-entry from inside an entry returns at once
  g_reenter = 1;
  QPut(0, ctx0, e0 | kCharge);
  Tick();
  g_reenter = 0;
  CHECK(calls[0] == 4, "the entry did not run for the re-entry test");
  *hook = nullptr;

  // the record budget (#2349): the ran and drain records are written up to 96 in all, then one combat_ecfx_lines record says the
  // later ones are counted, then combat_ecfx_sum carries the counts at most every 10 s, and once none has come for 10 s
  int ticks = 0;
  while (CountLines("\"event\":\"combat_ecfx_lines\"") == 0 && ticks < 200) {
    QPut(0, ctx0, e2 | kBoom);
    Tick();
    ++ticks;
  }
  CHECK(CountLines("\"event\":\"combat_ecfx_lines\"") == 1 && HasLine("\"records\":97", 1), "no combat_ecfx_lines record at the 97th record");
  CHECK(CountLines("\"event\":\"combat_ecfx_ran\"") + CountLines("\"event\":\"combat_ecfx_drain\"") == 96,
        "the ran and drain records written: %d, want 96", CountLines("\"event\":\"combat_ecfx_ran\"") + CountLines("\"event\":\"combat_ecfx_drain\""));
  for (int i = 0; i < 5; ++i) {  // within 10 s of the last line: counted, nothing written
    QPut(0, ctx0, e2 | kBoom);
    Tick();
  }
  CHECK(CountLines("\"event\":\"combat_ecfx_sum\"") == 0 && CountLines("\"event\":\"combat_ecfx_drain\"") + CountLines("\"event\":\"combat_ecfx_ran\"") == 96,
        "a record was written within 10 s of the cap");
  QPut(0, ctx0, e2 | kBoom);
  Tick(10000);  // the first record at least 10 s after the last line: the count goes out
  CHECK(CountLines("\"event\":\"combat_ecfx_sum\"") == 1 && HasLine("\"records\":103,\"since_last\":6"), "no count 10 s after the cap");
  QPut(0, ctx0, e2 | kBoom);
  Tick(400);
  Tick(9992);  // quiet for 9992 ms since the last record: nothing yet
  CHECK(CountLines("\"event\":\"combat_ecfx_sum\"") == 1, "the count went out before 10 s of quiet");
  Tick(8);  // quiet for 10 s (the next drain the 8 ms cadence lets through): the pending count is flushed
  CHECK(CountLines("\"event\":\"combat_ecfx_sum\"") == 2 && HasLine("\"records\":104,\"since_last\":1"), "the pending count was not flushed after 10 s of quiet");

  // the net tick registered by ecfx drives it: find it through a recvfrom-free path is covered in ec_test; here the tick function
  // is the registered one, so draining through EcfxTickForTest is the same code
  (void)free_slots;
}

static void MissingTests(const char* path) {
  ec::ResetForTest();
  ecfx::ResetForTest();
  ec::SetNoThreadForTest(true);
  void* lib = dlopen(path, RTLD_NOW);
  CHECK(lib != nullptr, "cannot load the stand-in lib: %s", dlerror());
  if (lib == nullptr) return;
  CHECK(Attach() == Status::kNoEntry, "a lib without the burst entry must not attach");
  CHECK(HasLine("\"status\":\"no_entry\"", 3) && HasLine("\"entry\":\"re33_ecent_ac4e93_17700\""), "no ERROR for a missing entry");
  CHECK(!GateOpenForTest(), "the gate must stay 0 when an entry is missing");
  int free_slots = 0;
  while (ec::OnNetTick(&Dummy)) ++free_slots;
  CHECK(free_slots == 4, "no net tick slot may be taken by a failed attach: %d free", free_slots);
  ec::ResetForTest();
  // a failed attach is not "waiting": it is reported again on the next try (the lib is still loaded and still wrong)
  CHECK(Attach() == Status::kNoEntry, "still no entry");
}

static void SlotsFullTest(const char* path) {
  ec::ResetForTest();
  ecfx::ResetForTest();
  void* lib = dlopen(path, RTLD_NOW);
  if (lib == nullptr) return;
  for (int i = 0; i < 4; ++i) ec::OnNetTick(&Dummy);
  CHECK(Attach() == Status::kNoTickSlot, "no free net tick slot: must stay off");
  CHECK(HasLine("\"status\":\"no_tick_slot\"", 3) && !GateOpenForTest(), "no ERROR / the gate opened");
}

// combat.ecfx: on waits for the wall library, off is off, ec not installed refuses, the plugin's neutronfx.on is warned about and changes nothing.
static void SwitchTest() {
  char tmpl[] = "/tmp/nevr-ecfx-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) std::exit(2);
  ec::ResetForTest();
  ecfx::ResetForTest();
  ecfx::SetNoThreadForTest(true);
  ModuleResult r = Install(true);
  CHECK(!r.installed && std::string(r.status) == "needs_ec" && HasLine("\"result\":\"needs_ec\"", 3), "ec not installed: must refuse: %s", r.status);
  ecfx::ResetForTest();
  CHECK(std::string(Install(false).status) == "off", "combat.ecfx false: must be off");
  ecfx::ResetForTest();
  ec::SetInstalledForTest(true);
  r = Install(true);
  CHECK(r.installed && std::string(r.status) == "waiting", "ec installed, combat.ecfx true: must wait for the wall library: %s", r.status);
  CHECK(std::string(Install(true).status) == "already_ran", "a second step must refuse");
  ec::SetInstalledForTest(false);
  const std::string f = std::string(tmpl) + "/neutronfx.on";
  FILE* fp = std::fopen(f.c_str(), "w");
  if (fp != nullptr) std::fclose(fp);
  nevr_quest::combat::legacy::SetDirectoryForTest(tmpl);
  ecfx::ResetForTest();
  g_lines.clear();
  r = Install(false);
  CHECK(std::string(r.status) == "off" && HasLine("\"event\":\"combat_ecfx_legacy_switch\",\"file\":\"neutronfx.on\"", 2),
        "neutronfx.on: must be warned about and change nothing: %s", r.status);
  nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
  unlink(f.c_str());
  rmdir(tmpl);
}

int main(int argc, char** argv) {
  sentinel::SetLogSink(&Capture);
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

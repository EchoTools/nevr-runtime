// Host test of dronekill.cpp (rows R45 / R47 / R48 + the Actor slot): the real module code against a fake game library laid out as
// the module reads it (component systems with the real read-shape words, a fake auto-target system for chase), a fake slot page for the
// GOT swaps (real mprotect, real /proc/self/maps), real SafeRead. The calls into game code through vtable slots are replaced through
// the module's test seam (a slot's words are aarch64 data on the host); everything else is the module as it ships.
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dronekill.h"
#include "dronekill_core.h"
#include "nevr/plugin_interface.h"
#include "plugin_log.h"

using namespace q15t;

static int g_fail = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      ++g_fail;                                        \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

// ---- log capture ----
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
static int CountLine(const char* needle, int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++n;
  return n;
}
static bool HasLine(const char* needle, int level = -1) { return CountLine(needle, level) > 0; }
static void Dump(const char* title) {  // Q15T_TEST_VERBOSE=1: the module's own log records of that scenario
  if (std::getenv("Q15T_TEST_VERBOSE") == nullptr) return;
  std::printf("---- %s\n", title);
  for (const auto& l : g_lines) std::printf("[%u] %s\n", l.level, l.text.c_str());
}

// ================================================================ the core (ported from the source's host test)
static void TestCore() {
  using namespace q15t::dk;
  CHECK(ThunkOk(kThunk), "kThunk");
  uint32_t t[kThunkWords];
  memcpy(t, kThunk, sizeof t);
  t[2] ^= 0x400;
  CHECK(!ThunkOk(t), "another vtable slot accepted");

  const uint64_t plt = 0x100000 + kCmpPlt, body = 0x100000 + 0x221110c;
  uint32_t w[kBodyWords];
  memcpy(w, kBody, sizeof w);
  const int64_t d = (static_cast<int64_t>(plt) - static_cast<int64_t>(body + kBodyBl * 4)) / 4;
  w[kBodyBl] = 0x94000000u | (static_cast<uint32_t>(d) & kBlMask);
  CHECK(BodyOk(w, body, plt), "body");
  CHECK(!BodyOk(w, body + 4, plt), "bl 4 bytes off the stub accepted");
  CHECK(!BodyOk(w, body, plt + 16), "another PLT stub accepted");
  for (int k : kBodyLdr) {
    uint32_t v[kBodyWords];
    memcpy(v, w, sizeof v);
    v[k] = (v[k] & ~kLdrImmMask) | (0x123u << 10);
    CHECK(BodyOk(v, body, plt), "field offset may differ");
    v[k] ^= 0x1;
    CHECK(!BodyOk(v, body, plt), "register may not differ");
  }
  for (int i = 0; i < kBodyWords; ++i) {
    uint32_t v[kBodyWords];
    memcpy(v, w, sizeof v);
    v[i] = 0xf9000008;
    CHECK(!BodyOk(v, body, plt), "a store in place of word %d accepted", i);
  }
  uint32_t v[kBodyWords];
  memcpy(v, w, sizeof v);
  v[kBodyBl] = (v[kBodyBl] & kBlMask) | 0x14000000u;
  CHECK(!BodyOk(v, body, plt), "b instead of bl accepted");

  uint32_t dw[kDisWords];
  memcpy(dw, kDis, sizeof dw);
  CHECK(DisableOk(dw), "kDis");
  dw[kDisBl] = 0x94000001;
  dw[3] = (dw[3] & ~kLdrImmMask) | (0x1a << 10);
  CHECK(DisableOk(dw), "disable: other bl / field offset");
  dw[3] ^= 0x1f;
  CHECK(!DisableOk(dw), "disable: other register accepted");
  memcpy(dw, kDis, sizeof dw);
  dw[kDisWords - 1] = 0xd65f03c0;
  CHECK(!DisableOk(dw), "disable: ret for the tail call accepted");
  memcpy(dw, kDis, sizeof dw);
  dw[kDisBl] = 0x14000001;
  CHECK(!DisableOk(dw), "disable: b for bl accepted");
  uint32_t ew[kEnWords];
  memcpy(ew, kEn, sizeof ew);
  CHECK(EnableOk(ew), "kEn");
  ew[kEnWords - 1] = 0xd61f0040;
  CHECK(!EnableOk(ew), "enable: tail call for the ret accepted");
  CHECK(kVtDisable == 0x210 && kDisCategory == 1 && kVtEnable == 0x208 && kVtComponent == 0x1b8 && kVtComponentId == 0x1b0, "vtable constants");
  // every non-masked word of the Disable / Enable shapes is compared; the masked ones take any immediate, never another register
  for (int i = 0; i < kDisWords; ++i) {
    uint32_t v2[kDisWords];
    memcpy(v2, kDis, sizeof v2);
    bool imm = false;
    for (int k : kDisImm) imm |= i == k;
    if (imm) {
      v2[i] = (v2[i] & ~kLdrImmMask) | (0x2b3u << 10);
      CHECK(DisableOk(v2), "disable word %d: another immediate refused", i);
      v2[i] ^= 0x1;
      CHECK(!DisableOk(v2), "disable word %d: another register accepted", i);
    } else if (i == kDisBl) {
      v2[i] = 0x94000000u | 0x123;
      CHECK(DisableOk(v2), "disable bl: another target refused");
    } else {
      v2[i] ^= 0x1;
      CHECK(!DisableOk(v2), "disable word %d altered but accepted", i);
    }
  }
  for (int i = 0; i < kEnWords; ++i) {
    uint32_t v2[kEnWords];
    memcpy(v2, kEn, sizeof v2);
    bool imm = false;
    for (int k : kEnImm) imm |= i == k;
    if (imm) {
      v2[i] = (v2[i] & ~kLdrImmMask) | (0x2b3u << 10);
      CHECK(EnableOk(v2), "enable word %d: another immediate refused", i);
      v2[i] ^= 0x1;
      CHECK(!EnableOk(v2), "enable word %d: another register accepted", i);
    } else if (i == kEnBl) {
      v2[i] = 0x94000000u | 0x123;
      CHECK(EnableOk(v2), "enable bl: another target refused");
      v2[i] = 0x14000000u | 0x123;
      CHECK(!EnableOk(v2), "enable: b for bl accepted");
    } else {
      v2[i] ^= 0x1;
      CHECK(!EnableOk(v2), "enable word %d altered but accepted", i);
    }
  }
  {  // the mask offsets come from the system's own Enable body (imm12 * 8): CR15TouchInteractCS has 0xc8 / 0xe0
    uint32_t e2[kEnWords];
    memcpy(e2, kEn, sizeof e2);
    e2[3] = (e2[3] & ~kLdrImmMask) | (0x19u << 10);
    e2[8] = (e2[8] & ~kLdrImmMask) | (0x1cu << 10);
    const MaskAt m = MaskOffsets(e2);
    CHECK(m.idx == 0xc8 && m.maskArr == 0xe0, "mask offsets %x %x", m.idx, m.maskArr);
  }

  // groups
  Table tb;
  const uint64_t model = 0x1111, sound = 0x5b8cc538e22ad937ull;
  Joined j = tb.On(0xa0, model, 1, 3, 1000);
  CHECK(j.pending && j.group == 0, "pending");
  j = tb.On(0xa0, model, 2, 4, 1100);
  CHECK(j.pending && tb.pendingN == 2, "pending 2");
  j = tb.On(0xd0, kDamageable, 9, 0x00050011, 1200);
  CHECK(!j.pending && j.group == 1 && j.pendingJoined == 2 && tb.pendingN == 0, "release takes the pending");
  CHECK(tb.Newest()->n == 3 && (tb.Newest()->handle & 0xffff) == 0x11 && tb.Newest()->comps[0].sysSym == kDamageable, "group shape");
  j = tb.On(0xb0, sound, 5, 6, 1300);
  CHECK(!j.pending && j.group == 1 && tb.Newest()->n == 4, "join the newest group");
  j = tb.On(0xb0, sound, 5, 6, 1200 + kJoinMs);
  CHECK(j.pending && tb.pendingN == 1, "exactly 5 s: no longer joins");
  j = tb.On(0xd0, kDamageable, 9, 0x22, 1200 + kJoinMs + kPendingMs);
  CHECK(j.group == 2 && j.pendingJoined == 0 && tb.pendingN == 0, "pending of 3 s or more not taken");
  j = tb.On(0xd0, kDamageable, 9, 0x70011, 20000);
  CHECK(j.group == 3, "group 3");
  Group* g = tb.Killed(0xabc0011);
  CHECK(g && g->number == 3 && g->dead, "K: low 16 bits, newest");
  g = tb.Killed(0x11);
  CHECK(g && g->number == 1, "K: the older one next");
  CHECK(tb.Killed(0x11) == nullptr, "K: then none");
  uint32_t live[kGroups];
  CHECK(tb.Live(live, kGroups) == 1 && live[0] == 0x22, "live list");
  Table big;
  big.On(0xd0, kDamageable, 9, 1, 0);
  for (int i = 0; i < kGroupComps + 5; ++i) big.On(0xb0, sound, i, i, 10);
  CHECK(big.Newest()->n == kGroupComps && big.Newest()->overflow == 6, "overflow counted");
  Table pend;
  for (int i = 0; i < kPending + 3; ++i) pend.On(0xb0, sound, i, i, 10 + i);
  CHECK(pend.pendingN == kPending && pend.pendingDropped == 3 && pend.pending[0].compSym == 3, "oldest pending dropped");
  Table many;
  for (int i = 0; i < kGroups + 2; ++i) many.On(0xd0, kDamageable, 9, 100 + i, i * 10000);
  CHECK(many.made == kGroups + 2 && many.Newest()->number == kGroups + 2 && many.Killed(100) == nullptr && many.Killed(101) == nullptr, "group ring");
  Table la;
  la.On(0xd0, kDamageable, 9, 0x5a, 0, 0xac70);
  CHECK(!la.LiveComp(0xe0, 100) && la.LiveActor(0xac70) && !la.LiveActor(0xbeef) && !la.LiveActor(0), "live actor");
  CHECK(la.LiveComp(0xd0, 0x5a), "the damageable is a live component");
  la.Killed(0x5a);
  CHECK(!la.LiveActor(0xac70), "dead group's actor not live");
  CHECK(!la.LiveComp(0xd0, 0x5a), "a dead group's component is not live");
  Table zero;
  zero.On(0xd0, kDamageable, 9, 0x5b, 0, 0);
  CHECK(!zero.LiveActor(0), "actor 0 (unknown) is never live");
  Table sp;
  sp.On(0xa0, model, 1, 3, 1000, 0xa1, 0x5000, 0x6000);  // pending, space and owner recorded
  CHECK(sp.SpaceOf(0x6000) == 0x5000 && sp.SpaceOf(0x6001) == 0 && sp.SpaceOf(0) == 0, "SpaceOf over the pending list");
  Table sp0;
  sp0.On(0xd0, kDamageable, 9, 0x5c, 1000, 0xa1, 0x5000, 0);  // a component whose owner was unreadable
  CHECK(sp0.SpaceOf(0) == 0, "an unknown owner has no space");
  CHECK(sp0.SpaceOf(0x77) == 0, "another owner has no space");
  sp0.On(0xb0, sound, 5, 6, 1100, 0xa1, 0x5000, 0x6000);
  CHECK(sp0.SpaceOf(0x6000) == 0x5000, "SpaceOf over the groups");

  // stage 1
  const uint64_t A = 0xaaa1, B = 0xbbb2, S = 0x5000, O = 0x6000;
  Table s1;
  s1.On(0xa0, model, 1, 3, 1000, A, S, O);
  s1.On(0xa0, model, 2, 4, 1000, B, S, O);
  s1.On(0xa0, model, 3, 5, 1000, 0, S, O);
  j = s1.On(0xd0, kDamageable, 9, 0x11, 1100, A, S, O);
  CHECK(j.group == 1 && j.pendingJoined == 2 && s1.pendingN == 1 && s1.pending[0].actor == B, "C1: own actor's pending");
  j = s1.On(0xd0, kDamageable, 9, 0x12, 1200, B, S, O);
  CHECK(j.group == 2 && j.pendingJoined == 1 && s1.pendingN == 0, "C1 second");
  j = s1.On(0xb0, sound, 5, 6, 9000, A, S, O);
  CHECK(!j.pending && j.group == 1, "an actor's component joins its own group at any age");
  j = s1.On(0xb0, sound, 5, 7, 1300, 0, S, O);
  CHECK(j.group == 2, "unknown actor: newest live < 5 s");
  j = s1.On(0xb0, sound, 5, 8, 1300, 0xccc3, S, O);
  CHECK(j.pending, "third actor: pending");
  j = s1.On(0xb0, sound, 5, 9, 1300, A, S, O);
  CHECK(j.group == 1, "an actor's own group wins over a newer group inside the join window (got %d)", j.group);
  const int n1 = s1.ByNumber(1)->n;
  s1.On(0xb0, sound, 5, 6, 9100, A, S, O);
  CHECK(s1.ByNumber(1)->n == n1, "duplicates once");
  g = s1.Killed(0x11, 10000, true);
  CHECK(g && g->number == 1 && g->dead && g->due == (1u << g->n) - 1 && g->dueMs == 10000, "R2 due");
  Due dd[64];
  int n = s1.TakeDue(10500, dd, 64);
  CHECK(n == g->n && dd[0].c.sysSym == kDamageable && dd[0].c.owner == O && dd[0].c.space == S && dd[0].c.actor == A, "due components");
  for (int i = 0; i < n; ++i)
    if (i != 1) s1.Done(dd[i].number, dd[i].idx);
  CHECK(s1.AnyDue() && s1.TakeDue(10600, dd, 64) == 1 && dd[0].idx == 1, "one left");
  CHECK(s1.TakeDue(10000 + kDueMs, dd, 64) == 0 && !s1.AnyDue() && s1.expired == 1, "due bits expire");
  g = s1.Killed(0x12);
  CHECK(g && g->number == 2 && g->due == 0 && !s1.AnyDue(), "stage-0 K sets no bit");
  j = s1.On(0xa0, model, 1, 3, 1100 + kSwallowMs - 1, A, S, O);
  CHECK(j.again && j.group == 1 && !j.pending && s1.AnyDue() && s1.TakeDue(1100 + kSwallowMs - 1, dd, 64) == 1 && dd[0].c.handle == 3, "R3");
  s1.Done(dd[0].number, dd[0].idx);
  j = s1.On(0xa0, model, 1, 3, 1100 + kSwallowMs, A, S, O);
  CHECK(!j.again && !s1.AnyDue(), "R3 expires at 25 s");
  Table r;
  r.On(0xd0, kDamageable, 9, 0x11, 0, A, S, O);
  r.Killed(0x11, 5, true);
  j = r.On(0xd0, kDamageable, 9, 0x11, 40000, A, S, O);
  CHECK(j.group == 2 && r.ByNumber(1) == nullptr && !r.ByNumber(2)->dead && r.ByNumber(2)->due == 0 && !r.AnyDue(), "C2 slot reuse");
  j = r.On(0xb0, sound, 5, 6, 40100, A, 0x7000, O);
  CHECK(r.spaceClears == 1 && r.ByNumber(2) == nullptr && j.pending && r.pendingN == 1 && r.space == 0x7000, "another space clears the table");
  Table m;
  for (int i = 0; i < kGroups + 1; ++i) m.On(0xd0, kDamageable, 9, 100 + i, i * 10000);
  CHECK(m.ByNumber(1) == nullptr && m.ByNumber(2) && m.ByNumber(kGroups + 1), "oldest slot taken");

  // touch
  TouchTable tt;
  Comp a = {0x1000, kR14Touch, kR14Touch, 0x00010005, 0}, b = {0x1000, kR14Touch, kR14Touch, 0x00020006, 0};
  const int sa = tt.Deployed(a, 1000);
  CHECK(tt.t[sa].state == kTouchReOn && tt.AnyDue(), "touch RE-ON");
  int due[kTouches];
  CHECK(tt.TakeDue(1100, due, kTouches) == 1 && due[0] == sa, "touch due");
  CHECK(tt.LockedOn(77, 1100) == -1, "a shot before the re-enable binds nothing");
  tt.ReOnDone(sa, true);
  CHECK(tt.t[sa].state == kTouchHeld && !tt.AnyDue(), "HELD");
  const int sb = tt.Deployed(b, 5000);
  tt.ReOnDone(sb, true);
  CHECK(tt.LockedOn(77, 6000) == sb && tt.t[sb].atHandle == 77 && tt.t[sb].state == kTouchOffDue, "locked on: the HELD one deployed last");
  CHECK(tt.LockedOn(77, 6010) == -1 && tt.LockedOn(88, 6020) == sa, "same drone again: none; other drone: the other record");
  tt.OffDone(sb);
  CHECK(tt.t[sb].state == kTouchDone, "off done");
  CHECK(tt.TakeDue(6020 + kDueMs, due, kTouches) == 0 && tt.expired == 1 && tt.t[sa].state == kTouchDone, "touch expiry");
  TouchTable u;
  const int su = u.Deployed(a, 0);
  u.ReOnDone(su, true);
  CHECK(u.LockedOn(5, kLockMs) == -1, "deployed kLockMs ago: not picked");
  {  // a new life of the same (system, handle) takes its slot back; 16 records fill 16 slots; the 17th takes the oldest
    TouchTable t2;
    const int first = t2.Deployed(a, 100);
    CHECK(t2.Deployed(a, 200) == first, "a new life took another slot");
    int used = 0;
    for (const auto& r : t2.t) used += r.state != kTouchFree;
    CHECK(used == 1, "one life, %d slots", used);
    TouchTable t6;  // records deployed at time 0 still take free slots
    const int s6a = t6.Deployed(a, 0);
    const int s6b = t6.Deployed(b, 0);
    CHECK(s6a != s6b, "two records deployed at time 0 share a slot");
    TouchTable t3;
    int slots[kTouches];
    for (int i = 0; i < kTouches; ++i) {
      Comp c = {0x1000, kR14Touch, kR14Touch, static_cast<uint32_t>(0x100 + i), 0};
      slots[i] = t3.Deployed(c, 1000 + 10 * (kTouches - i));  // the first is the youngest, the last the oldest
    }
    bool distinct = true;
    for (int i = 0; i < kTouches; ++i)
      for (int k = 0; k < i; ++k) distinct &= slots[i] != slots[k];
    CHECK(distinct, "16 records did not take 16 slots");
    Comp extra = {0x1000, kR14Touch, kR14Touch, 0x999, 0};
    CHECK(t3.Deployed(extra, 5000) == slots[kTouches - 1], "the 17th did not take the oldest slot");
    // OFF-DUE keeps its own clock: bound at 6000, still due 1999 ms later
    TouchTable t4;
    const int s4 = t4.Deployed(a, 1000);
    t4.ReOnDone(s4, true);
    CHECK(t4.LockedOn(5, 6000) == s4, "lock");
    CHECK(t4.AnyDue(), "an OFF-DUE record is due");
    CHECK(t4.TakeDue(6000 + kDueMs - 1, due, kTouches) == 1, "OFF-DUE expired early");
    t4.ReOnDone(s4, true);
    CHECK(t4.t[s4].state == kTouchOffDue, "ReOnDone changed an OFF-DUE record");
    TouchTable t5;
    const int s5 = t5.Deployed(a, 1000);
    t5.ReOnDone(s5, true);
    t5.OffDone(s5);
    CHECK(t5.t[s5].state == kTouchHeld, "OffDone changed a HELD record");
  }
  GunTable gt;
  uint32_t sh = 9;
  CHECK(!gt.Shown(0xa, &sh), "gun unknown");
  gt.Set(0xa, 1);
  gt.Set(0, 1);
  CHECK(gt.Shown(0xa, &sh) && sh == 1 && !gt.Shown(0, &sh), "gun state");
  for (uint64_t i = 0; i < kGuns; ++i) gt.Set(0x100 + i, 1);
  CHECK(!gt.Shown(0xa, &sh) && gt.Shown(0x100 + kGuns - 1, &sh), "gun table full: oldest dropped");
  gt.Set(0x200, 1);
  gt.Set(0x201, 1);
  CHECK(!gt.Shown(0x100, &sh) && !gt.Shown(0x101, &sh) && gt.Shown(0x102, &sh) && gt.Shown(0x200, &sh) && gt.Shown(0x201, &sh), "gun table: the two oldest dropped in order");

  // chase
  const float p[3] = {0, 0, 0};
  float vel[3], dist = 0;
  const float far3[3] = {0, -kChaseAbove, 20};
  CHECK(ChaseVelocity(p, far3, vel, &dist) && dist > 19.99f && dist < 20.01f && vel[2] > kChaseSpeed - 1e-3f && vel[2] < kChaseSpeed + 1e-3f, "capped at 7 m/s");
  const float hold[3] = {0, -kChaseAbove, kChaseDistance + 1};
  CHECK(ChaseVelocity(p, hold, vel) && vel[2] > kChaseGain - 1e-3f && vel[2] < kChaseGain + 1e-3f, "1 m out: 2 m/s");
  const float near3[3] = {0, -kChaseAbove, 3};
  CHECK(ChaseVelocity(p, near3, vel) && vel[2] > -kChaseSpeed - 1e-3f && vel[2] < -kChaseSpeed + 1e-3f, "too close: backs off, capped at 7 m/s (%f)", vel[2]);
  const float above[3] = {0, 0, 20};
  CHECK(ChaseVelocity(p, above, vel) && vel[1] > 0.17f && vel[1] < 0.18f, "the aim point is 0.5 m above the target (%f)", vel[1]);
  const float same[3] = {0, -kChaseAbove, 0};
  CHECK(!ChaseVelocity(p, same, vel), "on the target: no velocity");
  ChaseTable ct;
  Chase* ca = ct.Take(1, 2);
  CHECK(ca && ct.Find(1, 2) == ca && ct.Take(1, 2) == ca, "chase take");
  for (int i = 0; i < kChases; ++i)
    if (Chase* x = ct.Take(10, i)) x->moving = true;
  CHECK(ct.Take(99, 99) == nullptr, "all moving: no slot");
  ca = ct.Find(10, 3);
  ca->moving = false;
  CHECK(ct.Take(99, 99) == ca, "a resting slot is reused");
  ChaseTable ct2;
  Chase* mine = ct2.Take(1, 2);
  mine->moving = true;
  CHECK(ct2.Take(1, 2) == mine && ct2.Take(3, 4) != mine, "a moving record is found again, not duplicated");
  CHECK(!ct2.AnyDue(), "nothing due");
  mine->due = true;
  CHECK(ct2.AnyDue(), "due not seen");
}

// FNV-1a over a table's little-endian bytes
static uint64_t Fnv(const uint32_t* w, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i)
    for (int b = 0; b < 4; ++b) h = (h ^ ((w[i] >> (8 * b)) & 0xff)) * 0x100000001b3ull;
  return h;
}

// The facts the module's behaviour hangs on, as literals: the other tests refer to these constants by name, so a changed value
// would carry the tests with it. The tables and offsets are the source build's (the source build's dronekill_core); the shape tables were
// compared with the disassembly (docs/quest/q15t/modules/dronekill.md); the hashes are of the tables as read there.
static void TestPinnedConstants() {
  using namespace q15t::dk;
  CHECK(Fnv(kThunk, kThunkWords) == 0xa11a9be513e9cbecull, "kThunk changed");
  CHECK(Fnv(kBody, kBodyWords) == 0xa5c7bf27867c1289ull, "kBody changed");
  CHECK(Fnv(kDis, kDisWords) == 0xa47320eb0b3d04beull, "kDis changed");
  CHECK(Fnv(kEn, kEnWords) == 0x73426ca53412ef8cull, "kEn changed");
  CHECK(kThunkWords == 7 && kBodyWords == 54 && kBodyBl == 27 && kDisWords == 48 && kDisBl == 23 && kEnWords == 47 && kEnBl == 23, "shape sizes");
  CHECK(kBodyLdr[0] == 8 && kBodyLdr[1] == 13 && kBodyLdr[2] == 21 && kBodyLdr[3] == 42 && kBodyLdr[4] == 44 && kBodyLdr[5] == 48, "body ldr words");
  CHECK(kDisImm[0] == 3 && kDisImm[1] == 6 && kDisImm[2] == 8 && kDisImm[3] == 17 && kDisImm[4] == 20 && kDisImm[5] == 24 && kDisImm[6] == 26 &&
            kDisImm[7] == 31 && kDisImm[8] == 32,
        "disable imm words");
  CHECK(kEnImm[0] == 3 && kEnImm[1] == 8 && kEnImm[2] == 9 && kEnImm[3] == 19 && kEnImm[4] == 22 && kEnImm[5] == 24 && kEnImm[6] == 26 && kEnImm[7] == 30 &&
            kEnImm[8] == 31,
        "enable imm words");
  CHECK(kLdrImmMask == 0x003ffc00 && kBlMask == 0x03ffffff, "masks");
  CHECK(kCmpPlt == 0xf43610 && kCmpSlot == 0x36f91f0 && kCmpFn == 0x1524cb4, "comparator constants");
  CHECK(kSysOwner == 0x80 && kOwnerFlag == 0x368, "owner / invoke-immediate offsets");
  CHECK(kDroneActor == 0x10e40287a9bf5a13ull && kDamageable == 0xc9dc08cfc2f5920full && kR14Touch == 0x2E40131422F7B1FCull, "symbols");
  CHECK(kNcaModel == 0x38EE951A26FB816Aull && kGun == 0x72F91FFFA17C3C8Cull && kSatellite == 0x106C63A2927B7A4Dull && kPort == 0x45B09F0FA9F7FA33ull,
        "gun symbols");
  CHECK(kNetPhysics == 0x8FEE6557F995E4A3ull && kNetPhysicsPhys == 0x170, "net physics");
  CHECK(kJoinMs == 5000 && kPendingMs == 3000 && kSwallowMs == 25000 && kDueMs == 2000 && kLockMs == 120000 && kChaseMs == 100, "times");
  CHECK(kGroupComps == 20 && kGroups == 8 && kPending == 20 && kTouches == 16 && kGuns == 32 && kChases == 16, "table sizes");
  CHECK(kChaseSpeed == 7.0f && kChaseDistance == 7.0f && kChaseGain == 2.0f && kChaseAbove == 0.5f, "chase constants");
  CHECK(kAtIdx == 0xc8 && kAtRecs == 0xf8 && kAtStride == 0x90 && kAtTransforms == 0x130 && kAtPhysics == 0x138, "auto-target offsets");
  CHECK(kAtHasTarget == 0x63 && kAtTransform == 0x7c && kAtTargetIdx == 0x80 && kAtPhysIdx == 0x84 && kAtTargetSys == 0x88, "auto-target record offsets");
  CHECK(kDmIdx == 0xd0 && kDmRecs == 0x100 && kDmStride == 0xb8 && kDmPhysH == 0xc && kDmXfH == 0x8 && kDmPhys == 0x140 && kDmXf == 0x138 && kBodyPos == 0x8f8,
        "damageable offsets");
  CHECK(kPhIdx == 0xc8 && kPhOwners == 0x100, "physics offsets");
}

// ================================================================ the fake game
struct ND {
  uint64_t actor, space, system, component;
  uint32_t enable;
};
using ActorFn = uint32_t (*)(const void*, const void*, uint64_t, void**, void*, unsigned*);
using EnterFn = void (*)(void*, const ND*);
using FireFn = void (*)(void*, uint32_t, DkC3, uint32_t);
using AttackFn = void (*)(void*, uint32_t);

static const uint64_t kActorId = 0xfeedf00dcafe0001ull;
static uint64_t g_ms = 1000;
static uint64_t Clk() { return g_ms; }

static uintptr_t g_base = 0;
static uintptr_t* g_w = nullptr;
static uintptr_t g_fire_slot = 0x30;
static std::vector<std::string> g_ev;

alignas(16) static uint32_t g_thunk_words[dk::kThunkWords], g_body_words[dk::kBodyWords], g_dis_words[dk::kDisWords], g_en_words[dk::kEnWords];
alignas(16) static uintptr_t g_vtable[0x50], g_vtable2[0x50];
alignas(16) static uint32_t g_thunk_bad[dk::kThunkWords];
alignas(16) static uint8_t g_plt_marker[16];
alignas(16) static uint8_t g_gs[0x400];      // a gamespace: the invoke-immediate word at +0x368
alignas(16) static uint8_t g_gs2[0x400];     // the physics system's gamespace
alignas(16) static uint8_t g_sysmem[8 * 0x200];
alignas(16) static uint16_t g_idxtab[0x80];  // (slot, generation) pairs per handle
alignas(16) static uint16_t g_masks[32];
alignas(16) static uint8_t g_spacemem[16];
alignas(16) static uint8_t g_spacemem2[16];
static void* const g_space = g_spacemem;
static void* g_space_now = g_spacemem;  // the game space the fake Actor reports

struct SysEnt {
  uint64_t sym;
  uint8_t* sys;
};
struct CompEnt {
  uint8_t* sys;
  uint64_t sym;
  uint32_t handle;
};
static SysEnt g_syss[8];
static int g_nsys = 0;
static CompEnt g_comps[16];
static int g_ncomps = 0;
static uint32_t g_actor_ret = 1;
static uint64_t g_actor_val = 0;       // the SActor the fake Actor reports (0 = kActorId)
static bool g_actor_bad_out = false;   // Enter hands Actor unreadable out-pointers; the fake writes nothing
static bool g_actor_twice = false;     // Enter calls Actor for the drone twice; the second call reports another space and actor
static int g_drone_actor_calls = 0;
static int g_actor_calls = 0, g_find_calls = 0, g_comp_calls = 0, g_bad_actor = 0;

struct EnterCall {
  void* node;
  const ND* data;
  ND copy;
};
static std::vector<EnterCall> g_enters;
static bool g_enter_calls_actor = true;
static uint64_t g_other_actor_first = 0;  // call Actor for this other actor before Enter's own

struct DisCall {
  void* sys;
  uint64_t handle;
  uint32_t cat;
  bool enable;
};
static std::vector<DisCall> g_dis;
static void (*g_dis_hook)() = nullptr;  // runs inside the engine's DisableComponent (a tick that re-enters)

static bool SameNd(const ND& a, const ND& b) {
  return a.actor == b.actor && a.space == b.space && a.system == b.system && a.component == b.component && a.enable == b.enable;
}
static void W64(uint8_t* p, size_t off, uint64_t v) { memcpy(p + off, &v, 8); }

static uintptr_t ActorWrapPtr() { return g_w[0x20 / 8]; }
static uint32_t FakeActor(const void*, const void*, uint64_t sym, void** sp, void* actor_out, unsigned*) {
  ++g_actor_calls;
  g_ev.push_back("actor");
  if (g_actor_bad_out) return 1;
  if (sym == dk::kDroneActor && ++g_drone_actor_calls == 2 && g_actor_twice) {
    const uint64_t other = 0xdead;
    *sp = g_spacemem2;
    memcpy(actor_out, &other, 8);
    return 1;
  }
  if (sym != dk::kDroneActor) {  // another actor's call: other out-values, which a capture must not take
    const uint64_t other = 0xbad;
    *sp = reinterpret_cast<void*>(0x7777);
    memcpy(actor_out, &other, 8);
    return 1;
  }
  *sp = g_space_now;
  const uint64_t val = g_actor_val != 0 ? g_actor_val : kActorId;
  memcpy(actor_out, &val, 8);
  return g_actor_ret;
}
static bool g_enter_deref = true;
static void FakeEnter(void* node, const ND* d) {
  if (!g_enter_deref) {  // the test passed an unreadable data pointer: the real function is reached with it, untouched
    g_ev.push_back("enter-bad-pointer");
    return;
  }
  g_ev.push_back("enter");
  g_enters.push_back({node, d, *d});
  if (!g_enter_calls_actor) return;
  void* sp = nullptr;
  uint64_t actor = 0;
  unsigned fl = 0;
  auto aw = reinterpret_cast<ActorFn>(ActorWrapPtr());
  if (g_other_actor_first != 0) aw(nullptr, nullptr, g_other_actor_first, &sp, &actor, &fl);
  if (g_actor_bad_out) {
    aw(nullptr, nullptr, d->actor, reinterpret_cast<void**>(0x10), reinterpret_cast<void*>(0x10), &fl);
    return;
  }
  aw(nullptr, nullptr, d->actor, &sp, &actor, &fl);
  if (g_actor_twice) aw(nullptr, nullptr, d->actor, &sp, &actor, &fl);
}

static void* FakeFind(const void* space, uint64_t sym) {
  ++g_find_calls;
  g_ev.push_back("find");
  if (space != g_spacemem && space != g_spacemem2) return nullptr;
  for (int i = 0; i < g_nsys; ++i)
    if (g_syss[i].sym == sym) return g_syss[i].sys;
  return nullptr;
}
static uint64_t FakeComponent(uintptr_t, const void* sys, uint64_t actor, uint64_t sym) {
  ++g_comp_calls;
  g_ev.push_back("component");
  if (actor != kActorId) ++g_bad_actor;
  for (int i = 0; i < g_ncomps; ++i)
    if (g_comps[i].sys == sys && g_comps[i].sym == sym) return g_comps[i].handle;
  return 0xffff;
}
static void FakeEnable(uintptr_t, void* sys, uint64_t handle, uint32_t cat) {
  g_dis.push_back({sys, handle, cat, true});
  g_masks[g_idxtab[(handle & 0xffff) * 2]] &= static_cast<uint16_t>(~cat);
}
static void FakeDisable(uintptr_t, void* sys, uint64_t handle, uint32_t cat) {
  g_dis.push_back({sys, handle, cat, false});
  g_masks[g_idxtab[(handle & 0xffff) * 2]] |= static_cast<uint16_t>(cat);
  if (g_dis_hook != nullptr) {
    void (*h)() = g_dis_hook;
    g_dis_hook = nullptr;
    h();
  }
}

alignas(16) static uint8_t g_recsA[0x90 * 4];

// fire / attack
struct FireCall {
  void* cs;
  uint32_t handle;
  DkC3 v;
  uint32_t remote;
  bool acc_non_null;
  float acc[3];
  int acc_remote;
  float after[3];  // the accessor read again after a nested shot returned (the outer shot's own values again)
  int after_remote;
};
static std::vector<FireCall> g_fires;
static void FakeFire(void* cs, uint32_t h, DkC3 v, uint32_t remote) {
  g_ev.push_back("fire");
  FireCall c{cs, h, v, remote, false, {0, 0, 0}, -1, {0, 0, 0}, -1};
  int rem = -1;
  const float* a = DroneKillShotVelocity(&rem);
  if (a != nullptr) {
    c.acc_non_null = true;
    memcpy(c.acc, a, sizeof c.acc);
    c.acc_remote = rem;
  }
  const size_t mine = g_fires.size();
  g_fires.push_back(c);
  if (cs == reinterpret_cast<void*>(0x7001)) {  // a shot inside a shot (nested test)
    FireFn inner = reinterpret_cast<FireFn>(g_w[g_fire_slot / 8]);
    inner(reinterpret_cast<void*>(0x7002), 9, DkC3{1, 2, 3}, 1);
    int rem2 = -1;
    const float* a2 = DroneKillShotVelocity(&rem2);
    if (a2 != nullptr) memcpy(g_fires[mine].after, a2, sizeof g_fires[mine].after);
    g_fires[mine].after_remote = rem2;
  }
}
struct AttackCall {
  void* cs;
  uint32_t handle;
};
static std::vector<AttackCall> g_attacks;
static bool g_attack_sets_target = false;  // the real AttackTarget acquires the target: chase must read the record after it ran
static void FakeAttack(void* cs, uint32_t h) {
  g_ev.push_back("attack");
  g_attacks.push_back({cs, h});
  if (g_attack_sets_target) static_cast<uint8_t*>(static_cast<void*>(g_recsA))[2 * dk::kAtStride + dk::kAtHasTarget] = 1;
}

// chase fakes
struct SetVel {
  void* physics;
  uint32_t handle;
  DkC3 v;
};
static std::vector<SetVel> g_setvels;
static float g_me_pos[3] = {0, 0, 0}, g_target_pos[3] = {10, 0, 0};
static bool g_have_body = true;
static uint32_t g_local_ret = 1;
alignas(16) static uint8_t g_body[0x1000];
alignas(16) static uint8_t g_xf_me[16], g_xf_target[16];
struct L2wCall {
  const void* transforms;
  uint32_t handle;
};
static std::vector<L2wCall> g_l2w;
static std::vector<uint32_t> g_primary_bodies;
static const void* g_local_sys = nullptr;
static uint32_t g_local_handle = 0;
static DkTransfQ FakeL2w(const void* transforms, uint32_t handle) {
  g_l2w.push_back({transforms, handle});
  DkTransfQ q{};
  const float* p = transforms == static_cast<const void*>(g_xf_me) ? g_me_pos : g_target_pos;
  memcpy(q.p, p, sizeof q.p);
  return q;
}
static DkPhTarget FakePrimary(const void*, uint32_t body) {
  g_primary_bodies.push_back(body);
  DkPhTarget t{};
  t.tag = 1;
  t.body = body;
  return t;
}
static void* FakeResolve(const DkPhTarget*) { return g_have_body ? static_cast<void*>(g_body) : nullptr; }
static void FakeSetVel(void* physics, uint32_t handle, DkC3 v) { g_setvels.push_back({physics, handle, v}); }
static uint32_t FakeLocal(const void* sys, uint32_t handle) {
  g_local_sys = sys;
  g_local_handle = handle;
  return g_local_ret;
}

static DroneKillFns Fns() {
  DroneKillFns f;
  f.find = &FakeFind;
  f.l2w = &FakeL2w;
  f.primary = &FakePrimary;
  f.resolve = &FakeResolve;
  f.set_vel = &FakeSetVel;
  f.local = &FakeLocal;
  return f;
}
static const DroneKillCalls g_calls = {&FakeComponent, &FakeEnable, &FakeDisable};

static uintptr_t V(const void* fn) { return reinterpret_cast<uintptr_t>(fn) - g_base; }
static DroneKillSlots Slots() {
  return {0x20, V(reinterpret_cast<void*>(&FakeActor)), 0x28, V(reinterpret_cast<void*>(&FakeEnter)), g_fire_slot,
          V(reinterpret_cast<void*>(&FakeFire)),        0x38, V(reinterpret_cast<void*>(&FakeAttack)), 0x40, 0x1234,
          reinterpret_cast<uintptr_t>(g_plt_marker) - g_base};
}

// a fresh slot page (page 0: the slots; page 1: a second page for a slot that must live elsewhere)
static void NewPage(bool fire_in_exec_page) {
  void* p = mmap(nullptr, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  g_w = static_cast<uintptr_t*>(p);
  g_base = reinterpret_cast<uintptr_t>(p);
  g_fire_slot = fire_in_exec_page ? 0x1030 : 0x30;
  g_w[0x20 / 8] = reinterpret_cast<uintptr_t>(&FakeActor);
  g_w[0x28 / 8] = reinterpret_cast<uintptr_t>(&FakeEnter);
  g_w[g_fire_slot / 8] = reinterpret_cast<uintptr_t>(&FakeFire);
  g_w[0x38 / 8] = reinterpret_cast<uintptr_t>(&FakeAttack);
  g_w[0x40 / 8] = g_base + 0x1234;
  mprotect(p, 4096, PROT_READ);
  mprotect(static_cast<char*>(p) + 4096, 4096, fire_in_exec_page ? (PROT_READ | PROT_EXEC) : PROT_READ);
}

static uint8_t* MakeSys(uint64_t sym) {
  uint8_t* s = g_sysmem + 0x200 * g_nsys;
  memset(s, 0, 0x200);
  W64(s, 0, reinterpret_cast<uintptr_t>(g_vtable));
  W64(s, dk::kSysOwner, reinterpret_cast<uintptr_t>(g_gs));
  const dk::MaskAt at = dk::MaskOffsets(dk::kEn);
  W64(s, at.idx, reinterpret_cast<uintptr_t>(g_idxtab));
  W64(s, at.maskArr, reinterpret_cast<uintptr_t>(g_masks));
  g_syss[g_nsys++] = {sym, s};
  return s;
}

// ================================================================ world and helpers
static const uint64_t kSound = 0x5b8cc538e22ad937ull;
static uint8_t *g_dmg, *g_snd, *g_touch, *g_model, *g_np;
static uint8_t g_node[16];

// chase world
alignas(16) static uint8_t g_cs[0x200], g_ts[0x200], g_phys[0x300], g_recsT[0xb8 * 4], g_owners[16 * 4];
alignas(16) static uint16_t g_idxA[0x40], g_idxT[0x40], g_idxP[0x40];
static const uint32_t kAtHandle = 0x00030005, kPhysH = 0x00010004;  // generations 3 and 1

static void BuildChase() {
  memset(g_cs, 0, sizeof g_cs);
  memset(g_ts, 0, sizeof g_ts);
  memset(g_phys, 0, sizeof g_phys);
  memset(g_recsA, 0, sizeof g_recsA);
  memset(g_recsT, 0, sizeof g_recsT);
  memset(g_owners, 0, sizeof g_owners);
  memset(g_idxA, 0, sizeof g_idxA);
  memset(g_idxT, 0, sizeof g_idxT);
  memset(g_idxP, 0, sizeof g_idxP);
  memset(g_body, 0, sizeof g_body);
  auto P = [](const void* p) { return reinterpret_cast<uintptr_t>(p); };
  W64(g_cs, dk::kSysOwner, P(g_gs2));
  W64(g_cs, dk::kAtIdx, P(g_idxA));
  W64(g_cs, dk::kAtRecs, P(g_recsA));
  W64(g_cs, dk::kAtTransforms, P(g_xf_me));
  W64(g_cs, dk::kAtPhysics, P(g_phys));
  g_idxA[(kAtHandle & 0xffff) * 2] = 2;                                  // slot of the auto-target handle
  g_idxA[(kAtHandle & 0xffff) * 2 + 1] = static_cast<uint16_t>(kAtHandle >> 16);  // generation = handle >> 16
  uint8_t* rec = g_recsA + 2 * dk::kAtStride;
  rec[dk::kAtHasTarget] = 1;
  const uint32_t xf = 0x77;
  memcpy(rec + dk::kAtTransform, &xf, 4);
  const uint16_t th = 3;
  memcpy(rec + dk::kAtTargetIdx, &th, 2);
  memcpy(rec + dk::kAtPhysIdx, &kPhysH, 4);
  W64(rec, dk::kAtTargetSys, P(g_ts));
  W64(g_ts, dk::kDmIdx, P(g_idxT));
  g_idxT[3 * 2] = 1;  // the target's damageable slot
  W64(g_ts, dk::kDmRecs, P(g_recsT));
  uint8_t* drec = g_recsT + 1 * dk::kDmStride;
  const uint32_t body_h = 9, xf_h = 0x55;
  memcpy(drec + dk::kDmPhysH, &body_h, 4);
  memcpy(drec + dk::kDmXfH, &xf_h, 4);
  W64(g_ts, dk::kDmPhys, P(g_body));  // any non-null pointer
  W64(g_ts, dk::kDmXf, P(g_xf_target));
  W64(g_phys, dk::kSysOwner, P(g_gs));
  W64(g_phys, dk::kPhIdx, P(g_idxP));
  g_idxP[(kPhysH & 0xffff) * 2] = 2;  // the physics record's slot (its owner entry is slot * 16)
  g_idxP[(kPhysH & 0xffff) * 2 + 1] = static_cast<uint16_t>(kPhysH >> 16);
  W64(g_phys, dk::kPhOwners, P(g_owners));
  W64(g_owners, 2 * 16, kActorId);
  W64(g_np, dk::kNetPhysicsPhys, P(g_phys));
  memcpy(g_body + dk::kBodyPos, g_target_pos, sizeof g_target_pos);
  g_me_pos[0] = g_me_pos[1] = g_me_pos[2] = 0;
  g_target_pos[0] = 10;
  g_target_pos[1] = 0;
  g_target_pos[2] = 0;
  memcpy(g_body + dk::kBodyPos, g_target_pos, sizeof g_target_pos);
  g_have_body = true;
  g_local_ret = 1;
}

static void SetFlag(uint32_t v) { memcpy(g_gs + dk::kOwnerFlag, &v, 4); }

static DroneKillInstall Reset(DroneKillWords words, bool install = true) {
  ResetDroneKillForTest();
  g_lines.clear();
  g_ev.clear();
  g_enters.clear();
  g_dis.clear();
  g_fires.clear();
  g_attacks.clear();
  g_setvels.clear();
  g_nsys = g_ncomps = 0;
  g_actor_calls = g_find_calls = g_comp_calls = g_bad_actor = 0;
  g_actor_ret = 1;
  g_actor_val = 0;
  g_actor_bad_out = g_actor_twice = false;
  g_drone_actor_calls = 0;
  g_dis_hook = nullptr;
  g_attack_sets_target = false;
  g_l2w.clear();
  g_primary_bodies.clear();
  g_local_sys = nullptr;
  g_local_handle = 0;
  g_enter_calls_actor = true;
  g_other_actor_first = 0;
  g_enter_deref = true;
  g_space_now = g_spacemem;
  g_ms = 1000;
  memset(g_gs, 0, sizeof g_gs);
  memset(g_gs2, 0, sizeof g_gs2);
  memset(g_idxtab, 0, sizeof g_idxtab);
  memset(g_masks, 0, sizeof g_masks);
  SetFlag(1);  // inside the invoke-immediate window
  NewPage(false);
  memcpy(g_thunk_words, dk::kThunk, sizeof g_thunk_words);
  memcpy(g_body_words, dk::kBody, sizeof g_body_words);
  const int64_t d = (reinterpret_cast<intptr_t>(g_plt_marker) - reinterpret_cast<intptr_t>(g_body_words + dk::kBodyBl)) / 4;
  g_body_words[dk::kBodyBl] = 0x94000000u | (static_cast<uint32_t>(d) & dk::kBlMask);
  memcpy(g_dis_words, dk::kDis, sizeof g_dis_words);
  memcpy(g_en_words, dk::kEn, sizeof g_en_words);
  memset(g_vtable, 0, sizeof g_vtable);
  g_vtable[dk::kVtComponentId / 8] = reinterpret_cast<uintptr_t>(g_body_words);
  g_vtable[dk::kVtComponent / 8] = reinterpret_cast<uintptr_t>(g_thunk_words);
  g_vtable[dk::kVtEnable / 8] = reinterpret_cast<uintptr_t>(g_en_words);
  g_vtable[dk::kVtDisable / 8] = reinterpret_cast<uintptr_t>(g_dis_words);
  g_dmg = MakeSys(dk::kDamageable);
  g_snd = MakeSys(kSound);
  g_touch = MakeSys(dk::kR14Touch);
  g_model = MakeSys(dk::kNcaModel);
  g_np = MakeSys(dk::kNetPhysics);
  auto comp = [](uint8_t* sys, uint64_t sym, uint32_t handle) {
    g_comps[g_ncomps++] = {sys, sym, handle};
    g_idxtab[(handle & 0xffff) * 2] = static_cast<uint16_t>(handle & 0xffff);  // slot = index
    g_masks[handle & 0xffff] = 0;
  };
  comp(g_dmg, dk::kDamageable, 0x00050011);
  comp(g_snd, kSound, 0x00060006);
  comp(g_touch, dk::kR14Touch, 0x00010005);
  comp(g_model, dk::kGun, 0x00070007);
  comp(g_model, dk::kSatellite, 0x00070008);
  comp(g_model, dk::kPort, 0x00070009);
  comp(g_np, dk::kNetPhysics, 0x0001000a);
  BuildChase();
  SetDroneKillCallsForTest(&g_calls);
  SetDroneKillClockForTest(&Clk);
  if (!install) return DroneKillInstall{};
  return InstallDroneKill(g_base, words, Fns(), Slots());
}

static void CallEnter(const ND* d) { reinterpret_cast<EnterFn>(g_w[0x28 / 8])(g_node, d); }
static void CallFire(void* cs, uint32_t h, DkC3 v, uint32_t remote) { reinterpret_cast<FireFn>(g_w[g_fire_slot / 8])(cs, h, v, remote); }
static void CallAttack(void* cs, uint32_t h) { reinterpret_cast<AttackFn>(g_w[0x38 / 8])(cs, h); }
static ND Drone(uint64_t sys, uint64_t comp, uint32_t en) { return ND{dk::kDroneActor, 0, sys, comp, en}; }
static const uint32_t kDmgHandle = 0x00050011;  // index 0x11

static DroneKillWords Only(bool kill, bool lockon, bool gun, bool chase, bool shot) {
  DroneKillWords w;
  w.kill = kill;
  w.lockon = lockon;
  w.gun = gun;
  w.chase = chase;
  w.shot = shot;
  return w;
}

// ================================================================ tests
static void TestInstallPinned() {
  ResetDroneKillForTest();
  g_lines.clear();
  const size_t sz = 0x3800000;  // the pinned vaddrs reach 0x36f91f8
  void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  CHECK(p != MAP_FAILED, "mmap");
  const uintptr_t base = reinterpret_cast<uintptr_t>(p);
  auto put = [&](uintptr_t slot, uintptr_t fn) {
    const uintptr_t v = base + fn;
    memcpy(static_cast<char*>(p) + slot, &v, 8);
  };
  put(0x36e2080, 0x2059714);  // Actor
  put(0x36cacf8, 0x263b700);  // R45 Enter
  put(0x36c37a8, 0x210bff8);  // R48 FireBulletLocal
  put(0x36e6610, 0x210b8c4);  // R47 AttackTarget
  put(0x36f91f0, 0x1524cb4);  // comparator
  mprotect(p, sz, PROT_READ);
  const DroneKillSlots& k = kDroneKillPinned;
  CHECK(k.actor_slot == 0x36e2080 && k.actor_fn == 0x2059714, "actor constants");
  CHECK(k.enter_slot == 0x36cacf8 && k.enter_fn == 0x263b700, "enter constants");
  CHECK(k.fire_slot == 0x36c37a8 && k.fire_fn == 0x210bff8, "fire constants");
  CHECK(k.attack_slot == 0x36e6610 && k.attack_fn == 0x210b8c4, "attack constants");
  CHECK(k.cmp_slot == 0x36f91f0 && k.cmp_fn == 0x1524cb4 && k.cmp_plt == 0xf43610, "comparator constants");
  const DroneKillInstall r = InstallDroneKill(base, DroneKillWords{}, Fns());
  CHECK(r.ok && r.actor.status == SwapStatus::kOk && r.enter.status == SwapStatus::kOk && r.fire.status == SwapStatus::kOk &&
            r.attack.status == SwapStatus::kOk,
        "pinned install");
  CHECK(r.comparator_ok && r.needs_tick, "comparator / needs_tick");
  auto word = [&](uintptr_t slot) {
    uintptr_t v;
    memcpy(&v, static_cast<char*>(p) + slot, 8);
    return v;
  };
  CHECK(word(0x36e2080) != base + 0x2059714 && word(0x36cacf8) != base + 0x263b700 && word(0x36c37a8) != base + 0x210bff8 &&
            word(0x36e6610) != base + 0x210b8c4,
        "a word was not swapped");
  CHECK(word(0x36f91f0) == base + 0x1524cb4, "the comparator slot is read only");
  CHECK(CountLine("dronekill hook install", 1) == 4, "four INFO install lines, got %d", CountLine("dronekill hook install", 1));
  CHECK(HasLine("dronekill: on (QA)", 1) && HasLine("k_feed=none in the plugin"), "summary line");
  CHECK(DroneKillShotActive(), "shot active");
  munmap(p, sz);
}

static void TestInstallVariants() {
  // kill only: FireBulletLocal / AttackTarget words stay; the tick is needed
  DroneKillInstall r = Reset(Only(true, false, false, false, false));
  CHECK(r.ok && r.needs_tick && g_w[g_fire_slot / 8] == reinterpret_cast<uintptr_t>(&FakeFire) && g_w[0x38 / 8] == reinterpret_cast<uintptr_t>(&FakeAttack),
        "kill only wraps fire/attack");
  CHECK(g_w[0x20 / 8] != reinterpret_cast<uintptr_t>(&FakeActor) && g_w[0x28 / 8] != reinterpret_cast<uintptr_t>(&FakeEnter), "actor+enter swapped");
  CHECK(!DroneKillShotActive(), "shot active without lockon/shot");
  // shot only: fire swapped, no tick needed
  r = Reset(Only(false, false, false, false, true));
  CHECK(r.ok && !r.needs_tick && g_w[g_fire_slot / 8] != reinterpret_cast<uintptr_t>(&FakeFire) && g_w[0x38 / 8] == reinterpret_cast<uintptr_t>(&FakeAttack) &&
            DroneKillShotActive(),
        "shot only");
  // lockon alone swaps fire
  r = Reset(Only(false, true, false, false, false));
  CHECK(r.ok && g_w[g_fire_slot / 8] != reinterpret_cast<uintptr_t>(&FakeFire) && DroneKillShotActive(), "lockon swaps fire");
  // chase swaps attack
  r = Reset(Only(false, false, false, true, false));
  CHECK(r.ok && g_w[0x38 / 8] != reinterpret_cast<uintptr_t>(&FakeAttack) && g_w[g_fire_slot / 8] == reinterpret_cast<uintptr_t>(&FakeFire), "chase swaps attack only");
  CHECK(r.needs_tick, "chase alone needs the tick");
  r = Reset(Only(false, false, true, false, false));
  CHECK(r.ok && !r.needs_tick, "gun alone needs no tick");

  // a slot that does not hold the pinned function: nothing is swapped (all or none), loud
  r = Reset(DroneKillWords{}, false);
  {
    uintptr_t* w = g_w;
    mprotect(w, 4096, PROT_READ | PROT_WRITE);
    w[0x38 / 8] = 0x4242420;  // AttackTarget's word
    mprotect(w, 4096, PROT_READ);
  }
  r = InstallDroneKill(g_base, DroneKillWords{}, Fns(), Slots());
  CHECK(!r.ok && r.attack.status == SwapStatus::kUnexpectedOriginal, "foreign slot word accepted");
  CHECK(g_w[0x20 / 8] == reinterpret_cast<uintptr_t>(&FakeActor) && g_w[0x28 / 8] == reinterpret_cast<uintptr_t>(&FakeEnter) &&
            g_w[g_fire_slot / 8] == reinterpret_cast<uintptr_t>(&FakeFire) && g_w[0x38 / 8] == 0x4242420,
        "a word changed although the install was refused");
  CHECK(HasLine("dronekill hook install", 3) && HasLine("dronekill: OFF, nothing changed", 3), "refusal not loud");
  CHECK(!DroneKillShotActive(), "shot active after a refusal");

  // the third swap fails (FireBulletLocal's slot is in an executable page): the two earlier words are put back
  r = Reset(DroneKillWords{}, false);
  NewPage(true);  // a fresh page whose fire slot lives in an executable page
  r = InstallDroneKill(g_base, DroneKillWords{}, Fns(), Slots());
  CHECK(!r.ok && r.fire.status == SwapStatus::kExecutablePage, "exec page accepted: %s", StatusName(r.fire.status));
  CHECK(g_w[0x20 / 8] == reinterpret_cast<uintptr_t>(&FakeActor) && g_w[0x28 / 8] == reinterpret_cast<uintptr_t>(&FakeEnter),
        "the earlier swaps were not put back");
  CHECK(HasLine("the earlier words were put back", 2), "rollback not reported as a WARNING");
  CHECK(HasLine("dronekill hook install", 3), "the failed swap was not an ERROR line");
  CHECK(!DroneKillShotActive(), "shot active after a failed swap");

  // a swap that fails after a hook that is not wanted (fire): the rollback skips it, so it ends quiet (WARNING, every word back)
  r = Reset(Only(true, false, true, true, false), false);
  NewPage(true);
  {
    mprotect(g_w + 4096 / 8, 4096, PROT_READ | PROT_WRITE);
    g_w[0x1038 / 8] = reinterpret_cast<uintptr_t>(&FakeAttack);
    mprotect(g_w + 4096 / 8, 4096, PROT_READ | PROT_EXEC);
    DroneKillSlots sl = Slots();
    sl.attack_slot = 0x1038;
    r = InstallDroneKill(g_base, Only(true, false, true, true, false), Fns(), sl);
  }
  CHECK(!r.ok && r.attack.status == SwapStatus::kExecutablePage, "attack in an executable page: %s", StatusName(r.attack.status));
  CHECK(g_w[0x20 / 8] == reinterpret_cast<uintptr_t>(&FakeActor) && g_w[0x28 / 8] == reinterpret_cast<uintptr_t>(&FakeEnter), "earlier words not put back");
  CHECK(HasLine("the earlier words were put back", 2) && !HasLine("the earlier words were put back", 3), "the unwanted hook was rolled back");

  // FindComponentSystemMT not resolved: refuse
  r = Reset(DroneKillWords{}, false);
  DroneKillFns none{};
  r = InstallDroneKill(g_base, DroneKillWords{}, none, Slots());
  CHECK(!r.ok && HasLine("FindComponentSystemMT is not resolved", 3) && g_w[0x28 / 8] == reinterpret_cast<uintptr_t>(&FakeEnter), "no find");

  // the same module twice: refused
  r = Reset(DroneKillWords{});
  CHECK(r.ok, "install");
  const DroneKillInstall r2 = InstallDroneKill(g_base, DroneKillWords{}, Fns(), Slots());
  CHECK(!r2.ok && HasLine("already installed", 3), "second install accepted");

  // chase without its functions: chase off, loud, the rest installs
  r = Reset(DroneKillWords{}, false);
  DroneKillFns f = Fns();
  f.local = nullptr;
  r = InstallDroneKill(g_base, DroneKillWords{}, f, Slots());
  CHECK(r.ok && HasLine("dronekill: chase OFF", 3) && g_w[0x38 / 8] == reinterpret_cast<uintptr_t>(&FakeAttack), "chase without IsLocallyControlled");

  // base 0
  g_lines.clear();
  r = InstallDroneKill(0, DroneKillWords{}, Fns(), Slots());
  CHECK(!r.ok && HasLine("no base address", 3), "base 0");
  // each function chase calls, missing: chase off, loud
  for (int which = 0; which < 5; ++which) {
    r = Reset(DroneKillWords{}, false);
    DroneKillFns ff = Fns();
    if (which == 0) ff.l2w = nullptr;
    if (which == 1) ff.primary = nullptr;
    if (which == 2) ff.resolve = nullptr;
    if (which == 3) ff.set_vel = nullptr;
    if (which == 4) ff.local = nullptr;
    r = InstallDroneKill(g_base, DroneKillWords{}, ff, Slots());
    CHECK(r.ok && HasLine("dronekill: chase OFF", 3) && g_w[0x38 / 8] == reinterpret_cast<uintptr_t>(&FakeAttack), "chase wrapped without function %d", which);
  }

  // the comparator's slot does not hold the comparator: loud; Component() look-ups off
  r = Reset(DroneKillWords{}, false);
  {
    mprotect(g_w, 4096, PROT_READ | PROT_WRITE);
    g_w[0x40 / 8] = 0x99;
    mprotect(g_w, 4096, PROT_READ);
  }
  r = InstallDroneKill(g_base, DroneKillWords{}, Fns(), Slots());
  CHECK(r.ok && !r.comparator_ok && HasLine("the comparator slot does not hold its function", 2), "comparator warning");
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&d);
  CHECK(g_comp_calls == 0 && HasLine("not looked up"), "Component() called with the comparator slot wrong");

  // the function check: each address must be base + its pinned vaddr
  {
    const uintptr_t b = 0x7000000000ull;
    const uintptr_t v[6] = {0x1804a10, 0x16f6ef0, 0x1677d34, 0x19a27ac, 0x167bec0, 0x2175630};
    const void* a[6];
    for (int i = 0; i < 6; ++i) a[i] = reinterpret_cast<const void*>(b + v[i]);
    DroneKillFns o{};
    CHECK(ResolveDroneKillFnsFromAddressesForTest(b, a, &o) && o.find != nullptr && o.l2w != nullptr && o.primary != nullptr && o.resolve != nullptr &&
              o.set_vel != nullptr && o.local != nullptr,
          "the pinned addresses were refused");
    CHECK(reinterpret_cast<uintptr_t>(o.find) == b + v[0] && reinterpret_cast<uintptr_t>(o.local) == b + v[5], "functions in the wrong order");
    for (int bad = 0; bad < 6; ++bad) {
      const void* c[6];
      memcpy(c, a, sizeof c);
      c[bad] = reinterpret_cast<const void*>(b + v[bad] + 4);
      DroneKillFns o2{};
      g_lines.clear();
      CHECK(!ResolveDroneKillFnsFromAddressesForTest(b, c, &o2) && o2.find == nullptr && HasLine("not at its pinned address", 3), "function %d at the wrong address accepted", bad);
      c[bad] = nullptr;
      CHECK(!ResolveDroneKillFnsFromAddressesForTest(b, c, &o2), "function %d missing accepted", bad);
    }
  }

  // functions are resolved from the game library: none on the host
  DroneKillFns out{};
  g_lines.clear();
  CHECK(!ResolveDroneKillFns(0x7000000000ull, &out) && HasLine("libr15.so is not loaded", 3) && out.find == nullptr, "resolve on the host");
}

// A wrapper called before its real function is known (the module reset under a live slot) calls nothing and faults nothing.
static void TestWrappersWithoutReal() {
  Reset(DroneKillWords{});
  auto aw = reinterpret_cast<ActorFn>(g_w[0x20 / 8]);
  void* sp = nullptr;
  uint64_t actor = 0;
  unsigned fl = 0;
  g_actor_ret = 0;
  CHECK(aw(nullptr, nullptr, dk::kDroneActor, &sp, &actor, &fl) == 0, "the Actor wrapper changed a 0 result");
  g_actor_ret = 7;
  CHECK(aw(nullptr, nullptr, dk::kDroneActor, &sp, &actor, &fl) == 7, "the Actor wrapper changed the result");
  CHECK(aw(nullptr, nullptr, 0x1234, &sp, &actor, &fl) == 1, "the Actor wrapper changed another actor's result");
  ResetDroneKillForTest();
  g_actor_calls = 0;
  g_enters.clear();
  g_fires.clear();
  g_attacks.clear();
  CHECK(aw(nullptr, nullptr, dk::kDroneActor, &sp, &actor, &fl) == 0 && g_actor_calls == 0, "the Actor wrapper called a null real function");
  ND dd = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&dd);
  CallFire(reinterpret_cast<void*>(0x6001), 1, DkC3{1, 2, 3}, 0);
  CallAttack(g_cs, kAtHandle);
  CHECK(g_enters.empty() && g_fires.empty() && g_attacks.empty(), "a wrapper called a null real function");
}

static void TestEnterOther() {
  Reset(DroneKillWords{});
  ND d{0x1234, 0, dk::kDamageable, dk::kDamageable, 1};
  CallEnter(&d);
  CHECK(g_enters.size() == 1 && g_enters[0].data == &d && g_enters[0].node == g_node && SameNd(g_enters[0].copy, d), "args to the real Enter");
  CHECK(g_find_calls == 0 && g_comp_calls == 0, "work done for another actor");
  CHECK(!HasLine("dronekill: component") && !HasLine("dronekill: release") && !HasLine("enter note"), "a line for another actor");
  const DroneKillStats s = GetDroneKillStats();
  CHECK(s.enter_other == 1 && s.enter_drone == 0, "counters %u/%u", s.enter_other, s.enter_drone);
  // an unreadable data pointer: the real Enter is reached with it, nothing else
  g_enter_deref = false;
  g_ev.clear();
  CallEnter(reinterpret_cast<const ND*>(0x1000));
  CHECK(g_ev.size() == 1 && g_ev[0] == "enter-bad-pointer", "unreadable data pointer: %zu events", g_ev.size());
  g_enter_deref = false;
  g_ev.clear();
  CallEnter(nullptr);
  CHECK(g_ev.size() == 1 && g_ev[0] == "enter-bad-pointer", "null data pointer");
  g_enter_deref = true;
}

static void TestDroneEnterKill() {
  DroneKillInstall r = Reset(DroneKillWords{});
  CHECK(r.ok, "install");
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&d);
  CHECK(GetDroneKillStats().enter_drone == 1 && GetDroneKillStats().enter_other == 0, "drone Enter counters");
  CHECK(g_enters.size() == 1 && g_enters[0].data == &d && SameNd(g_enters[0].copy, d), "the real Enter got other arguments");
  CHECK(g_ev.size() >= 4 && g_ev[0] == "enter" && g_ev[1] == "actor" && g_ev[2] == "find" && g_ev[3] == "component", "order: the real Enter first, then the look-ups");
  CHECK(g_bad_actor == 0, "Component() got another actor");
  CHECK(HasLine("dronekill: release (damageable on)", 1) && HasLine("index=17") && HasLine("group=1"), "release line");
  ND s = Drone(kSound, kSound, 1);
  CallEnter(&s);
  CHECK(HasLine("dronekill: component") && HasLine("to=group 1"), "the sound joins the group");
  ND off = Drone(kSound, kSound, 0);
  CallEnter(&off);
  CHECK(HasLine("enable=off") && HasLine("to=(off)"), "an OFF enter is only noted");
  CHECK(!HasLine("dronekill: touch deployed"), "a non-touch OFF enter was taken for a touch release");

  // K: the group is shut down at the tick, the disables go through DisableComponent(handle, 1)
  DroneKillOnKilled(0xabcd0011u);
  CHECK(HasLine("dronekill: K") && HasLine("group shut down") && HasLine("dronekill: K index=17 tid="), "K line");
  CHECK(g_dis.empty(), "disabled in the K thread");
  DroneKillTick(0);
  CHECK(HasLine("dronekill: tick running (first call)", 1), "first tick line");
  CHECK(g_dis.size() == 2, "disables at the tick: %zu", g_dis.size());  // the damageable and the sound (the OFF enter did not join)
  CHECK(g_dis.size() >= 2 && g_dis[0].sys == g_dmg && g_dis[0].handle == kDmgHandle && g_dis[0].cat == 1 && !g_dis[0].enable, "first disable");
  CHECK(g_dis.size() >= 2 && g_dis[1].sys == g_snd && g_dis[1].handle == 0x00060006 && g_dis[1].cat == 1, "second disable");
  CHECK(HasLine("dronekill: component off") && HasLine("result=OFF"), "off lines");
  const size_t nd = g_dis.size();
  DroneKillTick(0);
  CHECK(g_dis.size() == nd, "disabled again");
  CHECK(GetDroneKillStats().due_runs == 1 && GetDroneKillStats().ticks == 2, "tick counters");
  Dump("drone Enter, K, tick");

  // R3: the script turns a component of the dead group on again: it is off again at the next tick. The verdict on the DisableComponent
  // body is cached per vtable: a body that changes after the first disable is not read again
  g_dis.clear();
  g_dis_words[dk::kDisWords - 1] = 0xd65f03c0;
  ND again = Drone(kSound, kSound, 1);
  g_ms += 5000;
  g_lines.clear();
  CallEnter(&again);
  CHECK(HasLine("dronekill: re-enabled in a dead group") && HasLine("off again at the next tick"), "R3 line");
  CHECK(!HasLine("dronekill: component") && !HasLine("dronekill: release"), "R3: the Enter was logged as an ordinary one too");
  DroneKillTick(0);
  CHECK(g_dis.size() == 1 && g_dis[0].sys == g_snd, "R3: not disabled again");
}

static void TestKillRules() {
  // a K for an index no live group has
  Reset(DroneKillWords{});
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&d);
  DroneKillOnKilled(0x99);
  CHECK(HasLine("NO live group has that index") && HasLine("live_groups=1"), "K without a group");
  DroneKillTick(0);
  CHECK(g_dis.empty(), "disabled for a K that matched no group");
  // 16 bits of the index are compared
  DroneKillOnKilled(0xabcd0000u | (kDmgHandle & 0xffff));
  CHECK(HasLine("group shut down"), "the high 16 bits must not matter");

  // no kill word: the K is only recorded
  Reset(Only(false, true, true, true, true));
  CallEnter(&d);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  CHECK(HasLine("would be shut down (nothing done)"), "no-kill K line");
  DroneKillTick(0);
  CHECK(g_dis.empty(), "disabled without the kill word");

  // the invoke-immediate word is 0: Enter would queue here, we never do: nothing is disabled, it waits; due bits expire after 2 s
  Reset(DroneKillWords{});
  CallEnter(&d);
  ND s = Drone(kSound, kSound, 1);
  CallEnter(&s);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  SetFlag(0);
  DroneKillTick(0);
  CHECK(g_dis.empty(), "disabled outside the window");
  CHECK(HasLine("waiting_invoke_immediate_0=2", 2), "waiting not reported");
  SetFlag(1);
  g_ms += 1000;
  DroneKillTick(0);
  CHECK(g_dis.size() == 2, "disables after the window opened: %zu", g_dis.size());
  Reset(DroneKillWords{});
  CallEnter(&d);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  SetFlag(0);
  DroneKillTick(0);
  SetFlag(1);
  g_ms += 2001;
  DroneKillTick(0);
  CHECK(g_dis.empty() && HasLine("expired=1"), "an expired due bit still acted");

  // a tick that is re-entered from inside an engine call does nothing the second time
  Reset(DroneKillWords{});
  CallEnter(&d);
  CallEnter(&s);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  g_dis_hook = [] { DroneKillTick(0); };
  DroneKillTick(0);
  CHECK(g_dis.size() == 2, "a re-entered tick disabled again: %zu calls", g_dis.size());
  CHECK(GetDroneKillStats().due_runs == 1, "re-entered tick ran");

  // no kill word: a component of a dead group that is enabled again is only noted, nothing is made due
  Reset(Only(false, true, true, true, true));
  CallEnter(&d);
  CallEnter(&s);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  CallEnter(&s);
  CHECK(HasLine("would be off again (no kill word)"), "R3 without the kill word");
  DroneKillTick(0);
  CHECK(GetDroneKillStats().due_runs == 0 && g_dis.empty(), "the tick ran for a group that nothing marked due");

  // the system's game space was unreadable at the Enter: nothing to compare later
  Reset(DroneKillWords{});
  W64(g_dmg, dk::kSysOwner, 0);
  CallEnter(&d);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  DroneKillTick(0);
  CHECK(g_dis.empty() && HasLine("no game space recorded"), "disabled with no recorded game space");

  // the system's vtable changed to one whose Component() is not the read shape
  Reset(DroneKillWords{});
  CallEnter(&d);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  memcpy(g_vtable2, g_vtable, sizeof g_vtable);
  memcpy(g_thunk_bad, g_thunk_words, sizeof g_thunk_bad);
  g_thunk_bad[2] ^= 0x400;
  g_vtable2[dk::kVtComponent / 8] = reinterpret_cast<uintptr_t>(g_thunk_bad);
  W64(g_dmg, 0, reinterpret_cast<uintptr_t>(g_vtable2));
  DroneKillTick(0);
  CHECK(g_dis.empty() && HasLine("Component() not the read shape"), "disabled through an unknown Component()");

  // the system moved to another game space since the Enter: skipped for good
  Reset(DroneKillWords{});
  CallEnter(&d);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  W64(g_dmg, dk::kSysOwner, reinterpret_cast<uintptr_t>(g_gs2));
  DroneKillTick(0);
  CHECK(g_dis.empty() && HasLine("system's game space changed"), "disabled in another game space");
  // the system is not found again
  Reset(DroneKillWords{});
  CallEnter(&d);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  g_syss[0].sym = 0;
  DroneKillTick(0);
  CHECK(g_dis.empty() && HasLine("system not found again in its game space"), "disabled although the system is gone");
  // the handle changed
  Reset(DroneKillWords{});
  CallEnter(&d);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  g_comps[0].handle += 0x10000;
  DroneKillTick(0);
  CHECK(g_dis.empty() && HasLine("handle changed"), "disabled although the handle changed");
  // DisableComponent is not the read shape
  Reset(DroneKillWords{});
  CallEnter(&d);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  g_dis_words[dk::kDisWords - 1] = 0xd65f03c0;
  DroneKillTick(0);
  CHECK(g_dis.empty() && HasLine("DisableComponent not the read shape"), "disabled through an unknown body");
}

static void TestEnterLookups() {
  // Actor not called by the real Enter / returns 0 / another actor's call first
  Reset(DroneKillWords{});
  g_enter_calls_actor = false;
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&d);
  CHECK(HasLine("no actor (Enter found none") && g_find_calls == 0, "no actor");
  g_actor_ret = 0;
  g_enter_calls_actor = true;
  CallEnter(&d);
  CHECK(CountLine("no actor (Enter found none") == 2 && g_find_calls == 0, "Actor returned 0");
  g_actor_ret = 1;
  g_other_actor_first = 0x999;
  CallEnter(&d);
  CHECK(HasLine("dronekill: release (damageable on)") && g_bad_actor == 0, "another actor's call was taken for Enter's own");
  g_other_actor_first = 0;

  // no such system / no component / wrong shapes
  Reset(DroneKillWords{});
  g_syss[0].sym = 0;
  CallEnter(&d);
  CHECK(HasLine("no such system in the actor's game space"), "no system");
  Reset(DroneKillWords{});
  g_ncomps = 0;
  CallEnter(&d);
  CHECK(HasLine("no component under that name") && !HasLine("dronekill: release"), "no component");
  Reset(DroneKillWords{});
  g_thunk_words[2] ^= 0x400;
  CallEnter(&d);
  CHECK(HasLine("not looked up") && g_comp_calls == 0, "Component thunk shape");
  Reset(DroneKillWords{});
  g_body_words[10] = 0xf9000008;
  CallEnter(&d);
  CHECK(HasLine("not looked up") && g_comp_calls == 0, "Component body shape");
  Reset(DroneKillWords{});
  g_body_words[dk::kBodyBl] += 1;  // a bl that does not reach the comparator's PLT stub
  CallEnter(&d);
  CHECK(HasLine("not looked up") && g_comp_calls == 0, "Component body: bl target");
  Reset(DroneKillWords{});
  {
    // a tagged (odd) vtable pointer whose slots, read at +1, would hold the right words: still refused
    alignas(16) static uint8_t oddvt[0x400];
    memset(oddvt, 0, sizeof oddvt);
    for (size_t slot : {dk::kVtComponentId, dk::kVtComponent, dk::kVtEnable, dk::kVtDisable}) memcpy(oddvt + 1 + slot, &g_vtable[slot / 8], 8);
    W64(g_dmg, 0, reinterpret_cast<uintptr_t>(oddvt) + 1);
    CallEnter(&d);
    CHECK(HasLine("not looked up") && g_comp_calls == 0, "odd vtable");
  }
  // Actor says yes but its out-pointers cannot be read: no capture
  Reset(DroneKillWords{});
  g_actor_bad_out = true;
  CallEnter(&d);
  CHECK(HasLine("no actor (Enter found none") && g_find_calls == 0, "unreadable Actor outputs were used");
  // Actor called twice for the drone: the first call's capture stands
  Reset(DroneKillWords{});
  g_actor_twice = true;
  CallEnter(&d);
  CHECK(HasLine("dronekill: release (damageable on)") && g_bad_actor == 0, "the second Actor call replaced the capture (bad actor calls %d)", g_bad_actor);
  // a component handle whose index is 0xffff is none, whatever its generation
  Reset(DroneKillWords{});
  g_comps[0].handle = 0x0007ffff;
  CallEnter(&d);
  CHECK(HasLine("no component under that name") && !HasLine("dronekill: release"), "an 0xffff index with a generation taken for a component");
  // an OFF damageable is an ordinary component line, not a release; a first component with no group waits as pending
  Reset(DroneKillWords{});
  ND dmg_off = Drone(dk::kDamageable, dk::kDamageable, 0);
  CallEnter(&dmg_off);
  CHECK(HasLine("dronekill: component") && HasLine("enable=off") && !HasLine("dronekill: release"), "a damageable OFF logged as a release");
  Reset(DroneKillWords{});
  ND snd_on = Drone(kSound, kSound, 1);
  CallEnter(&snd_on);
  CHECK(HasLine("to=pending"), "a component with no group is pending");
  // the shape verdict is cached per vtable: the second Enter reads no words again (a corrupted body after the first is not seen)
  Reset(DroneKillWords{});
  CallEnter(&d);
  g_body_words[10] = 0xf9000008;
  g_comp_calls = 0;
  ND s = Drone(kSound, kSound, 1);
  CallEnter(&s);
  CHECK(g_comp_calls == 1, "the shape check was not cached: %d component calls", g_comp_calls);

  // the log of one kind stops at its cap, once, loudly
  Reset(DroneKillWords{});
  g_enter_calls_actor = false;
  for (int i = 0; i < 30; ++i) CallEnter(&d);
  CHECK(CountLine("dronekill: enter note") == 24, "note cap: %d", CountLine("dronekill: enter note"));
  CHECK(CountLine("dronekill: log cap reached") == 1, "cap line %d", CountLine("dronekill: log cap reached"));

  // every capped kind stops at its own number
  Reset(DroneKillWords{});
  CallEnter(&d);
  for (int i = 0; i < 200; ++i) CallEnter(&snd_on);
  CHECK(CountLine("dronekill: release") + CountLine("dronekill: component") == 160, "enter lines: %d", CountLine("dronekill: release") + CountLine("dronekill: component"));
  CHECK(CountLine("kind=enter") == 1, "enter cap line");
  Reset(DroneKillWords{});
  CallEnter(&d);
  CallEnter(&snd_on);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  for (int i = 0; i < 80; ++i) CallEnter(&snd_on);
  CHECK(CountLine("re-enabled in a dead group") == 64 && CountLine("kind=off") == 1, "off lines: %d", CountLine("re-enabled in a dead group"));
  Reset(DroneKillWords{});
  CallEnter(&d);
  for (int i = 0; i < 60; ++i) DroneKillOnKilled(0x99);
  CHECK(CountLine("dronekill: K") == 48 && CountLine("kind=k") == 1, "K lines: %d", CountLine("dronekill: K"));
  Reset(DroneKillWords{});
  ND gun_on = Drone(dk::kNcaModel, dk::kGun, 1);
  for (int i = 0; i < 100; ++i) CallEnter(&gun_on);
  CHECK(CountLine("dronekill: gun mirrored") == 96 && CountLine("kind=f357") == 1, "gun lines: %d", CountLine("dronekill: gun mirrored"));

  // the K feed and the tick before an install are inert
  ResetDroneKillForTest();
  g_lines.clear();
  DroneKillOnKilled(1);
  DroneKillTick(0);
  CHECK(g_lines.empty() && GetDroneKillStats().ticks == 0, "inert before install");
}

static void TestGun() {
  Reset(DroneKillWords{});
  ND drone_dmg = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&drone_dmg);  // records the drone's group (not needed for the gun)
  g_enters.clear();
  ND gun = Drone(dk::kNcaModel, dk::kGun, 1);
  CallEnter(&gun);
  CHECK(g_enters.size() == 3, "gun: %zu real Enters", g_enters.size());
  if (g_enters.size() == 3) {
    CHECK(g_enters[0].data == &gun && g_enters[0].copy.component == dk::kGun, "the first is the script's own");
    CHECK(g_enters[1].data != &gun && g_enters[1].copy.component == dk::kSatellite && g_enters[1].copy.enable == 1 && g_enters[1].copy.actor == dk::kDroneActor &&
              g_enters[1].copy.system == dk::kNcaModel,
          "satellite copy");
    CHECK(g_enters[2].copy.component == dk::kPort && g_enters[2].copy.enable == 1, "port copy");
    CHECK(gun.component == dk::kGun && gun.enable == 1, "the script's data was written");
  }
  CHECK(HasLine("dronekill: gun mirrored") && HasLine("gun=shown"), "gun line");
  // hidden: the same
  g_enters.clear();
  ND gun0 = Drone(dk::kNcaModel, dk::kGun, 0);
  CallEnter(&gun0);
  CHECK(g_enters.size() == 3 && g_enters[1].copy.enable == 0 && g_enters[2].copy.enable == 0, "hidden gun mirrored");
  // a drone without a port: only the satellite
  g_enters.clear();
  for (int i = 0; i < g_ncomps; ++i)
    if (g_comps[i].sym == dk::kPort) g_comps[i].sym = 0;
  ND gun1 = Drone(dk::kNcaModel, dk::kGun, 1);
  CallEnter(&gun1);
  CHECK(g_enters.size() == 2 && g_enters[1].copy.component == dk::kSatellite && HasLine("not mirrored"), "no port: %zu enters", g_enters.size());
  for (int i = 0; i < g_ncomps; ++i)
    if (g_comps[i].sym == 0) g_comps[i].sym = dk::kPort;

  // the port follows the gun: a port Enter that differs from the gun's state gets one more Enter with the gun's state
  g_enters.clear();
  ND gun2 = Drone(dk::kNcaModel, dk::kGun, 1);
  CallEnter(&gun2);
  g_enters.clear();
  ND port0 = Drone(dk::kNcaModel, dk::kPort, 0);
  CallEnter(&port0);
  CHECK(g_enters.size() == 2 && g_enters[1].copy.component == dk::kPort && g_enters[1].copy.enable == 1 && g_enters[1].data != &port0, "port follows the gun");
  CHECK(HasLine("dronekill: port follows the gun"), "port line");
  g_enters.clear();
  ND port1 = Drone(dk::kNcaModel, dk::kPort, 1);
  CallEnter(&port1);
  CHECK(g_enters.size() == 1, "port equal to the gun: %zu enters", g_enters.size());
  // unknown gun state (another game's actor... a fresh table): nothing extra
  Reset(DroneKillWords{});
  g_enters.clear();
  CallEnter(&port0);
  CHECK(g_enters.size() == 1, "port with no known gun state: %zu enters", g_enters.size());
  // the gun's symbols in another system are not the gun
  Reset(DroneKillWords{});
  g_comps[g_ncomps++] = {g_snd, dk::kGun, 0x00060012};  // the other system has a "gun" and the mirror targets too
  g_idxtab[0x12 * 2] = 0x12;
  g_comps[g_ncomps++] = {g_snd, dk::kSatellite, 0x00060010};
  g_comps[g_ncomps++] = {g_snd, dk::kPort, 0x00060011};
  g_idxtab[0x10 * 2] = 0x10;
  g_idxtab[0x11 * 2] = 0x11;
  g_enters.clear();
  ND not_gun = Drone(kSound, dk::kGun, 1);
  CallEnter(&not_gun);
  CHECK(g_enters.size() == 1, "a component named like the gun in another system was mirrored");
  // a K clears the gun's state (upstream Shutdown's SetGun(DRONE_ACTOR, 0))
  Reset(DroneKillWords{});
  CallEnter(&drone_dmg);
  CallEnter(&gun2);
  DroneKillOnKilled(kDmgHandle & 0xffff);
  g_enters.clear();
  ND port2 = Drone(dk::kNcaModel, dk::kPort, 1);
  CallEnter(&port2);
  CHECK(g_enters.size() == 2 && g_enters[1].copy.enable == 0, "after the K the port follows a hidden gun: %zu", g_enters.size());
  // no gun word: nothing mirrored
  Reset(Only(true, true, false, true, true));
  g_enters.clear();
  CallEnter(&gun);
  CHECK(g_enters.size() == 1, "gun mirrored without the word");
  // a drone Enter in another game space clears the gun state (a level change)
  Reset(DroneKillWords{});
  CallEnter(&gun2);
  g_space_now = g_spacemem2;
  CallEnter(&drone_dmg);
  g_space_now = g_spacemem;
  g_enters.clear();
  CallEnter(&port0);
  CHECK(g_enters.size() == 1, "the gun state survived a level change: %zu enters", g_enters.size());
}

static void TestLockOn() {
  DroneKillInstall r = Reset(DroneKillWords{});
  CHECK(r.ok, "install");
  ND dmg = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&dmg);  // the drone's live group (its actor)
  ND t0 = Drone(dk::kR14Touch, dk::kR14Touch, 0);
  CallEnter(&t0);  // the script made it ungrabbable
  CHECK(HasLine("dronekill: touch deployed"), "no deployed line");
  g_dis.clear();
  DroneKillTick(0);
  CHECK(g_dis.empty(), "touch on again before the script's disable was applied (mask bit clear)");
  g_masks[0x5] |= 1;  // the script's disable is applied
  DroneKillTick(0);
  CHECK(g_dis.size() == 1 && g_dis[0].enable && g_dis[0].sys == g_touch && g_dis[0].handle == 0x00010005 && g_dis[0].cat == 1, "on again: %zu calls", g_dis.size());
  CHECK(HasLine("dronekill: touch on again") && HasLine("on again: grabbable until it locks on"), "on-again line");
  // EnableComponent's body is read once per vtable: a body that changes later is not read again
  g_en_words[dk::kEnWords - 1] = 0xd61f0040;
  g_dis.clear();
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  DroneKillTick(0);
  CHECK(g_dis.size() == 1 && g_dis[0].enable, "the EnableComponent verdict was not cached: %zu calls", g_dis.size());
  g_en_words[dk::kEnWords - 1] = 0xd65f03c0;
  // the drone's first shot: the touch goes off at the next tick, only while the drone is live
  g_dis.clear();
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{0, 0, 1}, 0);
  CHECK(g_fires.size() == 1, "fire not passed on");
  DroneKillTick(0);
  CHECK(g_dis.size() == 1 && !g_dis[0].enable && g_dis[0].sys == g_touch && g_dis[0].cat == 1, "locked on: %zu calls", g_dis.size());
  CHECK(HasLine("dronekill: touch locked on") && HasLine("result=off"), "locked-on line");
  // a second shot of the same drone does nothing more
  g_dis.clear();
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{0, 0, 1}, 0);
  DroneKillTick(0);
  CHECK(g_dis.empty(), "second shot disabled again");

  // the invoke-immediate word is 0: it waits (the script's disable is applied but we never queue)
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  SetFlag(0);
  DroneKillTick(0);
  CHECK(g_dis.empty(), "enabled outside the window");
  SetFlag(1);
  g_ms += 500;
  DroneKillTick(0);
  CHECK(g_dis.size() == 1 && g_dis[0].enable, "on again after the window opened");

  // a dead drone: the touch is left alone
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  DroneKillTick(0);
  g_dis.clear();
  DroneKillOnKilled(kDmgHandle & 0xffff);
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{0, 0, 1}, 0);
  g_dis.clear();
  DroneKillTick(0);
  size_t offs = 0;
  for (const auto& c : g_dis)
    if (c.sys == g_touch && !c.enable) ++offs;
  CHECK(offs == 0 && HasLine("belongs to no live drone"), "a dead drone's touch was disabled: %zu", offs);

  // EnableComponent is not the read shape: skipped for good
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  g_en_words[dk::kEnWords - 1] = 0xd61f0040;  // a tail call (br x2) for the ret
  DroneKillTick(0);
  CHECK(g_dis.empty() && HasLine("EnableComponent not the read shape"), "enabled through an unknown body");

  // a script's ENABLE of the touch is not a release
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  {
    ND t1 = Drone(dk::kR14Touch, dk::kR14Touch, 1);
    CallEnter(&t1);
    CHECK(!HasLine("dronekill: touch deployed"), "an ON touch Enter was taken for a release");
  }
  // the touch's system moved to another game space: skipped for good
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  W64(g_touch, dk::kSysOwner, reinterpret_cast<uintptr_t>(g_gs2));
  DroneKillTick(0);
  CHECK(g_dis.empty() && HasLine("system's game space changed"), "the touch was enabled in another game space");
  // its disable mask cannot be read
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  W64(g_touch, dk::MaskOffsets(dk::kEn).idx, 0);
  DroneKillTick(0);
  CHECK(g_dis.empty() && HasLine("disable mask unreadable"), "the touch was enabled with an unreadable mask");
  // skipped for good is DONE, not HELD: a later shot binds nothing
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  g_en_words[dk::kEnWords - 1] = 0xd61f0040;
  DroneKillTick(0);
  g_en_words[dk::kEnWords - 1] = 0xd65f03c0;
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{0, 0, 1}, 0);
  g_dis.clear();
  DroneKillTick(0);
  CHECK(g_dis.empty() && !HasLine("dronekill: touch locked on"), "a record that was skipped for good locked on");
  // a drone Enter in another game space is a level change: the touch records go
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  g_space_now = g_spacemem2;
  CallEnter(&dmg);
  g_space_now = g_spacemem;
  DroneKillTick(0);
  CHECK(g_dis.empty(), "a touch record survived a level change");
  // ... but a second drone Enter in the same game space is not
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallEnter(&t0);
  ND snd = Drone(kSound, kSound, 1);
  CallEnter(&snd);
  g_masks[0x5] |= 1;
  DroneKillTick(0);
  CHECK(g_dis.size() == 1 && g_dis[0].enable, "a touch record was cleared by a drone Enter in the same game space");
  // the touch belongs to a live drone through its group's components, not only its actor
  Reset(DroneKillWords{});
  g_actor_val = 0xaaa1;
  CallEnter(&dmg);
  ND t_on = Drone(dk::kR14Touch, dk::kR14Touch, 1);
  CallEnter(&t_on);  // joins the drone's group
  g_actor_val = 0xbbb2;
  CallEnter(&t0);  // the release arrives under another actor
  g_masks[0x5] |= 1;
  DroneKillTick(0);
  g_dis.clear();
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{0, 0, 1}, 0);
  DroneKillTick(0);
  CHECK(g_dis.size() == 1 && !g_dis[0].enable && g_dis[0].sys == g_touch, "a component of a live group was not live (%zu calls)", g_dis.size());

  // no lockon word: nothing is tracked, the touch is not re-enabled
  Reset(Only(true, false, true, true, true));
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  DroneKillTick(0);
  CHECK(g_dis.empty() && !HasLine("dronekill: touch deployed"), "touch handled without the lockon word");
}

static void TestFire() {
  Reset(Only(false, false, false, false, true));
  CHECK(DroneKillShotVelocity(nullptr) == nullptr, "accessor non-null outside a shot");
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{1, 2, 3}, 1);
  CHECK(g_fires.size() == 1, "fires");
  const FireCall& c = g_fires[0];
  CHECK(c.cs == reinterpret_cast<void*>(0x6001) && c.handle == 0x00020003 && c.v.x == 1 && c.v.y == 2 && c.v.z == 3 && c.remote == 1, "args to the real FireBulletLocal");
  CHECK(c.acc_non_null && c.acc[0] == 1 && c.acc[1] == 2 && c.acc[2] == 3 && c.acc_remote == 1, "accessor inside the real call");
  CHECK(DroneKillShotVelocity(nullptr) == nullptr, "accessor non-null after the shot");
  CallFire(reinterpret_cast<void*>(0x6001), 1, DkC3{4, 5, 6}, 0);
  CHECK(g_fires[1].acc_remote == 0 && g_fires[1].acc[0] == 4, "second shot / local");
  CHECK(GetDroneKillStats().fires == 2, "fire counter");
  // a shot inside a shot: the outer shot's values are back after the inner returns
  g_fires.clear();
  CallFire(reinterpret_cast<void*>(0x7001), 8, DkC3{7, 8, 9}, 0);
  // fires[0] is the outer (pushed before its nested call); fires[1] the inner
  CHECK(g_fires.size() == 2 && g_fires[0].cs == reinterpret_cast<void*>(0x7001) && g_fires[1].cs == reinterpret_cast<void*>(0x7002), "nested order");
  CHECK(g_fires.size() == 2 && g_fires[1].acc[0] == 1 && g_fires[1].acc[1] == 2 && g_fires[1].acc[2] == 3 && g_fires[1].acc_remote == 1, "inner shot's values");
  CHECK(g_fires.size() == 2 && g_fires[0].after[0] == 7 && g_fires[0].after[1] == 8 && g_fires[0].after[2] == 9, "the outer values are not back after the inner shot");
  CHECK(g_fires.size() == 2 && g_fires[0].after_remote == 0, "the outer shot's remote flag is not back after the inner shot (%d)", g_fires[0].after_remote);
  CHECK(DroneKillShotVelocity(nullptr) == nullptr, "accessor non-null after the nested shots");
  // nothing is tracked for lock-on without its word: no table work
  CHECK(!HasLine("touch"), "touch lines without lockon");
}

static void TestChase() {
  DroneKillInstall r = Reset(DroneKillWords{});
  CHECK(r.ok, "install");
  ND dmg = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&dmg);  // a drone Enter: records the game space seen with this physics system's owner
  CallAttack(g_cs, kAtHandle);
  CHECK(g_attacks.size() == 1 && g_attacks[0].cs == g_cs && g_attacks[0].handle == kAtHandle, "args to the real AttackTarget");
  float want[3], dist = 0;
  const float q[3] = {10, 0, 0};
  CHECK(dk::ChaseVelocity(g_me_pos, q, want, &dist), "expected velocity");
  CHECK(HasLine("dronekill: chase") && HasLine("locally_controlled=1"), "chase line");
  CHECK(GetDroneKillStats().attacks == 1, "attack counter");
  CHECK(g_primary_bodies.size() == 1 && g_primary_bodies[0] == 9, "the target's body handle is the damageable record's +0xc");
  CHECK(g_l2w.size() == 1 && g_l2w[0].transforms == g_xf_me && g_l2w[0].handle == 0x77, "the drone's transform handle is the record's +0x7c");
  CHECK(g_local_sys == g_np && g_local_handle == 0x0001000a, "IsLocallyControlled got the net physics component's handle");
  CHECK(g_setvels.empty(), "velocity set in the attack call (only at the tick)");
  DroneKillTick(0);
  CHECK(g_setvels.size() == 1, "set_vel calls: %zu", g_setvels.size());
  if (g_setvels.size() == 1) {
    CHECK(g_setvels[0].physics == g_phys && g_setvels[0].handle == kPhysH, "set_vel target");
    CHECK(std::fabs(g_setvels[0].v.x - want[0]) < 1e-5f && std::fabs(g_setvels[0].v.y - want[1]) < 1e-5f && std::fabs(g_setvels[0].v.z - want[2]) < 1e-5f,
          "velocity (%f %f %f) want (%f %f %f)", g_setvels[0].v.x, g_setvels[0].v.y, g_setvels[0].v.z, want[0], want[1], want[2]);
  }
  DroneKillTick(0);
  CHECK(g_setvels.size() == 1, "one write per AttackTarget");
  Dump("chase");

  // the body position is the one read; the transform is the fallback
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  g_target_pos[0] = 0;
  g_target_pos[1] = 0;
  g_target_pos[2] = 30;  // the l2w answer for the target; the body at +0x8f8 says (10,0,0)
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.size() == 1 && std::fabs(g_setvels[0].v.x - want[0]) < 1e-5f, "the body's position was not preferred");
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  g_have_body = false;
  float p2[3] = {0, 0, 30}, want2[3];
  g_target_pos[0] = 0;
  g_target_pos[1] = 0;
  g_target_pos[2] = 30;
  CHECK(dk::ChaseVelocity(g_me_pos, p2, want2), "expected 2");
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.size() == 1 && std::fabs(g_setvels[0].v.z - want2[2]) < 1e-5f, "the transform fallback");
  CHECK(g_l2w.size() == 2 && g_l2w[1].transforms == g_xf_target && g_l2w[1].handle == 0x55, "the fallback's transform handle is the damageable record's +8");

  // the fallback's transform system pointer is null: nothing is computed, no call with a null system
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  g_have_body = false;
  W64(g_ts, dk::kDmXf, 0);
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.empty() && g_l2w.size() == 1, "a null transform system was used (%zu l2w calls)", g_l2w.size());
  // the drone's own transform system pointer is null
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  W64(g_cs, dk::kAtTransforms, 0);
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.empty() && g_l2w.empty(), "a null transform system was passed to the game (%zu l2w calls)", g_l2w.size());
  // the auto-target system's own game space is unreadable
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  W64(g_cs, dk::kSysOwner, 0);
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "written with no game space for the auto-target system");
  // the real AttackTarget acquires the target: the record is read after it, not before
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  (g_recsA + 2 * dk::kAtStride)[dk::kAtHasTarget] = 0;
  g_attack_sets_target = true;
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.size() == 1, "the target AttackTarget just acquired was not chased (%zu)", g_setvels.size());
  // on the target: the two positions coincide, nothing is written
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  {
    const float on[3] = {0, -dk::kChaseAbove, 0};
    memcpy(g_body + dk::kBodyPos, on, sizeof on);
  }
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "a zero velocity was written for a drone on its target");

  // the physics system's game space is not the one recorded for any drone component: not local
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  W64(g_phys, dk::kSysOwner, reinterpret_cast<uintptr_t>(g_gs2));
  {
    const uint32_t one = 1;
    memcpy(g_gs2 + dk::kOwnerFlag, &one, 4);
  }
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "written for a physics system whose game space no drone component was seen in");
  // the net physics component's physics system is another one
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  W64(g_np, dk::kNetPhysicsPhys, 0x1234);
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "written although the net physics component belongs to another physics system");
  // the physics record has no owner actor
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  W64(g_owners, 2 * 16, 0);
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "written for a physics record with no owner");
  // another peer controls the drone: nothing is written (fails closed)
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  g_local_ret = 0;
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.empty() && HasLine("locally_controlled=0"), "set although not locally controlled");
  // the game space of the physics system was never seen: not local
  Reset(DroneKillWords{});
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.empty() && g_find_calls == 0, "set although no game space was recorded for the owner (%d finds)", g_find_calls);
  // the drone has no R14NetPhysics component: upstream-exact, treated as local
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  for (int i = 0; i < g_ncomps; ++i)
    if (g_comps[i].sym == dk::kNetPhysics) g_comps[i].sym = 0;
  g_local_ret = 0;
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.size() == 1, "no net physics: %zu writes", g_setvels.size());

  // outside the invoke-immediate window: nothing is written, and it is not retried (one write per AttackTarget)
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  SetFlag(0);
  DroneKillTick(0);
  SetFlag(1);
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "written outside the window");
  // older than kChaseMs at the tick
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_ms += dk::kChaseMs;
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "a stale velocity was written");
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_ms += dk::kChaseMs - 1;
  DroneKillTick(0);
  CHECK(g_setvels.size() == 1, "a fresh velocity was dropped");
  // the physics handle's generation changed between the attack and the tick: not the same drone
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_idxP[(kPhysH & 0xffff) * 2 + 1] = 9;
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "written to a recycled physics handle");
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_idxA[(kAtHandle & 0xffff) * 2 + 1] = 9;  // the auto-target handle recycled
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "written for a recycled auto-target handle");
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  W64(g_phys, dk::kSysOwner, reinterpret_cast<uintptr_t>(g_gs2));  // the physics system's game space changed
  {
    const uint32_t one = 1;
    memcpy(g_gs2 + dk::kOwnerFlag, &one, 4);  // and that one is inside its window
  }
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "written after the physics system's owner changed");
  // the auto-target system's game space changed
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  W64(g_cs, dk::kSysOwner, reinterpret_cast<uintptr_t>(g_gs));
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "written after the auto-target system's owner changed");
  // the record's physics handle changed
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  {
    const uint32_t other = 0x00010009;
    memcpy(g_recsA + 2 * dk::kAtStride + dk::kAtPhysIdx, &other, 4);
  }
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "written after the record's physics handle changed");
  // the auto-target system's physics pointer changed
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  W64(g_cs, dk::kAtPhysics, reinterpret_cast<uintptr_t>(g_gs));
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "written after the physics pointer changed");
  // a stale velocity also stops the drone being "moving": a later lost target writes no stop
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_ms += dk::kChaseMs;
  DroneKillTick(0);
  (g_recsA + 2 * dk::kAtStride)[dk::kAtHasTarget] = 0;
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "a stop was written for a drone whose velocity had gone stale");
  // one write per AttackTarget even while the tick keeps running for something else (a touch waiting for its disable)
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  ND touch_off = Drone(dk::kR14Touch, dk::kR14Touch, 0);
  CallEnter(&touch_off);
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  DroneKillTick(0);
  DroneKillTick(0);
  CHECK(g_setvels.size() == 1, "a velocity was written again by a later tick: %zu", g_setvels.size());
  // a level change drops the chase records, with the chase word alone
  Reset(Only(false, false, false, true, false));
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_space_now = g_spacemem2;
  CallEnter(&dmg);
  g_space_now = g_spacemem;
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "a chase record survived a level change");
  // the chase log line stops at 8, once, loudly
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  for (int i = 0; i < 12; ++i) {
    CallAttack(g_cs, kAtHandle);
    DroneKillTick(0);
  }
  CHECK(CountLine("dronekill: chase") == 8 && CountLine("kind=chase") == 1, "chase lines: %d", CountLine("dronekill: chase"));

  // the target is lost: a resting drone writes nothing; a moving one is stopped with a zero velocity
  Reset(DroneKillWords{});
  CallEnter(&dmg);
  (g_recsA + 2 * dk::kAtStride)[dk::kAtHasTarget] = 0;
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.empty(), "a resting drone with no target was written");
  (g_recsA + 2 * dk::kAtStride)[dk::kAtHasTarget] = 1;
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.size() == 1, "moving");
  (g_recsA + 2 * dk::kAtStride)[dk::kAtHasTarget] = 0;
  CallAttack(g_cs, kAtHandle);
  DroneKillTick(0);
  CHECK(g_setvels.size() == 2 && g_setvels[1].v.x == 0 && g_setvels[1].v.y == 0 && g_setvels[1].v.z == 0, "no stop after the target was lost: %zu", g_setvels.size());
  CHECK(CountLine("dronekill: chase") == 1, "the stop was logged as a chase (%d lines)", CountLine("dronekill: chase"));

  // no chase word: AttackTarget is not wrapped, nothing is set
  Reset(Only(true, true, true, false, true));
  CHECK(g_w[0x38 / 8] == reinterpret_cast<uintptr_t>(&FakeAttack), "attack wrapped without chase");
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  SetLogContext(&ctx);
  TestCore();
  TestPinnedConstants();
  TestInstallPinned();
  TestInstallVariants();
  TestWrappersWithoutReal();
  TestEnterOther();
  TestDroneEnterKill();
  TestKillRules();
  TestEnterLookups();
  TestGun();
  TestLockOn();
  TestFire();
  TestChase();
  if (g_fail == 0) std::printf("dronekill_test: all checks passed\n");
  else std::printf("dronekill_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}

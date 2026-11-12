// Host test of dronekill (src/quest/combat/dronekill.cpp, #2187): the real module and its four CallbackThunks, installed by the real GotHook into
// four JUMP_SLOTs of a libr15 image built in memory (combat_libr15_image.h), called through the slots, against a fake game laid out as the module
// reads it (component systems with the real read-shape words, a fake auto-target system for chase), real SafeRead (process_vm_readv). The calls
// into game code through vtable slots are replaced through the module's test seam (a slot's words are aarch64 data on the host).
//
// The assertions of the combat plugin's dronekill_test (#1591): the assertions on what the module does are the same. What changed is the fixture (the plugin's
// fake slot page is a libr15 image and the manager's install; a slot the manager cannot write is one outside the image's writable segment, where
// the plugin test used an executable page), the log lines (sentinel records; the text a check looks for is the record's event and fields, see
// Render), the API names (namespace nevr_quest::combat::dronekill), and the scope: a combat map is loaded and the module's tick has seen it before a
// test drives a drone (Reset). New here: an Echo Arena level, a lobby and no level at all, where every hook only calls the game's function, the
// scope line, and the production step's refusals.
#include <atomic>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/dronekill.h"
#include "quest/combat/dronekill_core.h"
#include "quest/combat/ec_core.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/ec_page.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_scope_world.h"
#include "quest/tests/ec_stub.h"
#include "quest/tests/key_hog.h"

// The arena path's read count, through safe_read.h's test seam: every SafeRead after StartReadCount adds one.
namespace {
std::atomic<std::uint64_t> g_arenaReads{0};
[[maybe_unused]] void StartReadCount() {
  g_arenaReads.store(0);
  nevr_quest::combat::SetReadCounterForTest(&g_arenaReads);
}
[[maybe_unused]] std::uint64_t ReadCount() { return g_arenaReads.load(); }
// The arena path's level scans, through combat_level.h's process count: ScanCount is the scans since StartScanCount.
std::uint64_t g_scanBase = 0;
[[maybe_unused]] void StartScanCount() { g_scanBase = nevr_quest::combat::level::ScanCalls(); }
[[maybe_unused]] std::uint64_t ScanCount() { return nevr_quest::combat::level::ScanCalls() - g_scanBase; }
}  // namespace

using namespace nevr_quest::combat::dronekill;
using sentinel::GotStatus;
namespace scope = nevr_quest::combat::scope;
namespace world = combat_test::scope_world;

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
  std::string raw;  // the JSON line as the sentinel wrote it: a field Render leaves out (`level`) is checked here
};
static std::vector<Line> g_lines;
// A sentinel record as the plugin test's lines read: `<event> key=value ...` (string values unquoted), at the plugin's level numbers (1 Info,
// 2 Warning, 3 Error, 0 Debug), so a check names an event or a field.
static std::string Render(const char* json) {
  std::string out, key;
  const char* p = json;
  bool first = true;
  while (*p != 0) {
    if (*p != '"') {
      ++p;
      continue;
    }
    std::string k;
    for (++p; *p != 0 && *p != '"'; ++p) k += *p;
    if (*p == '"') ++p;
    while (*p == ':' || *p == ' ') ++p;
    std::string v;
    if (*p == '"') {
      for (++p; *p != 0 && *p != '"'; ++p) {
        if (*p == '\\' && p[1] != 0) ++p;
        v += *p;
      }
      if (*p == '"') ++p;
    } else {
      while (*p != 0 && *p != ',' && *p != '}') v += *p++;
    }
    if (k == "ts_ms" || k == "level" || k == "ts") continue;
    if (k == "event") {
      out = v + out;
      first = false;
      continue;
    }
    out += " " + k + "=" + v;
  }
  (void)first;
  return out;
}
static void Sink(sentinel::LogLevel level, const char* line) {
  const uint32_t lv = level == sentinel::LogLevel::kInfo ? 1 : level == sentinel::LogLevel::kWarn ? 2 : level == sentinel::LogLevel::kError ? 3 : 0;
  // The combat level answer's records and the tick's (combat_scope_*, combat_tick_*) are those rows' own, checked by combat_scope_test and the
  // tick's tests; this test reads dronekill's.
  if (std::strstr(line, "\"event\":\"combat_scope_") != nullptr || std::strstr(line, "\"event\":\"combat_tick_") != nullptr) return;
  g_lines.push_back({lv, Render(line), std::string(line)});
}
static int CountLine(const char* needle, int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++n;
  return n;
}
static bool HasLine(const char* needle, int level = -1) { return CountLine(needle, level) > 0; }
// One raw line that holds every needle (a raw JSON fragment, e.g. "\"in_combat\":0"): the fields of one record, checked together.
static bool HasRawLine(std::initializer_list<const char*> needles) {
  for (const auto& l : g_lines) {
    bool all = true;
    for (const char* n : needles) all = all && l.raw.find(n) != std::string::npos;
    if (all) return true;
  }
  return false;
}
// The raw JSON fragment of a level field: `"level":"0x%016llx"` for the level's symbol hash, as the scope's records write it (combat_scope.h).
static std::string LevelField(const char* name) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "\"level\":\"0x%016llx\"", static_cast<unsigned long long>(EchoVR::CSymbol64Hash(name)));
  return buf;
}
static void Dump(const char* title) {  // NEVR_TEST_VERBOSE=1: the module's own log records of that scenario
  if (std::getenv("NEVR_TEST_VERBOSE") == nullptr) return;
  std::printf("---- %s\n", title);
  for (const auto& l : g_lines) std::printf("[%u] %s\n      raw: %s\n", l.level, l.text.c_str(), l.raw.c_str());
}

// ================================================================ the core (ported from the source's host test)
static void TestCore() {
  using namespace dk;
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
  CHECK(j.pending && tb.t.pendingN == 2, "pending 2");
  j = tb.On(0xd0, kDamageable, 9, 0x00050011, 1200);
  CHECK(!j.pending && j.group == 1 && j.pendingJoined == 2 && tb.t.pendingN == 0, "release takes the pending");
  CHECK(tb.Newest()->n == 3 && (tb.Newest()->handle & 0xffff) == 0x11 && tb.Newest()->comps[0].x.sysSym == kDamageable, "group shape");
  j = tb.On(0xb0, sound, 5, 6, 1300);
  CHECK(!j.pending && j.group == 1 && tb.Newest()->n == 4, "join the newest group");
  j = tb.On(0xb0, sound, 5, 6, 1200 + kJoinMs);
  CHECK(j.pending && tb.t.pendingN == 1, "exactly 5 s: no longer joins");
  j = tb.On(0xd0, kDamageable, 9, 0x22, 1200 + kJoinMs + kPendingMs);
  CHECK(j.group == 2 && j.pendingJoined == 0 && tb.t.pendingN == 0, "pending of 3 s or more not taken");
  j = tb.On(0xd0, kDamageable, 9, 0x70011, 20000);
  CHECK(j.group == 3, "group 3");
  Group* g = tb.Killed(0xabc0011);
  CHECK(g && g->x.number == 1 && g->dead, "K: low 16 bits, first in table order (D9)");
  g = tb.Killed(0x11);
  CHECK(g && g->x.number == 3, "K: the next one (D9)");
  CHECK(tb.Killed(0x11) == nullptr, "K: then none");
  uint32_t live[kGroups];
  CHECK(tb.Live(live, kGroups) == 1 && live[0] == 0x22, "live list");
  Table big;
  big.On(0xd0, kDamageable, 9, 1, 0);
  for (int i = 0; i < kGroupComps + 5; ++i) big.On(0xb0, sound, i, i, 10);
  CHECK(big.Newest()->n == kGroupComps && big.Newest()->x.overflow == 6, "overflow counted");
  Table pend;
  for (int i = 0; i < kPending + 7; ++i) pend.On(0xb0, sound, i, i, 10 + i);
  CHECK(pend.t.pendingN == 7 && pend.t.pendingDropped == 16 && pend.t.pending[0].x.compSym == 16, "the 17th empties the list, then is added (D6)");
  Table many;
  for (int i = 0; i < kGroups + 2; ++i) many.On(0xd0, kDamageable, 9, 100 + i, i * 10000);
  CHECK(many.made == kGroups + 2 && many.Newest()->x.number == kGroups + 2 && many.Killed(100) == nullptr && many.Killed(101) == nullptr, "group ring");
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
  CHECK(j.group == 1 && j.pendingJoined == 2 && s1.t.pendingN == 1 && s1.t.pending[0].actor == B, "C1: own actor's pending");
  j = s1.On(0xd0, kDamageable, 9, 0x12, 1200, B, S, O);
  CHECK(j.group == 2 && j.pendingJoined == 1 && s1.t.pendingN == 0, "C1 second");
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
  CHECK(g && g->x.number == 1 && g->dead && g->x.due == (1u << g->n) - 1 && g->x.dueMs == 10000, "R2 due");
  Due dd[64];
  int n = s1.TakeDue(10500, dd, 64);
  CHECK(n == g->n && dd[0].c.x.sysSym == kDamageable && dd[0].c.x.owner == O && dd[0].c.x.space == S && dd[0].c.actor == A, "due components");
  for (int i = 0; i < n; ++i)
    if (i != 1) s1.Done(dd[i].number, dd[i].idx);
  CHECK(s1.AnyDue() && s1.TakeDue(10600, dd, 64) == 1 && dd[0].idx == 1, "one left");
  CHECK(s1.TakeDue(10000 + kDueMs, dd, 64) == 0 && !s1.AnyDue() && s1.expired == 1, "due bits expire");
  g = s1.Killed(0x12);
  CHECK(g && g->x.number == 2 && g->x.due == 0 && !s1.AnyDue(), "stage-0 K sets no bit");
  j = s1.On(0xa0, model, 1, 3, 1100 + kSwallowMs - 1, A, S, O);
  CHECK(j.again && j.group == 1 && !j.pending && s1.AnyDue() && s1.TakeDue(1100 + kSwallowMs - 1, dd, 64) == 1 && dd[0].c.handle == 3, "R3");
  s1.Done(dd[0].number, dd[0].idx);
  j = s1.On(0xa0, model, 1, 3, 1100 + kSwallowMs, A, S, O);
  CHECK(j.again && s1.AnyDue() && s1.TakeDue(1100 + kSwallowMs, dd, 64) == 1, "R3 at exactly 25 s is still again (D8)");
  s1.Done(dd[0].number, dd[0].idx);
  j = s1.On(0xa0, model, 1, 3, 1100 + kSwallowMs + 1, A, S, O);
  CHECK(!j.again && !s1.AnyDue(), "R3 ends after 25 s (D8)");
  Table r;
  r.On(0xd0, kDamageable, 9, 0x11, 0, A, S, O);
  r.Killed(0x11, 5, true);
  j = r.On(0xd0, kDamageable, 9, 0x11, 40000, A, S, O);
  CHECK(j.group == 2 && r.ByNumber(1) == nullptr && !r.ByNumber(2)->dead && r.ByNumber(2)->x.due == 0 && !r.AnyDue(), "C2 slot reuse");
  j = r.On(0xb0, sound, 5, 6, 40100, A, 0x7000, O);
  CHECK(r.t.spaceClears == 1 && r.ByNumber(2) == nullptr && j.pending && r.t.pendingN == 1 && r.t.space == 0x7000, "another space clears the table");
  Table m;
  for (int i = 0; i < kGroups + 1; ++i) m.On(0xd0, kDamageable, 9, 100 + i, i * 10000);
  CHECK(m.ByNumber(1) == nullptr && m.ByNumber(2) && m.ByNumber(kGroups + 1), "oldest slot taken");
  // D7: two live groups of one actor: the first in table order takes the next component of that actor
  Table d7a;
  d7a.On(0xd0, kDamageable, 9, 0x31, 1000, A);
  d7a.On(0xd0, kDamageable, 9, 0x32, 2000, A);
  CHECK(d7a.On(0xb0, sound, 7, 9, 2500, A).group == 1, "D7: first live group of the actor in table order");
  // D7: two groups released in the same millisecond: the first in table order takes a component of no actor
  Table d7b;
  d7b.On(0xd0, kDamageable, 9, 0x41, 3000, 0);
  d7b.On(0xd0, kDamageable, 9, 0x42, 3000, 0);
  CHECK(d7b.On(0xb0, sound, 7, 9, 3100, 0).group == 1, "D7: the first in table order on a release-time tie");

  // touch
  TouchTable tt;
  Comp a = {0x1000, 0x00010005, 0, 0, QComp{kR14Touch, kR14Touch, 0, 0}}, b = {0x1000, 0x00020006, 0, 0, QComp{kR14Touch, kR14Touch, 0, 0}};
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
      Comp c;
      c.system = 0x1000;
      c.x.sysSym = kR14Touch;
      c.x.compSym = kR14Touch;
      c.handle = static_cast<uint32_t>(0x100 + i);
      c.at = 0;
      slots[i] = t3.Deployed(c, 1000 + 10 * (kTouches - i));  // the first is the youngest, the last the oldest
    }
    bool distinct = true;
    for (int i = 0; i < kTouches; ++i)
      for (int k = 0; k < i; ++k) distinct &= slots[i] != slots[k];
    CHECK(distinct, "16 records did not take 16 slots");
    Comp extra = {0x1000, 0x999, 0, 0, QComp{kR14Touch, kR14Touch, 0, 0}};
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
// compared with the disassembly (docs/quest/combat/dronekill.md); the hashes are of the tables as read there.
static void TestPinnedConstants() {
  using namespace dk;
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
  CHECK(kScopeFrames == 32, "the scope is asked every 32 frames");
  CHECK(kGroupComps == 20 && kGroups == 8 && kPending == 16 && kTouches == 16 && kGuns == 32 && kChases == 16, "table sizes");
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
// The combat level answer's own clock (combat_scope.h), in ms. Each test frame moves it one look spacing, so each frame's refresh looks at the game as it
// is then (on the headset the answer looks every 250 ms and this module asks every kScopeFrames frames, 267 ms or more: each ask reads a later look).
static int64_t g_scopeMs = 1000;
// Armed for one tick (g_armedReads >= 0 counts the reads since the arming): the first read is the record's own (LastLook reads the clock once, and
// the record's freshness is judged on it); every later read in that tick sees the answer kStaleAfterMs + 1 older. So a live read of the answer, made after
// the record, disagrees with it (TestTheRecordDecidesNotTheLiveRead). -1: not armed.
static int g_armedReads = -1;
// Every read of the scope's clock (LookIsFresh, LastLook, Refresh), so ExpectInert can show the arena path of the hooks reads none: the freshness test
// is in the in-scope bodies, after the handlers' test of the cached scope (README rule 9).
static int g_scopeClockReads = 0;
static int64_t ScopeClock() noexcept {
  ++g_scopeClockReads;
  if (g_armedReads >= 0 && g_armedReads++ >= 1) return g_scopeMs + scope::kStaleAfterMs + 1;
  return g_scopeMs;
}

static uintptr_t g_base = 0;
// The four slots in the image's writable segment (g_fire_slot moves below it, out of the image's writable part, to make an install fail), the
// comparator's slot beside them.
using combat_test::Image;
static Image g_image;
static constexpr uint64_t kRw = Image::kRwStart;
static constexpr uint64_t kSlotActor = kRw + 0x20, kSlotEnter = kRw + 0x28, kSlotFire = kRw + 0x30, kSlotAttack = kRw + 0x38, kSlotCmp = kRw + 0x40;
static constexpr uint64_t kExecFire = 0x2030, kExecAttack = 0x2038;  // in the image's R|X segment: the manager refuses them (slot_outside_image)
static uintptr_t& W(uint64_t vaddr) { return *reinterpret_cast<uintptr_t*>(g_base + vaddr); }
static uintptr_t g_fire_slot = 0;
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
static uint32_t g_actor_seen = 0;  // what the Enter fake got back from its own Actor call (through the slot)
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
static bool g_enter_actor_on_other_thread = false;
static const ND* g_nested_first = nullptr;  // the fake Enter enters this drone node (through the slot) before its own Actor call  // while the real Enter runs, another thread calls Actor for the drone (other out-values)

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

static uintptr_t ActorWrapPtr() { return W(kSlotActor); }
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
  if (g_enter_actor_on_other_thread) {
    g_enter_actor_on_other_thread = false;
    const uint64_t keep = g_actor_val;
    g_actor_val = 0xB0B;  // the other thread's Actor reports another actor than this Enter's
    std::thread([aw] {
      void* sp2 = nullptr;
      uint64_t actor2 = 0;
      unsigned fl2 = 0;
      aw(nullptr, nullptr, dk::kDroneActor, &sp2, &actor2, &fl2);
    }).join();
    g_actor_val = keep;
  }
  if (g_nested_first != nullptr) {
    const ND* const inner = g_nested_first;
    g_nested_first = nullptr;
    reinterpret_cast<EnterFn>(W(kSlotEnter))(node, inner);
  }
  if (g_other_actor_first != 0) aw(nullptr, nullptr, g_other_actor_first, &sp, &actor, &fl);
  if (g_actor_bad_out) {
    aw(nullptr, nullptr, d->actor, reinterpret_cast<void**>(0x10), reinterpret_cast<void*>(0x10), &fl);
    return;
  }
  g_actor_seen = aw(nullptr, nullptr, d->actor, &sp, &actor, &fl);
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
static bool g_other_thread_saw_no_shot = false;  // set by the thread FakeFire starts for cs 0x8001
static void FakeFire(void* cs, uint32_t h, DkC3 v, uint32_t remote) {
  g_ev.push_back("fire");
  FireCall c{cs, h, v, remote, false, {0, 0, 0}, -1, {0, 0, 0}, -1};
  int rem = -1;
  const float* a = ShotVelocity(&rem);
  if (a != nullptr) {
    c.acc_non_null = true;
    memcpy(c.acc, a, sizeof c.acc);
    c.acc_remote = rem;
  }
  const size_t mine = g_fires.size();
  g_fires.push_back(c);
  if (cs == reinterpret_cast<void*>(0x8001)) {  // another thread, outside any shot, reads the accessor and then makes a shot of its own
    std::thread([] {
      g_other_thread_saw_no_shot = ShotVelocity(nullptr) == nullptr;
      reinterpret_cast<FireFn>(W(g_fire_slot))(reinterpret_cast<void*>(0x8002), 3, DkC3{4, 5, 6}, 0);
    }).join();
    int rem2 = -1;
    const float* a2 = ShotVelocity(&rem2);
    if (a2 != nullptr) memcpy(g_fires[mine].after, a2, sizeof g_fires[mine].after);
    g_fires[mine].after_remote = rem2;
  }
  if (cs == reinterpret_cast<void*>(0x7001)) {  // a shot inside a shot (nested test)
    FireFn inner = reinterpret_cast<FireFn>(W(g_fire_slot));
    inner(reinterpret_cast<void*>(0x7002), 9, DkC3{1, 2, 3}, 1);
    int rem2 = -1;
    const float* a2 = ShotVelocity(&rem2);
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

static Fns FakeFns() {
  Fns f;
  f.find = &FakeFind;
  f.l2w = &FakeL2w;
  f.primary = &FakePrimary;
  f.resolve = &FakeResolve;
  f.set_vel = &FakeSetVel;
  f.local = &FakeLocal;
  return f;
}
static const Calls g_calls = {&FakeComponent, &FakeEnable, &FakeDisable};

// ---- the image, the levels ----
// The four slots in the image's writable segment (g_fire_slot moves below it, out of the image's writable part, to make an install fail), the
// comparator's slot beside them.
static bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
static bool NoLookup(const char*, sentinel::ElfImage*) { return false; }
// A libr15 image with the tick's slot row, on which the tick is installed and the scope row registers its refresh: the combat level answer can
// change, as the shipped table leaves it before this module's row.
static Image g_tickImage;
static bool TickImageLookup(const char* module, sentinel::ElfImage* out) {
  if (!g_tickImage.Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_tickImage.Elf();
  return true;
}
static void StartTheScope() {
  nevr_quest::combat::tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&ScopeClock);
  g_tickImage.Build({world::TickRow()});
  world::PutTickSlot(g_tickImage);
  CHECK(world::StartScope(&TickImageLookup), "the tick and the scope row did not start");
}

// CR15Game's two gamespace tables and a gamespace's level, where combat_level.cpp reads them (the offsets spelled here, as in headset_gates_test).
constexpr size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;
struct Gamespace {
  uintptr_t words[0x60 / 8 + 1] = {};
};
struct Game {
  uintptr_t words[0x2000 / 8] = {};
  uintptr_t table0[8][2] = {};
  Gamespace spaces[8];
};
static Game g_game;
static uintptr_t GameAddress() { return reinterpret_cast<uintptr_t>(g_game.words); }
static void LoadLevels(std::initializer_list<const char*> names) {
  g_game = Game{};
  int row = 0;
  for (const char* name : names) {
    Gamespace& gs = g_game.spaces[row];
    gs.words[kGamespaceOwner / 8] = GameAddress();
    gs.words[kGamespaceLevel / 8] = static_cast<uintptr_t>(EchoVR::CSymbol64Hash(name));
    g_game.table0[row][0] = 0x5555;
    g_game.table0[row][1] = reinterpret_cast<uintptr_t>(&gs);
    ++row;
  }
  g_game.words[kTable0 / 8] = reinterpret_cast<uintptr_t>(g_game.table0);
  g_game.words[kCount0 / 8] = static_cast<uintptr_t>(row);
}

// One frame of the game-thread tick: the combat level answer's refresh (the scope row registers it before this module's callback), then the module's.
static void Frame(uintptr_t game = GameAddress()) {
  g_scopeMs += scope::kLookEveryMs;
  scope::Refresh(game);
  Tick(game);
}

// Ticks until the module has asked the level `asks` times (it asks on the frames whose count is a multiple of kScopeFrames), the last ask included.
static void TickAsks(unsigned asks) {
  for (unsigned done = 0; done < asks;) {
    if (CurrentStats().ticks % kScopeFrames == 0) ++done;
    Frame();
  }
}

static uintptr_t V(const void* fn) { return reinterpret_cast<uintptr_t>(fn) - g_base; }
static Pins Slots() {
  return {kSlotActor, V(reinterpret_cast<void*>(&FakeActor)), kSlotEnter, V(reinterpret_cast<void*>(&FakeEnter)), g_fire_slot,
          V(reinterpret_cast<void*>(&FakeFire)), kSlotAttack, V(reinterpret_cast<void*>(&FakeAttack)), kSlotCmp, 0x1234,
          reinterpret_cast<uintptr_t>(g_plt_marker) - g_base};
}

// A fresh libr15 image whose four JUMP_SLOTs hold the fakes (the fire slot outside the writable segment when `fire_in_exec_page`), and an
// attack relocation outside it too (kExecAttack), for the rollback case.
static void NewPage(bool fire_in_exec_page) {
  g_fire_slot = fire_in_exec_page ? kExecFire : kSlotFire;
  g_image.Build({{kActorSymbol, kSlotActor, false, false, 0}, {kEnterSymbol, kSlotEnter, false, false, 0},
                 {kFireSymbol, g_fire_slot, false, false, 0}, {kAttackSymbol, fire_in_exec_page ? kExecAttack : kSlotAttack, false, false, 0}});
  g_base = g_image.Base();
  W(kSlotActor) = reinterpret_cast<uintptr_t>(&FakeActor);
  W(kSlotEnter) = reinterpret_cast<uintptr_t>(&FakeEnter);
  W(g_fire_slot) = reinterpret_cast<uintptr_t>(&FakeFire);
  W(kSlotAttack) = reinterpret_cast<uintptr_t>(&FakeAttack);
  W(kSlotCmp) = g_base + 0x1234;
}

// The install, then the scope a combat map gives (Reset loads one): the module's first tick asks combat_level and keeps the answer.
static Result InstallDroneKill(Words words, const Fns& fns, const Pins& pins) {
  const Result r = InstallAt(&Lookup, pins, words, fns);
  if (r.ok) SetScopeForTest(true);
  return r;
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

static Result Reset(Words words, bool install = true) {
  ResetForTest();
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
  g_enter_actor_on_other_thread = false;
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
  SetCallsForTest(&g_calls);
  SetClockForTest(&Clk);
  scope::ResetForTest();
  scope::SetClockForTest(&ScopeClock);
  g_scopeMs = 1000;
  LoadLevels({"mpl_combat_dyson"});
  if (!install) return Result{};
  return InstallDroneKill(words, FakeFns(), Slots());
}

static void CallEnter(const ND* d) { reinterpret_cast<EnterFn>(W(kSlotEnter))(g_node, d); }
static void CallFire(void* cs, uint32_t h, DkC3 v, uint32_t remote) { reinterpret_cast<FireFn>(W(g_fire_slot))(cs, h, v, remote); }
static void CallAttack(void* cs, uint32_t h) { reinterpret_cast<AttackFn>(W(kSlotAttack))(cs, h); }
static ND Drone(uint64_t sys, uint64_t comp, uint32_t en) { return ND{dk::kDroneActor, 0, sys, comp, en}; }
static const uint32_t kDmgHandle = 0x00050011;  // index 0x11

static Words Only(bool kill, bool lockon, bool gun, bool chase, bool shot) {
  Words w;
  w.kill = kill;
  w.lockon = lockon;
  w.gun = gun;
  w.chase = chase;
  w.shot = shot;
  w.explosion = false;  // the explosion (#1737) is TestExplosion's: Only names the five rules alone
  return w;
}

// ================================================================ tests
// No pthread key left for the wrappers' per-thread state: the install refuses before it swaps anything. Runs before any successful
// install, because a key made by one stays for the process.
static void TestNoThreadKey() {
  // a module refused for a slot that does not hold the pinned function has made no key: the install below, with no key left, must still
  // be refused for the key
  Reset(Words{}, false);
  W(kSlotAttack) = 0x4242420;
  CHECK(!InstallDroneKill(Words{}, FakeFns(), Slots()).ok && HasLine("a slot does not hold the pinned function"),
        "the slot mismatch was not refused");
  KeyHog hog;
  const Result r = Reset(Only(true, true, true, true, true));
  CHECK(!r.ok, "installed with no pthread key");
  CHECK(W(kSlotActor) == reinterpret_cast<uintptr_t>(&FakeActor) && W(kSlotEnter) == reinterpret_cast<uintptr_t>(&FakeEnter) &&
            W(g_fire_slot) == reinterpret_cast<uintptr_t>(&FakeFire) && W(kSlotAttack) == reinterpret_cast<uintptr_t>(&FakeAttack),
        "no key: a word was swapped");
  const std::string code = "error=" + std::to_string(EAGAIN);  // what pthread_key_create answers when the process has no key left
  CHECK(HasLine("result=refused", 3) && HasLine("no pthread key") && HasLine(code.c_str()), "no key: the refusal is not loud, or carries no error code");
  CHECK(!ShotActive(), "no key: shot active");
}

static void TestInstallPinned() {
  ResetForTest();
  g_lines.clear();
  const Pins& k = kPins;
  CHECK(k.actorSlot == 0x36e2080 && k.actorFn == 0x2059714, "actor constants");
  CHECK(k.enterSlot == 0x36cacf8 && k.enterFn == 0x263b700, "enter constants");
  CHECK(k.fireSlot == 0x36c37a8 && k.fireFn == 0x210bff8, "fire constants");
  CHECK(k.attackSlot == 0x36e6610 && k.attackFn == 0x210b8c4, "attack constants");
  CHECK(k.cmpSlot == 0x36f91f0 && k.cmpFn == 0x1524cb4 && k.cmpPlt == 0xf43610, "comparator constants");
  CHECK(std::strcmp(kActorSymbol, "_ZN10NRadEngine19SComponentNodeUtils5ActorERKNS_13SScriptThreadERKNS_15CncaGameSpaceMTENS_9CSymbol64ERPS4_RNS_6SActorERj") == 0 &&
            std::strcmp(kEnterSymbol, "_ZN10NRadEngine20CEnableComponentNode5EnterERKNS_24SEnableComponentNodeDataE") == 0 &&
            std::strcmp(kFireSymbol, "_ZN10NRadEngine8NRadGame19CR15NetAutoTargetCS15FireBulletLocalENS_18SCompactPoolHandleENS_8C3VectorEj") == 0 &&
            std::strcmp(kAttackSymbol, "_ZN10NRadEngine8NRadGame19CR15NetAutoTargetCS12AttackTargetENS_18SCompactPoolHandleE") == 0,
        "the symbols are the readelf names");
  // the production pins on an image holding them: the four slots hold base + the pinned functions (the fakes stand at those addresses: the
  // manager requires an executable original, so the image's own code segment is not one; the fakes are placed by pins below)
  g_image.Build({{kActorSymbol, k.actorSlot, false, false, k.actorFn}, {kEnterSymbol, k.enterSlot, false, false, k.enterFn},
                 {kFireSymbol, k.fireSlot, false, false, k.fireFn}, {kAttackSymbol, k.attackSlot, false, false, k.attackFn}});
  g_base = g_image.Base();
  Pins pins = k;  // the production slots; the functions are the fakes'
  pins.actorFn = V(reinterpret_cast<void*>(&FakeActor));
  pins.enterFn = V(reinterpret_cast<void*>(&FakeEnter));
  pins.fireFn = V(reinterpret_cast<void*>(&FakeFire));
  pins.attackFn = V(reinterpret_cast<void*>(&FakeAttack));
  W(k.actorSlot) = reinterpret_cast<uintptr_t>(&FakeActor);
  W(k.enterSlot) = reinterpret_cast<uintptr_t>(&FakeEnter);
  W(k.fireSlot) = reinterpret_cast<uintptr_t>(&FakeFire);
  W(k.attackSlot) = reinterpret_cast<uintptr_t>(&FakeAttack);
  W(k.cmpSlot) = g_base + k.cmpFn;
  const Result r = InstallDroneKill(Words{}, FakeFns(), pins);
  CHECK(r.ok && r.actor == GotStatus::kOk && r.enter == GotStatus::kOk && r.fire == GotStatus::kOk && r.attack == GotStatus::kOk, "pinned install");
  CHECK(r.comparatorOk && r.needsTick, "comparator / needs_tick");
  CHECK(W(k.actorSlot) != reinterpret_cast<uintptr_t>(&FakeActor) && W(k.enterSlot) != reinterpret_cast<uintptr_t>(&FakeEnter) &&
            W(k.fireSlot) != reinterpret_cast<uintptr_t>(&FakeFire) && W(k.attackSlot) != reinterpret_cast<uintptr_t>(&FakeAttack),
        "a word was not swapped");
  CHECK(W(k.cmpSlot) == g_base + k.cmpFn, "the comparator slot is read only");
  CHECK(CountLine("combat_dronekill_hook", 1) == 4, "four INFO install lines, got %d", CountLine("combat_dronekill_hook", 1));
  CHECK(HasLine("result=installed", 1) && HasLine("combat_dronekill_install"), "summary line");
  CHECK(ShotActive(), "shot active");
  ResetForTest();
}

static void TestInstallVariants() {
  // kill only: FireBulletLocal / AttackTarget words stay; the tick is needed
  Result r = Reset(Only(true, false, false, false, false));
  CHECK(r.ok && r.needsTick && W(g_fire_slot) == reinterpret_cast<uintptr_t>(&FakeFire) && W(kSlotAttack) == reinterpret_cast<uintptr_t>(&FakeAttack),
        "kill only wraps fire/attack");
  CHECK(W(kSlotActor) != reinterpret_cast<uintptr_t>(&FakeActor) && W(kSlotEnter) != reinterpret_cast<uintptr_t>(&FakeEnter), "actor+enter swapped");
  CHECK(!ShotActive(), "shot active without lockon/shot");
  // shot only: fire swapped, no tick needed
  r = Reset(Only(false, false, false, false, true));
  CHECK(r.ok && !r.needsTick && W(g_fire_slot) != reinterpret_cast<uintptr_t>(&FakeFire) && W(kSlotAttack) == reinterpret_cast<uintptr_t>(&FakeAttack) &&
            ShotActive(),
        "shot only");
  // lockon alone swaps fire
  r = Reset(Only(false, true, false, false, false));
  CHECK(r.ok && W(g_fire_slot) != reinterpret_cast<uintptr_t>(&FakeFire) && ShotActive(), "lockon swaps fire");
  // chase swaps attack
  r = Reset(Only(false, false, false, true, false));
  CHECK(r.ok && W(kSlotAttack) != reinterpret_cast<uintptr_t>(&FakeAttack) && W(g_fire_slot) == reinterpret_cast<uintptr_t>(&FakeFire), "chase swaps attack only");
  CHECK(r.needsTick, "chase alone needs the tick");
  r = Reset(Only(false, false, true, false, false));
  CHECK(r.ok && !r.needsTick, "gun alone needs no tick");

  // a slot that does not hold the pinned function: nothing is swapped (all or none), loud
  r = Reset(Words{}, false);
  W(kSlotAttack) = 0x4242420;  // AttackTarget's word
  r = InstallDroneKill(Words{}, FakeFns(), Slots());
  CHECK(!r.ok && r.attack == GotStatus::kOriginalMismatch, "foreign slot word accepted");
  CHECK(W(kSlotActor) == reinterpret_cast<uintptr_t>(&FakeActor) && W(kSlotEnter) == reinterpret_cast<uintptr_t>(&FakeEnter) &&
            W(g_fire_slot) == reinterpret_cast<uintptr_t>(&FakeFire) && W(kSlotAttack) == 0x4242420,
        "a word changed although the install was refused");
  CHECK(HasLine("combat_dronekill_hook", 3) && HasLine("result=refused", 3), "refusal not loud");
  CHECK(!ShotActive(), "shot active after a refusal");

  // the third swap fails (FireBulletLocal's slot is in an executable page): the two earlier words are put back
  r = Reset(Words{}, false);
  NewPage(true);  // a fresh page whose fire slot lives in an executable page
  r = InstallDroneKill(Words{}, FakeFns(), Slots());
  CHECK(!r.ok && r.fire == GotStatus::kSlotOutsideImage, "exec page accepted: %s", sentinel::GotStatusName(r.fire));
  CHECK(W(kSlotActor) == reinterpret_cast<uintptr_t>(&FakeActor) && W(kSlotEnter) == reinterpret_cast<uintptr_t>(&FakeEnter),
        "the earlier swaps were not put back");
  CHECK(HasLine("combat_dronekill_rollback", 2), "rollback not reported as a WARNING");
  CHECK(HasLine("combat_dronekill_hook", 3), "the failed swap was not an ERROR line");
  CHECK(!ShotActive(), "shot active after a failed swap");

  // a swap that fails after a hook that is not wanted (fire): the rollback skips it, so it ends quiet (WARNING, every word back)
  r = Reset(Only(true, false, true, true, false), false);
  NewPage(true);
  {
    W(kExecAttack) = reinterpret_cast<uintptr_t>(&FakeAttack);
    Pins sl = Slots();
    sl.attackSlot = kExecAttack;
    r = InstallDroneKill(Only(true, false, true, true, false), FakeFns(), sl);
  }
  CHECK(!r.ok && r.attack == GotStatus::kSlotOutsideImage, "attack in an executable page: %s", sentinel::GotStatusName(r.attack));
  CHECK(W(kSlotActor) == reinterpret_cast<uintptr_t>(&FakeActor) && W(kSlotEnter) == reinterpret_cast<uintptr_t>(&FakeEnter), "earlier words not put back");
  CHECK(HasLine("combat_dronekill_rollback", 2) && !HasLine("combat_dronekill_rollback", 3), "the unwanted hook was rolled back");

  // FindComponentSystemMT not resolved: refuse
  r = Reset(Words{}, false);
  Fns none{};
  r = InstallDroneKill(Words{}, none, Slots());
  CHECK(!r.ok && HasLine("FindComponentSystemMT is not resolved", 3) && W(kSlotEnter) == reinterpret_cast<uintptr_t>(&FakeEnter), "no find");

  // the same module twice: refused
  r = Reset(Words{});
  CHECK(r.ok, "install");
  const Result r2 = InstallDroneKill(Words{}, FakeFns(), Slots());
  CHECK(!r2.ok && HasLine("already_ran", 3), "second install accepted");

  // chase without its functions: chase off, loud, the rest installs
  r = Reset(Words{}, false);
  Fns f = FakeFns();
  f.local = nullptr;
  r = InstallDroneKill(Words{}, f, Slots());
  CHECK(r.ok && HasLine("combat_dronekill_chase_off", 3) && W(kSlotAttack) == reinterpret_cast<uintptr_t>(&FakeAttack), "chase without IsLocallyControlled");

  // no libr15 loaded
  ResetForTest();
  g_lines.clear();
  r = InstallAt(&NoLookup, Slots(), Words{}, FakeFns());
  CHECK(!r.ok && HasLine("module_not_loaded", 3), "base 0");
  // each function chase calls, missing: chase off, loud
  for (int which = 0; which < 5; ++which) {
    r = Reset(Words{}, false);
    Fns ff = FakeFns();
    if (which == 0) ff.l2w = nullptr;
    if (which == 1) ff.primary = nullptr;
    if (which == 2) ff.resolve = nullptr;
    if (which == 3) ff.set_vel = nullptr;
    if (which == 4) ff.local = nullptr;
    r = InstallDroneKill(Words{}, ff, Slots());
    CHECK(r.ok && HasLine("combat_dronekill_chase_off", 3) && W(kSlotAttack) == reinterpret_cast<uintptr_t>(&FakeAttack), "chase wrapped without function %d", which);
  }

  // the comparator's slot does not hold the comparator: loud; Component() look-ups off
  r = Reset(Words{}, false);
  {
    W(kSlotCmp) = 0x99;
  }
  r = InstallDroneKill(Words{}, FakeFns(), Slots());
  CHECK(r.ok && !r.comparatorOk && HasLine("the comparator slot does not hold its function", 2), "comparator warning");
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&d);
  CHECK(g_comp_calls == 0 && HasLine("not looked up"), "Component() called with the comparator slot wrong");

  // the function check: each address must be base + its pinned vaddr
  {
    const uintptr_t b = 0x7000000000ull;
    const uintptr_t v[6] = {0x1804a10, 0x16f6ef0, 0x1677d34, 0x19a27ac, 0x167bec0, 0x2175630};
    const void* a[6];
    for (int i = 0; i < 6; ++i) a[i] = reinterpret_cast<const void*>(b + v[i]);
    Fns o{};
    CHECK(ResolveFnsFromAddressesForTest(b, a, &o) && o.find != nullptr && o.l2w != nullptr && o.primary != nullptr && o.resolve != nullptr &&
              o.set_vel != nullptr && o.local != nullptr,
          "the pinned addresses were refused");
    CHECK(reinterpret_cast<uintptr_t>(o.find) == b + v[0] && reinterpret_cast<uintptr_t>(o.local) == b + v[5], "functions in the wrong order");
    for (int bad = 0; bad < 6; ++bad) {
      const void* c[6];
      memcpy(c, a, sizeof c);
      c[bad] = reinterpret_cast<const void*>(b + v[bad] + 4);
      Fns o2{};
      g_lines.clear();
      CHECK(!ResolveFnsFromAddressesForTest(b, c, &o2) && o2.find == nullptr && HasLine("not at its pinned address", 3), "function %d at the wrong address accepted", bad);
      c[bad] = nullptr;
      CHECK(!ResolveFnsFromAddressesForTest(b, c, &o2), "function %d missing accepted", bad);
    }
  }

  // functions are resolved from the game library: none on the host
  Fns out{};
  g_lines.clear();
  CHECK(!ResolveFns(0x7000000000ull, &out) && HasLine("libr15.so is not loaded", 3) && out.find == nullptr, "resolve on the host");
}

// A wrapper called before its real function is known (the module reset under a live slot) calls nothing and faults nothing.
static void TestWrappersWithoutReal() {
  Reset(Words{});
  auto aw = reinterpret_cast<ActorFn>(W(kSlotActor));
  void* sp = nullptr;
  uint64_t actor = 0;
  unsigned fl = 0;
  g_actor_ret = 0;
  CHECK(aw(nullptr, nullptr, dk::kDroneActor, &sp, &actor, &fl) == 0, "the Actor wrapper changed a 0 result");
  g_actor_ret = 7;
  CHECK(aw(nullptr, nullptr, dk::kDroneActor, &sp, &actor, &fl) == 7, "the Actor wrapper changed the result");
  CHECK(aw(nullptr, nullptr, 0x1234, &sp, &actor, &fl) == 1, "the Actor wrapper changed another actor's result");
  ResetForTest();
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
  Reset(Words{});
  ND d{0x1234, 0, dk::kDamageable, dk::kDamageable, 1};
  CallEnter(&d);
  CHECK(g_enters.size() == 1 && g_enters[0].data == &d && g_enters[0].node == g_node && SameNd(g_enters[0].copy, d), "args to the real Enter");
  CHECK(g_find_calls == 0 && g_comp_calls == 0, "work done for another actor");
  CHECK(!HasLine("combat_dronekill_component") && !HasLine("combat_dronekill_release") && !HasLine("combat_dronekill_note"), "a line for another actor");
  const Stats s = CurrentStats();
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
  Words w;
  w.explosion = false;  // this test's sound OFF Enters are the game's alone; the explosion's hold of that off is TestExplosion's
  Result r = Reset(w);
  CHECK(r.ok, "install");
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&d);
  CHECK(CurrentStats().enter_drone == 1 && CurrentStats().enter_other == 0, "drone Enter counters");
  CHECK(g_enters.size() == 1 && g_enters[0].data == &d && SameNd(g_enters[0].copy, d), "the real Enter got other arguments");
  CHECK(g_ev.size() >= 4 && g_ev[0] == "enter" && g_ev[1] == "actor" && g_ev[2] == "find" && g_ev[3] == "component", "order: the real Enter first, then the look-ups");
  CHECK(g_bad_actor == 0, "Component() got another actor");
  CHECK(CurrentStats().capturing == 0, "a drone Enter is still counted in flight after it returned");
  g_actor_ret = 7;
  ND seen = Drone(kSound, kSound, 0);
  CallEnter(&seen);
  CHECK(g_actor_seen == 7, "inside a drone Enter, Actor's result changed: %u", g_actor_seen);
  g_actor_ret = 1;
  CHECK(HasLine("combat_dronekill_release", 1) && HasLine("index=17") && HasLine("group=1"), "release line");
  ND s = Drone(kSound, kSound, 1);
  CallEnter(&s);
  CHECK(HasLine("combat_dronekill_component") && HasLine("to=group 1"), "the sound joins the group");
  ND off = Drone(kSound, kSound, 0);
  CallEnter(&off);
  CHECK(HasLine("enable=off") && HasLine("to=(off)"), "an OFF enter is only noted");
  CHECK(!HasLine("step=deployed"), "a non-touch OFF enter was taken for a touch release");

  // K: the group is shut down at the tick, the disables go through DisableComponent(handle, 1)
  OnKilled(0xabcd0011u);
  CHECK(HasLine("combat_dronekill_k ") && HasLine("result=shut_down") && HasLine("combat_dronekill_k index=17 tid="), "K line");
  CHECK(g_dis.empty(), "disabled in the K thread");
  Frame();
  CHECK(HasLine("combat_dronekill_tick_first", 1), "first tick line");
  CHECK(g_dis.size() == 2, "disables at the tick: %zu", g_dis.size());  // the damageable and the sound (the OFF enter did not join)
  CHECK(g_dis.size() >= 2 && g_dis[0].sys == g_dmg && g_dis[0].handle == kDmgHandle && g_dis[0].cat == 1 && !g_dis[0].enable, "first disable");
  CHECK(g_dis.size() >= 2 && g_dis[1].sys == g_snd && g_dis[1].handle == 0x00060006 && g_dis[1].cat == 1, "second disable");
  CHECK(HasLine("combat_dronekill_off") && HasLine("result=OFF"), "off lines");
  const size_t nd = g_dis.size();
  Frame();
  CHECK(g_dis.size() == nd, "disabled again");
  CHECK(CurrentStats().due_runs == 1 && CurrentStats().ticks == 2, "tick counters");
  Dump("drone Enter, K, tick");

  // R3: the script turns a component of the dead group on again: it is off again at the next tick. The verdict on the DisableComponent
  // body is cached per vtable: a body that changes after the first disable is not read again
  g_dis.clear();
  g_dis_words[dk::kDisWords - 1] = 0xd65f03c0;
  ND again = Drone(kSound, kSound, 1);
  g_ms += 5000;
  g_lines.clear();
  CallEnter(&again);
  CHECK(HasLine("combat_dronekill_reenabled") && HasLine("action=off_at_next_tick"), "R3 line");
  CHECK(!HasLine("combat_dronekill_component") && !HasLine("combat_dronekill_release"), "R3: the Enter was logged as an ordinary one too");
  Frame();
  CHECK(g_dis.size() == 1 && g_dis[0].sys == g_snd, "R3: not disabled again");
}

static void TestNestedEnter() {
  {  // a drone node whose actor symbol is readable and whose enable word is not: the game's Enter alone, nothing read past the readable part
    Reset(Words{});
    const size_t pg = 4096;
    uint8_t* m = static_cast<uint8_t*>(mmap(nullptr, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    mprotect(m + pg, pg, PROT_NONE);
    uint8_t* const tail = m + pg - 0x20;  // actor, space, system, component readable; enable (+0x20) in the PROT_NONE page
    const uint64_t drone = dk::kDroneActor;
    memcpy(tail, &drone, 8);
    g_enter_deref = false;
    g_ev.clear();
    g_find_calls = 0;
    CallEnter(reinterpret_cast<const ND*>(tail));
    CHECK(g_ev.size() == 1 && g_ev[0] == "enter-bad-pointer" && g_find_calls == 0, "a node cut by a page was read past its readable part");
    g_enter_deref = true;
    munmap(m, 2 * pg);
  }
  Reset(Words{});
  {  // a drone Enter inside a drone Enter: when the inner one returns, the outer one's capture is armed again for its own Actor call
    const uint32_t tracked = CurrentStats().enter_drone;
    ND inner = Drone(kSound, kSound, 0);
    g_nested_first = &inner;
    g_lines.clear();
    ND outer = Drone(dk::kR14Touch, dk::kR14Touch, 1);
    CallEnter(&outer);
    CHECK(CurrentStats().enter_drone == tracked + 2, "the nested Enters were not both drone Enters");
    CHECK(CountLine("no actor (Enter found none") == 0, "the outer Enter lost its capture to the inner one: %d notes", CountLine("no actor (Enter found none"));
  }
}

// The wrappers' state is per thread: what one thread is in the middle of is not seen by another thread's calls.
static void TestStateIsPerThread() {
  // the capture: another thread's Actor call for the drone, while this thread is inside a drone Enter, is not this Enter's
  Result r = Reset(Words{});
  CHECK(r.ok, "install");
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  g_enter_actor_on_other_thread = true;
  CallEnter(&d);
  CHECK(g_drone_actor_calls == 2, "Actor calls for the drone: %d, want the other thread's and this Enter's", g_drone_actor_calls);
  CHECK(g_comp_calls > 0 && g_bad_actor == 0, "the Enter took the other thread's actor (%d Component calls on another actor)", g_bad_actor);
  CHECK(HasLine("combat_dronekill_release") && HasLine("group=1"), "release line");

  // the shot: another thread outside any shot sees none, a shot of its own sees its own values, and this thread's are back after it
  r = Reset(Only(false, false, false, false, true));
  CHECK(r.ok, "install");
  g_other_thread_saw_no_shot = false;
  CallFire(reinterpret_cast<void*>(0x8001), 2, DkC3{1, 2, 3}, 1);
  CHECK(g_other_thread_saw_no_shot, "another thread saw this thread's shot");
  CHECK(g_fires.size() == 2 && g_fires[1].cs == reinterpret_cast<void*>(0x8002), "the other thread's shot did not reach the real call");
  CHECK(g_fires.size() == 2 && g_fires[1].acc_non_null && g_fires[1].acc[0] == 4 && g_fires[1].acc[1] == 5 && g_fires[1].acc[2] == 6 && g_fires[1].acc_remote == 0,
        "the other thread's shot did not see its own values");
  CHECK(g_fires.size() == 2 && g_fires[0].after[0] == 1 && g_fires[0].after[1] == 2 && g_fires[0].after[2] == 3 && g_fires[0].after_remote == 1,
        "this thread's shot was changed by the other thread's");
  CHECK(ShotVelocity(nullptr) == nullptr, "accessor non-null after the shots");
  // the test seam is per thread as well: it answers on the thread that set it
  const float v[3] = {7, 8, 9};
  SetShotForTest(v, 1);
  int rem = -1;
  const float* a = ShotVelocity(&rem);
  CHECK(a != nullptr && a[0] == 7 && a[2] == 9 && rem == 1, "the seam does not answer on its own thread");
  bool other_none = false;
  std::thread([&] { other_none = ShotVelocity(nullptr) == nullptr; }).join();
  CHECK(other_none, "the seam answered on another thread");
  SetShotForTest(nullptr, 0);
  CHECK(ShotVelocity(nullptr) == nullptr, "the seam did not clear");

  // the tick guard: a tick on another thread, while this thread is inside its tick, runs (the guard stops only this thread's re-entry)
  r = Reset(Words{});
  CHECK(r.ok, "install");
  ND s = Drone(kSound, kSound, 1);
  CallEnter(&d);
  CallEnter(&s);
  OnKilled(kDmgHandle & 0xffff);
  g_dis_hook = [] { std::thread([] { Frame(); }).join(); };
  Frame();
  CHECK(CurrentStats().due_runs == 2, "the tick on another thread did not run (%u runs)", CurrentStats().due_runs);
}

static void TestKillRules() {
  // a K for an index no live group has
  Reset(Words{});
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&d);
  OnKilled(0x99);
  CHECK(HasLine("result=no_live_group") && HasLine("live_groups=1"), "K without a group");
  Frame();
  CHECK(g_dis.empty(), "disabled for a K that matched no group");
  // 16 bits of the index are compared
  OnKilled(0xabcd0000u | (kDmgHandle & 0xffff));
  CHECK(HasLine("result=shut_down"), "the high 16 bits must not matter");

  // no kill word: the K is only recorded
  Reset(Only(false, true, true, true, true));
  CallEnter(&d);
  OnKilled(kDmgHandle & 0xffff);
  CHECK(HasLine("result=would_shut_down"), "no-kill K line");
  Frame();
  CHECK(g_dis.empty(), "disabled without the kill word");

  // the invoke-immediate word is 0: Enter would queue here, we never do: nothing is disabled, it waits; due bits expire after 2 s
  Reset(Words{});
  CallEnter(&d);
  ND s = Drone(kSound, kSound, 1);
  CallEnter(&s);
  OnKilled(kDmgHandle & 0xffff);
  SetFlag(0);
  Frame();
  CHECK(g_dis.empty(), "disabled outside the window");
  CHECK(HasLine("waiting_invoke_immediate_0=2", 2), "waiting not reported");
  SetFlag(1);
  g_ms += 1000;
  Frame();
  CHECK(g_dis.size() == 2, "disables after the window opened: %zu", g_dis.size());
  Reset(Words{});
  CallEnter(&d);
  OnKilled(kDmgHandle & 0xffff);
  SetFlag(0);
  Frame();
  SetFlag(1);
  g_ms += 2001;
  Frame();
  CHECK(g_dis.empty() && HasLine("expired=1"), "an expired due bit still acted");

  // a tick that is re-entered from inside an engine call does nothing the second time
  Reset(Words{});
  CallEnter(&d);
  CallEnter(&s);
  OnKilled(kDmgHandle & 0xffff);
  g_dis_hook = [] { Frame(); };
  Frame();
  CHECK(g_dis.size() == 2, "a re-entered tick disabled again: %zu calls", g_dis.size());
  CHECK(CurrentStats().due_runs == 1, "re-entered tick ran");

  // no kill word: a component of a dead group that is enabled again is only noted, nothing is made due
  Reset(Only(false, true, true, true, true));
  CallEnter(&d);
  CallEnter(&s);
  OnKilled(kDmgHandle & 0xffff);
  CallEnter(&s);
  CHECK(HasLine("action=none_no_kill_word"), "R3 without the kill word");
  Frame();
  CHECK(CurrentStats().due_runs == 0 && g_dis.empty(), "the tick ran for a group that nothing marked due");

  // the system's game space was unreadable at the Enter: nothing to compare later
  Reset(Words{});
  W64(g_dmg, dk::kSysOwner, 0);
  CallEnter(&d);
  OnKilled(kDmgHandle & 0xffff);
  Frame();
  CHECK(g_dis.empty() && HasLine("no game space recorded"), "disabled with no recorded game space");

  // the system's vtable changed to one whose Component() is not the read shape
  Reset(Words{});
  CallEnter(&d);
  OnKilled(kDmgHandle & 0xffff);
  memcpy(g_vtable2, g_vtable, sizeof g_vtable);
  memcpy(g_thunk_bad, g_thunk_words, sizeof g_thunk_bad);
  g_thunk_bad[2] ^= 0x400;
  g_vtable2[dk::kVtComponent / 8] = reinterpret_cast<uintptr_t>(g_thunk_bad);
  W64(g_dmg, 0, reinterpret_cast<uintptr_t>(g_vtable2));
  Frame();
  CHECK(g_dis.empty() && HasLine("Component() not the read shape"), "disabled through an unknown Component()");
  g_comp_calls = 0;  // the verdict is cached per vtable: a second drone Enter on that system is still not looked up
  g_lines.clear();
  CallEnter(&d);
  CHECK(g_comp_calls == 0 && HasLine("not looked up"), "a vtable refused once was looked up the second time");

  // the system moved to another game space since the Enter: skipped for good
  Reset(Words{});
  CallEnter(&d);
  OnKilled(kDmgHandle & 0xffff);
  W64(g_dmg, dk::kSysOwner, reinterpret_cast<uintptr_t>(g_gs2));
  Frame();
  CHECK(g_dis.empty() && HasLine("system's game space changed"), "disabled in another game space");
  // the system is not found again
  Reset(Words{});
  CallEnter(&d);
  OnKilled(kDmgHandle & 0xffff);
  g_syss[0].sym = 0;
  Frame();
  CHECK(g_dis.empty() && HasLine("system not found again in its game space"), "disabled although the system is gone");
  // the handle changed
  Reset(Words{});
  CallEnter(&d);
  OnKilled(kDmgHandle & 0xffff);
  g_comps[0].handle += 0x10000;
  Frame();
  CHECK(g_dis.empty() && HasLine("handle changed"), "disabled although the handle changed");
  // DisableComponent is not the read shape
  Reset(Words{});
  CallEnter(&d);
  OnKilled(kDmgHandle & 0xffff);
  g_dis_words[dk::kDisWords - 1] = 0xd65f03c0;
  Frame();
  CHECK(g_dis.empty() && HasLine("DisableComponent not the read shape"), "disabled through an unknown body");
}

static void TestEnterLookups() {
  // Actor not called by the real Enter / returns 0 / another actor's call first
  Reset(Words{});
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
  CHECK(HasLine("combat_dronekill_release") && g_bad_actor == 0, "another actor's call was taken for Enter's own");
  g_other_actor_first = 0;

  // no such system / no component / wrong shapes
  Reset(Words{});
  g_syss[0].sym = 0;
  CallEnter(&d);
  CHECK(HasLine("no such system in the actor's game space"), "no system");
  Reset(Words{});
  g_ncomps = 0;
  CallEnter(&d);
  CHECK(HasLine("no component under that name") && !HasLine("combat_dronekill_release"), "no component");
  Reset(Words{});
  g_thunk_words[2] ^= 0x400;
  CallEnter(&d);
  CHECK(HasLine("not looked up") && g_comp_calls == 0, "Component thunk shape");
  Reset(Words{});
  g_body_words[10] = 0xf9000008;
  CallEnter(&d);
  CHECK(HasLine("not looked up") && g_comp_calls == 0, "Component body shape");
  Reset(Words{});
  g_body_words[dk::kBodyBl] += 1;  // a bl that does not reach the comparator's PLT stub
  CallEnter(&d);
  CHECK(HasLine("not looked up") && g_comp_calls == 0, "Component body: bl target");
  Reset(Words{});
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
  Reset(Words{});
  g_actor_bad_out = true;
  CallEnter(&d);
  CHECK(HasLine("no actor (Enter found none") && g_find_calls == 0, "unreadable Actor outputs were used");
  // Actor called twice for the drone: the first call's capture stands
  Reset(Words{});
  g_actor_twice = true;
  CallEnter(&d);
  CHECK(HasLine("combat_dronekill_release") && g_bad_actor == 0, "the second Actor call replaced the capture (bad actor calls %d)", g_bad_actor);
  // a component handle whose index is 0xffff is none, whatever its generation
  Reset(Words{});
  g_comps[0].handle = 0x0007ffff;
  CallEnter(&d);
  CHECK(HasLine("no component under that name") && !HasLine("combat_dronekill_release"), "an 0xffff index with a generation taken for a component");
  // an OFF damageable is an ordinary component line, not a release; a first component with no group waits as pending
  Reset(Words{});
  ND dmg_off = Drone(dk::kDamageable, dk::kDamageable, 0);
  CallEnter(&dmg_off);
  CHECK(HasLine("combat_dronekill_component") && HasLine("enable=off") && !HasLine("combat_dronekill_release"), "a damageable OFF logged as a release");
  Reset(Words{});
  ND snd_on = Drone(kSound, kSound, 1);
  CallEnter(&snd_on);
  CHECK(HasLine("to=pending"), "a component with no group is pending");
  // the shape verdict is cached per vtable: the second Enter reads no words again (a corrupted body after the first is not seen)
  Reset(Words{});
  CallEnter(&d);
  g_body_words[10] = 0xf9000008;
  g_comp_calls = 0;
  ND s = Drone(kSound, kSound, 1);
  CallEnter(&s);
  CHECK(g_comp_calls == 1, "the shape check was not cached: %d component calls", g_comp_calls);

  // the log of one kind stops at its cap, once, loudly
  Reset(Words{});
  g_enter_calls_actor = false;
  for (int i = 0; i < 30; ++i) CallEnter(&d);
  CHECK(CountLine("combat_dronekill_note") == 24, "note cap: %d", CountLine("combat_dronekill_note"));
  CHECK(CountLine("combat_dronekill_cap") == 1, "cap line %d", CountLine("combat_dronekill_cap"));

  // every capped kind stops at its own number
  Reset(Words{});
  CallEnter(&d);
  for (int i = 0; i < 200; ++i) CallEnter(&snd_on);
  CHECK(CountLine("combat_dronekill_release") + CountLine("combat_dronekill_component") == 160, "enter lines: %d", CountLine("combat_dronekill_release") + CountLine("combat_dronekill_component"));
  CHECK(CountLine("kind=enter") == 1, "enter cap line");
  Reset(Words{});
  CallEnter(&d);
  CallEnter(&snd_on);
  OnKilled(kDmgHandle & 0xffff);
  for (int i = 0; i < 80; ++i) CallEnter(&snd_on);
  CHECK(CountLine("combat_dronekill_reenabled") == 64 && CountLine("kind=off") == 1, "off lines: %d", CountLine("combat_dronekill_reenabled"));
  Reset(Words{});
  CallEnter(&d);
  for (int i = 0; i < 60; ++i) OnKilled(0x99);
  CHECK(CountLine("combat_dronekill_k ") == 48 && CountLine("kind=k") == 1, "K lines: %d", CountLine("combat_dronekill_k "));
  Reset(Words{});
  ND gun_on = Drone(dk::kNcaModel, dk::kGun, 1);
  for (int i = 0; i < 100; ++i) CallEnter(&gun_on);
  CHECK(CountLine("step=mirrored") == 96 && CountLine("kind=f357") == 1, "gun lines: %d", CountLine("step=mirrored"));

  // the K feed and the tick before an install are inert
  ResetForTest();
  g_lines.clear();
  OnKilled(1);
  Frame();
  CHECK(g_lines.empty() && CurrentStats().ticks == 0, "inert before install");
}

static void TestGun() {
  {  // a port Enter for a drone whose gun state is not known yet: only the script's own Enter, the state as the script gave it
    Reset(Words{});
    g_enters.clear();
    ND port = Drone(dk::kNcaModel, dk::kPort, 1);
    CallEnter(&port);
    CHECK(g_enters.size() == 1 && g_enters[0].data == &port, "a port with no known gun state got %zu real Enters", g_enters.size());
  }
  Reset(Words{});
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
  CHECK(HasLine("step=mirrored") && HasLine("gun=shown"), "gun line");
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
  CHECK(g_enters.size() == 2 && g_enters[1].copy.component == dk::kSatellite && HasLine("step=missing"), "no port: %zu enters", g_enters.size());
  for (int i = 0; i < g_ncomps; ++i)
    if (g_comps[i].sym == 0) g_comps[i].sym = dk::kPort;

  {  // D4: the gun state is kept by the node's actor symbol, not the captured SActor: two drones share one gun state,
     // and a port Enter after the second drone's hidden gun follows it
    Reset(Words{});
    g_actor_val = 0xa1;
    ND gunA = Drone(dk::kNcaModel, dk::kGun, 1);
    CallEnter(&gunA);
    g_actor_val = 0xb2;
    ND gunB = Drone(dk::kNcaModel, dk::kGun, 0);
    CallEnter(&gunB);
    g_enters.clear();
    g_actor_val = 0xa1;
    ND portA = Drone(dk::kNcaModel, dk::kPort, 1);
    CallEnter(&portA);
    CHECK(g_enters.size() == 2 && g_enters[1].copy.enable == 0, "D4: one gun state for every drone: the first drone's port follows the second's hidden gun (%zu enters)", g_enters.size());
    g_actor_val = 0;
  }
  // the port follows the gun: a port Enter that differs from the gun's state gets one more Enter with the gun's state
  g_enters.clear();
  ND gun2 = Drone(dk::kNcaModel, dk::kGun, 1);
  CallEnter(&gun2);
  g_enters.clear();
  ND port0 = Drone(dk::kNcaModel, dk::kPort, 0);
  CallEnter(&port0);
  CHECK(g_enters.size() == 2 && g_enters[1].copy.component == dk::kPort && g_enters[1].copy.enable == 1 && g_enters[1].data != &port0, "port follows the gun");
  CHECK(HasLine("step=port_follows"), "port line");
  g_enters.clear();
  ND port1 = Drone(dk::kNcaModel, dk::kPort, 1);
  CallEnter(&port1);
  CHECK(g_enters.size() == 1, "port equal to the gun: %zu enters", g_enters.size());
  // unknown gun state (another game's actor... a fresh table): nothing extra
  Reset(Words{});
  g_enters.clear();
  CallEnter(&port0);
  CHECK(g_enters.size() == 1, "port with no known gun state: %zu enters", g_enters.size());
  // the gun's symbols in another system are not the gun
  Reset(Words{});
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
  Reset(Words{});
  CallEnter(&drone_dmg);
  CallEnter(&gun2);
  OnKilled(kDmgHandle & 0xffff);
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
  Reset(Words{});
  CallEnter(&gun2);
  g_space_now = g_spacemem2;
  CallEnter(&drone_dmg);
  g_space_now = g_spacemem;
  g_enters.clear();
  CallEnter(&port0);
  CHECK(g_enters.size() == 1, "the gun state survived a level change: %zu enters", g_enters.size());
}

static void TestLockOn() {
  Result r = Reset(Words{});
  CHECK(r.ok, "install");
  ND dmg = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&dmg);  // the drone's live group (its actor)
  ND t0 = Drone(dk::kR14Touch, dk::kR14Touch, 0);
  CallEnter(&t0);  // the script made it ungrabbable
  CHECK(HasLine("step=deployed"), "no deployed line");
  g_dis.clear();
  Frame();
  CHECK(g_dis.empty(), "touch on again before the script's disable was applied (mask bit clear)");
  g_masks[0x5] |= 1;  // the script's disable is applied
  Frame();
  CHECK(g_dis.size() == 1 && g_dis[0].enable && g_dis[0].sys == g_touch && g_dis[0].handle == 0x00010005 && g_dis[0].cat == 1, "on again: %zu calls", g_dis.size());
  CHECK(HasLine("step=on_again") && HasLine("result=on tid="), "on-again line");
  // EnableComponent's body is read once per vtable: a body that changes later is not read again
  g_en_words[dk::kEnWords - 1] = 0xd61f0040;
  g_dis.clear();
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  Frame();
  CHECK(g_dis.size() == 1 && g_dis[0].enable, "the EnableComponent verdict was not cached: %zu calls", g_dis.size());
  g_en_words[dk::kEnWords - 1] = 0xd65f03c0;
  // the drone's first shot: the touch goes off at the next tick, only while the drone is live
  g_dis.clear();
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{0, 0, 1}, 0);
  CHECK(g_fires.size() == 1, "fire not passed on");
  Frame();
  CHECK(g_dis.size() == 1 && !g_dis[0].enable && g_dis[0].sys == g_touch && g_dis[0].cat == 1, "locked on: %zu calls", g_dis.size());
  CHECK(HasLine("step=locked_on") && HasLine("result=off"), "locked-on line");
  // a second shot of the same drone does nothing more
  g_dis.clear();
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{0, 0, 1}, 0);
  Frame();
  CHECK(g_dis.empty(), "second shot disabled again");

  // the invoke-immediate word is 0: it waits (the script's disable is applied but we never queue)
  Reset(Words{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  SetFlag(0);
  Frame();
  CHECK(g_dis.empty(), "enabled outside the window");
  SetFlag(1);
  g_ms += 500;
  Frame();
  CHECK(g_dis.size() == 1 && g_dis[0].enable, "on again after the window opened");

  // a dead drone: the touch is left alone
  Reset(Words{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  Frame();
  g_dis.clear();
  OnKilled(kDmgHandle & 0xffff);
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{0, 0, 1}, 0);
  g_dis.clear();
  Frame();
  size_t offs = 0;
  for (const auto& c : g_dis)
    if (c.sys == g_touch && !c.enable) ++offs;
  CHECK(offs == 0 && HasLine("belongs to no live drone"), "a dead drone's touch was disabled: %zu", offs);

  // EnableComponent is not the read shape: skipped for good
  Reset(Words{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  g_en_words[dk::kEnWords - 1] = 0xd61f0040;  // a tail call (br x2) for the ret
  Frame();
  CHECK(g_dis.empty() && HasLine("EnableComponent not the read shape"), "enabled through an unknown body");

  // a script's ENABLE of the touch is not a release
  Reset(Words{});
  CallEnter(&dmg);
  {
    ND t1 = Drone(dk::kR14Touch, dk::kR14Touch, 1);
    CallEnter(&t1);
    CHECK(!HasLine("step=deployed"), "an ON touch Enter was taken for a release");
  }
  // the touch's system moved to another game space: skipped for good
  Reset(Words{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  W64(g_touch, dk::kSysOwner, reinterpret_cast<uintptr_t>(g_gs2));
  Frame();
  CHECK(g_dis.empty() && HasLine("system's game space changed"), "the touch was enabled in another game space");
  // its disable mask cannot be read
  Reset(Words{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  W64(g_touch, dk::MaskOffsets(dk::kEn).idx, 0);
  Frame();
  CHECK(g_dis.empty() && HasLine("disable mask unreadable"), "the touch was enabled with an unreadable mask");
  // skipped for good is DONE, not HELD: a later shot binds nothing
  Reset(Words{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  g_en_words[dk::kEnWords - 1] = 0xd61f0040;
  Frame();
  g_en_words[dk::kEnWords - 1] = 0xd65f03c0;
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{0, 0, 1}, 0);
  g_dis.clear();
  Frame();
  CHECK(g_dis.empty() && !HasLine("step=locked_on"), "a record that was skipped for good locked on");
  // a drone Enter in another game space is a level change: the touch records go
  Reset(Words{});
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  g_space_now = g_spacemem2;
  CallEnter(&dmg);
  g_space_now = g_spacemem;
  Frame();
  CHECK(g_dis.empty(), "a touch record survived a level change");
  // ... but a second drone Enter in the same game space is not
  Reset(Words{});
  CallEnter(&dmg);
  CallEnter(&t0);
  ND snd = Drone(kSound, kSound, 1);
  CallEnter(&snd);
  g_masks[0x5] |= 1;
  Frame();
  CHECK(g_dis.size() == 1 && g_dis[0].enable, "a touch record was cleared by a drone Enter in the same game space");
  // the touch belongs to a live drone through its group's components, not only its actor
  Reset(Words{});
  g_actor_val = 0xaaa1;
  CallEnter(&dmg);
  ND t_on = Drone(dk::kR14Touch, dk::kR14Touch, 1);
  CallEnter(&t_on);  // joins the drone's group
  g_actor_val = 0xbbb2;
  CallEnter(&t0);  // the release arrives under another actor
  g_masks[0x5] |= 1;
  Frame();
  g_dis.clear();
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{0, 0, 1}, 0);
  Frame();
  CHECK(g_dis.size() == 1 && !g_dis[0].enable && g_dis[0].sys == g_touch, "a component of a live group was not live (%zu calls)", g_dis.size());

  // no lockon word: nothing is tracked, the touch is not re-enabled
  Reset(Only(true, false, true, true, true));
  CallEnter(&dmg);
  CallEnter(&t0);
  g_masks[0x5] |= 1;
  Frame();
  CHECK(g_dis.empty() && !HasLine("step=deployed"), "touch handled without the lockon word");
}

static void TestFire() {
  Reset(Only(false, false, false, false, true));
  CHECK(ShotVelocity(nullptr) == nullptr, "accessor non-null outside a shot");
  {
    int untouched = 77;  // outside a shot the accessor answers null and leaves *remote alone
    CHECK(ShotVelocity(&untouched) == nullptr && untouched == 77, "accessor outside a shot wrote *remote (%d)", untouched);
  }
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{1, 2, 3}, 1);
  CHECK(g_fires.size() == 1, "fires");
  const FireCall& c = g_fires[0];
  CHECK(c.cs == reinterpret_cast<void*>(0x6001) && c.handle == 0x00020003 && c.v.x == 1 && c.v.y == 2 && c.v.z == 3 && c.remote == 1, "args to the real FireBulletLocal");
  CHECK(c.acc_non_null && c.acc[0] == 1 && c.acc[1] == 2 && c.acc[2] == 3 && c.acc_remote == 1, "accessor inside the real call");
  CHECK(ShotVelocity(nullptr) == nullptr, "accessor non-null after the shot");
  CallFire(reinterpret_cast<void*>(0x6001), 1, DkC3{4, 5, 6}, 0);
  CHECK(g_fires[1].acc_remote == 0 && g_fires[1].acc[0] == 4, "second shot / local");
  CHECK(CurrentStats().fires == 2, "fire counter");
  // a shot inside a shot: the outer shot's values are back after the inner returns
  g_fires.clear();
  CallFire(reinterpret_cast<void*>(0x7001), 8, DkC3{7, 8, 9}, 0);
  // fires[0] is the outer (pushed before its nested call); fires[1] the inner
  CHECK(g_fires.size() == 2 && g_fires[0].cs == reinterpret_cast<void*>(0x7001) && g_fires[1].cs == reinterpret_cast<void*>(0x7002), "nested order");
  CHECK(g_fires.size() == 2 && g_fires[1].acc[0] == 1 && g_fires[1].acc[1] == 2 && g_fires[1].acc[2] == 3 && g_fires[1].acc_remote == 1, "inner shot's values");
  CHECK(g_fires.size() == 2 && g_fires[0].after[0] == 7 && g_fires[0].after[1] == 8 && g_fires[0].after[2] == 9, "the outer values are not back after the inner shot");
  CHECK(g_fires.size() == 2 && g_fires[0].after_remote == 0, "the outer shot's remote flag is not back after the inner shot (%d)", g_fires[0].after_remote);
  CHECK(ShotVelocity(nullptr) == nullptr, "accessor non-null after the nested shots");
  // nothing is tracked for lock-on without its word: no table work
  CHECK(!HasLine("touch"), "touch lines without lockon");
}

static void TestChase() {
  Result r = Reset(Words{});
  CHECK(r.ok, "install");
  ND dmg = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&dmg);  // a drone Enter: records the game space seen with this physics system's owner
  CallAttack(g_cs, kAtHandle);
  CHECK(g_attacks.size() == 1 && g_attacks[0].cs == g_cs && g_attacks[0].handle == kAtHandle, "args to the real AttackTarget");
  float want[3], dist = 0;
  const float q[3] = {10, 0, 0};
  CHECK(dk::ChaseVelocity(g_me_pos, q, want, &dist), "expected velocity");
  CHECK(HasLine("combat_dronekill_chase ") && HasLine("locally_controlled=1"), "chase line");
  CHECK(CurrentStats().attacks == 1, "attack counter");
  CHECK(g_primary_bodies.size() == 1 && g_primary_bodies[0] == 9, "the target's body handle is the damageable record's +0xc");
  CHECK(g_l2w.size() == 1 && g_l2w[0].transforms == g_xf_me && g_l2w[0].handle == 0x77, "the drone's transform handle is the record's +0x7c");
  CHECK(g_local_sys == g_np && g_local_handle == 0x0001000a, "IsLocallyControlled got the net physics component's handle");
  CHECK(g_setvels.empty(), "velocity set in the attack call (only at the tick)");
  Frame();
  CHECK(g_setvels.size() == 1, "set_vel calls: %zu", g_setvels.size());
  if (g_setvels.size() == 1) {
    CHECK(g_setvels[0].physics == g_phys && g_setvels[0].handle == kPhysH, "set_vel target");
    CHECK(std::fabs(g_setvels[0].v.x - want[0]) < 1e-5f && std::fabs(g_setvels[0].v.y - want[1]) < 1e-5f && std::fabs(g_setvels[0].v.z - want[2]) < 1e-5f,
          "velocity (%f %f %f) want (%f %f %f)", g_setvels[0].v.x, g_setvels[0].v.y, g_setvels[0].v.z, want[0], want[1], want[2]);
  }
  Frame();
  CHECK(g_setvels.size() == 1, "one write per AttackTarget");
  Dump("chase");

  // the body position is the one read; the transform is the fallback
  Reset(Words{});
  CallEnter(&dmg);
  g_target_pos[0] = 0;
  g_target_pos[1] = 0;
  g_target_pos[2] = 30;  // the l2w answer for the target; the body at +0x8f8 says (10,0,0)
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.size() == 1 && std::fabs(g_setvels[0].v.x - want[0]) < 1e-5f, "the body's position was not preferred");
  Reset(Words{});
  CallEnter(&dmg);
  g_have_body = false;
  float p2[3] = {0, 0, 30}, want2[3];
  g_target_pos[0] = 0;
  g_target_pos[1] = 0;
  g_target_pos[2] = 30;
  CHECK(dk::ChaseVelocity(g_me_pos, p2, want2), "expected 2");
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.size() == 1 && std::fabs(g_setvels[0].v.z - want2[2]) < 1e-5f, "the transform fallback");
  CHECK(g_l2w.size() == 2 && g_l2w[1].transforms == g_xf_target && g_l2w[1].handle == 0x55, "the fallback's transform handle is the damageable record's +8");

  // the fallback's transform system pointer is null: nothing is computed, no call with a null system
  Reset(Words{});
  CallEnter(&dmg);
  g_have_body = false;
  W64(g_ts, dk::kDmXf, 0);
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.empty() && g_l2w.size() == 1, "a null transform system was used (%zu l2w calls)", g_l2w.size());
  // the drone's own transform system pointer is null
  Reset(Words{});
  CallEnter(&dmg);
  W64(g_cs, dk::kAtTransforms, 0);
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.empty() && g_l2w.empty(), "a null transform system was passed to the game (%zu l2w calls)", g_l2w.size());
  // the auto-target system's own game space is unreadable
  Reset(Words{});
  CallEnter(&dmg);
  W64(g_cs, dk::kSysOwner, 0);
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.empty(), "written with no game space for the auto-target system");
  // the real AttackTarget acquires the target: the record is read after it, not before
  Reset(Words{});
  CallEnter(&dmg);
  (g_recsA + 2 * dk::kAtStride)[dk::kAtHasTarget] = 0;
  g_attack_sets_target = true;
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.size() == 1, "the target AttackTarget just acquired was not chased (%zu)", g_setvels.size());
  // on the target: the two positions coincide, nothing is written
  Reset(Words{});
  CallEnter(&dmg);
  {
    const float on[3] = {0, -dk::kChaseAbove, 0};
    memcpy(g_body + dk::kBodyPos, on, sizeof on);
  }
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.empty(), "a zero velocity was written for a drone on its target");

  // the physics system's game space is not the one recorded for any drone component: not local
  Reset(Words{});
  CallEnter(&dmg);
  W64(g_phys, dk::kSysOwner, reinterpret_cast<uintptr_t>(g_gs2));
  {
    const uint32_t one = 1;
    memcpy(g_gs2 + dk::kOwnerFlag, &one, 4);
  }
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.empty(), "written for a physics system whose game space no drone component was seen in");
  // the net physics component's physics system is another one
  Reset(Words{});
  CallEnter(&dmg);
  W64(g_np, dk::kNetPhysicsPhys, 0x1234);
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.empty(), "written although the net physics component belongs to another physics system");
  // the physics record has no owner actor
  Reset(Words{});
  CallEnter(&dmg);
  W64(g_owners, 2 * 16, 0);
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.empty(), "written for a physics record with no owner");
  // another peer controls the drone: nothing is written (fails closed)
  Reset(Words{});
  CallEnter(&dmg);
  g_local_ret = 0;
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.empty() && HasLine("locally_controlled=0"), "set although not locally controlled");
  // the game space of the physics system was never seen: not local
  Reset(Words{});
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.empty() && g_find_calls == 0, "set although no game space was recorded for the owner (%d finds)", g_find_calls);
  // the drone has no R14NetPhysics component: upstream-exact, treated as local
  Reset(Words{});
  CallEnter(&dmg);
  for (int i = 0; i < g_ncomps; ++i)
    if (g_comps[i].sym == dk::kNetPhysics) g_comps[i].sym = 0;
  g_local_ret = 0;
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.size() == 1, "no net physics: %zu writes", g_setvels.size());

  // outside the invoke-immediate window: nothing is written, and it is not retried (one write per AttackTarget)
  Reset(Words{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  SetFlag(0);
  Frame();
  SetFlag(1);
  Frame();
  CHECK(g_setvels.empty(), "written outside the window");
  // older than kChaseMs at the tick
  Reset(Words{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_ms += dk::kChaseMs;
  Frame();
  CHECK(g_setvels.empty(), "a stale velocity was written");
  Reset(Words{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_ms += dk::kChaseMs - 1;
  Frame();
  CHECK(g_setvels.size() == 1, "a fresh velocity was dropped");
  // the physics handle's generation changed between the attack and the tick: not the same drone
  Reset(Words{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_idxP[(kPhysH & 0xffff) * 2 + 1] = 9;
  Frame();
  CHECK(g_setvels.empty(), "written to a recycled physics handle");
  Reset(Words{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_idxA[(kAtHandle & 0xffff) * 2 + 1] = 9;  // the auto-target handle recycled
  Frame();
  CHECK(g_setvels.empty(), "written for a recycled auto-target handle");
  Reset(Words{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  W64(g_phys, dk::kSysOwner, reinterpret_cast<uintptr_t>(g_gs2));  // the physics system's game space changed
  {
    const uint32_t one = 1;
    memcpy(g_gs2 + dk::kOwnerFlag, &one, 4);  // and that one is inside its window
  }
  Frame();
  CHECK(g_setvels.empty(), "written after the physics system's owner changed");
  // the auto-target system's game space changed
  Reset(Words{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  W64(g_cs, dk::kSysOwner, reinterpret_cast<uintptr_t>(g_gs));
  Frame();
  CHECK(g_setvels.empty(), "written after the auto-target system's owner changed");
  // the record's physics handle changed
  Reset(Words{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  {
    const uint32_t other = 0x00010009;
    memcpy(g_recsA + 2 * dk::kAtStride + dk::kAtPhysIdx, &other, 4);
  }
  Frame();
  CHECK(g_setvels.empty(), "written after the record's physics handle changed");
  // the auto-target system's physics pointer changed
  Reset(Words{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  W64(g_cs, dk::kAtPhysics, reinterpret_cast<uintptr_t>(g_gs));
  Frame();
  CHECK(g_setvels.empty(), "written after the physics pointer changed");
  // a stale velocity also stops the drone being "moving": a later lost target writes no stop
  Reset(Words{});
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_ms += dk::kChaseMs;
  Frame();
  (g_recsA + 2 * dk::kAtStride)[dk::kAtHasTarget] = 0;
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.empty(), "a stop was written for a drone whose velocity had gone stale");
  // one write per AttackTarget even while the tick keeps running for something else (a touch waiting for its disable)
  Reset(Words{});
  CallEnter(&dmg);
  ND touch_off = Drone(dk::kR14Touch, dk::kR14Touch, 0);
  CallEnter(&touch_off);
  CallAttack(g_cs, kAtHandle);
  Frame();
  Frame();
  Frame();
  CHECK(g_setvels.size() == 1, "a velocity was written again by a later tick: %zu", g_setvels.size());
  // a level change drops the chase records, with the chase word alone
  Reset(Only(false, false, false, true, false));
  CallEnter(&dmg);
  CallAttack(g_cs, kAtHandle);
  g_space_now = g_spacemem2;
  CallEnter(&dmg);
  g_space_now = g_spacemem;
  Frame();
  CHECK(g_setvels.empty(), "a chase record survived a level change");
  // the chase log line stops at 8, once, loudly
  Reset(Words{});
  CallEnter(&dmg);
  for (int i = 0; i < 12; ++i) {
    CallAttack(g_cs, kAtHandle);
    Frame();
  }
  CHECK(CountLine("combat_dronekill_chase ") == 8 && CountLine("kind=chase") == 1, "chase lines: %d", CountLine("combat_dronekill_chase "));

  // the target is lost: a resting drone writes nothing; a moving one is stopped with a zero velocity
  Reset(Words{});
  CallEnter(&dmg);
  (g_recsA + 2 * dk::kAtStride)[dk::kAtHasTarget] = 0;
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.empty(), "a resting drone with no target was written");
  (g_recsA + 2 * dk::kAtStride)[dk::kAtHasTarget] = 1;
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.size() == 1, "moving");
  (g_recsA + 2 * dk::kAtStride)[dk::kAtHasTarget] = 0;
  CallAttack(g_cs, kAtHandle);
  Frame();
  CHECK(g_setvels.size() == 2 && g_setvels[1].v.x == 0 && g_setvels[1].v.y == 0 && g_setvels[1].v.z == 0, "no stop after the target was lost: %zu", g_setvels.size());
  CHECK(CountLine("combat_dronekill_chase ") == 1, "the stop was logged as a chase (%d lines)", CountLine("combat_dronekill_chase "));

  // no chase word: AttackTarget is not wrapped, nothing is set
  Reset(Only(true, true, true, false, true));
  CHECK(W(kSlotAttack) == reinterpret_cast<uintptr_t>(&FakeAttack), "attack wrapped without chase");
}

// ================================================================ arena (same APK): out of a combat map every hook only calls the game's function
static void ExpectInert(const char* where) {
  g_lines.clear();
  g_ev.clear();
  g_enters.clear();
  g_fires.clear();
  g_attacks.clear();
  g_dis.clear();
  g_setvels.clear();
  g_find_calls = g_comp_calls = 0;
  const Stats before = CurrentStats();
  // what follows is the arena path: no read of game memory and no scan of the levels on any hook (the scope was refreshed before this), counted
  StartReadCount();
  StartScanCount();
  g_scopeClockReads = 0;  // and no read of the scope's clock: the freshness test is in the in-scope bodies only
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&d);
  ND touch = Drone(dk::kR14Touch, dk::kR14Touch, 0);
  CallEnter(&touch);
  CHECK(g_enters.size() == 2 && g_enters[0].data == &d && SameNd(g_enters[0].copy, d), "%s: the game's Enter was not called as the caller called it", where);
  CHECK(g_find_calls == 0 && g_comp_calls == 0, "%s: a drone Enter was looked up (%d finds, %d components)", where, g_find_calls, g_comp_calls);
  void* sp = nullptr;
  uint64_t actor = 0;
  unsigned fl = 0;
  g_actor_ret = 7;
  CHECK(reinterpret_cast<ActorFn>(W(kSlotActor))(nullptr, nullptr, dk::kDroneActor, &sp, &actor, &fl) == 7, "%s: Actor's result changed", where);
  CallFire(reinterpret_cast<void*>(0x6001), 1, DkC3{1, 2, 3}, 0);
  CHECK(g_fires.size() == 1 && !g_fires[0].acc_non_null, "%s: a shot frame was made (dronemuzzle would act)", where);
  g_attack_sets_target = true;
  CallAttack(g_cs, kAtHandle);
  CHECK(g_attacks.size() == 1, "%s: the game's AttackTarget was not called", where);
  OnKilled(0x11);
  CHECK(g_scopeClockReads == 0, "%s: %d reads of the scope's clock on the arena path of the hooks", where, g_scopeClockReads);
  CHECK(ReadCount() == 0 && ScanCount() == 0,
        "%s: %llu reads and %llu scans on the arena path of the hooks", where,
        static_cast<unsigned long long>(ReadCount()),
        static_cast<unsigned long long>(ScanCount()));
  StartScanCount();
  const uint32_t looksBefore = scope::Looks();
  for (uint32_t i = 0; i < 2 * kScopeFrames + 1; ++i) Frame();  // two more asks of the level: the same answer says nothing
  // the only scans are the combat level answer's own looks (combat_scope.h): this module's tick reads the answer and scans nothing of its own
  CHECK(ScanCount() == scope::Looks() - looksBefore, "%s: %llu scans in %u frames for %u looks of the combat level answer", where,
        static_cast<unsigned long long>(ScanCount()), 2 * kScopeFrames + 1, scope::Looks() - looksBefore);
  const Stats after = CurrentStats();
  CHECK(after.enter_drone == before.enter_drone && after.enter_other == before.enter_other && after.fires == before.fires &&
            after.attacks == before.attacks && after.due_runs == before.due_runs,
        "%s: a hook counted work it did", where);
  CHECK(g_dis.empty() && g_setvels.empty(), "%s: a component was turned off or a velocity set", where);
  CHECK(g_lines.empty(), "%s: %zu dronekill lines", where, g_lines.size());
  for (const auto& l : g_lines) std::printf("  line: %s\n", l.text.c_str());
}

static void TestArenaLevelsAreLeftAlone() {
  for (const char* level : {"mpl_arena_a", "mpl_lobby_b2", "mpl_lobby_b_combat", ""}) {
    Result r = Reset(Words{});
    CHECK(r.ok, "install");
    if (level[0] != 0) LoadLevels({level});
    else LoadLevels({});
    SetScopeForTest(false);  // the module starts out of scope (fail closed); the harness's Reset puts it in one
    g_lines.clear();
    Frame();  // the first tick asks the level
    CHECK(!InCombatScope(), "%s: in scope", level);
    CHECK(!HasLine("combat_dronekill_scope "), "%s: a scope line with no change of scope", level);
    ExpectInert(level[0] != 0 ? level : "no level");
    // on an arena level no counter moves and no D11 line goes out; the stats record is written only at the process end (NoteEnd, not run here)
    CHECK(!HasLine("combat_dronekill_stats") && !HasLine("combat_dronekill_expired"), "%s: a stats or expired line on an arena level", level);
  }
  // a combat map: in scope, one line saying so
  Reset(Words{});
  SetScopeForTest(false);
  g_lines.clear();
  Frame();
  CHECK(InCombatScope() && HasLine("combat_dronekill_scope in_combat=1", 1), "a combat map is not the scope");
  // no game (0): out of scope
  Reset(Words{});
  SetScopeForTest(false);
  Frame(0);
  CHECK(!InCombatScope(), "no game is in scope");
}

// A combat map, then an arena level: the dead group the server named is not turned off in the arena, the tables are empty, and the line on the way
// out carries each hook's calls over the combat scope; back on a combat map the module acts again.
static void TestScopeEndsAtTheLevelChange() {
  CHECK(kLeaveAsks == 3, "the scope ends after %u no-level answers, want 3", kLeaveAsks);
  Result r = Reset(Words{});
  CHECK(r.ok, "install");
  SetScopeForTest(false);  // out of scope before the first ask, so that ask is the entry (its change line is checked below)
  g_lines.clear();
  Frame();  // the first tick: the combat map, the scope starts
  CHECK(InCombatScope(), "the combat map is not the scope");
  CHECK(HasRawLine({"\"in_combat\":1", LevelField("mpl_combat_dyson").c_str()}), "the entry line does not carry the combat map's level of the look");
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&d);
  OnKilled(0x11);
  CHECK(HasLine("result=shut_down"), "the K did not shut the group down");
  LoadLevels({"mpl_arena_a"});
  for (uint32_t i = 1; i < kScopeFrames; ++i) Frame();  // in scope until the next ask: the due work runs
  const size_t inScope = g_dis.size();
  CHECK(inScope == 1, "the dead group was not turned off while the combat map was the scope (%zu)", inScope);
  ND s = Drone(kSound, kSound, 1);
  CallEnter(&s);  // the dead group's component on again: due at the next tick
  g_lines.clear();
  g_dis.clear();
  TickAsks(2);  // the arena level read, but not for long enough to end the scope: the due work still runs
  CHECK(InCombatScope() && !HasLine("combat_dronekill_scope in_combat=0"), "the scope ended before the third no-level answer");
  CHECK(HasLine("combat_dronekill_scope_wait", 0), "no scope_wait line for a no-level answer that ended nothing");
  const size_t beforeLeave = g_dis.size();
  g_lines.clear();
  TickAsks(1);  // the last answer
  CHECK(!InCombatScope() && HasLine("combat_dronekill_scope in_combat=0", 1) && HasLine("enter_calls="), "the scope line on the way out");
  CHECK(HasRawLine({"\"event\":\"combat_dronekill_scope\"", "\"in_combat\":0", LevelField("mpl_arena_a").c_str()}),
        "the exit line does not carry the arena level of the look");
  CHECK(g_dis.size() == beforeLeave, "a component was turned off in the arena");
  g_dis.clear();
  ExpectInert("after the change");
  // back to a combat map: it acts again
  LoadLevels({"mpl_combat_fission"});
  g_lines.clear();
  g_dis.clear();
  for (uint32_t i = 0; i < kScopeFrames; ++i) Frame();
  CHECK(InCombatScope() && HasLine("combat_dronekill_scope in_combat=1", 1), "back on a combat map: not in scope");
  // the arena scope's line counts the calls made in it: ExpectInert made two Enter calls (the fake Enter calls Actor once each), one Actor of its
  // own, one FireBulletLocal and one AttackTarget
  CHECK(HasLine("actor_calls=3 enter_calls=2 fire_calls=1 attack_calls=1"), "the scope line does not count the arena scope's calls");
  CHECK(g_dis.empty(), "back on a combat map: the group the server named in the earlier combat scope was turned off");
  g_find_calls = 0;
  CallEnter(&d);
  CHECK(g_find_calls > 0 && HasLine("combat_dronekill_release"), "back on a combat map: the drone was not looked up");
  // the tables were emptied when the scope ended: the server's 'K' for the group of the earlier combat scope finds none, and nothing ran due
  const uint32_t runs = CurrentStats().due_runs;
  g_lines.clear();
  OnKilled(0x11);
  Frame();
  CHECK(HasLine("result=shut_down"), "the new drone's group is not the one the K names");
  CHECK(CurrentStats().due_runs == runs + 1, "due work ran %u times", CurrentStats().due_runs - runs);
  ResetForTest();
  Reset(Words{});
  g_lines.clear();
  CallEnter(&d);
  OnKilled(0x11);
  LoadLevels({"mpl_arena_a"});
  TickAsks(kLeaveAsks);
  CHECK(!InCombatScope(), "not out of scope");
  LoadLevels({"mpl_combat_dyson"});
  const uint32_t before = CurrentStats().due_runs;
  for (uint32_t i = 0; i < kScopeFrames; ++i) Frame();  // back in at n = 64, then one tick in scope
  g_lines.clear();
  OnKilled(0x11);
  CHECK(HasLine("result=no_live_group"), "the group of the earlier combat scope is still in the table");
  CHECK(CurrentStats().due_runs == before, "due work from the earlier combat scope ran %u times", CurrentStats().due_runs - before);
  // a group still live when the scope ended is gone too
  ResetForTest();
  Reset(Words{});
  CallEnter(&d);
  LoadLevels({"mpl_arena_a"});
  TickAsks(kLeaveAsks);
  LoadLevels({"mpl_combat_dyson"});
  for (uint32_t i = 0; i < kScopeFrames; ++i) Frame();
  CHECK(InCombatScope(), "not back in scope");
  g_lines.clear();
  OnKilled(0x11);
  CHECK(HasLine("result=no_live_group"), "a live group of the earlier combat scope is still in the table");
  // a 'K' just before the last no-combat-map answer: its due work never ran in scope, and leaving drops it with the tables, so nothing runs for it on
  // the next combat map
  ResetForTest();
  Reset(Words{});
  CallEnter(&d);
  LoadLevels({"mpl_arena_a"});
  TickAsks(kLeaveAsks - 1);                                  // two no-combat-map answers: still in scope
  while (CurrentStats().ticks % kScopeFrames != 0) Frame();  // up to the frame of the last ask
  OnKilled(0x11);                                            // due at the next tick
  Frame();                                                   // the third answer: the scope ends before the tick's due work
  CHECK(!InCombatScope(), "the third no-combat-map answer did not end the scope");
  const uint32_t dueRuns = CurrentStats().due_runs;
  LoadLevels({"mpl_combat_dyson"});
  for (uint32_t i = 0; i < kScopeFrames; ++i) Frame();  // back in at the next ask, then ticks in scope
  CHECK(InCombatScope() && CurrentStats().due_runs == dueRuns, "due work of the earlier combat scope ran %u times", CurrentStats().due_runs - dueRuns);
}

// The combat level answer is believed for kStaleAfterMs after its look (combat_scope.h). The scope's refresh and this module's ask run in the same tick,
// the scope's first, so on the headset a stale answer means the tick itself stopped; the rule still holds here: an answer the scope stopped refreshing
// is no combat map, counted like any other, and kLeaveAsks of them end the scope.
static void TestAStaleAnswerIsNoCombatMap() {
  Result r = Reset(Words{});
  CHECK(r.ok, "install");
  Frame();  // a look and an ask: the combat map
  CHECK(InCombatScope(), "the combat map is not the scope");
  g_scopeMs += scope::kStaleAfterMs + 1;  // the scope's refresh stops: its last look is older than it is believed
  g_lines.clear();
  const uintptr_t game = GameAddress();
  for (unsigned asks = 0; asks < kLeaveAsks;) {  // this module's tick alone
    if (CurrentStats().ticks % kScopeFrames == 0) ++asks;
    Tick(game);
  }
  CHECK(!InCombatScope() && HasLine("combat_dronekill_scope in_combat=0", 1), "a stale combat answer kept the scope");
}

// A stopped tick (README rule 9, dronekill.md "Arena"): the tick stops in a combat map and the scope's clock moves on 60 s. Nothing asks again, so the
// tick's copy (InCombatScope) still says combat; every in-scope body and OnKilled judge the answer at their call (scope::LookIsFresh), so each hook
// calls the game's function as it was called and does nothing else: no look-up, no counter, no shot frame, no 'K' acted on, no line. The tick back:
// the next frame looks again, and the drone's Enter is looked up.
static void TestAStoppedTickPassesEveryCallThrough() {
  Result r = Reset(Words{});
  CHECK(r.ok, "install");
  Frame();  // a look and an ask: the combat map is the scope
  CHECK(InCombatScope(), "the combat map is not the scope");
  g_scopeMs += 60000;  // the game tick is stopped: no Frame and no Tick, so nothing asks again
  CHECK(!scope::LookIsFresh(), "stopped: the scope still believes its look 60 s on");
  CHECK(InCombatScope(), "stopped: the tick's copy changed with no tick");
  g_lines.clear();
  g_enters.clear();
  g_fires.clear();
  g_attacks.clear();
  g_find_calls = g_comp_calls = 0;
  const Stats before = CurrentStats();
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&d);
  CallFire(reinterpret_cast<void*>(0x6001), 1, DkC3{1, 2, 3}, 0);
  CallAttack(g_cs, kAtHandle);
  OnKilled(0x11);
  CHECK(g_enters.size() == 1 && g_enters[0].data == &d && SameNd(g_enters[0].copy, d), "stopped: the game's Enter was not called as the caller called it");
  CHECK(g_find_calls == 0 && g_comp_calls == 0, "stopped: a drone Enter was looked up (%d finds, %d components)", g_find_calls, g_comp_calls);
  CHECK(g_fires.size() == 1 && !g_fires[0].acc_non_null, "stopped: a shot frame was made (dronemuzzle would act)");
  CHECK(g_attacks.size() == 1, "stopped: the game's AttackTarget was not called");
  const Stats after = CurrentStats();
  CHECK(after.enter_drone == before.enter_drone && after.enter_other == before.enter_other && after.fires == before.fires &&
            after.attacks == before.attacks,
        "stopped: a hook counted work it did");
  CHECK(g_lines.empty(), "stopped: %zu dronekill lines (the 'K' was acted on, or a drone was looked up)", g_lines.size());
  Frame();  // the tick is back: a fresh look, and the copy still says combat
  g_find_calls = 0;
  CallEnter(&d);
  CHECK(g_find_calls > 0 && HasLine("combat_dronekill_release"), "the tick back: the drone was not looked up");
}

// The scope's decision and its change line come from the one record the tick reads (LastLook): a live read of the answer made after the record is
// stale in this tick, so a decision that asked the answer again would stay out of the scope and write no change line. Out of scope on an arena level,
// then a combat map loaded: the ask whose record says combat map starts the scope, its line names the map's level, and no live read decides it.
static void TestTheRecordDecidesNotTheLiveRead() {
  Result r = Reset(Words{});
  CHECK(r.ok, "install");
  SetScopeForTest(false);  // the harness's Reset starts the module in its scope (InstallDroneKill); this case starts out of it
  LoadLevels({"mpl_arena_a"});
  Frame();  // the first ask: an arena level, out of scope, no change line
  CHECK(!InCombatScope() && !HasLine("combat_dronekill_scope in_combat=1"), "an arena level is the scope");
  LoadLevels({"mpl_combat_fission"});
  while (CurrentStats().ticks % kScopeFrames != 0) Frame();  // up to the frame of the next ask
  g_lines.clear();
  g_scopeMs += scope::kLookEveryMs;
  scope::Refresh(GameAddress());  // this frame's look: the combat map, the record the ask reads
  g_armedReads = 0;               // the ask's own read of the record is the first clock read; every later read is stale
  Tick(GameAddress());
  g_armedReads = -1;
  CHECK(InCombatScope(), "the record says a combat map, but the scope did not start: a live read decided it");
  CHECK(HasLine("combat_dronekill_scope in_combat=1", 1), "no change line for the record's combat map");
  CHECK(HasRawLine({"\"in_combat\":1", LevelField("mpl_combat_fission").c_str()}), "the change line does not name the record's level");
}

// The production step: the switch, the tick, the kill feed, the refusals.
static void TestProductionStep() {
  Reset(Words{}, false);
  nevr_quest::combat::tick::ResetForTest();
  ec_stub::Reset();
  const nevr_quest::combat::ModuleResult off = Install(false);
  CHECK(!off.installed && std::strcmp(off.status, "off") == 0 && HasLine("combat_dronekill_install result=off", 1), "off");
  CHECK(W(kSlotEnter) == reinterpret_cast<uintptr_t>(&FakeEnter) && nevr_quest::combat::tick::Count() == 0 && ec_stub::DroneKilled() == nullptr,
        "off installed something");
  const Pins pins = Slots();
  const Fns fns = FakeFns();
  // the combat level answer cannot change (the tick is not installed, so the scope row registered no refresh): refused before the image is looked up,
  // nothing hooked or registered, no kill feed
  SetInstallForTest(&NoLookup, nullptr, nullptr);
  const nevr_quest::combat::ModuleResult dead = Install(true);
  CHECK(!dead.installed && std::strcmp(dead.status, "scope_not_live") == 0 && nevr_quest::combat::tick::Count() == 0 && ec_stub::DroneKilled() == nullptr,
        "a scope that cannot change: %s", dead.status);
  CHECK(HasLine("combat_dronekill_install result=refused reason=scope_not_live", 3), "scope_not_live is not an Error line");
  SetInstallForTest(&Lookup, &pins, &fns);
  const nevr_quest::combat::ModuleResult deadHooks = Install(true);
  CHECK(!deadHooks.installed && std::strcmp(deadHooks.status, "scope_not_live") == 0 && W(kSlotEnter) == reinterpret_cast<uintptr_t>(&FakeEnter),
        "a scope that cannot change hooked a slot: %s", deadHooks.status);
  StartTheScope();
  const nevr_quest::combat::ModuleResult on = Install(true);
  CHECK(on.installed && std::strcmp(on.status, "installed") == 0, "on: %s", on.status);
  CHECK(nevr_quest::combat::tick::Count() == 2 && ec_stub::DroneKilled() == &OnKilled && HasLine("combat_dronekill_link"), "tick / kill feed");
  CHECK(W(kSlotEnter) != reinterpret_cast<uintptr_t>(&FakeEnter), "the Enter slot was not hooked");
  CHECK(!InCombatScope(), "in scope before any tick");
  const nevr_quest::combat::ModuleResult again = Install(true);
  CHECK(nevr_quest::combat::tick::Count() == 2, "a second step registered the tick again");
  CHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0, "a second step: %s", again.status);
  // the kill feed through ec: a 'K' reaches the module
  Frame();
  ND d = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&d);
  ec_stub::DroneKilled()(0x11);
  CHECK(HasLine("result=shut_down"), "the K through ec's handler did not reach the kill rule");
  // no libr15: refused, nothing registered
  Reset(Words{}, false);
  StartTheScope();
  ec_stub::Reset();
  SetInstallForTest(&NoLookup, nullptr, nullptr);
  const nevr_quest::combat::ModuleResult none = Install(true);
  CHECK(!none.installed && std::strcmp(none.status, "module_not_loaded") == 0 && nevr_quest::combat::tick::Count() == 1 && ec_stub::DroneKilled() == nullptr,
        "no libr15: %s", none.status);
  // the functions resolved from the real library: none on the host
  Reset(Words{}, false);
  StartTheScope();
  SetInstallForTest(&Lookup, &pins, nullptr);
  const nevr_quest::combat::ModuleResult noFns = Install(true);
  CHECK(!noFns.installed && std::strcmp(noFns.status, "engine_export_missing") == 0, "no functions: %s", noFns.status);
  // the step on a build that is not the pinned one: refused, no kill feed
  Reset(Words{}, false);
  StartTheScope();
  ec_stub::Reset();
  g_image.Build({{kActorSymbol, kSlotActor, false, false, 0}}, "0000000000000000000000000000000000000000");
  SetInstallForTest(&Lookup, &pins, &fns);
  const nevr_quest::combat::ModuleResult wrong = Install(true);
  CHECK(!wrong.installed && std::strcmp(wrong.status, "build_id_mismatch") == 0 && ec_stub::DroneKilled() == nullptr, "another build's step: %s", wrong.status);
  // a 'K' before the install: nothing
  ResetForTest();
  SetScopeForTest(true);
  g_lines.clear();
  OnKilled(0x11);
  CHECK(g_lines.empty(), "a K before the install was logged");
  // a build that is not the pinned one
  Reset(Words{}, false);
  g_image.Build({{kActorSymbol, kSlotActor, false, false, 0}}, "0000000000000000000000000000000000000000");
  CHECK(std::strcmp(InstallAt(&Lookup, Slots(), Words{}, FakeFns()).status, "build_id_mismatch") == 0, "another build");
  nevr_quest::combat::tick::ResetForTest();
  ResetForTest();
  g_tickImage.Drop();
}

// ---- D11 and the stats record (#2359)

static bool StatsHas(const char* name, unsigned long long v) {
  char needle[64];
  std::snprintf(needle, sizeof needle, " %s=%llu ", name, v);
  for (const auto& l : g_lines)
    if (l.text.find("combat_dronekill_stats") != std::string::npos && l.text.find(needle) != std::string::npos) return true;
  return false;
}

// D11: a drone's damageable going off is the drone's end: its group is dead, and a component of it that goes on again is held off (R3)
static void TestD11DroneEnd() {
  Reset(Words{});
  g_lines.clear();
  ND rel = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&rel);
  ND snd = Drone(kSound, kSound, 1);
  CallEnter(&snd);
  ND off = Drone(dk::kDamageable, dk::kDamageable, 0);
  CallEnter(&off);
  CHECK(HasLine("combat_dronekill_expired", 1) && HasLine("group=1"), "the drone's end writes no expired line");
  CHECK(CurrentStats().expired == 1, "the drone's end is not counted: %u", CurrentStats().expired);
  OnKilled(0xabcd0011u);
  CHECK(HasLine("result=no_live_group"), "the ended group is still live for a K");
  ND again = Drone(kSound, kSound, 1);
  CallEnter(&again);
  CHECK(HasLine("combat_dronekill_reenabled", 1), "a component of the ended group is not held off");
}

// F5 (#2359 review): a drone that K killed and then expires is a repeat end. Its group is already dead, so the end counts and logs nothing
static void TestD11KilledThenExpired() {
  Reset(Only(true, false, false, false, false));
  g_lines.clear();
  ND rel = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&rel);
  OnKilled(kDmgHandle & 0xffff);
  CHECK(HasLine("combat_dronekill_k ") && HasLine("result=shut_down"), "K did not shut the drone's group down");
  ND off = Drone(dk::kDamageable, dk::kDamageable, 0);
  CallEnter(&off);
  CHECK(!HasLine("combat_dronekill_expired"), "a drone K killed and then expired wrote an expired line");
  CHECK(CurrentStats().expired == 0, "a drone K killed and then expired was counted as an end: %u", CurrentStats().expired);
}

// D11 notice: the first 16 expired lines are written, the 17th drone end writes the one notice, and the count goes on past the cap
static void TestD11LogCap() {
  Reset(Words{});
  g_lines.clear();
  for (int i = 0; i < 17; ++i) {
    ND rel = Drone(dk::kDamageable, dk::kDamageable, 1);
    CallEnter(&rel);
    ND off = Drone(dk::kDamageable, dk::kDamageable, 0);
    CallEnter(&off);
  }
  CHECK(CountLine("combat_dronekill_expired", 1) == 16, "%d expired lines for 17 drone ends", CountLine("combat_dronekill_expired", 1));
  CHECK(CountLine("combat_dronekill_log_cap", 2) == 1 && HasLine("lines=expired"), "no notice at the 17th drone end");
  ND rel = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&rel);
  ND off = Drone(dk::kDamageable, dk::kDamageable, 0);
  CallEnter(&off);
  CHECK(CountLine("combat_dronekill_expired", 1) == 16 && CountLine("combat_dronekill_log_cap", 2) == 1, "the 18th drone end writes a line or a second notice");
  CHECK(CurrentStats().expired == 18, "the count stops at the cap: %u", CurrentStats().expired);
}

// The stats record (#2359): every counter CurrentStats() returns, and expired, written once at the process end (Info, final 1)
static void TestStatsRecord() {
  Reset(Words{});
  g_lines.clear();
  ND rel = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&rel);
  ND snd = Drone(kSound, kSound, 1);
  CallEnter(&snd);
  ND other{0x55, 0, kSound, kSound, 1};
  CallEnter(&other);
  CallFire(reinterpret_cast<void*>(0x6001), 0x00020003, DkC3{1, 2, 3}, 1);
  CallAttack(reinterpret_cast<void*>(0x6001), 1);
  OnKilled(0xabcd0011u);
  Frame();
  ND off = Drone(dk::kDamageable, dk::kDamageable, 0);
  CallEnter(&off);
  const Stats s = CurrentStats();
  // K killed the drone before its end, so the end is a repeat and expired stays 0 (a live end is counted in TestStatsRecordLiveEnd)
  CHECK(s.enter_drone == 3 && s.enter_other == 1 && s.fires == 1 && s.attacks == 1 && s.ticks >= 1 && s.due_runs >= 1 && s.expired == 0 && s.capturing == 0,
        "the counters: drone %u other %u fires %u attacks %u ticks %u due %u expired %u", s.enter_drone, s.enter_other, s.fires, s.attacks, s.ticks,
        s.due_runs, s.expired);
  g_lines.clear();
  NoteEnd();
  CHECK(CountLine("combat_dronekill_stats", 1) == 1 && StatsHas("final", 1), "no final stats record");
  CHECK(StatsHas("enter_drone", s.enter_drone) && StatsHas("enter_other", s.enter_other) && StatsHas("fires", s.fires) &&
            StatsHas("attacks", s.attacks) && StatsHas("ticks", s.ticks) && StatsHas("due_runs", s.due_runs) && StatsHas("capturing", s.capturing) &&
            StatsHas("expired", s.expired),
        "a counter is missing from the stats record");
}

// The stats record's expired counter on a live end (#2359 review): a drone that ends while its group is alive is counted once
static void TestStatsRecordLiveEnd() {
  Reset(Words{});
  g_lines.clear();
  ND rel = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&rel);
  ND off = Drone(dk::kDamageable, dk::kDamageable, 0);
  CallEnter(&off);
  CHECK(CurrentStats().expired == 1, "a live drone end is not counted: %u", CurrentStats().expired);
  g_lines.clear();
  NoteEnd();
  CHECK(CountLine("combat_dronekill_stats", 1) == 1 && StatsHas("expired", 1), "the live end is not in the stats record");
}

// The Debug stats line every kStatsEvery drone Enters, and none before it
static void TestStatsPeriodic() {
  Reset(Words{});
  g_lines.clear();
  for (unsigned i = 0; i < kStatsEvery - 1; ++i) {
    ND off = Drone(kSound, kSound, 0);
    CallEnter(&off);
  }
  CHECK(CountLine("combat_dronekill_stats") == 0, "a stats line before %u drone Enters", kStatsEvery);
  ND off = Drone(kSound, kSound, 0);
  CallEnter(&off);
  CHECK(CountLine("combat_dronekill_stats", 0) == 1 && StatsHas("final", 0) && StatsHas("enter_drone", kStatsEvery),
        "no Debug stats line at %u drone Enters", kStatsEvery);
}

// ================================================================ the explosion (#1737, dronekill.h "The explosion")
// The drone library's two entries are the fakes below; the entry check's dladdr answer is FakeEntryInfo (the library's name and base);
// the page words are written on the stand-in EC page (ec_stub.cpp). The context's state word at +0x18 is 0x5151; each entry records it
// and writes its own (the module puts the saved one back after both).
namespace ecc = nevr_quest::combat::ec::core;
alignas(16) static uint8_t g_fxctx[0x40];
struct EntryCall {
  const char* which;
  void* ctx;
  uint64_t state;
};
static std::vector<EntryCall> g_entry_calls;
static void EntryRecord(const char* which, void* ctx) {
  uint64_t st = 0;
  memcpy(&st, static_cast<uint8_t*>(ctx) + 0x18, 8);
  g_entry_calls.push_back({which, ctx, st});
  const uint64_t mine = 0x7777;
  memcpy(static_cast<uint8_t*>(ctx) + 0x18, &mine, 8);
}
static void FakeSpawnEntry(void* ctx) { EntryRecord("spawn", ctx); }
static void FakeSoundEntry(void* ctx) { EntryRecord("sound", ctx); }
static int g_entry_mode = 0;  // 0: the drone library at the built offsets; 1: another library; 2: 4 bytes past the entry's start; 3: no answer
static bool FakeEntryInfo(uintptr_t fn, EntryInfo* out) {
  if (g_entry_mode == 3) return false;
  const uintptr_t off = fn == reinterpret_cast<uintptr_t>(&FakeSpawnEntry)   ? ecc::kDroneSpawnEntry
                        : fn == reinterpret_cast<uintptr_t>(&FakeSoundEntry) ? ecc::kDroneSoundEntry
                                                                             : 0x40;
  out->fname = g_entry_mode == 1 ? "/data/app/x/lib/arm64/libac4e9374a146d3a4.so" : "/data/app/x/lib/arm64/lib221a0a3a2e56d247.so";
  out->fbase = fn - off - (g_entry_mode == 2 ? 4 : 0);
  return true;
}
static void SetPage(uint32_t off, uint64_t v) { memcpy(sentinel::EcPage() + ecc::kDroneBlock + off, &v, 8); }
static uint64_t CtxState() {
  uint64_t v = 0;
  memcpy(&v, g_fxctx + 0x18, 8);
  return v;
}
static void ArmExplosion() {
  g_entry_calls.clear();
  g_entry_mode = 0;
  SetEntryInfoForTest(&FakeEntryInfo);
  memset(g_fxctx, 0, sizeof g_fxctx);
  W64(g_fxctx, 0x18, 0x5151);
  SetPage(ecc::kDroneCtx, reinterpret_cast<uintptr_t>(g_fxctx));
  SetPage(ecc::kDroneSpawn, reinterpret_cast<uintptr_t>(&FakeSpawnEntry));
  SetPage(ecc::kDroneSoundFn, reinterpret_cast<uintptr_t>(&FakeSoundEntry));
}
// the damageable takes `handle` (a second drone is another damageable handle of the same system)
static void DamageableHandle(uint32_t handle) {
  g_comps[0].handle = handle;
  g_idxtab[(handle & 0xffff) * 2] = static_cast<uint16_t>(handle & 0xffff);
}
static void Release(uint32_t handle = kDmgHandle) {
  DamageableHandle(handle);
  ND rel = Drone(dk::kDamageable, dk::kDamageable, 1);
  CallEnter(&rel);
}
static void EndOf(uint32_t handle = kDmgHandle) {
  DamageableHandle(handle);
  ND end = Drone(dk::kDamageable, dk::kDamageable, 0);
  CallEnter(&end);
}
static const uint32_t kNcaHandle = 0x00060006;  // the world's kSound system and component (kSound is kNcaPhysics)
// the first-system off: true when the real Enter ran
static bool FirstOff() {
  const size_t before = g_enters.size();
  ND first = Drone(dk::kNcaPhysics, dk::kNcaPhysics, 0);
  CallEnter(&first);
  return g_enters.size() != before;
}
static int NcaOffs() {
  int n = 0;
  for (const DisCall& d : g_dis) n += d.sys == g_snd && d.handle == kNcaHandle && !d.enable ? 1 : 0;
  return n;
}
static Words Held() {  // the hold on (`combat.dronekill_hold`, off by default)
  Words w;
  w.hold = true;
  return w;
}
static Words NoExplosion() {
  Words w;
  w.explosion = false;
  return w;
}

static void TestExplosion() {
  CHECK(dk::kNcaPhysics == kSound, "the world's sound system is not kNcaPhysics");
  {  // KQueuesOneExplosion, CtxStateSavedZeroedRestored, SecondKQueuesNothing
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    OnKilled(0xabcd0011u);
    CHECK(g_entry_calls.empty(), "the explosion ran on the 'K' thread, not at the tick");
    Frame();
    CHECK(g_entry_calls.size() == 2, "a 'K' ran %zu entries at the tick, want 2", g_entry_calls.size());
    CHECK(g_entry_calls.size() == 2 && std::strcmp(g_entry_calls[0].which, "spawn") == 0 && std::strcmp(g_entry_calls[1].which, "sound") == 0 &&
              g_entry_calls[0].ctx == g_fxctx && g_entry_calls[1].ctx == g_fxctx,
          "the entries: spawn then sound, with the release's context");
    CHECK(g_entry_calls.size() == 2 && g_entry_calls[0].state == 0 && g_entry_calls[1].state == 0, "the state word was not 0 at an entry");
    CHECK(CtxState() == 0x5151, "the state word was not put back: 0x%llx", static_cast<unsigned long long>(CtxState()));
    CHECK(HasLine("combat_dronekill_explosion ") && HasLine("reason=killed") && HasLine("sound_delay_ms=0") && HasLine("entries=2"), "the explosion line");
    CHECK(CurrentStats().explosions == 1, "explosions %u", CurrentStats().explosions);
    OnKilled(0xabcd0011u);
    Frame();
    CHECK(g_entry_calls.size() == 2, "a second 'K' for the same drone ran the explosion again");
    Dump("explosion: K");
  }
  {  // ExpiryAliveFiresExplosionThenHeldOffAt2500
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    ND on = Drone(kSound, kSound, 1);
    CallEnter(&on);
    CHECK(!FirstOff(), "the first-system off of the only live drone ran the real Enter (not held)");
    CHECK(HasLine("combat_dronekill_hold ") && HasLine("step=held") && HasLine("ctx_match=1"), "the hold line");
    CHECK(CurrentStats().held == 1, "held %u", CurrentStats().held);
    EndOf();
    CHECK(NcaOffs() == 0 && g_entry_calls.empty(), "the damageable off turned the held component off or exploded on the script's thread");
    const uint64_t t0 = g_ms;
    Frame();
    CHECK(g_entry_calls.size() == 2 && HasLine("reason=expired") && HasLine("sound_delay_ms=2500"), "the expiry's explosion at the tick");
    CHECK(NcaOffs() == 0, "the held component went off with the explosion");
    g_ms = t0 + 2499;
    Frame();
    CHECK(NcaOffs() == 0, "the held component went off 2499 ms after the explosion");
    g_ms = t0 + 2500;
    Frame();
    CHECK(NcaOffs() == 1, "the held component did not go off 2500 ms after the explosion (%d)", NcaOffs());
    CHECK(HasLine("step=released") && HasLine("why=off"), "the release line");
    Frame();
    CHECK(NcaOffs() == 1, "the held component went off twice");
    Dump("explosion: expiry");
  }
  {  // ExpiryDeadReleasesHeldNow: with the kill word off a 'K' marks the group dead and queues nothing; its end then has the held off now
    Words w = Held();
    w.kill = false;
    CHECK(Reset(w).ok, "install");
    ArmExplosion();
    Release();
    CHECK(!FirstOff(), "not held");
    OnKilled(0xabcd0011u);
    EndOf();
    Frame();
    CHECK(g_entry_calls.empty(), "a dead drone's end exploded");
    CHECK(NcaOffs() == 1, "a dead drone's held component did not go off at the next tick");
    CHECK(HasLine("why=dead"), "the released line names the dead drone");
  }
  {  // a 'K' on a drone whose first-system off is held: the explosion with nothing held, the held component off at the tick
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    CHECK(!FirstOff(), "not held");
    OnKilled(0xabcd0011u);
    Frame();
    CHECK(g_entry_calls.size() == 2 && HasLine("reason=killed"), "the kill's explosion");
    CHECK(NcaOffs() == 1 && HasLine("why=killed"), "the held component did not go off at the kill's tick");
  }
  {  // HoldLetGoWithoutDamageableAt2500
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    const uint64_t t0 = g_ms;
    CHECK(!FirstOff(), "not held");
    g_ms = t0 + 2499;
    Frame();
    CHECK(NcaOffs() == 0, "let go at 2499 ms");
    g_ms = t0 + 2500;
    Frame();
    CHECK(NcaOffs() == 1 && HasLine("step=let_go"), "a hold no damageable off closed was not let go at 2500 ms");
    EndOf();
    Frame();
    CHECK(g_entry_calls.empty(), "an end after the let-go exploded (the expiry was closed)");
  }
  {  // UnresolvableRunsRealEnter: no live group, two live groups, no such component
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    CHECK(FirstOff() && g_enters.size() == 1, "with no live drone the real Enter was not run once");
    CHECK(HasLine("step=not_held") && HasLine("why=no_live_group"), "the not_held line (no live group)");
    Release(kDmgHandle);
    Release(0x00050012);
    g_lines.clear();
    CHECK(FirstOff(), "with two live drones the real Enter did not run");
    CHECK(HasLine("why=ambiguous"), "the not_held line (two live drones)");
    CHECK(CurrentStats().held == 0, "held with two live drones");
  }
  {
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    g_comps[1].sym = 0x1234;  // the drone's ncaPhysics component is not there
    CHECK(FirstOff(), "an unresolved component did not run the real Enter");
    CHECK(HasLine("why=no_component"), "the not_held line (no component)");
    CHECK(HasLine("step=end_open"), "the real Enter ran and its actor did not open the drone's expiry");
    EndOf();
    Frame();
    CHECK(g_entry_calls.size() == 2 && HasLine("reason=expired") && HasLine("sound_delay_ms=0") && NcaOffs() == 0,
          "a drone whose first-system off was not held did not explode at its end, with nothing held");
  }
  {  // the key off: the real Enter runs, nothing is queued
    CHECK(Reset(NoExplosion()).ok, "install");
    ArmExplosion();
    Release();
    CHECK(FirstOff(), "with the explosion off the first-system off was held");
    CHECK(Reset([] { Words x = NoExplosion(); x.hold = true; return x; }()).ok, "install");
    ArmExplosion();
    Release();
    CHECK(FirstOff() && HasLine("hold=0"), "with the explosion off and the hold on the first-system off was held");
    OnKilled(0xabcd0011u);
    Frame();
    CHECK(g_entry_calls.empty() && !HasLine("combat_dronekill_hold"), "with the explosion off an explosion ran");
    CHECK(HasLine("explosion=0"), "the install line does not say explosion=0");
  }
  {  // ForeignEntryRefused: another library, and 4 bytes past the entry's start; the held component still goes off on time
    for (int mode : {1, 2, 3}) {
      CHECK(Reset(Held()).ok, "install");
      ArmExplosion();
      g_entry_mode = mode;
      Release();
      CHECK(!FirstOff(), "not held");
      EndOf();
      const uint64_t t0 = g_ms;
      Frame();
      CHECK(g_entry_calls.empty(), "mode %d: a foreign entry was called", mode);
      CHECK(HasLine("combat_dronekill_explosion_skipped") && HasLine("reason=foreign_entry"), "mode %d: no foreign_entry line", mode);
      g_ms = t0 + 2500;
      Frame();
      CHECK(NcaOffs() == 1, "mode %d: the held component did not go off on time", mode);
    }
  }
  {  // NoEntry: the page holds no entry
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    SetPage(ecc::kDroneSpawn, 0);
    Release();
    OnKilled(0xabcd0011u);
    Frame();
    CHECK(g_entry_calls.empty() && HasLine("reason=no_entry"), "no_entry");
  }
  {  // NoContextSkipped: the page held no context at the release
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    SetPage(ecc::kDroneCtx, 0);
    Release();
    OnKilled(0xabcd0011u);
    Frame();
    CHECK(g_entry_calls.empty() && HasLine("reason=no_context"), "no_context at a kill");
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    SetPage(ecc::kDroneCtx, 0);
    Release();
    CHECK(!FirstOff(), "not held");
    EndOf();
    Frame();
    CHECK(g_entry_calls.empty() && HasLine("reason=no_context") && NcaOffs() == 1, "no_context at an end: the held component off now");
  }
  {  // QueueFullReleasesNow: eight shot-down drones wait at once; a ninth that runs out has its held component off now
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    for (uint32_t i = 0; i < 8; ++i) {
      Release(0x00050011 + i);
      OnKilled(0x11 + i);
      EndOf(0x00050011 + i);  // its end script ran (a shot-down drone that has not ended could still be the one a first-system off is for)
    }
    Release(0x00050020);
    CHECK(!FirstOff(), "the ninth drone's first-system off was not held");
    EndOf(0x00050020);
    CHECK(HasLine("reason=queue_full"), "no queue_full line");
    Frame();
    CHECK(NcaOffs() == 1 && HasLine("why=queue_full"), "the held component of a full queue did not go off at the tick");
    CHECK(g_entry_calls.size() == 16, "the eight queued explosions: %zu entries", g_entry_calls.size());
  }
  {  // a game space that defers the disable (owner +0x368 = 0): the held component waits, goes off once it may, and is given up after 2 s
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    const uint64_t t0 = g_ms;
    CHECK(!FirstOff(), "not held");
    SetFlag(0);
    g_ms = t0 + 2500;
    Frame();
    CHECK(NcaOffs() == 0, "the held component went off while its game space defers the disable");
    g_ms = t0 + 2600;
    SetFlag(1);
    Frame();
    CHECK(NcaOffs() == 1, "the held component did not go off once its game space let it");
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    const uint64_t t1 = g_ms;
    CHECK(!FirstOff(), "not held");
    SetFlag(0);
    g_ms = t1 + 2500;
    Frame();
    g_ms = t1 + 2500 + 1999;
    Frame();
    CHECK(!HasLine("step=stays_on"), "given up before 2 s");
    g_ms = t1 + 2500 + 2000;
    Frame();
    CHECK(HasLine("step=stays_on", 2) && HasLine("why=invoke_immediate_0") && NcaOffs() == 0,
          "a held component that never could go off was not given up at 2 s with a Warning that it stays on");
    g_ms += 100;
    SetFlag(1);
    Frame();
    CHECK(NcaOffs() == 0, "a held component was tried again after it was given up");
  }
  {  // a release that reuses the slot of a group with an open hold: that hold's component goes off at the next tick
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    CHECK(!FirstOff(), "not held");
    Release();
    Frame();
    CHECK(NcaOffs() == 1, "a hold whose group a release took over stayed held");
  }
  {  // the scope ends while an explosion waits with a held component: both go with it, and the held component stays on (a Warning says so)
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    CHECK(!FirstOff(), "not held");
    EndOf();
    const uint64_t t0 = g_ms;
    LoadLevels({"mpl_arena_a"});
    TickAsks(kLeaveAsks);
    CHECK(!InCombatScope(), "the arena level did not end the scope");
    CHECK(g_entry_calls.size() == 2, "the expiry's explosion did not run in the asks before the scope ended (%zu)", g_entry_calls.size());
    CHECK(HasLine("step=dropped", 2) && HasLine("count=1"), "the held component the queued explosion kept was dropped without a Warning");
    LoadLevels({"mpl_combat_dyson"});
    for (uint32_t i = 0; i < kScopeFrames; ++i) Frame();
    CHECK(InCombatScope(), "the combat level did not start the scope again");
    g_entry_calls.clear();
    g_ms = t0 + 2500;
    Release();
    CHECK(!FirstOff(), "not held");  // due work again: a tick that runs whatever the queue and the off list still hold
    Frame();
    CHECK(g_entry_calls.empty() && NcaOffs() == 0, "work queued before the scope ended ran after it");
  }
  {  // out of a combat map: the first-system off is the game's alone
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    SetScopeForTest(false);
    CHECK(FirstOff(), "out of scope the first-system off was held");
  }
}


// ---- the hold is its own switch, off by default (#1737 review): with it off the first-system off is the game's own Enter, and the drone's
// end still explodes once that off named its group by its actor
static void TestExplosionHoldOff() {
  {  // HoldOffExpiryExplodes
    CHECK(Reset(Words{}).ok, "install");
    CHECK(HasLine("explosion=1") && HasLine("hold=0"), "the install line does not say explosion=1 hold=0 by default");
    ArmExplosion();
    Release();
    CHECK(FirstOff(), "with the hold off (the default) the first-system off did not run the real Enter");
    CHECK(!HasLine("step=held") && CurrentStats().held == 0, "with the hold off a hold was taken");
    CHECK(HasLine("step=end_open"), "the first-system off did not open the drone's expiry");
    EndOf();
    const uint64_t t0 = g_ms;
    Frame();
    CHECK(g_entry_calls.size() == 2 && HasLine("reason=expired") && HasLine("sound_delay_ms=0"), "the expiry's explosion with nothing held");
    g_ms = t0 + 2500;
    Frame();
    CHECK(NcaOffs() == 0 && !HasLine("step=stays_on") && !HasLine("step=released"), "the module turned (or tried to turn) a component off with the hold off");
  }
  {  // no first-system off seen: the end is the game's own, no explosion (PC: the expiry is not open)
    CHECK(Reset(Words{}).ok, "install");
    ArmExplosion();
    Release();
    EndOf();
    Frame();
    CHECK(g_entry_calls.empty(), "an end with no first-system off exploded");
  }
  {  // two drones out: the first-system off names its own drone by its actor
    CHECK(Reset(Words{}).ok, "install");
    ArmExplosion();
    g_actor_val = 0xA1;
    Release(kDmgHandle);
    g_actor_val = 0xB2;
    Release(0x00050012);
    CHECK(FirstOff() && HasLine("group=2 step=end_open"), "the second drone's first-system off did not open its own expiry");
    EndOf(kDmgHandle);  // (the fake Actor still answers 0xB2: the end is the damageable's, by handle)
    Frame();
    CHECK(g_entry_calls.empty(), "the other drone's end exploded");
    EndOf(0x00050012);
    Frame();
    CHECK(g_entry_calls.size() == 2 && HasLine("reason=expired"), "the drone whose first-system off ran did not explode");
    g_actor_val = 0;
  }
  {  // two live drones of one actor: the off cannot name one, so neither expiry opens
    CHECK(Reset(Words{}).ok, "install");
    ArmExplosion();
    Release(kDmgHandle);
    Release(0x00050012);
    CHECK(FirstOff() && HasLine("step=end_not_open") && !HasLine("step=end_open"), "an off shared by two drones' actor opened an expiry");
  }
}

// ---- a dead drone ends while another is live (#1737 review): the shot-down drone's end script still runs its first-system off, which is
// not the live drone's, so no hold is taken (two groups can still run their end)
static void TestExplosionDeadEndsWhileAnotherLive() {
  CHECK(Reset(Held()).ok, "install");
  ArmExplosion();
  g_actor_val = 0xA1;
  Release(kDmgHandle);
  g_actor_val = 0xB2;
  Release(0x00050012);
  OnKilled(kDmgHandle & 0xffff);  // A shot down while B is out
  Frame();
  CHECK(g_entry_calls.size() == 2 && HasLine("reason=killed"), "A's kill did not explode");
  g_actor_val = 0xA1;
  g_lines.clear();
  CHECK(FirstOff(), "A's own first-system off was held (as B's): A's real Enter was skipped");
  CHECK(HasLine("step=not_held") && HasLine("why=ambiguous") && CurrentStats().held == 0, "the not_held line for a dead and a live drone");
  EndOf(kDmgHandle);
  const uint64_t t0 = g_ms;
  g_ms = t0 + 2500;
  Frame();
  CHECK(NcaOffs() == 0, "B's component was turned off while B is alive");
  // B's own end: A's end is seen, so B is the one drone that can still end, and its first-system off is held
  g_actor_val = 0xB2;
  CHECK(!FirstOff() && HasLine("step=held"), "B's own first-system off was not held after A's end");
  EndOf(0x00050012);
  Frame();
  CHECK(g_entry_calls.size() == 4 && HasLine("sound_delay_ms=2500"), "B's expiry did not explode with its held component");
  g_ms += 2500;
  Frame();
  CHECK(NcaOffs() == 1, "B's held component did not go off 2500 ms after its explosion");
  g_actor_val = 0;
  // a dead drone alone (its end not seen yet): not held, and not taken for a live one
  CHECK(Reset(Held()).ok, "install");
  ArmExplosion();
  Release();
  OnKilled(kDmgHandle & 0xffff);
  CHECK(FirstOff() && HasLine("why=dead_group"), "a dead drone's first-system off was held");
  // a drone shot down long after its release (40 s: past the swallow window) whose end script runs later, while another drone is live: the
  // dead drone's first-system off is not the live one's, so no hold is taken. Nothing in the code bounds when a dead drone's end runs, so a dead
  // group stays a candidate until its own end has been seen, not for a time.
  CHECK(Reset(Held()).ok, "install");
  ArmExplosion();
  g_actor_val = 0xA1;
  Release(kDmgHandle);
  g_actor_val = 0xB2;
  Release(0x00050012);
  g_ms += 40000;
  OnKilled(kDmgHandle & 0xffff);  // A shot down 40 s after its release, while B is out
  Frame();
  g_actor_val = 0xA1;
  g_lines.clear();
  CHECK(FirstOff(), "A's first-system off (40 s after its release) was held as B's: A's real Enter was skipped");
  CHECK(HasLine("step=not_held") && HasLine("why=ambiguous") && CurrentStats().held == 0, "the not_held line for a drone shot down 40 s after its release");
  EndOf(kDmgHandle);
  g_actor_val = 0xB2;
  CHECK(!FirstOff() && HasLine("step=held"), "B's first-system off was not held once A's end was seen");
  g_actor_val = 0;
  // a dead group whose end was seen is no candidate however young; a dead group whose end was not seen stays one however old
  CHECK(Reset(Held()).ok, "install");
  ArmExplosion();
  Release(kDmgHandle);
  OnKilled(kDmgHandle & 0xffff);
  g_ms += 3600000;  // an hour: nothing bounds the end script
  CHECK(FirstOff() && HasLine("why=dead_group"), "a dead drone whose end is not seen was no candidate after an hour (its off was held as another's)");
}

// ---- a hold is taken only when it can be let go (#1737 review), and a held component that cannot be turned off says it stays on
static void TestExplosionHoldNeedsOff() {
  {  // the system's DisableComponent is not the read shape: no hold
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    g_dis_words[3] ^= 0x1;
    Release();
    CHECK(FirstOff() && HasLine("why=no_disable_shape"), "a hold was taken on a system whose DisableComponent is not the read shape");
  }
  {  // the system's game space (its owner word) is unreadable: no hold
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    W64(g_snd, dk::kSysOwner, 0);
    Release();
    CHECK(FirstOff() && HasLine("why=owner_unreadable"), "a hold was taken on a system whose invoke-immediate word cannot be read");
  }
  {  // the component changed after the hold: at 2500 ms after the explosion it cannot go off, a Warning says it stays on
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    CHECK(!FirstOff(), "not held");
    g_comps[1].handle = 0x00060016;
    EndOf();
    const uint64_t t0 = g_ms;
    Frame();
    g_ms = t0 + 2500;
    Frame();
    CHECK(HasLine("step=stays_on", 2) && HasLine("why=handle changed") && !HasLine("step=released"),
          "a held component that could not go off was not reported at Warning as staying on");
  }
  {  // the same at the let-go
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    const uint64_t t0 = g_ms;
    CHECK(!FirstOff(), "not held");
    g_comps[1].handle = 0x00060016;
    g_ms = t0 + 2500;
    Frame();
    CHECK(HasLine("step=let_go") && HasLine("step=stays_on", 2), "a let-go that could not go off was not reported as staying on");
  }
  {  // the off list is full: a hold a release takes over, and a 'K' on a held drone, each try now and warn when it stays on
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    SetFlag(0);  // the game space defers every disable: nothing leaves the off list
    for (int i = 0; i < 17; ++i) {
      Release();
      CHECK(!FirstOff(), "cycle %d: not held", i);
    }
    CHECK(CountLine("step=stays_on", 2) == 0, "a stays_on line while the off list had room");
    Release();  // the 17th take-over: the list holds 16
    CHECK(CountLine("step=stays_on", 2) == 1, "a hold a release took over with the off list full was dropped without a Warning (%d)",
          CountLine("step=stays_on", 2));
    CHECK(!FirstOff(), "not held");
    OnKilled(kDmgHandle & 0xffff);
    CHECK(CountLine("step=stays_on", 2) == 2, "a 'K' on a held drone with the off list full did not warn that it stays on (%d)",
          CountLine("step=stays_on", 2));
  }
  {  // a drone Enter in another game space clears the table (D10): the open hold goes off at the next tick, not dropped
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    CHECK(!FirstOff(), "not held");
    g_space_now = g_spacemem2;
    ND on = Drone(kSound, kSound, 1);
    CallEnter(&on);
    Frame();
    CHECK(NcaOffs() == 1, "a hold the table's clear took away stayed held");
  }
  {  // the scope ends with a hold open: one Warning says dronekill forgot it and it stays on
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    CHECK(!FirstOff(), "not held");
    LoadLevels({"mpl_arena_a"});
    TickAsks(kLeaveAsks);
    CHECK(!InCombatScope(), "the arena level did not end the scope");
    CHECK(HasLine("step=dropped", 2) && HasLine("count=1") && HasLine("stay on"), "the open hold was dropped without a Warning that it stays on");
    CHECK(!HasLine("unloaded"), "the drop line claims the game space is unloaded, which nothing established");
    CHECK(NcaOffs() == 0, "a dropped hold was turned off");
  }
  {  // a read that fails once while the map is still loaded drops nothing: the hold goes off on time afterwards
    CHECK(Reset(Held()).ok, "install");
    ArmExplosion();
    Release();
    const uint64_t t0 = g_ms;
    CHECK(!FirstOff(), "not held");
    LoadLevels({});  // the game reads as no level (an unreadable table reads the same)
    TickAsks(2);
    CHECK(InCombatScope() && !HasLine("step=dropped"), "two failed reads ended the scope or dropped a hold");
    LoadLevels({"mpl_combat_dyson"});
    TickAsks(1);  // the map reads again: the count starts over
    LoadLevels({});
    TickAsks(2);
    CHECK(InCombatScope() && !HasLine("step=dropped"), "the no-level count was not reset by a read that found the map");
    LoadLevels({"mpl_combat_dyson"});
    g_ms = t0 + 2500;
    TickAsks(1);
    CHECK(NcaOffs() == 1 && HasLine("step=let_go") && !HasLine("step=dropped"), "a hold across two failed reads was not let go on time (%d)", NcaOffs());
  }
}

int main() {
  sentinel::SetLogSink(&Sink);
  TestCore();
  TestPinnedConstants();
  TestNoThreadKey();
  TestInstallPinned();
  TestInstallVariants();
  TestWrappersWithoutReal();
  TestEnterOther();
  TestDroneEnterKill();
  TestNestedEnter();
  TestStateIsPerThread();
  TestKillRules();
  TestEnterLookups();
  TestGun();
  TestD11DroneEnd();
  TestD11KilledThenExpired();
  TestD11LogCap();
  TestStatsRecord();
  TestStatsRecordLiveEnd();
  TestStatsPeriodic();
  TestLockOn();
  TestFire();
  TestChase();
  TestExplosion();
  TestExplosionHoldOff();
  TestExplosionDeadEndsWhileAnotherLive();
  TestExplosionHoldNeedsOff();
  TestArenaLevelsAreLeftAlone();
  TestScopeEndsAtTheLevelChange();
  TestAStaleAnswerIsNoCombatMap();
  TestAStoppedTickPassesEveryCallThrough();
  TestTheRecordDecidesNotTheLiveRead();
  TestProductionStep();
  if (g_fail == 0) std::printf("dronekill_test: all checks passed\n");
  else std::printf("dronekill_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}

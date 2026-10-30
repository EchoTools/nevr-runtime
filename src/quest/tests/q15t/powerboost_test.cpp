// Host test of powerboost (src/quest/plugins/q15t/src/powerboost.cpp + powerboost_core.h, rows R51..R56): the real install path (import_hook +
// got_swap, real mprotect and /proc/self/maps) against a fake game-library image (src/quest/tests/q15t/pbhud_image.h) with every slot at its
// real ELF vaddr; the engine functions are fakes that record their arguments. The arm64 thunk cannot run on this x86 host
// (src/quest/tests/q15t/pb_thunk_qemu.cpp runs it under qemu); here the plain function it calls (ConeAt) is tested directly.
#include <sys/mman.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "pbhud_image.h"
#include "plugin_log.h"
#include "powerboost.h"
#include "powerboost_core.h"

using namespace q15t;

// the thunk's two words (src/quest/plugins/q15t/src/powerboost.cpp; read by src/quest/plugins/q15t/src/powerboost_thunk.inc)
extern "C" std::atomic<uintptr_t> q15t_pb_cone_ret;
extern "C" std::atomic<uintptr_t> q15t_pb_resolve_real;

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
  if (std::getenv("PBHUD_LOG")) std::printf("LOG level=%u %s\n", level, t.c_str());  // PBHUD_LOG=1: every record as it is emitted
  g_lines.push_back({level, t});
}
static int Count(const char* needle, int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++n;
  return n;
}

// ELF vaddrs of the pinned build
static const uintptr_t kSlotNode = 0x36e3868, kFnNode = 0x23207b0;
static const uintptr_t kSlotHeal = 0x36f7820, kGotHeal = 0x3725b80, kFnHeal = 0x2137e74;
static const uintptr_t kSlotLink = 0x36d7d08, kFnLink = 0x2158954;
static const uintptr_t kSlotSend = 0x36e5778, kFnSend = 0x213e80c;
static const uintptr_t kSlotResolve = 0x36ca098, kFnResolve = 0x19a27ac;
static const uintptr_t kConeCall = 0x2157998, kConeRet = 0x215799c;
static const uint32_t kBlWord = 0x97b63672u;
static const uint64_t kDmgKey = 0xc9dc08cfc2f5920full;
static const uintptr_t kDmgVptr = 0x3693ea0;

// ---- fakes
struct HealCall {
  void* cs;
  uint64_t handle;
  float amount;
  uint64_t user;
};
struct SendCall {
  void* cs;
  uint64_t handle;
  float amount;
  uint32_t hit;
  uint64_t sym;
};
static std::vector<HealCall> g_heals;
static std::vector<SendCall> g_sends;
static int g_node_calls = 0, g_link_calls = 0;
static const void *g_node_arg = nullptr, *g_node_data = nullptr;
static const void *g_link_cs = nullptr, *g_link_target = nullptr, *g_link_pos = nullptr;
static uint64_t g_link_bubble = 0, g_link_argv = 0;
static bool g_node_heals = false;       // the fake node Enter takes its direct path: it calls Heal through the slot
static pbhud::Image* g_img = nullptr;

using HealFn = void (*)(void*, uint64_t, float, uint64_t);
using NodeFn = void (*)(void*, const uint8_t*);
using LinkFn = void (*)(void*, uint64_t, const void*, const void*, uint64_t);

static void FakeHeal(void* cs, uint64_t handle, float amount, uint64_t user) { g_heals.push_back({cs, handle, amount, user}); }
static void FakeNode(void* node, const uint8_t* data) {
  ++g_node_calls;
  g_node_arg = node;
  g_node_data = data;
  if (g_node_heals && data != nullptr) {
    float amount;
    uint32_t user;
    std::memcpy(&amount, data + 0x20, 4);
    std::memcpy(&user, data + 0x24, 4);
    reinterpret_cast<HealFn>(g_img->Get(kSlotHeal))(reinterpret_cast<void*>(0xc0), 0x10007, amount, user);
  }
}
static void FakeLink(void* cs, uint64_t bubble, const void* target, const void* pos, uint64_t arg) {
  ++g_link_calls;
  g_link_cs = cs;
  g_link_bubble = bubble;
  g_link_target = target;
  g_link_pos = pos;
  g_link_argv = arg;
}
static void FakeSend(void* cs, uint64_t handle, float amount, uint32_t hit, uint64_t sym) { g_sends.push_back({cs, handle, amount, hit, sym}); }

// cone world: bodies by key, the owner's key is 100
alignas(16) static uint8_t g_body_a[0xf40], g_body_b[0xf40], g_body_owner[0xf40];
alignas(16) static uint8_t g_frame[0x100], g_rec[0x60], g_phys[32];
static uint64_t g_primary_physics = 0;
static uint32_t g_primary_handle = 0;
static int g_resolve_calls = 0;
static const uint8_t* FakeResolve(const PhTarget* t) {
  ++g_resolve_calls;
  switch (t->w[0]) {
    case 1: return g_body_a;
    case 2: return g_body_b;
    case 100: return g_body_owner;
    default: return nullptr;
  }
}
static uint64_t g_owner_key = 100;
static PhTarget FakePrimary(const void* physics, uint32_t handle) {
  g_primary_physics = reinterpret_cast<uint64_t>(physics);
  g_primary_handle = handle;
  return PhTarget{{g_owner_key, 0, 0}};
}

static uint64_t g_now = 1000;
static uint64_t Clock() { return g_now; }

static PowerboostFns Fns() {
  PowerboostFns f{};
  f.send = &FakeSend;
  f.resolve = &FakeResolve;
  f.primary = &FakePrimary;
  return f;
}

static void SetPos(uint8_t* body, float x, float y, float z) {
  const float p[3] = {x, y, z};
  std::memcpy(body + 0x8f8, p, 12);
}
static void SetQuat(uint8_t* body, float x, float y, float z, float w) {
  const float q[4] = {x, y, z, w};
  std::memcpy(body + 0xbd0, q, 16);
}

// the image with every row's words at their real ELF vaddrs
static void SetupImage(pbhud::Image& img) {
  img.Real(kFnNode, reinterpret_cast<void*>(&FakeNode));
  img.Real(kFnHeal, reinterpret_cast<void*>(&FakeHeal));
  img.Real(kFnLink, reinterpret_cast<void*>(&FakeLink));
  img.Real(kFnResolve, reinterpret_cast<void*>(&FakeResolve));
  img.Slot(kSlotNode, kFnNode);
  img.Slot(kSlotHeal, kFnHeal);
  img.Slot(kGotHeal, kFnHeal);
  img.Slot(kSlotLink, kFnLink);
  img.Slot(kSlotSend, kFnSend);
  img.Slot(kSlotResolve, kFnResolve);
  img.Word32(kConeCall) = kBlWord;
}

static void Fresh(pbhud::Image& img) {
  g_lines.clear();
  g_heals.clear();
  g_sends.clear();
  g_node_calls = g_link_calls = 0;
  g_node_heals = false;
  g_resolve_calls = 0;
  g_now = 1000;
  g_img = &img;
  ResetPowerboostForTest();
  SetPowerboostClockForTest(&Clock);
  SetupImage(img);
}

static uintptr_t Slot(pbhud::Image& img, uintptr_t s) { return img.Get(s); }
static NodeFn NodeSlot(pbhud::Image& img) { return reinterpret_cast<NodeFn>(img.Get(kSlotNode)); }
static HealFn HealSlot(pbhud::Image& img) { return reinterpret_cast<HealFn>(img.Get(kSlotHeal)); }
static HealFn QueuedSlot(pbhud::Image& img) { return reinterpret_cast<HealFn>(img.Get(kGotHeal)); }
static LinkFn LinkSlot(pbhud::Image& img) { return reinterpret_cast<LinkFn>(img.Get(kSlotLink)); }

struct alignas(16) NodeData {
  uint8_t b[0x40];
};
static NodeData Data(float amount, uint32_t user) {
  NodeData d;
  std::memset(d.b, 0, sizeof d.b);
  std::memcpy(d.b + 0x20, &amount, 4);
  std::memcpy(d.b + 0x24, &user, 4);
  return d;
}

// the damageable component system, found the way the game finds it: game tables -> gamespace -> system array
alignas(16) static uintptr_t g_game[0x2000 / 8], g_gs[0x500 / 8], g_table[2], g_sys[4], g_dcs[0x140 / 8];
static void BuildWorld(pbhud::Image& img, bool with_dcs) {
  std::memset(g_game, 0, sizeof g_game);
  std::memset(g_gs, 0, sizeof g_gs);
  std::memset(g_dcs, 0, sizeof g_dcs);
  const uintptr_t game = reinterpret_cast<uintptr_t>(g_game), gs = reinterpret_cast<uintptr_t>(g_gs), dcs = reinterpret_cast<uintptr_t>(g_dcs);
  g_table[0] = 0x5555;
  g_table[1] = gs;
  g_game[0x1f48 / 8] = reinterpret_cast<uintptr_t>(g_table);
  g_game[0x1f78 / 8] = 1;
  g_gs[0x8 / 8] = game;
  g_gs[0x420 / 8] = reinterpret_cast<uintptr_t>(g_sys);
  g_gs[0x450 / 8] = with_dcs ? 1 : 0;
  g_sys[0] = kDmgKey;
  g_sys[1] = dcs;
  g_dcs[0] = img.base + kDmgVptr;
  g_dcs[0x80 / 8] = gs;
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  SetLogContext(&ctx);
  pbhud::Image img;
  Fresh(img);
  void* node = reinterpret_cast<void*>(0x5000);
  void* cs = reinterpret_cast<void*>(0xc0);

  // ---- the marker decode (powerboost_core.h)
  {
    using namespace pbcore;
    struct T {
      float amount;
      int kind, rez;
      uint32_t v, dr, s;
    } t[] = {
        {1250030.0f, 1, 0, 250030, 250, 30}, {2000045.0f, 2, 0, 45, 0, 45},     {3500007.0f, 1, 1, 500007, 500, 7},
        {4000001.0f, 2, 1, 1, 0, 1},         {6000000.0f, 3, 0, 0, 0, 0},      {6000123.0f, 3, 0, 123, 0, 0},
        {999999.0f, 0, 0, 0, 0, 0},          {5000000.0f, 0, 0, 0, 0, 0},      {5999999.0f, 0, 0, 0, 0, 0},
        {7000000.0f, 0, 0, 0, 0, 0},         {1e9f, 0, 0, 0, 0, 0},            {-1250030.0f, 0, 0, 0, 0, 0},
        {std::nanf(""), 0, 0, 0, 0, 0},      {INFINITY, 0, 0, 0, 0, 0},        {1000000.0f, 1, 0, 0, 0, 0},
    };
    for (const T& c : t) {
      const Marker m = Decode(c.amount);
      CHECK(m.kind == c.kind && m.rez == c.rez && m.v == c.v && m.drPermille == c.dr && m.seconds == c.s, "Decode(%g): kind %d rez %d v %u dr %u s %u",
            (double)c.amount, m.kind, m.rez, m.v, m.drPermille, m.seconds);
    }
  }

  // ---- off: nothing installed
  {
    const InstallResult r = InstallPowerboost(img.base, 0, true, nullptr);
    CHECK(std::strcmp(r.reason, "off") == 0, "off reason");
    CHECK(Slot(img, kSlotNode) == img.base + kFnNode && Slot(img, kSlotHeal) == img.base + kFnHeal && Slot(img, kSlotLink) == img.base + kFnLink, "off changed a slot");
    CHECK(Count("powerboost: off", 1) == 1 && GetPowerboostStats().stage == 0, "off not logged");
  }

  // ---- stage 1 (read-only): three words swapped, nothing else
  Fresh(img);
  PowerboostFns fns = Fns();
  InstallResult r = InstallPowerboost(img.base, kPowerboostRead, false, &fns);
  CHECK(r.status == SwapStatus::kOk && std::strcmp(r.reason, "") == 0, "stage 1 install: %s %s", StatusName(r.status), r.reason);
  CHECK(Slot(img, kSlotNode) != img.base + kFnNode && Slot(img, kSlotHeal) != img.base + kFnHeal && Slot(img, kSlotLink) != img.base + kFnLink, "a stage-1 slot not swapped");
  CHECK(Slot(img, kGotHeal) == img.base + kFnHeal && Slot(img, kSlotResolve) == img.base + kFnResolve && Slot(img, kSlotSend) == img.base + kFnSend, "stage 1 touched GLOB_DAT / ResolveBody / sender slot");
  CHECK(Count("powerboost: hook install", 1) == 3 && Count("powerboost: on", 1) == 1 && GetPowerboostStats().stage == 1, "stage 1 lines: %d", Count("powerboost: hook install", 1));

  // node: arguments and result pass through; a marker amount logs one line AFTER the real call, other amounts none
  {
    NodeData d = Data(1250030.0f, 77);
    NodeSlot(img)(node, d.b);
    CHECK(g_node_calls == 1 && g_node_arg == node && g_node_data == d.b, "node: real not called with the same arguments");
    CHECK(Count("powerboost: marker passed on") == 1 && Count("where=node") == 1 && Count("kind=1") == 1 && Count("user=77") == 1 && Count("seconds=30") == 1,
          "node marker line");
    g_lines.clear();
    d = Data(50.0f, 1);
    NodeSlot(img)(node, d.b);
    d = Data(5500000.0f, 1);  // [5e6, 6e6): above the minimum but not a marker
    NodeSlot(img)(node, d.b);
    CHECK(g_node_calls == 3 && Count("passed on") == 1 && Count("amount is not a marker") == 1, "non-marker amounts: %zu lines", g_lines.size());
    NodeSlot(img)(node, nullptr);  // an unreadable data argument: the real function still runs with it, no fault
    CHECK(g_node_calls == 4 && g_node_data == nullptr, "null data not passed on");
  }
  // heal: the float, the handle and the user go through unchanged; marker lines carry the index
  {
    g_lines.clear();
    HealSlot(img)(cs, 0x2000a, 1500000.0f, 9);
    CHECK(g_heals.size() == 1 && g_heals[0].cs == cs && g_heals[0].handle == 0x2000a && g_heals[0].amount == 1500000.0f && g_heals[0].user == 9, "heal: args changed");
    CHECK(Count("where=heal") == 1 && Count("index=10") == 1 && Count("kind=1") == 1, "heal marker line: %s", g_lines.empty() ? "" : g_lines[0].text.c_str());
    g_lines.clear();
    HealSlot(img)(cs, 3, 40.0f, 9);
    CHECK(g_heals.size() == 2 && g_lines.empty(), "an ordinary heal logged or changed");
  }
  // the node's marker is passed on in stage 1 even on the direct path: heal line, then node line
  {
    g_lines.clear();
    g_heals.clear();
    g_node_heals = true;
    NodeData d = Data(2000045.0f, 5);
    NodeSlot(img)(node, d.b);
    g_node_heals = false;
    CHECK(g_heals.size() == 1 && g_heals[0].amount == 2000045.0f, "stage 1 swallowed a heal");
    CHECK(g_lines.size() == 2 && g_lines[0].text.find("where=heal") != std::string::npos && g_lines[1].text.find("where=node") != std::string::npos, "heal line then node line");
  }
  // no per-call logging on the high-fan-in path
  {
    g_lines.clear();
    for (int i = 0; i < 5000; ++i) HealSlot(img)(cs, 3, 40.0f, 9);
    CHECK(g_lines.empty(), "per-call lines in Heal: %zu", g_lines.size());
  }
  // marker lines are capped (96), with one cap line announcing it
  {
    const int before = static_cast<int>(GetPowerboostStats().markers);  // marker lines so far (each counts against the cap of 96)
    g_lines.clear();
    for (int i = 0; i < 200; ++i) HealSlot(img)(cs, 3, 1000001.0f, 9);
    CHECK(Count("powerboost: marker passed on") == 96 - before && Count("lines capped", 1) == 1, "marker cap: %d lines, %d cap lines",
          Count("powerboost: marker passed on"), Count("lines capped"));
    CHECK(GetPowerboostStats().markers >= 200, "counter");
  }
  // link: the real call first; one line per (bubble, target) change, a repeat only after 1 s
  {
    g_lines.clear();
    alignas(16) uint8_t target[0x60] = {};
    const uint32_t h1 = 0x30011, h2 = 0x30012;
    float pos[3] = {1.5f, 2.5f, 3.5f};
    std::memcpy(target + 0x48, &h1, 4);
    LinkSlot(img)(cs, 0x50005, target, pos, 3);
    CHECK(g_link_calls == 1 && g_link_cs == cs && g_link_bubble == 0x50005 && g_link_target == target && g_link_pos == pos && g_link_argv == 3, "link args");
    CHECK(Count("powerboost: link") == 1 && Count("bubble=5") == 1 && Count("target=17") == 1 && Count("x=1.5") == 1 && Count("z=3.5") == 1, "link line: %s",
          g_lines.empty() ? "" : g_lines[0].text.c_str());
    g_now += 500;
    LinkSlot(img)(cs, 0x50005, target, pos, 3);
    CHECK(Count("powerboost: link") == 1, "repeat within 1 s logged");
    g_now += 600;
    LinkSlot(img)(cs, 0x50005, target, pos, 3);
    CHECK(Count("powerboost: link") == 2, "repeat after 1 s not logged");
    std::memcpy(target + 0x48, &h2, 4);
    LinkSlot(img)(cs, 0x50005, target, pos, 3);
    CHECK(Count("powerboost: link") == 3, "a new target not logged");
    LinkSlot(img)(cs, 0x50005, target, reinterpret_cast<const void*>(0x1000), 3);  // unreadable position
    g_now += 2000;
    LinkSlot(img)(cs, 0x50005, target, reinterpret_cast<const void*>(0x1000), 3);
    CHECK(Count("at=unreadable") == 1 && g_link_calls == 6, "unreadable position: %d lines, %d calls", Count("at=unreadable"), g_link_calls);
    // the target pointer itself unreadable: still passed on, no fault
    LinkSlot(img)(cs, 0x50006, reinterpret_cast<const void*>(0x1000), pos, 3);
    CHECK(g_link_calls == 7, "link with an unreadable target not passed on");
    // line cap
    g_lines.clear();
    for (int i = 0; i < 400; ++i) {
      g_now += 1100;
      LinkSlot(img)(cs, 0x50005, target, pos, 3);
    }
    CHECK(Count("powerboost: link", 1) <= 160 && Count("powerboost: lines capped") == 1, "link cap: %d", Count("powerboost: link", 1));
    CHECK(GetPowerboostStats().links >= 400, "link counter");
  }

  // ---- all three words or none: a later failure puts the earlier words back
  for (int bad = 1; bad <= 2; ++bad) {
    Fresh(img);
    const uintptr_t badslot = bad == 1 ? kSlotHeal : kSlotLink;
    img.SetSlot(badslot, 0x4242420);
    r = InstallPowerboost(img.base, kPowerboostRead, false, &fns);
    CHECK(r.status == SwapStatus::kUnexpectedOriginal && std::strcmp(r.reason, "hook_failed") == 0, "bad %d: %s %s", bad, StatusName(r.status), r.reason);
    CHECK(Slot(img, kSlotNode) == img.base + kFnNode, "bad %d: the node word was not put back", bad);
    CHECK(bad == 2 ? Slot(img, kSlotHeal) == img.base + kFnHeal : true, "bad %d: the heal word was not put back", bad);
    CHECK(Slot(img, badslot) == 0x4242420, "the foreign word was touched");
    CHECK(Count("powerboost: OFF", 3) == 1 && GetPowerboostStats().stage == 0, "no ERROR / stage after a refused install");
    CHECK(Count("powerboost: hook install", 3) == 1, "bad %d: the refused hook is not an ERROR line of its own", bad);
  }
  // a first-hook failure leaves everything alone
  Fresh(img);
  img.SetSlot(kSlotNode, 0x4242420);
  r = InstallPowerboost(img.base, kPowerboostRead, false, &fns);
  CHECK(r.status == SwapStatus::kUnexpectedOriginal && Slot(img, kSlotHeal) == img.base + kFnHeal && Slot(img, kSlotLink) == img.base + kFnLink, "first-hook failure touched other slots");

  // ---- stage 2 (swallow)
  Fresh(img);
  r = InstallPowerboost(img.base, kPowerboostSwallow, false, &fns);
  CHECK(r.status == SwapStatus::kOk && std::strcmp(r.reason, "") == 0 && GetPowerboostStats().stage == 2, "stage 2 install: %s %s", StatusName(r.status), r.reason);
  CHECK(Slot(img, kGotHeal) != img.base + kFnHeal && Slot(img, kGotHeal) != Slot(img, kSlotHeal) && Slot(img, kSlotSend) == img.base + kFnSend &&
            Slot(img, kSlotResolve) == img.base + kFnResolve,
        "GLOB_DAT word not swapped to its own wrapper / other slots touched");
  CHECK(Count("powerboost: swallow on", 1) == 1 && Count("powerboost: hook install", 1) == 4, "stage 2 lines");
  // the node's direct path: the marker never reaches Heal, a kind 1 is held for the next queued heal
  {
    g_lines.clear();
    g_heals.clear();
    g_node_heals = true;
    NodeData d = Data(1250030.0f, 5);
    NodeSlot(img)(node, d.b);
    g_node_heals = false;
    CHECK(g_heals.empty() && g_sends.empty(), "a marker reached Heal / was sent by the direct path");
    CHECK(Count("held for the next queued heal") == 1 && Count("where=node heal") == 1, "held line");
    CHECK(GetPowerboostStats().held == 1 && GetPowerboostStats().swallowed == 1, "held / swallowed counters");
    // the next queued heal (any amount) sends the held use: DamageFromClient(cs, handle, -amount, 0, 0), then runs that heal
    g_now += 500;
    QueuedSlot(img)(cs, 0x10009, 50.0f, 3);
    CHECK(g_sends.size() == 1 && g_sends[0].cs == cs && g_sends[0].handle == 0x10007 && g_sends[0].amount == -1250030.0f && g_sends[0].hit == 0 && g_sends[0].sym == 0,
          "held use not sent exactly: %zu sends", g_sends.size());
    CHECK(g_heals.size() == 1 && g_heals[0].amount == 50.0f && g_heals[0].handle == 0x10009 && g_heals[0].user == 3, "the ordinary queued heal did not run unchanged");
    CHECK(Count("use sent to the server") == 1 && Count("why=held from the node's direct heal") == 1, "sent line");
    // it is sent once
    QueuedSlot(img)(cs, 0x10009, 51.0f, 3);
    CHECK(g_sends.size() == 1 && g_heals.size() == 2, "held use sent twice");
  }
  // a held use older than 2 s, or for another damageable system, is dropped, not sent
  {
    for (int variant = 0; variant < 2; ++variant) {
      g_sends.clear();
      g_lines.clear();
      g_node_heals = true;
      NodeData d = Data(1250030.0f, 5);
      NodeSlot(img)(node, d.b);
      g_node_heals = false;
      g_now += variant == 0 ? 2000 : 100;  // exactly the hold: not "younger than"
      QueuedSlot(img)(variant == 0 ? cs : reinterpret_cast<void*>(0xd0), 0x10009, 50.0f, 3);
      CHECK(g_sends.empty() && Count("held use dropped") == 1, "variant %d: held use sent / not reported as dropped", variant);
      CHECK(Count(variant == 0 ? "no queued heal within the hold" : "another damageable system") == 1, "variant %d: drop reason", variant);
    }
    // a second direct kind 1 replaces the held one
    g_lines.clear();
    g_node_heals = true;
    NodeData d = Data(1250030.0f, 5);
    NodeSlot(img)(node, d.b);
    NodeSlot(img)(node, d.b);
    g_node_heals = false;
    CHECK(Count("an older held one dropped") == 1, "replaced held use not reported");
    QueuedSlot(img)(cs, 1, 1.0f, 1);  // drain it
  }
  // kinds 2 and 3 on the node's direct path, and any server-relayed marker through Heal: swallowed, never sent, never held
  {
    g_sends.clear();
    g_heals.clear();
    g_lines.clear();
    g_node_heals = true;
    NodeData d2 = Data(2000045.0f, 5), d3 = Data(6000000.0f, 5);
    NodeSlot(img)(node, d2.b);
    NodeSlot(img)(node, d3.b);
    g_node_heals = false;
    HealSlot(img)(cs, 0x10001, 1250030.0f, 5);  // relayed by the server (the DamageDealt callback): kind 1, not in the node
    HealSlot(img)(cs, 0x10001, 3250030.0f, 5);
    CHECK(g_heals.empty() && g_sends.empty(), "a marker reached Heal or was sent: %zu heals %zu sends", g_heals.size(), g_sends.size());
    CHECK(Count("relayed by the server: never re-sent") == 2 && Count("what=nothing sent") == 2, "swallow lines");
    QueuedSlot(img)(cs, 1, 1.0f, 1);
    CHECK(g_sends.empty() && g_heals.size() == 1, "something was held / sent after those");
    // an amount in [5e6, 6e6) or 7e6+ is not a marker: it is an ordinary heal and is passed on
    g_heals.clear();
    HealSlot(img)(cs, 1, 5500000.0f, 1);
    HealSlot(img)(cs, 1, 7500000.0f, 1);
    CHECK(g_heals.size() == 2, "non-marker amounts swallowed");
  }
  // the queued path directly: kind 1 is sent at once, kinds 2 / 3 swallowed, ordinary heals run
  {
    g_sends.clear();
    g_heals.clear();
    QueuedSlot(img)(cs, 0x10003, 3500007.0f, 1);
    CHECK(g_sends.size() == 1 && g_sends[0].handle == 0x10003 && g_sends[0].amount == -3500007.0f && g_heals.empty(), "queued kind 1 (rez) not sent as -amount");
    QueuedSlot(img)(cs, 0x10003, 4000001.0f, 1);
    QueuedSlot(img)(cs, 0x10003, 6000000.0f, 1);
    CHECK(g_sends.size() == 1 && g_heals.empty(), "queued kind 2 / 3 sent or run");
    QueuedSlot(img)(cs, 0x10003, 12.0f, 1);
    CHECK(g_sends.size() == 1 && g_heals.size() == 1 && g_heals[0].amount == 12.0f, "ordinary queued heal not run");
    CHECK(GetPowerboostStats().sent >= 2, "sent counter");
  }

  // the queued-heal word with swallow off (the window between its swap and the flag): it only passes everything through
  {
    g_heals.clear();
    g_sends.clear();
    PowerboostSwallowForTest(false);
    QueuedSlot(img)(cs, 0x10003, 1250030.0f, 1);
    QueuedSlot(img)(cs, 0x10003, 12.0f, 1);
    CHECK(g_heals.size() == 2 && g_heals[0].amount == 1250030.0f && g_sends.empty(), "queued word with swallow off did not pass everything through");
    PowerboostSwallowForTest(true);
  }

  // ---- stage 2 refusals leave stage 1 running and are loud
  struct Refuse {
    const char* what;
    int variant;
  } refusals[] = {{"sender's slot holds something else", 0}, {"Heal's GLOB_DAT holds something else", 1}, {"no sender function", 2}};
  for (const Refuse& rf : refusals) {
    Fresh(img);
    PowerboostFns f2 = Fns();
    if (rf.variant == 0) img.SetSlot(kSlotSend, 0x4242420);
    if (rf.variant == 1) img.SetSlot(kGotHeal, 0x4242420);
    if (rf.variant == 2) f2.send = nullptr;
    r = InstallPowerboost(img.base, kPowerboostSwallow, false, &f2);
    CHECK(r.status == SwapStatus::kOk && std::strcmp(r.reason, "swallow_off") == 0 && GetPowerboostStats().stage == 1, "%s: %s %s stage %d", rf.what, StatusName(r.status), r.reason,
          GetPowerboostStats().stage);
    CHECK(Count("powerboost: swallow OFF", 3) == 1, "%s: not an ERROR", rf.what);
    CHECK(Slot(img, kSlotNode) != img.base + kFnNode, "%s: stage 1 words are not in", rf.what);
    if (rf.variant == 1) CHECK(Slot(img, kGotHeal) == 0x4242420, "foreign GLOB_DAT word touched");
    // with swallow off, a marker passes through Heal as in stage 1
    HealSlot(img)(cs, 1, 1250030.0f, 1);
    CHECK(g_heals.size() == 1, "%s: a marker was swallowed although swallow is off", rf.what);
  }
  // engine functions that cannot be resolved (no libr15 on this host): stage 1 only, ERROR
  Fresh(img);
  r = InstallPowerboost(img.base, kPowerboostCone, true, nullptr);
  CHECK(std::strcmp(r.reason, "swallow_off") == 0 && GetPowerboostStats().stage == 1 && Count("engine functions not resolved", 3) == 1, "unresolved engine functions: %s stage %d",
        r.reason, GetPowerboostStats().stage);
  {
    PowerboostFns none{};
    CHECK(!ResolvePowerboostFns(img.base, &none), "ResolvePowerboostFns succeeded without a game library");
  }

  // ---- stage 3 (cone): the install
  Fresh(img);
  r = InstallPowerboost(img.base, kPowerboostCone, true, &fns);
  CHECK(r.status == SwapStatus::kOk && std::strcmp(r.reason, "") == 0 && GetPowerboostStats().stage == 3, "stage 3 install: %s %s stage %d", StatusName(r.status), r.reason,
        GetPowerboostStats().stage);
  CHECK(Slot(img, kSlotResolve) != img.base + kFnResolve && Slot(img, kSlotResolve) != 0, "ResolveBody's word not swapped");
  CHECK(Count("powerboost: cone on", 1) == 1 && Count("powerboost: hook install", 1) == 5, "stage 3 lines");
  CHECK(Count("return_address_elf_vaddr=0x215799c") == 1, "the return address is not named in the log");
  CHECK(GetPowerboostStats().cone_armed, "stage 3 is on but the cone is not armed");
  CHECK(q15t_pb_cone_ret.load() == img.base + kConeRet, "the thunk's return-address word is not base + 0x215799c");
  CHECK(q15t_pb_resolve_real.load() == img.base + kFnResolve, "the thunk's real-function word is not the real ResolveBody");
  {
    // the thunk's two words: the real ResolveBody (through the slot's expected word) and the return address that arms the cone
    // (visible through a pass-through call: the host stand-in calls the real function)
    const auto resolve = reinterpret_cast<const uint8_t* (*)(const PhTarget*)>(img.Get(kSlotResolve));
    PhTarget t{{1, 0, 0}};
    CHECK(resolve(&t) == g_body_a && g_resolve_calls == 1, "the swapped word does not reach the real ResolveBody");
  }
  // refusals leave stage 2 on, the ResolveBody word alone, and are loud
  struct ConeRefuse {
    const char* what;
    int variant;
  } cone_refusals[] = {{"no net tick wired", 0}, {"call site is not the bl", 1}, {"ResolveBody slot holds something else", 2}, {"no PrimaryBody", 3}};
  for (const ConeRefuse& rf : cone_refusals) {
    Fresh(img);
    PowerboostFns f3 = Fns();
    if (rf.variant == 1) img.Word32(kConeCall) = kBlWord ^ 1;
    if (rf.variant == 2) img.SetSlot(kSlotResolve, 0x4242420);
    if (rf.variant == 3) f3.primary = nullptr;
    r = InstallPowerboost(img.base, kPowerboostCone, rf.variant != 0, &f3);
    CHECK(std::strcmp(r.reason, "cone_off") == 0 && GetPowerboostStats().stage == 2 && Count("powerboost: cone OFF", 3) == 1, "%s: reason %s stage %d", rf.what, r.reason,
          GetPowerboostStats().stage);
    CHECK(rf.variant == 2 ? Slot(img, kSlotResolve) == 0x4242420 : Slot(img, kSlotResolve) == img.base + kFnResolve, "%s: ResolveBody's word was touched", rf.what);
    CHECK(!GetPowerboostStats().cone_armed && q15t_pb_cone_ret.load() == 0, "%s: the cone is armed although it was refused", rf.what);
    CHECK(Count("powerboost: swallow on", 1) == 1, "%s: stage 2 lost", rf.what);
  }

  // ---- stage 3: ConeAt (what the thunk calls for the one call it acts on)
  Fresh(img);
  r = InstallPowerboost(img.base, kPowerboostCone, true, &fns);
  CHECK(GetPowerboostStats().stage == 3, "stage 3 for the cone tests");
  BuildWorld(img, true);
  PowerboostGameTick(reinterpret_cast<uintptr_t>(g_game));
  {
    const uintptr_t dcs = reinterpret_cast<uintptr_t>(g_dcs);
    std::memset(g_body_a, 0, sizeof g_body_a);
    std::memset(g_body_b, 0, sizeof g_body_b);
    std::memset(g_body_owner, 0, sizeof g_body_owner);
    SetPos(g_body_owner, 100, 100, 100);
    SetQuat(g_body_owner, 0, 0, 0, 1);  // identity: the owner's +z is world +z
    // the frame as UpdateActiveHealBubble leaves it at the call: [sp+0xe8] -> radius (record + 0x38), [sp+0xa0] = the target's handle word
    std::memset(g_frame, 0, sizeof g_frame);
    std::memset(g_rec, 0, sizeof g_rec);
    const uint64_t physics = reinterpret_cast<uint64_t>(g_phys);
    std::memcpy(g_rec + 0, &physics, 8);
    const uint32_t owner_handle = 0x77;
    std::memcpy(g_rec + 8, &owner_handle, 4);
    const float radius = 15.0f + 60.0f / 100000.0f;  // 15 m, a 60 degree cone
    std::memcpy(g_rec + 0x38, &radius, 4);
    const uint64_t radius_at = reinterpret_cast<uint64_t>(g_rec + 0x38);
    std::memcpy(g_frame + 0xe8, &radius_at, 8);
    const uint32_t index = 0x40021;
    std::memcpy(g_frame + 0xa0, &index, 4);
    const PhTarget ta{{1, 0, 0}}, tb{{2, 0, 0}};

    // a target 10 m straight ahead of the owner: linked, the real body is returned, a link send is queued
    SetPos(g_body_a, 100, 100, 110);
    g_now = 5000;
    g_resolve_calls = 0;
    const uint8_t* got = PowerboostConeAtForTest(&ta, g_frame, dcs);
    CHECK(got == g_body_a, "a target in the cone did not get its own body");
    CHECK(g_primary_physics == physics && g_primary_handle == owner_handle, "the owner was not asked for with the record's physics system and handle");
    CHECK(g_resolve_calls == 2, "ResolveBody calls: %d (target, owner)", g_resolve_calls);
    // a target 10 m off to the side (90 degrees): outside the 60 degree cone: the far stand-in body
    SetPos(g_body_b, 110, 100, 100);
    got = PowerboostConeAtForTest(&tb, g_frame, dcs);
    CHECK(got != g_body_b && got != nullptr, "a target outside the cone kept its body");
    float sp[3];
    std::memcpy(sp, got + 0x8f8, 12);
    CHECK(sp[0] == 1e30f && sp[1] == 0.0f && sp[2] == 0.0f, "stand-in position %g %g %g", (double)sp[0], (double)sp[1], (double)sp[2]);
    // the stand-in is the size UpdateActiveHealBubble indexes (body stride 0xf38) and is zero but for the position
    bool zero = true;
    for (size_t i = 0; i < 0xf38; ++i)
      if ((i < 0x8f8 || i >= 0x8f8 + 12) && got[i] != 0) zero = false;
    CHECK(zero, "stand-in has stray non-zero bytes");
    // beyond floor(radius) = 15 m, straight ahead: rejected
    SetPos(g_body_b, 100, 100, 120);
    got = PowerboostConeAtForTest(&tb, g_frame, dcs);
    CHECK(got != g_body_b, "a target beyond the range kept its body");
    // the owner itself (closer than 0.3 m): never self-linked
    SetPos(g_body_b, 100, 100, 100.1f);  // dead ahead (0 degrees) and 0.1 m away: only the 0.3 m rule rejects it
    got = PowerboostConeAtForTest(&tb, g_frame, dcs);
    CHECK(got != g_body_b, "the owner itself kept its body");
    // a bubble that is not POWER BOOST's (no angle in the radius, or an angle above 180): left alone
    const float plain = 15.0f, big = 15.0f + 200.0f / 100000.0f;
    SetPos(g_body_b, 110, 100, 100);  // 90 degrees off: a 60 degree cone would reject it
    std::memcpy(g_rec + 0x38, &plain, 4);
    CHECK(PowerboostConeAtForTest(&tb, g_frame, dcs) == g_body_b, "a bubble without an angle was filtered");
    SetPos(g_body_b, 100, 100, 130);  // 30 m ahead: out of range, but not inside any cone of an angle above 180
    std::memcpy(g_rec + 0x38, &big, 4);
    CHECK(PowerboostConeAtForTest(&tb, g_frame, dcs) == g_body_b, "an angle above 180 was filtered");
    std::memcpy(g_rec + 0x38, &radius, 4);
    // unreadable frame / radius pointer / record: the real body, nothing queued
    CHECK(PowerboostConeAtForTest(&tb, nullptr, dcs) == g_body_b, "unreadable frame: not the real body");
    uint8_t bad_frame[0x100] = {};
    const uint64_t junk = 0x1000;
    std::memcpy(bad_frame + 0xe8, &junk, 8);
    CHECK(PowerboostConeAtForTest(&tb, bad_frame, dcs) == g_body_b, "unreadable radius: not the real body");
    // ResolveBody returned null for the target: null passes (UpdateActiveHealBubble's own path), nothing else is read
    const PhTarget tnull{{0xdead, 0, 0}};
    CHECK(PowerboostConeAtForTest(&tnull, g_frame, dcs) == nullptr, "a null body was replaced");
    // the owner has no body: reject with the stand-in
    g_owner_key = 0xdead;
    got = PowerboostConeAtForTest(&ta, g_frame, dcs);
    CHECK(got != g_body_a && got != nullptr, "no owner body: not rejected");
    g_owner_key = 100;
    // physics pointer unreadable: the real body
    const uint64_t zero_phys = 0;
    std::memcpy(g_rec + 0, &zero_phys, 8);
    CHECK(PowerboostConeAtForTest(&ta, g_frame, dcs) == g_body_a, "unreadable physics system: not the real body");
    std::memcpy(g_rec + 0, &physics, 8);
    CHECK(GetPowerboostStats().cone_calls >= 10 && GetPowerboostStats().cone_rejects >= 4, "cone counters %u / %u", GetPowerboostStats().cone_calls, GetPowerboostStats().cone_rejects);
    // the diagnostic line is rate-limited (2 / s), not per call
    const int lines_before = Count("powerboost: cone target");
    for (int i = 0; i < 300; ++i) PowerboostConeAtForTest(&ta, g_frame, dcs);
    CHECK(Count("powerboost: cone target") - lines_before <= 1, "cone diagnostics per call: %d", Count("powerboost: cone target") - lines_before);

    // ---- the link / unlink sends from the net tick
    // no use of ours within 30 s: another client's bubble, nothing is sent
    PowerboostNetTick();
    CHECK(g_sends.empty(), "a link marker was sent without a use of ours");
    CHECK(Count("no use of ours within 30 s") >= 1, "quiet reason not logged");
    // our own use (a queued kind-1 heal at t = 6000 ms): dr 250, 30 s
    g_now = 6000;
    QueuedSlot(img)(cs, 0x10003, 1250030.0f, 1);
    CHECK(g_sends.size() == 1, "the use itself not sent");
    g_sends.clear();
    // a fresh bubble: a target in the cone is linked and queued; the net tick sends 2e6 + dr*1000 + the seconds left, to the live system
    std::memset(g_frame + 0xa0, 0, 4);
    const uint32_t idx2 = 0x40022;
    std::memcpy(g_frame + 0xa0, &idx2, 4);
    SetPos(g_body_a, 100, 100, 105);
    g_now = 7000;
    CHECK(PowerboostConeAtForTest(&ta, g_frame, dcs) == g_body_a, "in-cone target not linked");
    PowerboostNetTick();
    // use at 6000, link seen at 7000, tick at 7000: left = (30000 - 1000 + 999) / 1000 = 29
    bool found = false;
    for (const SendCall& s : g_sends)
      if (s.cs == reinterpret_cast<void*>(dcs) && s.handle == idx2 && s.amount == -(2000000.0f + 250.0f * 1000.0f + 29.0f) && s.hit == 0 && s.sym == 0) found = true;
    CHECK(found, "link marker not sent as DamageFromClient(dcs, index, -(2e6 + dr*1000 + left), 0, 0): %zu sends", g_sends.size());
    CHECK(Count("powerboost: link marker sent to the server") >= 1, "no sent line");
    // the target leaves the cone: an unlink marker (6e6) after the minimum interval
    g_sends.clear();
    SetPos(g_body_a, 110, 100, 100);
    g_now = 7400;
    CHECK(PowerboostConeAtForTest(&ta, g_frame, dcs) != g_body_a, "target left the cone but kept its body");
    PowerboostNetTick();
    found = false;
    for (const SendCall& s : g_sends)
      if (s.handle == idx2 && s.amount == -6000000.0f) found = true;
    CHECK(found, "unlink marker not sent");
    // nothing pending: the net tick does nothing
    g_sends.clear();
    PowerboostNetTick();
    CHECK(g_sends.empty(), "a second net tick re-sent");
    // a change older than 2 s is not sent
    g_sends.clear();
    SetPos(g_body_a, 100, 100, 105);
    g_now = 8000;
    PowerboostConeAtForTest(&ta, g_frame, dcs);
    g_now = 10500;
    PowerboostNetTick();
    CHECK(g_sends.empty() && Count("older than 2 s") >= 1, "a stale change was sent");
    // a damageable system that is no longer live (a level changed): not sent
    g_sends.clear();
    BuildWorld(img, false);
    SetPos(g_body_a, 100, 100, 105);
    g_now = 11000;  // more than 2 s after the bubble was last seen: a new bubble, the in-cone target is linked afresh
    CHECK(PowerboostConeAtForTest(&ta, g_frame, dcs) == g_body_a, "fresh link for the live-system test");
    g_now = 11100;
    PowerboostNetTick();
    CHECK(g_sends.empty() && Count("its damageable system is gone") >= 1, "sent to a system that is not live");
    BuildWorld(img, true);
    // 30 s after the use: no more link markers
    g_sends.clear();
    SetPos(g_body_a, 100, 100, 105);
    g_now = 40000;
    PowerboostConeAtForTest(&ta, g_frame, dcs);
    PowerboostNetTick();
    CHECK(g_sends.empty(), "a link marker sent more than 30 s after our use");

    // ---- a full pending table (16) drops its oldest; the rest are sent
    {
      BuildWorld(img, true);
      g_sends.clear();
      g_now = 70000;
      QueuedSlot(img)(cs, 0x10003, 1250030.0f, 1);  // a use of ours, so the link markers go out
      g_sends.clear();
      static uint8_t rec[3][0x60], fr[3][0x100];
      const unsigned dropped_before = GetPowerboostStats().pending_dropped;
      SetPos(g_body_a, 100, 100, 105);
      uint32_t n = 0;
      for (int b = 0; b < 3; ++b) {  // three bubbles (records): 8 + 8 + 1 distinct targets = 17 pending changes
        std::memcpy(rec[b], g_rec, sizeof rec[b]);
        std::memcpy(fr[b], g_frame, sizeof fr[b]);
        const uint64_t ra = reinterpret_cast<uint64_t>(rec[b] + 0x38);
        std::memcpy(fr[b] + 0xe8, &ra, 8);
        for (int k = 0; k < (b == 2 ? 1 : 8); ++k) {
          const uint32_t ix = 0x50001 + n++;
          std::memcpy(fr[b] + 0xa0, &ix, 4);
          CHECK(PowerboostConeAtForTest(&ta, fr[b], dcs) == g_body_a, "full-table test: target %u not linked", ix);
        }
      }
      CHECK(GetPowerboostStats().pending_dropped == dropped_before + 1, "17th pending change: dropped %u", GetPowerboostStats().pending_dropped - dropped_before);
      CHECK(Count("pending full: oldest dropped") >= 1, "the drop is not reported");
      g_now = 70100;
      PowerboostNetTick();
      bool first = false, last = false;
      for (const SendCall& sc : g_sends) {
        first = first || sc.handle == 0x50001;
        last = last || sc.handle == 0x50011;
      }
      CHECK(g_sends.size() == 16 && !first && last, "full table: %zu sends, oldest sent %d, newest sent %d", g_sends.size(), first, last);
    }

    // a pending change for a damageable system that is not among the live ones (another one is live): dropped, not sent
    {
      g_sends.clear();
      g_lines.clear();
      g_now = 80000;
      QueuedSlot(img)(cs, 0x10003, 1250030.0f, 1);  // a use of ours
      g_sends.clear();
      uint32_t ix = 0x70001;
      std::memcpy(g_frame + 0xa0, &ix, 4);
      CHECK(PowerboostConeAtForTest(&ta, g_frame, dcs + 0x10) == g_body_a, "stale-system test: not linked");
      g_now = 80050;
      PowerboostNetTick();
      CHECK(g_sends.empty() && Count("its damageable system is gone") >= 1, "a change for a system that is not live was sent: %zu", g_sends.size());
    }

    // a use with the rez flag (3.5e6 band): the link marker carries it (+ 2e6)
    {
      g_sends.clear();
      g_now = 90000;
      QueuedSlot(img)(cs, 0x10003, 3500007.0f, 1);  // dr 500, 7 s, rez
      g_sends.clear();
      uint32_t ix = 0x60001;
      std::memcpy(g_frame + 0xa0, &ix, 4);
      CHECK(PowerboostConeAtForTest(&ta, g_frame, dcs) == g_body_a, "rez test: not linked");
      g_now = 90010;
      PowerboostNetTick();
      bool found = false;
      for (const SendCall& sc : g_sends)
        if (sc.handle == 0x60001 && sc.amount == -(2000000.0f + 500.0f * 1000.0f + 7.0f + 2000000.0f)) found = true;
      CHECK(found, "rez link marker not 2e6 + dr*1000 + left + 2e6: %zu sends", g_sends.size());
    }
  }

  // ---- the cone's hysteresis and rate limit: linked, the cone is 5 degrees wider; a change is sent at most every 250 ms
  {
    using namespace pbcore;
    ConeTable t;
    ConeVerdict v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 10.0f, 62.0f, 1000);
    CHECK(!v.ok && !v.send, "fresh target 62 deg off in a 60 deg cone: ok %d send %d", v.ok, v.send);
    v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 10.0f, 58.0f, 1100);
    CHECK(v.ok && v.send, "58 deg: not linked / not sent");
    v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 10.0f, 62.0f, 1200);
    CHECK(v.ok && !v.send, "linked target at 62 deg (5 deg slack): ok %d send %d", v.ok, v.send);
    v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 10.0f, 66.0f, 1300);
    CHECK(!v.ok && !v.send, "66 deg is out, but the change is 200 ms after the last send: ok %d send %d", v.ok, v.send);
    v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 10.0f, 66.0f, 1400);
    CHECK(!v.ok && v.send, "300 ms after the last send the unlink goes out: ok %d send %d", v.ok, v.send);
    v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 16.0f, 0.0f, 1500);
    CHECK(!v.ok, "beyond floor(radius) = 15 m");
  }
  // without stage 3 the net tick is inert
  Fresh(img);
  InstallPowerboost(img.base, kPowerboostSwallow, false, &fns);
  PowerboostNetTick();
  CHECK(g_sends.empty(), "net tick active below stage 3");

  if (g_fail == 0) std::printf("powerboost_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}

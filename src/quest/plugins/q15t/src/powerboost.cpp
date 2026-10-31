// powerboost (rows R51..R56). Port of the source build's powerboost; see powerboost.h and docs/quest/q15t/modules/powerboost.md.
// No namespace-scope object has a constructor; every read of game memory is a SafeRead; every wrapper calls the real
// function on any doubt; the only writes are data-page GOT swaps (import_hook.h / got_swap.h).
#include "powerboost.h"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <dlfcn.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gamespace.h"
#include "pbhud_log.h"
#include "plugin_log.h"
#include "powerboost_core.h"
#include "safe_read.h"

// The two words the naked thunk reads and the function it calls (powerboost_thunk.inc). Global scope, C names, hidden.
extern "C" {
__attribute__((visibility("hidden"))) std::atomic<std::uintptr_t> q15t_pb_cone_ret{0};      // base + 0x215799c once armed
__attribute__((visibility("hidden"))) std::atomic<std::uintptr_t> q15t_pb_resolve_real{0};  // the real ResolveBody
__attribute__((visibility("hidden"), used)) const std::uint8_t* q15t_pb_cone_at(const void* target, const std::uint8_t* frame,
                                                                                std::uintptr_t dcs);
}

#if defined(__aarch64__)
extern "C" {
#include "powerboost_thunk.inc"
}
#endif

namespace q15t {
namespace {

using namespace pbcore;

// ELF vaddrs of the pinned build (libr15 build id b243509c08ce677aeb95fa348016949b3fc45230); disassembly VA = ELF + 0x100000.
constexpr std::uintptr_t kSlotNode = 0x36e3868, kFnNode = 0x23207b0;   // R51 CR15NetDamageableHealNode::Enter
constexpr std::uintptr_t kSlotHeal = 0x36f7820, kGotHeal = 0x3725b80, kFnHeal = 0x2137e74;  // R52 CR15NetDamageableCS::Heal
constexpr std::uintptr_t kSlotLink = 0x36d7d08, kFnLink = 0x2158954;   // R53 CR15NetHealBubbleCS::SetTargetInsideBubble
constexpr std::uintptr_t kSlotSend = 0x36e5778, kFnSend = 0x213e80c;   // R54 CR15NetDamageableCS::DamageFromClient (called, not hooked)
constexpr std::uintptr_t kSlotResolve = 0x36ca098, kFnResolve = 0x19a27ac;  // R55 CPhTarget::ResolveBody
constexpr std::uintptr_t kFnPrimary = 0x1677d34;                       // CPhysicsCS::PrimaryBody (JUMP_SLOT 0x36f6110)
constexpr std::uintptr_t kConeCall = 0x2157998, kConeRet = 0x215799c;  // R56: `bl` to ResolveBody's stub (disassembly 0xfe5360) / its return
constexpr std::uint32_t kConeCallWord = 0x97b63672u;                   // bl 0xfe5360 at disassembly 0x2257998 (bytes 7236b697)
constexpr std::uint64_t kDamageableKey = 0xc9dc08cfc2f5920full;        // CR15NetDamageableCS key (source the source build's flowdiag:91)
constexpr std::uintptr_t kDamageableVptr = 0x3693ea0;                  // its vptr value (source the source build's flowdiag:93)

constexpr char kSymNode[] = "_ZN10NRadEngine8NRadGame25CR15NetDamageableHealNode5EnterERKNS0_29SR15NetDamageableHealNodeDataE";
constexpr char kSymHeal[] = "_ZN10NRadEngine8NRadGame19CR15NetDamageableCS4HealENS_18SCompactPoolHandleEfNS0_13SR15NetUserIdE";
constexpr char kSymLink[] =
    "_ZN10NRadEngine8NRadGame19CR15NetHealBubbleCS21SetTargetInsideBubbleENS_18SCompactPoolHandleENS_16CComponentHandleINS0_25CR15NetHealBubbleTargetCSEEERKNS_8C3VectorEj";
constexpr char kSymSend[] =
    "_ZN10NRadEngine8NRadGame19CR15NetDamageableCS16DamageFromClientENS_18SCompactPoolHandleEfNS0_24ER15NetDamageableHitBodyENS_9CSymbol64E";
constexpr char kSymResolve[] = "_ZNK10NRadEngine9CPhTarget11ResolveBodyEv";
constexpr char kSymPrimary[] = "_ZNK10NRadEngine10CPhysicsCS11PrimaryBodyENS_18SCompactPoolHandleE";

constexpr std::uintptr_t kDataAmount = 0x20;      // SR15NetDamageableHealNodeData: float amount, then u32 user at +0x24
constexpr std::uintptr_t kTargetHandle = 0x48;    // CComponentHandle<CR15NetHealBubbleTargetCS> in the link call's third argument
constexpr std::uint64_t kHoldMs = 2000, kPendingMs = 2000;
constexpr int kMarkerCap = 96, kLinkCap = 160, kSwallowCap = 96, kConeLineCap = 240, kSendCap = 160;
constexpr std::uintptr_t kFrRadius = 0xe8, kFrIndex = 0xa0;  // UpdateActiveHealBubble's frame at the cone call
constexpr std::uintptr_t kRecRadius = 0x38, kRecHandle = 0x8, kBodyPos = 0x8f8, kBodyQuat = 0xbd0;

using NodeFn = void (*)(void* node, const std::uint8_t* data);
using HealFn = void (*)(void* cs, std::uint64_t handle, float amount, std::uint64_t user);
using LinkFn = void (*)(void* cs, std::uint64_t bubble, const void* target, const void* pos, std::uint64_t arg);

std::uintptr_t g_base = 0;
PowerboostFns g_fns{};
std::atomic<std::uintptr_t> g_node{0}, g_heal{0}, g_link{0}, g_healq{0};
std::atomic<bool> g_swallow{false}, g_cone{false};
std::atomic<int> g_stage{0};
std::atomic<std::uintptr_t> g_game{0};
std::atomic<int> g_marker_lines{0}, g_link_lines{0}, g_swallow_lines{0}, g_cone_lines{0}, g_send_lines{0};
std::atomic<std::uint32_t> g_markers{0}, g_swallowed{0}, g_sent{0}, g_held_n{0}, g_links{0}, g_cone_calls{0}, g_cone_rejects{0},
    g_pending_sent{0}, g_pending_dropped{0};
std::uint64_t (*g_clock)() = nullptr;
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
LinkFilter g_filter;  // under g_mu
thread_local int t_in_node = 0;  // NodeWrap's real Enter is running on this thread

struct Held {
  void* cs = nullptr;
  std::uint64_t handle = 0, ms = 0;
  float amount = 0.0f;
};
Held g_held;  // under g_mu
UseState g_use{};
ConeTable g_cone_table;
struct Pending {
  std::uintptr_t dcs;
  std::uint32_t index;
  bool linked;
  std::uint64_t ms;
};
Pending g_pending[16];
int g_n_pending = 0;

// A rejected target's body (PB-3): all zero but +0x8f8 x = 1e30, so the game's own dist^2 is +inf and its test takes the
// outside branch (= upstream's radius^2 := 0). UpdateActiveHealBubble reads only +0x8f8 / +0x900 / +0xb38 / +0xbd0 of it.
struct alignas(16) StandIn {
  std::uint8_t lo[kBodyPos];
  float pos[3];
  std::uint8_t hi[0xf40 - kBodyPos - 12];
};
const StandIn kStandIn = {{}, {1e30f, 0.0f, 0.0f}, {}};

std::uint64_t NowMs() {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
}

std::uint32_t Tid() { return static_cast<std::uint32_t>(gettid()); }

struct Lock {
  Lock() { pthread_mutex_lock(&g_mu); }
  ~Lock() { pthread_mutex_unlock(&g_mu); }
};

// one line of a kind under its cap; one WARNING-free INFO line when the cap is first reached (a dropped line is announced)
bool Take(std::atomic<int>& n, int cap, const char* what) {
  const int k = n.fetch_add(1, std::memory_order_relaxed);
  if (k < cap) return true;
  if (k == cap) Rec().S("kind", what).U("cap", static_cast<unsigned>(cap)).Emit(NEVR_LOG_INFO, "powerboost: lines capped; later ones dropped");
  return false;
}

// where: "node" or "heal"; index: the heal's target index, -1 for the node
void MarkerLine(float amount, const char* where, long long index, std::uint32_t user) {
  g_markers.fetch_add(1, std::memory_order_relaxed);
  if (!Take(g_marker_lines, kMarkerCap, "marker")) return;
  const Marker m = Decode(amount);
  if (!m.kind) {
    Rec().F("amount", static_cast<double>(amount)).S("where", where).I("index", index).U("user", user).U("tid", Tid())
        .Emit(NEVR_LOG_INFO, "powerboost: amount is not a marker; passed on");
    return;
  }
  Rec().I("kind", m.kind).I("rez", m.rez).U("v", m.v).U("dr_permille", m.drPermille).U("seconds", m.seconds).S("where", where)
      .I("index", index).U("user", user).Emit(NEVR_LOG_INFO, "powerboost: marker passed on");
}

void SwallowLine(const Marker& m, const char* where, std::uint64_t handle, const char* what) {
  if (!Take(g_swallow_lines, kSwallowCap, "swallow")) return;
  Rec().I("kind", m.kind).I("rez", m.rez).U("v", m.v).S("where", where).U("index", handle & 0xffff).U("tid", Tid()).S("what", what)
      .Emit(NEVR_LOG_INFO, "powerboost: marker swallowed");
}

// Stage 2: the "I used it" marker goes to the server as DamageFromClient(cs, handle, -amount, 0, 0)
// (upstream SendDamageDealt(cs, index, -amount, 0, 0)). Called only from QueuedHealWrap = the game space's deferred-queue drain.
void RecordUse(float amount) {
  if (!g_cone.load(std::memory_order_acquire)) return;
  const Marker m = Decode(amount);
  Lock l;  // table operation only
  g_use = UseState{NowMs(), m.drPermille, m.seconds, m.rez != 0};
}

void Send(void* cs, std::uint64_t handle, float amount, const char* why) {
  if (g_fns.send == nullptr) return;
  RecordUse(amount);
  g_fns.send(cs, handle, -amount, 0, 0);
  g_sent.fetch_add(1, std::memory_order_relaxed);
  if (Take(g_send_lines, kSendCap, "send"))
    Rec().F("v", static_cast<double>(amount)).U("index", handle & 0xffff).S("why", why).U("tid", Tid())
        .Emit(NEVR_LOG_INFO, "powerboost: use sent to the server (DamageFromClient, -amount)");
}

void NodeWrap(void* node, const std::uint8_t* data) {
  const auto real = reinterpret_cast<NodeFn>(g_node.load(std::memory_order_acquire));
  if (real == nullptr) return;
  float amount = 0.0f;
  std::uint32_t user = 0;
  std::uint8_t raw[8];
  const bool have = data != nullptr && SafeRead(reinterpret_cast<std::uintptr_t>(data) + kDataAmount, raw, sizeof raw);
  if (have) {  // copied before the real call, never read after it (PC-4)
    std::memcpy(&amount, raw, sizeof amount);
    std::memcpy(&user, raw + 4, sizeof user);
  }
  ++t_in_node;
  real(node, data);
  --t_in_node;
  if (have && amount >= kMarkerMin) MarkerLine(amount, "node", -1, user);
}

void HealWrap(void* cs, std::uint64_t handle, float amount, std::uint64_t user) {
  if (g_swallow.load(std::memory_order_acquire) && amount >= kMarkerMin) {  // stage 2: a marker never reaches Heal; this path never sends
    const Marker m = Decode(amount);
    if (m.kind) {
      if (t_in_node && m.kind == 1) {  // the node's direct path: held for the next queued heal
        bool replaced;
        {
          Lock l;
          replaced = g_held.cs != nullptr;
          g_held.cs = cs;
          g_held.handle = handle;
          g_held.amount = amount;
          g_held.ms = NowMs();
        }
        g_held_n.fetch_add(1, std::memory_order_relaxed);
        g_swallowed.fetch_add(1, std::memory_order_relaxed);
        SwallowLine(m, "node heal", handle, replaced ? "held for the next queued heal (an older held one dropped)" : "held for the next queued heal");
      } else {
        g_swallowed.fetch_add(1, std::memory_order_relaxed);
        SwallowLine(m, t_in_node ? "node heal" : "heal", handle, t_in_node ? "nothing sent" : "relayed by the server: never re-sent");
      }
      return;
    }
  }
  const auto real = reinterpret_cast<HealFn>(g_heal.load(std::memory_order_acquire));
  if (real == nullptr) return;
  real(cs, handle, amount, user);  // PC-2: the float goes through
  if (amount >= kMarkerMin) MarkerLine(amount, "heal", static_cast<long long>(handle & 0xffff), static_cast<std::uint32_t>(user));
}

// Stage 2: the node's QUEUED heal (its SFunctor3 took Heal's GLOB_DAT word, ELF 0x3725b80; disassembly 0x2420904 `ldr x8, [x8, #0xb80]`),
// run by the deferred queue's drain on the game thread. A marker is swallowed; kind 1 is sent here; a held forward younger than kHoldMs too.
void QueuedHealWrap(void* cs, std::uint64_t handle, float amount, std::uint64_t user) {
  const auto real = reinterpret_cast<HealFn>(g_healq.load(std::memory_order_acquire));
  if (!g_swallow.load(std::memory_order_acquire)) {  // swallow is not (yet) on: the word only passes through
    if (real != nullptr) real(cs, handle, amount, user);
    return;
  }
  Held h;
  {
    Lock l;  // table operation only
    if (g_held.cs != nullptr) {
      h = g_held;
      g_held = Held();
    }
  }
  if (h.cs != nullptr) {  // sent only through the same damageable system this drain call runs for, and only while younger than kHoldMs
    if (h.cs == cs && NowMs() - h.ms < kHoldMs) {
      Send(cs, h.handle, h.amount, "held from the node's direct heal");
    } else if (Take(g_swallow_lines, kSwallowCap, "swallow")) {
      Rec().F("v", static_cast<double>(h.amount)).U("index", h.handle & 0xffff)
          .S("why", h.cs != cs ? "another damageable system (level change)" : "no queued heal within the hold")
          .Emit(NEVR_LOG_INFO, "powerboost: held use dropped");
    }
  }
  const Marker m = Decode(amount);
  if (!m.kind) {
    if (real != nullptr) real(cs, handle, amount, user);
    return;
  }
  g_swallowed.fetch_add(1, std::memory_order_relaxed);
  if (m.kind == 1) Send(cs, handle, amount, "queued node heal");
  else SwallowLine(m, "queued heal", handle, "nothing sent (the gs owns the buffs)");
}

void LinkWrap(void* cs, std::uint64_t bubble, const void* target, const void* pos, std::uint64_t arg) {
  const auto real = reinterpret_cast<LinkFn>(g_link.load(std::memory_order_acquire));
  if (real == nullptr) return;
  real(cs, bubble, target, pos, arg);
  g_links.fetch_add(1, std::memory_order_relaxed);
  if (g_link_lines.load(std::memory_order_relaxed) > kLinkCap) return;  // one more call than the cap: it announces the cap
  std::uint32_t handle = 0xffffffffu;  // ffffffff = unreadable
  SafeRead(reinterpret_cast<std::uintptr_t>(target) + kTargetHandle, &handle, sizeof handle);
  bool show;
  {
    Lock l;  // table operation only
    show = g_filter.Show(static_cast<std::uint32_t>(bubble), handle, NowMs());
  }
  if (!show || !Take(g_link_lines, kLinkCap, "link")) return;
  float v[3];
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(pos), v, sizeof v)) {
    Rec().U("bubble", bubble & 0xffff).U("target", handle & 0xffff).S("at", "unreadable").U("arg", arg).U("tid", Tid())
        .Emit(NEVR_LOG_INFO, "powerboost: link");
    return;
  }
  Rec().U("bubble", bubble & 0xffff).U("target", handle & 0xffff).F("x", static_cast<double>(v[0])).F("y", static_cast<double>(v[1]))
      .F("z", static_cast<double>(v[2])).U("arg", arg).U("tid", Tid()).Emit(NEVR_LOG_INFO, "powerboost: link");
}

// ---- stage 3: the cone and LinkState's sends

// Called only by the thunk, for ResolveBody's call that returns to ELF 0x215799c: target = the CPhTarget UpdateActiveHealBubble
// passes, frame = its sp, dcs = its x24 (the target's CR15NetDamageableCS*). Returns what ResolveBody would have, or the stand-in
// (a reject). On any unreadable word it returns what ResolveBody returned.
const std::uint8_t* ConeAt(const PhTarget* target, const std::uint8_t* frame, std::uintptr_t dcs) {
  const auto resolve = g_fns.resolve;
  if (resolve == nullptr) return nullptr;  // cannot happen: the thunk is armed only with it
  const std::uint8_t* body = resolve(target);
  g_cone_calls.fetch_add(1, std::memory_order_relaxed);
  const auto reject = []() {  // a stand-in body: the game's own test takes its outside branch
    g_cone_rejects.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<const std::uint8_t*>(&kStandIn);
  };
  if (body == nullptr) return body;  // UpdateActiveHealBubble's own null path: a player always has a body
  bool ok = true;
  const auto radius_at = Rd<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(frame) + kFrRadius, &ok);
  float radius = 0;
  if (ok) radius = Rd<float>(radius_at, &ok);
  if (!ok) return body;
  const int angle = ConeAngle(radius);
  if (angle < 1 || angle > 180) return body;  // not POWER BOOST's bubble
  const std::uintptr_t record = radius_at - kRecRadius;
  const auto physics = Rd<std::uintptr_t>(record, &ok);
  const auto handle = Rd<std::uint32_t>(record + kRecHandle, &ok);
  const auto index = Rd<std::uint32_t>(reinterpret_cast<std::uintptr_t>(frame) + kFrIndex, &ok);
  std::uint8_t probe[8];
  if (!ok || g_fns.primary == nullptr || !SafeRead(physics, probe, sizeof probe)) return body;
  const PhTarget ot = g_fns.primary(reinterpret_cast<const void*>(physics), handle);  // the owner, as UpdateActiveHealBubble's prologue
  const std::uint8_t* owner = resolve(&ot);
  if (owner == nullptr) return reject();
  float tp[3], op[3], q[4];
  const auto bd = reinterpret_cast<std::uintptr_t>(body), ow = reinterpret_cast<std::uintptr_t>(owner);
  if (!SafeRead(bd + kBodyPos, tp, sizeof tp) || !SafeRead(ow + kBodyPos, op, sizeof op) || !SafeRead(ow + kBodyQuat, q, sizeof q))
    return body;
  const float d[3] = {tp[0] - op[0], tp[1] - op[1], tp[2] - op[2]};
  const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (dist < kOwnerMinDist) return reject();  // the owner: never self-linked
  static const float kAxis[3] = {0, 0, 1};  // +z; no PowerBoost.ini on the Quest)
  const float off = AngleTo(q, kAxis, d, dist);
  const std::uint64_t now = NowMs();
  bool dropped = false;
  ConeVerdict v;
  {
    Lock l;  // PB-4: table work only
    v = ConeDecide(g_cone_table, record, dcs, index, radius, angle, dist, off, now);
    if (v.send) {  // handed to the net tick (the send context); a full table drops its oldest
      if (g_n_pending == 16) {
        std::memmove(g_pending, g_pending + 1, sizeof g_pending[0] * 15);
        --g_n_pending;
        dropped = true;
        g_pending_dropped.fetch_add(1, std::memory_order_relaxed);
      }
      g_pending[g_n_pending++] = Pending{dcs, index, v.ok, now};
    }
  }
  static std::atomic<std::uint64_t> last_line{0};  // diagnostics, twice a second
  std::uint64_t was = last_line.load(std::memory_order_relaxed);
  if ((now - was >= 500 || dropped) && last_line.compare_exchange_strong(was, now) && Take(g_cone_lines, kConeLineCap, "cone"))
    Rec().U("target", index & 0xffff).F("dist_m", static_cast<double>(dist)).F("max_m", static_cast<double>(std::floor(radius)))
        .F("off_deg", static_cast<double>(off)).I("max_deg", angle).S("verdict", v.ok ? "LINK" : "no").U("tid", Tid())
        .S("note", dropped ? "pending full: oldest dropped" : "")
        .Emit(NEVR_LOG_INFO, "powerboost: cone target");
  return v.ok ? body : reject();
}

#if defined(__aarch64__)
std::uintptr_t ThunkAddress() { return reinterpret_cast<std::uintptr_t>(&q15t_pb_resolve_thunk); }
#else
// Host build only (x86): the arm64 thunk cannot run here. This stand-in only gives the install path an address to swap in;
// it passes every call to the real function. The thunk's own behaviour is tested by src/quest/tests/q15t/pb_thunk_qemu.cpp under qemu.
const std::uint8_t* HostResolveStandIn(const PhTarget* target) {
  return reinterpret_cast<const std::uint8_t* (*)(const PhTarget*)>(q15t_pb_resolve_real.load(std::memory_order_acquire))(target);
}
std::uintptr_t ThunkAddress() { return reinterpret_cast<std::uintptr_t>(&HostResolveStandIn); }
#endif

}  // namespace

PowerboostStats GetPowerboostStats() {
  return {g_markers.load(), g_swallowed.load(), g_sent.load(), g_held_n.load(), g_links.load(), g_cone_calls.load(),
          g_cone_rejects.load(), g_pending_sent.load(), g_pending_dropped.load(), g_stage.load(), g_cone.load()};
}

void PowerboostGameTick(std::uintptr_t game) { g_game.store(game, std::memory_order_release); }

// The net tick (source: the game loop's window around CR15NetGame::UpdateBroadcaster, no lock of ours or the broadcaster's
// held): LinkState's client branch for each pending change younger than kPendingMs whose damageable system is
// still a live one (PB-5: a level unloaded since the cone saw it frees it).
void PowerboostNetTick() {
  if (!g_cone.load(std::memory_order_acquire)) return;
  Pending p[16];
  int n;
  UseState use;
  {
    Lock l;  // hand-over only
    n = g_n_pending;
    std::memcpy(p, g_pending, sizeof p[0] * static_cast<std::size_t>(n));
    g_n_pending = 0;
    use = g_use;
  }
  if (n == 0) return;
  CompSys live[8];
  const std::uintptr_t game = g_game.load(std::memory_order_acquire);
  const int n_live = game != 0 ? FindComponentSystems(game, kDamageableKey, g_base + kDamageableVptr, live, 8) : 0;
  const std::uint64_t now = NowMs();
  for (int i = 0; i < n; ++i) {
    bool alive = false;
    for (int k = 0; k < n_live; ++k) alive = alive || live[k].cs == p[i].dcs;
    const float marker = LinkMarker(use, p[i].linked, now);
    const char* why = !alive ? "its damageable system is gone (level change)"
                      : now - p[i].ms >= kPendingMs ? "older than 2 s"
                      : marker == 0.0f ? "no use of ours within 30 s (another client's bubble: quiet, pb 500)"
                                       : nullptr;
    if (why == nullptr && g_fns.send != nullptr) {
      g_fns.send(reinterpret_cast<void*>(p[i].dcs), p[i].index, -marker, 0, 0);
      g_pending_sent.fetch_add(1, std::memory_order_relaxed);
    }
    if (!Take(g_send_lines, kSendCap, "send")) continue;
    Rec r;
    r.S("direction", p[i].linked ? "link to" : "unlink from").U("index", p[i].index & 0xffff);
    if (why != nullptr) r.S("why", why).Emit(NEVR_LOG_INFO, "powerboost: link marker not sent");
    else r.F("marker", static_cast<double>(marker)).U("tid", Tid()).Emit(NEVR_LOG_INFO, "powerboost: link marker sent to the server");
  }
}

const std::uint8_t* PowerboostConeAtForTest(const PhTarget* target, const std::uint8_t* frame, std::uintptr_t dcs) {
  return ConeAt(target, frame, dcs);
}

bool ResolvePowerboostFns(std::uintptr_t base, PowerboostFns* out) {
  void* h = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return false;
  void* send = dlsym(h, kSymSend);
  void* resolve = dlsym(h, kSymResolve);
  void* primary = dlsym(h, kSymPrimary);
  if (send == nullptr || resolve == nullptr || primary == nullptr) return false;
  if (reinterpret_cast<std::uintptr_t>(send) != base + kFnSend || reinterpret_cast<std::uintptr_t>(resolve) != base + kFnResolve ||
      reinterpret_cast<std::uintptr_t>(primary) != base + kFnPrimary)
    return false;
  out->send = reinterpret_cast<decltype(out->send)>(send);
  out->resolve = reinterpret_cast<decltype(out->resolve)>(resolve);
  out->primary = reinterpret_cast<decltype(out->primary)>(primary);
  return true;
}

namespace {

struct HookDef {
  const char* name;
  const char* symbol;
  std::uintptr_t slot, fn, wrapper;
  std::atomic<std::uintptr_t>* real;
};

bool Swapped(const InstallResult& r) { return r.status == SwapStatus::kOk || r.status == SwapStatus::kRestoreFailed; }

void LogHook(const char* name, std::uintptr_t slot, const InstallResult& r) {
  Rec().S("hook", name).X("slot_elf_vaddr", slot).S("status", StatusName(r.status)).S("reason", r.reason)
      .Emit(r.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "powerboost: hook install");
}

}  // namespace

InstallResult InstallPowerboost(std::uintptr_t base, int stage, bool net_tick, const PowerboostFns* fns) {
  InstallResult out;
  if (stage < kPowerboostRead) {
    out.reason = "off";
    Rec().I("stage", stage).Emit(NEVR_LOG_INFO, "powerboost: off (nothing installed)");
    return out;
  }
  if (stage > kPowerboostCone) stage = kPowerboostCone;
  g_base = base;
  PowerboostFns f{};
  const bool have = fns != nullptr ? (f = *fns, true) : ResolvePowerboostFns(base, &f);
  if (!have && stage >= kPowerboostSwallow) {
    Rec().I("asked_stage", stage).Emit(NEVR_LOG_ERROR, "powerboost: engine functions not resolved (DamageFromClient / ResolveBody / PrimaryBody); stage 1 only");
    out.reason = "swallow_off";
    stage = kPowerboostRead;
  }
  g_fns = f;  // before any swap

  const HookDef hooks[3] = {
      {"CR15NetDamageableHealNode::Enter (R51)", kSymNode, kSlotNode, kFnNode, reinterpret_cast<std::uintptr_t>(&NodeWrap), &g_node},
      {"CR15NetDamageableCS::Heal (R52 JUMP_SLOT)", kSymHeal, kSlotHeal, kFnHeal, reinterpret_cast<std::uintptr_t>(&HealWrap), &g_heal},
      {"CR15NetHealBubbleCS::SetTargetInsideBubble (R53)", kSymLink, kSlotLink, kFnLink, reinterpret_cast<std::uintptr_t>(&LinkWrap),
       &g_link},
  };
  for (int i = 0; i < 3; ++i) {
    const InstallResult r = InstallImportHook(hooks[i].symbol, base, hooks[i].slot, hooks[i].wrapper, hooks[i].real, hooks[i].fn);
    LogHook(hooks[i].name, hooks[i].slot, r);
    if (Swapped(r)) continue;
    bool back = true;
    for (int k = 0; k < i; ++k) {  // all three words or none: put the earlier ones back
      const SwapResult s = SwapWord(base + hooks[k].slot, hooks[k].wrapper, base + hooks[k].fn);
      back = s.status == SwapStatus::kOk && back;
    }
    Rec().S("failed_hook", hooks[i].name).S("status", StatusName(r.status)).S("earlier_words_put_back", back ? "1" : "0")
        .Emit(NEVR_LOG_ERROR, "powerboost: OFF; nothing left installed");
    out.status = r.status;
    out.reason = back ? "hook_failed" : "hook_failed_no_rollback";
    return out;
  }
  out.status = SwapStatus::kOk;
  out.real_resolved = true;
  g_stage.store(kPowerboostRead, std::memory_order_release);

  if (stage >= kPowerboostSwallow) {
    // stage 2: the sender's JUMP_SLOT must hold DamageFromClient at its pinned address, and Heal's GLOB_DAT word must hold Heal
    bool ok = true;
    const auto send_word = Rd<std::uintptr_t>(base + kSlotSend, &ok);
    const char* why = f.send == nullptr ? "no sender function"
                      : (!ok || send_word != base + kFnSend) ? "DamageFromClient's slot does not hold DamageFromClient" : nullptr;
    if (why == nullptr) {
      const InstallResult r = InstallImportHook(kSymHeal, base, kGotHeal, reinterpret_cast<std::uintptr_t>(&QueuedHealWrap), &g_healq, kFnHeal);
      LogHook("CR15NetDamageableCS::Heal (R52 GLOB_DAT)", kGotHeal, r);
      if (!Swapped(r)) why = "Heal's GLOB_DAT word was not swapped";
    }
    if (why != nullptr) {
      Rec().S("why", why).Emit(NEVR_LOG_ERROR, "powerboost: swallow OFF; stage 1 only");
      out.reason = "swallow_off";
      return out;
    }
    g_swallow.store(true, std::memory_order_release);
    g_stage.store(kPowerboostSwallow, std::memory_order_release);
    Rec().S("effect", "markers never reach Heal; uses sent from queued node heals only").Emit(NEVR_LOG_INFO, "powerboost: swallow on");
  }

  if (stage >= kPowerboostCone) {
    bool ok = true;
    const auto call = Rd<std::uint32_t>(base + kConeCall, &ok);
    const char* why = !net_tick ? "no net tick is wired (the link / unlink sends need one)"
                      : (f.resolve == nullptr || f.primary == nullptr) ? "ResolveBody / PrimaryBody not resolved"
                      : (!ok || call != kConeCallWord) ? "UpdateActiveHealBubble's call site is not `bl` ResolveBody" : nullptr;
    if (why == nullptr) {
      q15t_pb_cone_ret.store(base + kConeRet, std::memory_order_release);  // both words before the one slot store (PB-1)
      g_cone.store(true, std::memory_order_release);
      const InstallResult r =
          InstallImportHook(kSymResolve, base, kSlotResolve, ThunkAddress(), &q15t_pb_resolve_real, kFnResolve);
      LogHook("CPhTarget::ResolveBody (R55, thunk) + kConeCall (R56)", kSlotResolve, r);
      if (!Swapped(r)) {
        g_cone.store(false, std::memory_order_release);
        q15t_pb_cone_ret.store(0, std::memory_order_release);
        why = "ResolveBody's slot was not swapped";
      }
    }
    if (why != nullptr) {
      Rec().S("why", why).Emit(NEVR_LOG_ERROR, "powerboost: cone OFF; stage 2 unchanged");
      out.reason = "cone_off";
      return out;
    }
    g_stage.store(kPowerboostCone, std::memory_order_release);
    Rec().X("return_address_elf_vaddr", kConeRet).Emit(NEVR_LOG_INFO, "powerboost: cone on");
  }
  Rec().I("stage", g_stage.load()).I("asked_stage", stage).Emit(NEVR_LOG_INFO, "powerboost: on");
  return out;
}

void SetPowerboostClockForTest(std::uint64_t (*now_ms)()) { g_clock = now_ms; }

void PowerboostSwallowForTest(bool on) { g_swallow.store(on, std::memory_order_release); }

void ResetPowerboostForTest() {
  g_base = 0;
  g_fns = PowerboostFns{};
  g_node = g_heal = g_link = g_healq = 0;
  g_swallow = g_cone = false;
  g_stage = 0;
  g_game = 0;
  g_marker_lines = g_link_lines = g_swallow_lines = g_cone_lines = g_send_lines = 0;
  g_markers = g_swallowed = g_sent = g_held_n = g_links = g_cone_calls = g_cone_rejects = g_pending_sent = g_pending_dropped = 0;
  q15t_pb_cone_ret = 0;
  q15t_pb_resolve_real = 0;
  Lock l;
  g_filter = LinkFilter();
  g_held = Held();
  g_use = UseState{};
  g_cone_table = ConeTable();
  g_n_pending = 0;
  std::memset(g_pending, 0, sizeof g_pending);
}

}  // namespace q15t

extern "C" const std::uint8_t* q15t_pb_cone_at(const void* target, const std::uint8_t* frame, std::uintptr_t dcs) {
  return q15t::PowerboostConeAtForTest(static_cast<const q15t::PhTarget*>(target), frame, dcs);
}


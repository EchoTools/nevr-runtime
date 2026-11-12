// POWER BOOST (rows R51..R56); see powerboost.h and docs/quest/combat/powerboost.md.
// No namespace-scope object has a constructor; every read of game memory is a SafeRead; every wrapper calls the game's function on any
// doubt; every hook goes through the hook manager (InstallThunk, InstallAsmEntry).
#include "quest/combat/powerboost.h"

#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/ec.h"
#include "quest/combat/powerboost_core.h"
#include "quest/combat/safe_read.h"
#include "quest/combat/thread_word.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "runtime/patch/powerboost_rules.h"

// The two words the entry reads, its continuation and the entry itself (powerboost_thunk.inc, callback_thunk.h rule 7). Global scope,
// C names, hidden. The call-site word is stored (release) before the slot changes and never again while the entry is in; the next word is
// the manager's (InstallAsmEntry's nextOut).
extern "C" {
__attribute__((visibility("hidden"))) std::uintptr_t nevr_combat_pb_cone_ret = 0;
__attribute__((visibility("hidden"))) void* nevr_combat_pb_next = nullptr;
__attribute__((visibility("hidden"), noinline)) const std::uint8_t* nevr_combat_pb_cone_at(const void* target, const std::uint8_t* frame,
                                                                                         std::uintptr_t dcs);
__attribute__((visibility("hidden"))) const std::uint8_t* nevr_combat_pb_resolve_entry(const void* target);
}

namespace nevr_quest::combat::powerboost {

namespace {

using namespace pbcore;                // the link line's filter (Quest only)
using namespace ::nevr_powerboost;     // the decode, the cone and the link markers, shared with PC (runtime/patch/powerboost_rules.h, #2339)
namespace pinned = sentinel::pinned;

constexpr std::uintptr_t kDataAmount = 0x20;      // SR15NetDamageableHealNodeData: float amount, then u32 user at +0x24
constexpr std::uintptr_t kTargetHandle = 0x48;    // CComponentHandle<CR15NetHealBubbleTargetCS> in the link call's third argument
constexpr std::uint64_t kHoldMs = 2000, kPendingMs = 2000;
constexpr int kMarkerCap = 96, kLinkCap = 160, kSwallowCap = 96, kConeLineCap = 240, kSendCap = 160, kScopeLineCap = 64;
constexpr std::uintptr_t kFrRadius = 0xe8, kFrIndex = 0xa0;  // UpdateActiveHealBubble's frame at the cone call
constexpr std::uintptr_t kRecRadius = 0x38, kRecHandle = 0x8, kBodyPos = 0x8f8, kBodyQuat = 0xbd0;
constexpr int kMaxPending = 16;

sentinel::GotHook g_nodeHook, g_healHook, g_queuedHook, g_linkHook, g_resolveHook;

std::atomic<bool> g_ran{false};
std::atomic<std::uintptr_t> g_base{0};
std::atomic<std::uintptr_t> g_damageableVptr{0};  // absolute
Fns g_fns{};  // written by the install before any hook goes in
std::atomic<bool> g_swallow{false}, g_cone{false};
std::atomic<int> g_stage{0};
std::atomic<std::uintptr_t> g_game{0};
// The scope: refreshed by the game tick, read by every wrapper. False until a tick saw a combat map or the combat lobby.
std::atomic<bool> g_inScope{false};
// The one place the module asks "is this combat?": the game tick's cached copy of the combat level answer (combat_scope.h), the handlers' test.
// The copy is only as fresh as the tick's last ask and nothing re-judges it while the tick is stopped, so every in-scope body starts with
// scope::LookIsFresh() (README rule 9): a combat answer older than kStaleAfterMs passes the call on.
inline bool Scoped() noexcept { return g_inScope.load(std::memory_order_acquire); }
std::atomic<std::uint32_t> g_frames{0};
std::atomic<int> g_scopeKind{-1};
std::atomic<int> g_markerLines{0}, g_linkLines{0}, g_swallowLines{0}, g_coneLines{0}, g_sendLines{0}, g_scopeLines{0};
std::atomic<std::uint32_t> g_markers{0}, g_swallowed{0}, g_sent{0}, g_heldN{0}, g_links{0}, g_coneCalls{0}, g_coneRejects{0},
    g_pendingSent{0}, g_pendingDropped{0}, g_scopeChecks{0};
std::atomic<std::uint64_t> g_lastConeLine{0};
std::uint64_t (*g_clock)() = nullptr;
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};
std::atomic<bool (*)(void (*)() noexcept) noexcept> g_registrar{nullptr};
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;  // a std::mutex would register a destructor at load time
LinkFilter g_filter;   // under g_mu
ThreadWord g_tInNode;  // per thread, how many real HealNode::Enter calls are running on it (a key, not thread_local)

struct Held {
  void* cs;
  std::uint64_t handle, ms;
  float amount;
};
Held g_held{};  // under g_mu
UseState g_use{};
ConeTable g_coneTable;
struct Pending {
  std::uintptr_t dcs;
  std::uint32_t index;
  bool linked;
  std::uint64_t ms;
};
Pending g_pending[kMaxPending];
int g_nPending = 0;                    // under g_mu
std::atomic<int> g_pendingPublished{0};  // g_nPending, for the net tick's lock-free idle check

// A rejected target's body: all zero but +0x8f8 x = 1e30, so the game's own dist^2 is +inf and its test takes the outside branch.
// UpdateActiveHealBubble reads only +0x8f8 / +0x900 / +0xb38 / +0xbd0 of it.
struct alignas(16) StandIn {
  std::uint8_t lo[kBodyPos];
  float pos[3];
  std::uint8_t hi[0xf40 - kBodyPos - 12];
};
const StandIn kStandIn = {{}, {1e30f, 0.0f, 0.0f}, {}};

std::uint64_t NowMs() noexcept {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
}

std::uint32_t Tid() noexcept { return static_cast<std::uint32_t>(gettid()); }

void Lock() noexcept { pthread_mutex_lock(&g_mu); }
void Unlock() noexcept { pthread_mutex_unlock(&g_mu); }

// A float as text for a log field (the log carries integers and strings only).
struct Num {
  char text[24];
  explicit Num(double v) noexcept { std::snprintf(text, sizeof(text), "%g", v); }
};

// One line of a kind under its cap; one Info line when the cap is first reached (a dropped line is announced).
bool Take(std::atomic<int>& n, int cap, const char* what) noexcept {
  const int k = n.fetch_add(1, std::memory_order_relaxed);
  if (k < cap) return true;
  if (k == cap) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_capped",
                        {{"kind", what}, {"cap", cap}, {"msg", "this kind of powerboost line reached its cap; later ones are counted, not logged"}});
  }
  return false;
}

// where: "node" or "heal"; index: the heal's target index, -1 for the node
void MarkerLine(float amount, const char* where, long long index, std::uint32_t user) noexcept {
  g_markers.fetch_add(1, std::memory_order_relaxed);
  if (!Take(g_markerLines, kMarkerCap, "marker")) return;
  const Marker m = Decode(amount);
  if (m.kind == kHeal) {
    const Num a(static_cast<double>(amount));
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_marker",
                        {{"kind", 0}, {"amount", a.text}, {"where", where}, {"index", index}, {"user", user}, {"tid", Tid()},
                         {"msg", "the amount is not a marker; passed on to the game as a heal"}});
    return;
  }
  if (m.kind == kDeclined) {
    // stage 1 (swallow off) swallows no marker: the game's Heal takes this amount as a heal. From stage 2 on it never reaches Heal.
    const bool passedOn = !g_swallow.load(std::memory_order_acquire);
    const Num a(static_cast<double>(amount));
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_marker",
                        {{"kind", m.kind}, {"rez", m.rez}, {"amount", a.text}, {"where", where}, {"index", index}, {"user", user}, {"tid", Tid()},
                         {"passed_on", passedOn ? 1 : 0},
                         {"msg", passedOn ? "the amount rounds out of its marker band: no buff, and the game's Heal takes it as a heal (stage 1 swallows no marker)"
                                           : "the amount rounds out of its marker band; declined: no buff, not passed on as a heal"}});
    return;
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_marker",
                      {{"kind", m.kind}, {"rez", m.rez}, {"v", m.v}, {"dr_permille", m.drPermille}, {"seconds", m.seconds}, {"where", where},
                       {"index", index}, {"user", user}, {"msg", "a POWER BOOST marker passed on"}});
}

void SwallowLine(const Marker& m, const char* where, std::uint64_t handle, const char* what) noexcept {
  if (!Take(g_swallowLines, kSwallowCap, "swallow")) return;
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_swallow",
                      {{"kind", m.kind}, {"rez", m.rez}, {"v", m.v}, {"where", where}, {"index", handle & 0xffff}, {"tid", Tid()}, {"what", what},
                       {"msg", "a POWER BOOST marker did not reach the game's Heal"}});
}

// Stage 2: the "I used it" marker goes to the server as DamageFromClient(cs, handle, -amount, 0, 0). The use is recorded for the cone.
void RecordUse(float amount) noexcept {
  if (!g_cone.load(std::memory_order_acquire)) return;
  const Marker m = Decode(amount);
  Lock();
  g_use = UseState{NowMs(), m.drPermille, m.seconds, m.rez != 0};
  Unlock();
}

void Send(void* cs, std::uint64_t handle, float amount, const char* why) noexcept {
  if (g_fns.send == nullptr) return;
  RecordUse(amount);
  g_fns.send(cs, handle, -amount, 0, 0);
  g_sent.fetch_add(1, std::memory_order_relaxed);
  if (Take(g_sendLines, kSendCap, "send")) {
    const Num v(static_cast<double>(amount));
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_use",
                        {{"v", v.text}, {"index", handle & 0xffff}, {"why", why}, {"tid", Tid()},
                         {"msg", "a POWER BOOST use was sent to the server (DamageFromClient, -amount)"}});
  }
}

// R51, in scope. The data is copied before the game's call and never read after it.
__attribute__((noinline)) void NodeAct(NodeThunk::Fn original, void* node, const std::uint8_t* data) noexcept {
  if (!scope::LookIsFresh()) return original(node, data);  // the tick stopped: the answer is not believed (combat_scope.h)
  float amount = 0.0f;
  std::uint32_t user = 0;
  std::uint8_t raw[8];
  const bool have = data != nullptr && SafeRead(reinterpret_cast<std::uintptr_t>(data) + kDataAmount, raw, sizeof raw);
  if (have) {
    std::memcpy(&amount, raw, sizeof amount);
    std::memcpy(&user, raw + 4, sizeof user);
  }
  const std::uintptr_t depth = g_tInNode.Get();
  g_tInNode.Set(depth + 1);
  original(node, data);
  g_tInNode.Set(depth);
  if (have && amount >= kMarkerMin) MarkerLine(amount, "node", -1, user);
}

// R52, Heal's JUMP_SLOT, in scope: the node's direct heal and the server's relayed damage-dealt message.
__attribute__((noinline)) void HealAct(HealThunk::Fn original, void* cs, std::uint64_t handle, float amount, std::uint64_t user) noexcept {
  if (!scope::LookIsFresh()) return original(cs, handle, amount, user);  // the tick stopped: the answer is not believed (combat_scope.h)
  if (g_swallow.load(std::memory_order_acquire) && amount >= kMarkerMin) {  // stage 2: a marker never reaches Heal; this path never sends
    const Marker m = Decode(amount);
    if (m.kind != kHeal) {
      const bool inNode = g_tInNode.Get() != 0;
      if (inNode && m.kind == kUse) {  // the node's direct path: held for the next queued heal
        Lock();
        const bool replaced = g_held.cs != nullptr;
        g_held = Held{cs, handle, NowMs(), amount};
        Unlock();
        g_heldN.fetch_add(1, std::memory_order_relaxed);
        g_swallowed.fetch_add(1, std::memory_order_relaxed);
        SwallowLine(m, "node heal", handle, replaced ? "held for the next queued heal (an older held one dropped)" : "held for the next queued heal");
      } else {
        g_swallowed.fetch_add(1, std::memory_order_relaxed);
        SwallowLine(m, inNode ? "node heal" : "heal", handle, inNode ? "nothing sent" : "relayed by the server: never re-sent");
      }
      return;
    }
  }
  original(cs, handle, amount, user);  // the float goes through
  if (amount >= kMarkerMin) MarkerLine(amount, "heal", static_cast<long long>(handle & 0xffff), static_cast<std::uint32_t>(user));
}

// R52, Heal's GLOB_DAT word: the node's QUEUED heal (its SFunctor3 took the word, `ldr x8,[x8,#0xb80]` at 0x2320904), run by the deferred
// queue's drain on the game thread. A marker is swallowed; kind 1 is sent here; a held one younger than kHoldMs too.
__attribute__((noinline)) void QueuedHealAct(QueuedHealThunk::Fn original, void* cs, std::uint64_t handle, float amount, std::uint64_t user) noexcept {
  if (!scope::LookIsFresh()) return original(cs, handle, amount, user);  // the tick stopped: the answer is not believed (combat_scope.h)
  if (!g_swallow.load(std::memory_order_acquire)) {  // swallow is not (yet) on: the word only passes through
    original(cs, handle, amount, user);
    return;
  }
  Held h{};
  Lock();
  if (g_held.cs != nullptr) {
    h = g_held;
    g_held = Held{};
  }
  Unlock();
  if (h.cs != nullptr) {  // sent only through the damageable system this drain call runs for, and only while younger than kHoldMs
    if (h.cs == cs && NowMs() - h.ms < kHoldMs) {
      Send(cs, h.handle, h.amount, "held from the node's direct heal");
    } else if (Take(g_swallowLines, kSwallowCap, "swallow")) {
      const Num v(static_cast<double>(h.amount));
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_held_dropped",
                          {{"v", v.text}, {"index", h.handle & 0xffff},
                           {"why", h.cs != cs ? "another damageable system (level change)" : "no queued heal within the hold"},
                           {"msg", "a held POWER BOOST use was not sent"}});
    }
  }
  const Marker m = Decode(amount);
  if (m.kind == kHeal) {
    original(cs, handle, amount, user);
    return;
  }
  g_swallowed.fetch_add(1, std::memory_order_relaxed);
  if (m.kind == kUse) {
    Send(cs, handle, amount, "queued node heal");
  } else {
    SwallowLine(m, "queued heal", handle,
                m.kind == kDeclined ? "declined: its value rounds out of its band; nothing sent, not a heal" : "nothing sent (the server owns the buffs)");
  }
}

// R53, in scope, after the game's call: one line per (bubble, target) change, or per second for the same pair.
__attribute__((noinline)) void LinkAct(std::uint64_t bubble, const void* target, const void* pos, std::uint64_t arg) noexcept {
  if (!scope::LookIsFresh()) return;  // the tick stopped: the answer is not believed (combat_scope.h); the game's call is made
  g_links.fetch_add(1, std::memory_order_relaxed);
  if (g_linkLines.load(std::memory_order_relaxed) > kLinkCap) return;  // one more call than the cap: it announces the cap
  std::uint32_t handle = 0xffffffffu;  // ffffffff = unreadable
  SafeRead(reinterpret_cast<std::uintptr_t>(target) + kTargetHandle, &handle, sizeof handle);
  Lock();
  const bool show = g_filter.Show(static_cast<std::uint32_t>(bubble), handle, NowMs());
  Unlock();
  if (!show || !Take(g_linkLines, kLinkCap, "link")) return;
  float v[3];
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(pos), v, sizeof v)) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_link",
                        {{"bubble", bubble & 0xffff}, {"target", handle & 0xffff}, {"at", "unreadable"}, {"arg", arg}, {"tid", Tid()}});
    return;
  }
  const Num x(static_cast<double>(v[0])), y(static_cast<double>(v[1])), z(static_cast<double>(v[2]));
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_link",
                      {{"bubble", bubble & 0xffff}, {"target", handle & 0xffff}, {"x", x.text}, {"y", y.text}, {"z", z.text}, {"arg", arg},
                       {"tid", Tid()}});
}

// The handlers. Outside the scope (an arena match) each is a load of the cached scope and a tail call of the game's function; the work in
// scope is in the *Act functions, so the arena path carries none of their frame.
void NodeHandler(NodeThunk::Fn original, void* node, const std::uint8_t* data) noexcept {
  if (!Scoped()) return original(node, data);
  NodeAct(original, node, data);
}
void HealHandler(HealThunk::Fn original, void* cs, std::uint64_t handle, float amount, std::uint64_t user) noexcept {
  if (!Scoped()) return original(cs, handle, amount, user);
  HealAct(original, cs, handle, amount, user);
}
// R52, Heal's GLOB_DAT word (QueuedHealAct): not combat, the word only passes through.
void QueuedHealHandler(QueuedHealThunk::Fn original, void* cs, std::uint64_t handle, float amount, std::uint64_t user) noexcept {
  if (!Scoped()) return original(cs, handle, amount, user);
  QueuedHealAct(original, cs, handle, amount, user);
}
void LinkHandler(LinkThunk::Fn original, void* cs, std::uint64_t bubble, const void* target, const void* pos, std::uint64_t arg) noexcept {
  original(cs, bubble, target, pos, arg);
  if (Scoped()) LinkAct(bubble, target, pos, arg);
}

NEVR_HOOK_RECORD(kNodeRecord, NodeThunk, &NodeHandler);
NEVR_HOOK_RECORD(kHealRecord, HealThunk, &HealHandler);
NEVR_HOOK_RECORD(kQueuedRecord, QueuedHealThunk, &QueuedHealHandler);
NEVR_HOOK_RECORD(kLinkRecord, LinkThunk, &LinkHandler);

using ResolveFn = const std::uint8_t* (*)(const PhTarget*);
ResolveFn Next() noexcept { return reinterpret_cast<ResolveFn>(__atomic_load_n(&nevr_combat_pb_next, __ATOMIC_ACQUIRE)); }

// ---- stage 3: the cone ------------------------------------------------------------------------------------------------------------

// Whether `cs` is a live CR15NetDamageableCS of the game (key and vptr in one of its gamespaces' system tables, cs+0x80 the gamespace).
bool LiveDamageable(std::uintptr_t game, std::uintptr_t cs) noexcept {
  constexpr std::uint64_t kMaxGamespaces = 64, kMaxSystems = 1024;
  constexpr std::uintptr_t kTables[2][2] = {{0x1f48, 0x1f78}, {0x1f88, 0x1fb8}};  // {table pointer, count} offsets in CR15Game
  const std::uintptr_t vptr = g_damageableVptr.load(std::memory_order_acquire);
  for (const auto& t : kTables) {
    bool ok = true;
    const auto tab = SafeRead<std::uintptr_t>(game + t[0], &ok);
    const auto cnt = SafeRead<std::uint64_t>(game + t[1], &ok);
    if (!ok || tab == 0 || cnt > kMaxGamespaces) continue;
    for (std::uint64_t i = 0; i < cnt; ++i) {
      bool ok2 = true;
      const auto gs = SafeRead<std::uintptr_t>(tab + i * 16 + 8, &ok2);
      if (!ok2 || gs == 0) continue;
      const auto sys = SafeRead<std::uintptr_t>(gs + 0x420, &ok2);
      const auto ns = SafeRead<std::uint64_t>(gs + 0x450, &ok2);
      if (!ok2 || SafeRead<std::uintptr_t>(gs + 0x8, &ok2) != game || !ok2 || ns > kMaxSystems) continue;
      for (std::uint64_t j = 0; j < ns; ++j) {
        bool ok3 = true;
        if (SafeRead<std::uint64_t>(sys + j * 16, &ok3) != kDamageableKey || !ok3) continue;
        const auto c = SafeRead<std::uintptr_t>(sys + j * 16 + 8, &ok3);
        if (!ok3 || c != cs) continue;
        if (SafeRead<std::uintptr_t>(c, &ok3) == vptr && SafeRead<std::uintptr_t>(c + 0x80, &ok3) == gs && ok3) return true;
      }
    }
  }
  return false;
}

const std::uint8_t* Reject() noexcept {  // a stand-in body: the game's own test takes its outside branch
  g_coneRejects.fetch_add(1, std::memory_order_relaxed);
  return reinterpret_cast<const std::uint8_t*>(&kStandIn);
}

}  // namespace

const std::uint8_t* ConeAt(const PhTarget* target, const std::uint8_t* frame, std::uintptr_t dcs) noexcept;

// The one call the entry acts on: ResolveBody's call that returns to base + 0x215799c. target = the CPhTarget UpdateActiveHealBubble passes,
// frame = its sp, dcs = its x24 (the target's CR15NetDamageableCS*). Returns what ResolveBody would have, or the stand-in (a reject). On
// any unreadable word, and outside combat, it returns what ResolveBody returned.
const std::uint8_t* ConeAt(const PhTarget* target, const std::uint8_t* frame, std::uintptr_t dcs) noexcept {
  const ResolveFn resolve = Next();
  if (resolve == nullptr) return nullptr;  // cannot happen: the manager publishes the next word before the slot changes
  const std::uint8_t* body = resolve(target);
  if (!Scoped() || !g_cone.load(std::memory_order_acquire) || !scope::LookIsFresh()) return body;
  g_coneCalls.fetch_add(1, std::memory_order_relaxed);
  if (body == nullptr) return body;  // UpdateActiveHealBubble's own null path
  bool ok = true;
  const auto radiusAt = SafeRead<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(frame) + kFrRadius, &ok);
  float radius = 0;
  if (ok) radius = SafeRead<float>(radiusAt, &ok);
  if (!ok) return body;
  const int angle = ConeAngle(radius);
  // not POWER BOOST's bubble: neither (a radius that is not finite, Repair Matrix's whole one), or MEDIGUN's band, which is PC's (the Quest
  // has no MEDIGUN and leaves such a bubble to the game's own radius check)
  if (BubbleOf(angle) != Bubble::kPowerBoost) return body;
  const std::uintptr_t record = radiusAt - kRecRadius;
  const auto physics = SafeRead<std::uintptr_t>(record, &ok);
  const auto handle = SafeRead<std::uint32_t>(record + kRecHandle, &ok);
  const auto index = SafeRead<std::uint32_t>(reinterpret_cast<std::uintptr_t>(frame) + kFrIndex, &ok);
  std::uint8_t probe[8];
  if (!ok || g_fns.primary == nullptr || !SafeRead(physics, probe, sizeof probe)) return body;
  const PhTarget ot = g_fns.primary(reinterpret_cast<const void*>(physics), handle);  // the owner, as UpdateActiveHealBubble's prologue
  const std::uint8_t* owner = resolve(&ot);
  if (owner == nullptr) return Reject();
  float tp[3], op[3], q[4];
  const auto bd = reinterpret_cast<std::uintptr_t>(body), ow = reinterpret_cast<std::uintptr_t>(owner);
  if (!SafeRead(bd + kBodyPos, tp, sizeof tp) || !SafeRead(ow + kBodyPos, op, sizeof op) || !SafeRead(ow + kBodyQuat, q, sizeof q)) return body;
  const float d[3] = {tp[0] - op[0], tp[1] - op[1], tp[2] - op[2]};
  const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  if (dist < kOwnerMinDist) return Reject();  // the owner: never self-linked
  static constexpr float kAxis[3] = {0, 0, 1};  // +z; there is no PowerBoost.ini on the Quest
  const float off = AngleTo(q, kAxis, d, dist);
  const std::uint64_t now = NowMs();
  bool dropped = false;
  Lock();  // table work only
  const ConeVerdict v = ConeDecide(g_coneTable, record, dcs, index, radius, angle, dist, off, now);
  if (v.send) {  // handed to the net tick (the send context); a full table drops its oldest
    if (g_nPending == kMaxPending) {
      std::memmove(g_pending, g_pending + 1, sizeof g_pending[0] * (kMaxPending - 1));
      --g_nPending;
      dropped = true;
      g_pendingDropped.fetch_add(1, std::memory_order_relaxed);
    }
    g_pending[g_nPending++] = Pending{dcs, index, v.ok, now};
    g_pendingPublished.store(g_nPending, std::memory_order_release);
  }
  Unlock();
  std::uint64_t was = g_lastConeLine.load(std::memory_order_relaxed);  // diagnostics, twice a second
  if ((now - was >= 500 || dropped) && g_lastConeLine.compare_exchange_strong(was, now) && Take(g_coneLines, kConeLineCap, "cone")) {
    const Num dm(static_cast<double>(dist)), mm(static_cast<double>(std::floor(radius))), od(static_cast<double>(off));
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_cone",
                        {{"target", index & 0xffff}, {"dist_m", dm.text}, {"max_m", mm.text}, {"off_deg", od.text}, {"max_deg", angle},
                         {"verdict", v.ok ? "LINK" : "no"}, {"tid", Tid()}, {"note", dropped ? "pending full: oldest dropped" : ""}});
  }
  return v.ok ? body : Reject();
}

namespace {
NEVR_ASM_ENTRY_RECORD(kResolveEntry, ::nevr_combat_pb_resolve_entry, ::nevr_combat_pb_cone_at);

ModuleResult Refused(const char* token, int stage, const char* consequence) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kError, "combat_powerboost_install",
                      {{"result", "refused"}, {"stage", 0}, {"asked_stage", stage}, {"reason", token}, {"consequence", consequence}});
  return {false, token};
}

constexpr const char* kNothing = "nothing is installed: POWER BOOST markers reach the game as heals";

const char* StatusOf(sentinel::GotStatus status) noexcept { return sentinel::GotStatusName(status); }

void LogHook(const char* hook, std::uint64_t slot, sentinel::GotStatus status) noexcept {
  char at[19];
  sentinel::LogFields(status == sentinel::GotStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_powerboost_hook",
                      {{"hook", hook}, {"slot_vaddr", sentinel::HexString(at, slot)}, {"status", StatusOf(status)}});
}

template <typename Thunk, typename Record>
sentinel::GotStatus InstallOne(const char* name, sentinel::GotHook& hook, const sentinel::GotTarget& target, const Record& record,
                               sentinel::ImageLookup lookup) noexcept {
  Thunk::Arm(record);
  const sentinel::GotStatus got = sentinel::InstallThunk<Thunk>(hook, target, lookup);
  if (got != sentinel::GotStatus::kOk) Thunk::Disarm();
  LogHook(name, *target.slotVaddr, got);
  return got;
}

bool DefaultRegistrar(void (*tick)() noexcept) noexcept { return ec::OnNetTick(tick); }

}  // namespace

bool InScope() noexcept { return Scoped(); }

void GameTick(std::uintptr_t game) noexcept {
  const std::uintptr_t was = g_game.load(std::memory_order_relaxed);
  if (was != game) g_game.store(game, std::memory_order_release);
  const std::uint32_t frame = g_frames.fetch_add(1, std::memory_order_relaxed);
  if (was == game && frame % kScopeEveryFrames != 0) return;  // low cadence: once every kScopeEveryFrames frames, or a new game
  g_scopeChecks.fetch_add(1, std::memory_order_relaxed);
  // One record of the combat level answer (combat_scope.h LastLook) decides, classes and names the level, so the line describes the look that decided.
  const scope::Look look = scope::LastLook();
  const bool in = scope::InCombatOrLobby(look);
  const scope::LookClass cls = scope::ClassOf(look);
  g_inScope.store(in, std::memory_order_release);
  if (g_scopeKind.exchange(static_cast<int>(cls), std::memory_order_acq_rel) != static_cast<int>(cls) &&
      Take(g_scopeLines, kScopeLineCap, "scope")) {
    char lv[19];
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_scope",
                        {{"in_combat", in ? 1 : 0}, {"kind", scope::LookClassName(cls)}, {"level", sentinel::HexString(lv, look.level)},
                         {"msg", in ? "a combat map or the combat lobby is loaded: POWER BOOST acts"
                                    : "no combat map or combat lobby is loaded: POWER BOOST only passes every call on"}});
  }
}

// The net tick (the EC client's recvfrom, ec.h): LinkState's client branch for each pending change younger than kPendingMs whose damageable
// system is still a live one (a level unloaded since the cone saw it frees it).
void NetTick() noexcept {
  if (g_pendingPublished.load(std::memory_order_acquire) == 0 || !g_cone.load(std::memory_order_acquire)) return;
  Pending p[kMaxPending];
  int n;
  UseState use;
  Lock();  // hand-over only
  n = g_nPending;
  std::memcpy(p, g_pending, sizeof p[0] * static_cast<std::size_t>(n));
  g_nPending = 0;
  g_pendingPublished.store(0, std::memory_order_release);
  use = g_use;
  Unlock();
  const std::uintptr_t game = g_game.load(std::memory_order_acquire);
  const std::uint64_t now = NowMs();
  for (int i = 0; i < n; ++i) {
    const bool alive = game != 0 && LiveDamageable(game, p[i].dcs);
    const float marker = LinkMarker(use, p[i].linked, now);
    const char* why = !alive ? "its damageable system is gone (level change)"
                      : now - p[i].ms >= kPendingMs ? "older than 2 s"
                      : marker == 0.0f ? "no use of ours within 30 s (another client's bubble)"
                                       : nullptr;
    if (why == nullptr && g_fns.send != nullptr) {
      g_fns.send(reinterpret_cast<void*>(p[i].dcs), p[i].index, -marker, 0, 0);
      g_pendingSent.fetch_add(1, std::memory_order_relaxed);
    }
    if (!Take(g_sendLines, kSendCap, "send")) continue;
    if (why != nullptr) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_link_marker",
                          {{"direction", p[i].linked ? "link to" : "unlink from"}, {"index", p[i].index & 0xffff}, {"sent", 0}, {"why", why}});
    } else {
      const Num mk(static_cast<double>(marker));
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_link_marker",
                          {{"direction", p[i].linked ? "link to" : "unlink from"}, {"index", p[i].index & 0xffff}, {"sent", 1}, {"marker", mk.text},
                           {"tid", Tid()}, {"msg", "a link marker was sent to the server"}});
    }
  }
}

bool ResolveFns(std::uintptr_t base, const Pins& pins, Fns* out) noexcept {
  void* h = dlopen(pinned::kLibR15, RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return false;
  void* send = dlsym(h, kSendSymbol);
  void* resolve = dlsym(h, kResolveSymbol);
  void* primary = dlsym(h, kPrimarySymbol);
  dlclose(h);
  if (send == nullptr || resolve == nullptr || primary == nullptr) return false;
  if (reinterpret_cast<std::uintptr_t>(send) != base + pins.sendFn || reinterpret_cast<std::uintptr_t>(resolve) != base + pins.resolveFn ||
      reinterpret_cast<std::uintptr_t>(primary) != base + pins.primaryFn)
    return false;
  out->send = reinterpret_cast<decltype(out->send)>(send);
  out->resolve = reinterpret_cast<decltype(out->resolve)>(resolve);
  out->primary = reinterpret_cast<decltype(out->primary)>(primary);
  return true;
}

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, int stage, const Fns* fns) noexcept {
  if (stage < kRead || stage > kCone) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_powerboost_install",
                        {{"result", "off"}, {"stage", 0}, {"asked_stage", stage}, {"reason", "combat.powerboost_stage"},
                         {"consequence", "combat.powerboost_stage is 0: nothing is installed; POWER BOOST markers reach the game as heals"}});
    return {false, "off", true};  // off by its key: counted apart on the phase line, neither installed nor failed
  }
  if (g_ran.exchange(true, std::memory_order_acq_rel)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_powerboost_install",
                        {{"result", "refused"}, {"reason", "already_ran"}, {"consequence", "a second step installs nothing"}});
    return {false, "already_ran"};
  }
  // The scope is the combat level answer (combat_scope.h): when it cannot change (the tick or the scope row's refresh is not installed) the module
  // could never act, so nothing is hooked.
  if (!scope::Live()) return Refused("scope_not_live", stage, "the combat level answer cannot change: nothing is installed");
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(pinned::kLibR15, &image)) return Refused("module_not_loaded", stage, kNothing);
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id", stage, kNothing);
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch", stage, kNothing);
  if (const int keyError = g_tInNode.Create(); keyError != 0) {  // the node mark is per thread, in a pthread key
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_powerboost_install",
                        {{"result", "refused"}, {"stage", 0}, {"asked_stage", stage}, {"reason", "no_thread_key"}, {"error", keyError},
                         {"consequence", "no pthread key for the per-thread node mark: nothing is installed"}});
    return {false, "no_thread_key"};
  }
  // The scope comes from the game tick: without it the module could never act.
  if (!tick::Add(&GameTick)) return Refused("tick_table_full", stage, "the game tick has no room for the scope check: nothing is installed");
  const std::uintptr_t base = image.base;
  g_base.store(base, std::memory_order_release);
  g_damageableVptr.store(base + pins.damageableVptr, std::memory_order_release);
  const int asked = stage;
  const char* reason = "ok";
  Fns f{};
  const bool have = fns != nullptr ? (f = *fns, true) : ResolveFns(base, pins, &f);
  if (!have && stage >= kSwallow) {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_powerboost_fns",
                        {{"asked_stage", stage}, {"status", "not_resolved"},
                         {"consequence", "DamageFromClient, ResolveBody or PrimaryBody is not the pinned build's: stage 1 only"}});
    reason = "swallow_off";
    stage = kRead;
  }
  g_fns = f;  // before any hook goes in

  // Stage 1: all three words or none.
  struct Row {
    const char* name;
    sentinel::GotHook* hook;
    std::uint64_t slot;
  };
  const Row rows[3] = {{"node", &g_nodeHook, pins.nodeSlot}, {"heal", &g_healHook, pins.healSlot}, {"link", &g_linkHook, pins.linkSlot}};
  sentinel::GotStatus got = InstallOne<NodeThunk>("node", g_nodeHook, NodeTarget(base, pins), kNodeRecord, lookup);
  int done = got == sentinel::GotStatus::kOk ? 1 : 0;
  if (done == 1) {
    got = InstallOne<HealThunk>("heal", g_healHook, HealTarget(base, pins), kHealRecord, lookup);
    done += got == sentinel::GotStatus::kOk ? 1 : 0;
  }
  if (done == 2) {
    got = InstallOne<LinkThunk>("link", g_linkHook, LinkTarget(base, pins), kLinkRecord, lookup);
    done += got == sentinel::GotStatus::kOk ? 1 : 0;
  }
  if (done < 3) {
    bool back = true;
    for (int k = 0; k < done; ++k) back = rows[k].hook->Remove() == sentinel::GotStatus::kOk && back;  // the manager puts the words back
    NodeThunk::Disarm();
    HealThunk::Disarm();
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_powerboost_install",
                        {{"result", "refused"}, {"stage", 0}, {"asked_stage", asked}, {"reason", back ? "hook_failed" : "hook_failed_no_rollback"},
                         {"failed_hook", rows[done].name}, {"status", StatusOf(got)}, {"earlier_hooks_removed", back ? 1 : 0},
                         {"consequence", kNothing}});
    return {false, back ? "hook_failed" : "hook_failed_no_rollback"};
  }
  g_stage.store(kRead, std::memory_order_release);

  if (stage >= kSwallow) {
    // the sender's JUMP_SLOT must hold DamageFromClient at its pin; then Heal's GLOB_DAT word
    bool ok = true;
    const auto sendWord = SafeRead<std::uintptr_t>(base + pins.sendSlot, &ok);
    const char* why = f.send == nullptr ? "no sender function"
                      : (!ok || sendWord != base + pins.sendFn) ? "DamageFromClient's slot does not hold DamageFromClient" : nullptr;
    if (why == nullptr &&
        InstallOne<QueuedHealThunk>("queued_heal", g_queuedHook, QueuedHealTarget(base, pins), kQueuedRecord, lookup) != sentinel::GotStatus::kOk) {
      why = "Heal's GLOB_DAT word was not hooked";
    }
    if (why != nullptr) {
      sentinel::LogFields(sentinel::LogLevel::kError, "combat_powerboost_swallow_off",
                          {{"why", why}, {"consequence", "POWER BOOST markers are only recorded (stage 1); they still reach the game as heals"}});
      reason = "swallow_off";
      stage = kRead;
    } else {
      g_swallow.store(true, std::memory_order_release);
      g_stage.store(kSwallow, std::memory_order_release);
    }
  }

  if (stage >= kCone) {
    bool ok = true;
    const auto call = SafeRead<std::uint32_t>(base + pins.coneCall, &ok);
    const auto registrar = g_registrar.load(std::memory_order_acquire);
    const char* why = (f.resolve == nullptr || f.primary == nullptr) ? "ResolveBody / PrimaryBody not resolved"
                      : (!ok || call != kConeCallWord) ? "UpdateActiveHealBubble's call site is not `bl` ResolveBody"
                      : !(registrar != nullptr ? registrar : &DefaultRegistrar)(&NetTick) ? "the EC client's net tick could not take the link / unlink sends"
                                                                                          : nullptr;
    if (why == nullptr) {
      __atomic_store_n(&nevr_combat_pb_cone_ret, base + pins.coneRet, __ATOMIC_RELEASE);  // before the one slot store
      g_cone.store(true, std::memory_order_release);
      const sentinel::GotStatus r = sentinel::InstallAsmEntry(g_resolveHook, ResolveTarget(base, pins), kResolveEntry, &nevr_combat_pb_next, lookup);
      LogHook("resolve", pins.resolveSlot, r);
      if (r != sentinel::GotStatus::kOk) {
        g_cone.store(false, std::memory_order_release);
        __atomic_store_n(&nevr_combat_pb_cone_ret, std::uintptr_t{0}, __ATOMIC_RELEASE);
        why = "ResolveBody's slot was not hooked";
      }
    }
    if (why != nullptr) {
      sentinel::LogFields(sentinel::LogLevel::kError, "combat_powerboost_cone_off",
                          {{"why", why}, {"consequence", "no cone: every target in a POWER BOOST bubble stays linked; stage 2 is unchanged"}});
      reason = "cone_off";
    } else {
      g_stage.store(kCone, std::memory_order_release);
    }
  }
  const int on = g_stage.load(std::memory_order_acquire);
  const bool full = on == asked;
  char ret[19];
  sentinel::LogFields(full ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_powerboost_install",
                      {{"result", full ? "installed" : "partial"}, {"stage", on}, {"asked_stage", asked}, {"reason", reason},
                       {"return_address_vaddr", on == kCone ? sentinel::HexString(ret, pins.coneRet) : "none"},
                       {"consequence", on == kCone      ? "markers never reach Heal, uses are sent, a target outside the cone is unlinked; in a combat map or the combat lobby only"
                                       : on == kSwallow ? "markers never reach Heal and uses are sent; no cone; in a combat map or the combat lobby only"
                                                        : "markers are only recorded; they still reach the game as heals"}});
  return {full, full ? "installed" : "partial"};
}

ModuleResult Install(int stage) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins, stage, nullptr);
}

Stats CurrentStats() noexcept {
  return {g_markers.load(),    g_swallowed.load(),      g_sent.load(),         g_heldN.load(),       g_links.load(),
          g_coneCalls.load(),  g_coneRejects.load(),    g_pendingSent.load(),  g_pendingDropped.load(), g_scopeChecks.load(),
          g_stage.load(),      g_cone.load()};
}

const std::uint8_t* ConeAtForTest(const PhTarget* target, const std::uint8_t* frame, std::uintptr_t dcs) noexcept {
  return ConeAt(target, frame, dcs);
}

void SetClockForTest(std::uint64_t (*nowMs)()) noexcept { g_clock = nowMs; }
void SwallowForTest(bool on) noexcept { g_swallow.store(on, std::memory_order_release); }
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetPinsForTest(const Pins* pins) noexcept { g_pinsOverride.store(pins, std::memory_order_release); }
void SetNetTickRegistrarForTest(bool (*reg)(void (*)() noexcept) noexcept) noexcept { g_registrar.store(reg, std::memory_order_release); }
std::uintptr_t ConeRetForTest() noexcept { return __atomic_load_n(&nevr_combat_pb_cone_ret, __ATOMIC_ACQUIRE); }
std::uintptr_t NextForTest() noexcept { return reinterpret_cast<std::uintptr_t>(Next()); }

void ResetForTest() noexcept {
  for (sentinel::GotHook* h : {&g_nodeHook, &g_healHook, &g_queuedHook, &g_linkHook, &g_resolveHook}) h->Forget();
  NodeThunk::Reset();
  HealThunk::Reset();
  QueuedHealThunk::Reset();
  LinkThunk::Reset();
  __atomic_store_n(&nevr_combat_pb_next, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
  __atomic_store_n(&nevr_combat_pb_cone_ret, std::uintptr_t{0}, __ATOMIC_RELEASE);
  g_ran = false;
  g_base = 0;
  g_damageableVptr = 0;
  g_fns = Fns{};
  g_swallow = g_cone = false;
  g_stage = 0;
  g_game = 0;
  g_inScope = false;
  g_frames = 0;
  g_scopeKind = -1;
  g_lastConeLine = 0;
  for (std::atomic<int>* n : {&g_markerLines, &g_linkLines, &g_swallowLines, &g_coneLines, &g_sendLines, &g_scopeLines}) n->store(0);
  for (std::atomic<std::uint32_t>* n : {&g_markers, &g_swallowed, &g_sent, &g_heldN, &g_links, &g_coneCalls, &g_coneRejects, &g_pendingSent,
                                        &g_pendingDropped, &g_scopeChecks})
    n->store(0);
  g_lookup = nullptr;
  g_pinsOverride = nullptr;
  g_registrar = nullptr;
  Lock();
  g_filter = LinkFilter();
  g_held = Held{};
  g_use = UseState{};
  g_coneTable = ConeTable();
  g_nPending = 0;
  g_pendingPublished = 0;
  std::memset(g_pending, 0, sizeof g_pending);
  Unlock();
}

}  // namespace nevr_quest::combat::powerboost

// ---- the entry and its continuation ---------------------------------------------------------------------------------------------

namespace pb = nevr_quest::combat::powerboost;

extern "C" const std::uint8_t* nevr_combat_pb_cone_at(const void* target, const std::uint8_t* frame, std::uintptr_t dcs) {
  return pb::ConeAt(static_cast<const pb::PhTarget*>(target), frame, dcs);
}

#if defined(__aarch64__)
#include "quest/combat/powerboost_thunk.inc"
#else
// Host build only (x86-64): the arm64 entry cannot run here. This stand-in passes every call to the next word; the entry itself runs under
// qemu (src/quest/tests/pb_thunk_qemu.cpp, src/quest/combat/check/powerboost_thunk.sh), and its continuation is tested directly.
extern "C" __attribute__((visibility("hidden"), noinline)) const std::uint8_t* nevr_combat_pb_resolve_entry(const void* target) {
  const auto next = reinterpret_cast<const std::uint8_t* (*)(const void*)>(__atomic_load_n(&nevr_combat_pb_next, __ATOMIC_ACQUIRE));
  return next != nullptr ? next(target) : nullptr;
}
#endif

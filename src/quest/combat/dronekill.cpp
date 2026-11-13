// dronekill (dronekill.h, docs/quest/combat/dronekill.md): four CallbackThunks on libr15's JUMP_SLOTs through the hook manager, reads through
// combat/safe_read.h, calls into libr15 only after its read shapes match, every act scoped to a loaded combat map.
#include "quest/combat/dronekill.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>

#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/dronekill_core.h"
#include "quest/combat/line_budget.h"
#include "quest/combat/ec.h"
#include "quest/combat/ec_core.h"
#include "quest/combat/safe_read.h"
#include "quest/combat/thread_word.h"
#include "quest/sentinel/ec_page.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::dronekill {

namespace {

using namespace dk;
namespace pinned = sentinel::pinned;
using sentinel::LogLevel;

struct NodeData {
  std::uint64_t actor, space, system, component;
  std::uint32_t enable;
};
struct Capture {
  bool have = false;
  std::uint32_t ok = 0;
  void* space = nullptr;
  std::uint64_t actor = 0;
};
using ComponentFn = std::uint64_t (*)(const void* system, std::uint64_t actor, std::uint64_t component_sym);
using DisableFn = void (*)(void* system, std::uint64_t handle, std::uint32_t category);
using EnableFn = void (*)(void* system, std::uint64_t handle, std::uint32_t category);

// One thunk per hooked slot (callback_thunk.h).
struct ActorTag {};
struct EnterTag {};
struct FireTag {};
struct AttackTag {};
using ActorThunk = sentinel::CallbackThunk<ActorTag, std::uint32_t(const void*, const void*, std::uint64_t, void**, void*, unsigned*)>;
using EnterThunk = sentinel::CallbackThunk<EnterTag, void(void*, const NodeData*)>;
using FireThunk = sentinel::CallbackThunk<FireTag, void(void*, std::uint32_t, DkC3, std::uint32_t)>;
using AttackThunk = sentinel::CallbackThunk<AttackTag, void(void*, std::uint32_t)>;

constexpr int kLineCap = 160, kNoteCap = 24, kOffLineCap = 64, kKLineCap = 48, kF357LineCap = 96;
constexpr unsigned kExpiredLines = 16;  // the expired lines (D11) written before the one notice: the first 16 of a run
constexpr int kVtCache = 16;

// The four handles: Actor, Enter, FireBulletLocal, AttackTarget.
constexpr int kHookCount = 4;
sentinel::GotHook g_hooks[kHookCount];

// ---- set before the first install, read by the handlers ----
std::uintptr_t g_base = 0, g_cmp_plt = 0;
Fns g_fns{};
bool g_cmp_ok = false;
std::atomic<bool> g_installed{false};
bool g_kill = false, g_lockon = false, g_gun = false, g_chase = false, g_shot = false, g_explosion = false;
bool g_hold = false;  // `combat.dronekill_hold` (and the explosion on): the first-system off is held back (dronekill.h "The hold")
std::atomic<bool> g_shot_swapped{false};

// The scope (dronekill.h, "Arena"): written by the tick only, read by every handler with one relaxed load. The copy is only as fresh as the tick's
// last ask, so every in-scope body and OnKilled also check scope::LookIsFresh() (README rule 9): a combat answer older than kStaleAfterMs passes the call.
std::atomic<bool> g_inScope{false};
// Drone Enters in flight on any thread: the Actor handler reads its per-thread capture only while one is.
std::atomic<int> g_capturing{0};

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
Table g_table;           // under g_mu
TouchTable g_touch;      // under g_mu
GunTable g_guns;         // under g_mu
ChaseTable g_chases;     // under g_mu
void* g_aux_space = nullptr;  // under g_mu: the game space of the three tables above
FxQueue g_fx;            // under g_mu: the explosions due (#1737)
OffRetries g_offs;       // under g_mu: held components due to go off, retried while their game space defers the disable
std::atomic<bool> g_any_due{false};  // written under g_mu only, read without it

std::atomic<std::uintptr_t> g_vt[kVtCache] = {};      // vtables whose Component slots were checked: low bit 1 = the read shapes
std::atomic<std::uintptr_t> g_vt_dis[kVtCache] = {};  // the same for slot +0x210 DisableComponent
std::atomic<std::uintptr_t> g_vt_en[kVtCache] = {};   // the same for slot +0x208 EnableComponent

// The per-thread state of the handlers (thread_word.h: a pthread key, never thread_local). A pointer word points at a frame on the stack of
// the handler that set it (0: none), so a nested call restores its caller's frame when it returns.
ThreadWord g_t_cap;      // the Capture of the drone Enter running on this thread; set only while it runs
ThreadWord g_t_shot;     // the ShotFrame of the FireBulletLocal running on this thread
ThreadWord g_t_in_tick;  // 1 while this thread runs TickBody
struct ShotFrame {
  DkC3 velocity;
  std::uint32_t remote;
};
ShotFrame g_test_shot;  // what SetShotForTest points this thread's word at (a test seam: one frame for the process)

Capture* CurrentCapture() noexcept { return reinterpret_cast<Capture*>(g_t_cap.Get()); }
void SetCapture(Capture* c) noexcept { g_t_cap.Set(reinterpret_cast<std::uintptr_t>(c)); }

std::atomic<int> g_lines{0}, g_notes{0}, g_off_lines{0}, g_k_lines{0}, g_f357_lines{0}, g_chase_lines{0};
std::atomic<std::uint32_t> g_enter_drone{0}, g_enter_other{0}, g_fires{0}, g_attacks{0}, g_ticks{0}, g_due_runs{0};
std::atomic<std::uint32_t> g_expired{0};  // drone ends the D11 rule saw (the stats record's expired)
std::atomic<unsigned> g_expired_lines{0};  // the expired lines the budget has counted
std::atomic<std::uint32_t> g_explosions{0}, g_held{0};  // explosions run, first-system offs held (the stats record)
std::atomic<unsigned> g_fx_lines{0}, g_fx_skip_lines{0}, g_hold_lines{0}, g_stays_on_lines{0};
constexpr unsigned kFxLines = 64, kFxSkipLines = 16, kHoldLines = 64, kStaysOnLines = 16;

// What the scope line reports: each hook's thunk calls when the scope last changed, and when.
std::uint64_t g_scopeSince[kHookCount] = {};
std::uint64_t g_scopeSinceMs = 0;

std::uint64_t (*g_clock_ms)() = nullptr;
const Calls* g_test_calls = nullptr;
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};
std::atomic<const Fns*> g_fnsOverride{nullptr};
std::atomic<bool (*)(std::uintptr_t, EntryInfo*)> g_entryInfo{nullptr};

std::uint64_t NowMs() noexcept {
  if (g_clock_ms != nullptr) return g_clock_ms();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
}

std::uint32_t Tid() noexcept { return static_cast<std::uint32_t>(syscall(SYS_gettid)); }

const char* Fmt(char (&b)[32], double v) noexcept {
  std::snprintf(b, sizeof b, "%.6f", v);
  return b;
}

// One capped line kind; the line that hits the cap says so once (a silent stop would look like the module went quiet).
bool Allow(std::atomic<int>& n, int cap, const char* what) noexcept {
  const int v = n.fetch_add(1, std::memory_order_relaxed);
  if (v < cap) return true;
  if (v == cap) {
    sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_cap",
                        {{"kind", what}, {"cap", cap}, {"msg", "this kind of dronekill line reached its cap: later ones are counted, not written"}});
  }
  return false;
}

// ---- calls into game code through vtable slots (the test replaces them) ----
std::uint64_t CallComponent(std::uintptr_t fn, const void* system, std::uint64_t actor, std::uint64_t sym) noexcept {
  if (g_test_calls != nullptr) return g_test_calls->component(fn, system, actor, sym);
  return reinterpret_cast<ComponentFn>(fn)(system, actor, sym);
}
void CallDisable(std::uintptr_t fn, void* system, std::uint64_t handle, std::uint32_t category) noexcept {
  if (g_test_calls != nullptr) return g_test_calls->disable(fn, system, handle, category);
  reinterpret_cast<DisableFn>(fn)(system, handle, category);
}
void CallEnable(std::uintptr_t fn, void* system, std::uint64_t handle, std::uint32_t category) noexcept {
  if (g_test_calls != nullptr) return g_test_calls->enable(fn, system, handle, category);
  reinterpret_cast<EnableFn>(fn)(system, handle, category);
}

// A vtable-keyed cache of "this vtable's slot holds the read shape": low bit 1 = yes. A full cache only means the words are read again.
int CachedShape(std::atomic<std::uintptr_t>* cache, std::uintptr_t vt, bool* hit) noexcept {
  *hit = false;
  for (int i = 0; i < kVtCache; ++i) {
    const std::uintptr_t v = cache[i].load(std::memory_order_acquire);
    if ((v & ~static_cast<std::uintptr_t>(1)) == vt) {
      *hit = true;
      return static_cast<int>(v & 1);
    }
  }
  return 0;
}
void CacheShape(std::atomic<std::uintptr_t>* cache, std::uintptr_t vt, bool ok) noexcept {
  for (int i = 0; i < kVtCache; ++i) {
    std::uintptr_t empty = 0;
    if (cache[i].compare_exchange_strong(empty, vt | (ok ? 1 : 0), std::memory_order_acq_rel)) break;
    if ((empty & ~static_cast<std::uintptr_t>(1)) == vt) break;
  }
}

bool ReadVtable(const void* system, std::uintptr_t* vt) noexcept {
  return SafeRead(reinterpret_cast<std::uintptr_t>(system), vt, sizeof *vt) && *vt != 0 && (*vt & 1) == 0;
}

// 1 = both slots hold the read shapes, 0 = not (or unreadable). Reads only. *fn = the Component() thunk's address.
int ComponentShape(const void* system, std::uintptr_t* fn) noexcept {
  std::uintptr_t vt = 0;
  if (!ReadVtable(system, &vt)) return 0;
  std::uintptr_t thunk = 0, body = 0;
  if (!SafeRead(vt + kVtComponent, &thunk, sizeof thunk) || !SafeRead(vt + kVtComponentId, &body, sizeof body)) return 0;
  *fn = thunk;
  bool hit = false;
  const int cached = CachedShape(g_vt, vt, &hit);
  if (hit) return cached;
  std::uint32_t tw[kThunkWords], bw[kBodyWords];
  const bool ok = SafeRead(thunk, tw, sizeof tw) && SafeRead(body, bw, sizeof bw) && ThunkOk(tw) && BodyOk(bw, body, g_base + g_cmp_plt);
  CacheShape(g_vt, vt, ok);
  return ok ? 1 : 0;
}

// Stage 1: 1 = the system's DisableComponent slot holds the read 48-word shape, 0 = not. Reads only.
int DisableShape(const void* system, std::uintptr_t* fn) noexcept {
  std::uintptr_t vt = 0, body = 0;
  if (!ReadVtable(system, &vt) || !SafeRead(vt + kVtDisable, &body, sizeof body)) return 0;
  *fn = body;
  bool hit = false;
  const int cached = CachedShape(g_vt_dis, vt, &hit);
  if (hit) return cached;
  std::uint32_t w[kDisWords];
  const bool ok = SafeRead(body, w, sizeof w) && DisableOk(w);
  CacheShape(g_vt_dis, vt, ok);
  return ok ? 1 : 0;
}

// F3: 1 = the system's EnableComponent slot holds the read 47-word shape, with the disable-mask offsets of its own body in *at.
int EnableShape(const void* system, std::uintptr_t* fn, MaskAt* at) noexcept {
  std::uintptr_t vt = 0, body = 0;
  if (!ReadVtable(system, &vt) || !SafeRead(vt + kVtEnable, &body, sizeof body)) return 0;
  *fn = body;
  std::uint32_t w[kEnWords];
  if (!SafeRead(body, w, sizeof w)) return 0;
  *at = MaskOffsets(w);
  bool hit = false;
  const int cached = CachedShape(g_vt_en, vt, &hit);
  if (hit) return cached;
  const bool ok = EnableOk(w);
  CacheShape(g_vt_en, vt, ok);
  return ok ? 1 : 0;
}

// F3: 1 = the component's category-1 disable bit is set (it is off), 0 = clear, -1 = unreadable
int DisableBit(std::uintptr_t system, std::uint32_t handle, const MaskAt& at) noexcept {
  std::uintptr_t idx = 0, masks = 0;
  std::uint16_t slot = 0, mask = 0;
  if (!SafeRead(system + at.idx, &idx, sizeof idx) || !idx) return -1;
  if (!SafeRead(idx + (handle & 0xffff) * 4ull, &slot, sizeof slot)) return -1;
  if (!SafeRead(system + at.maskArr, &masks, sizeof masks) || !masks) return -1;
  if (!SafeRead(masks + slot * 2ull, &mask, sizeof mask)) return -1;
  return mask & 1;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Actor's handler: the real call, untouched arguments and result; while a drone Enter runs on this thread, a copy of the two out-parameters
// (game space, SActor) the real call just wrote. The module never calls Actor itself. With no drone Enter in flight anywhere (always, outside a
// combat map) it returns after the call and one relaxed load.
__attribute__((noinline)) void ActorCapture(std::uint32_t r, std::uint64_t actor_sym, void** space_out, void* actor_out) noexcept {
  Capture* const c = CurrentCapture();
  // Enter passes [data+0] = the drone actor's symbol in x2: an Actor call for another actor that re-enters this slot inside the real Actor
  // returns first and must not be taken for Enter's own
  if (c != nullptr && !c->have && actor_sym == kDroneActor) {
    c->have = true;
    c->ok = r;
    if (r != 0 && space_out != nullptr && actor_out != nullptr) {
      void* sp = nullptr;
      std::uint64_t actor = 0;
      if (SafeRead(reinterpret_cast<std::uintptr_t>(space_out), &sp, sizeof sp) &&
          SafeRead(reinterpret_cast<std::uintptr_t>(actor_out), &actor, sizeof actor)) {
        c->space = sp;
        c->actor = actor;
      }
    }
  }
}
std::uint32_t ActorHandler(ActorThunk::Fn real, const void* thread, const void* space, std::uint64_t actor_sym, void** space_out, void* actor_out,
                           unsigned* flag_out) noexcept {
  const std::uint32_t r = real(thread, space, actor_sym, space_out, actor_out, flag_out);
  if (g_capturing.load(std::memory_order_relaxed) == 0) return r;
  ActorCapture(r, actor_sym, space_out, actor_out);
  return r;
}

void Note(const NodeData& d, std::uint32_t tid, const char* why) noexcept {
  if (!Allow(g_notes, kNoteCap, "note")) return;
  char sys[19], comp[19];
  sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_note",
                      {{"system", sentinel::HexString(sys, d.system)}, {"component", sentinel::HexString(comp, d.component)},
                       {"enable", d.enable ? "on" : "off"}, {"tid", tid}, {"why", why},
                       {"msg", "a drone node was left to the game alone: dronekill could not look its component up"}});
}

void F357Line(const char* event, const char* msg, std::initializer_list<sentinel::LogField> fields) noexcept {
  if (!Allow(g_f357_lines, kF357LineCap, "f357")) return;
  sentinel::LogField all[8] = {{"msg", msg}, {"msg", msg}, {"msg", msg}, {"msg", msg}, {"msg", msg}, {"msg", msg}, {"msg", msg}, {"msg", msg}};
  std::size_t n = 0;
  for (const sentinel::LogField& f : fields) {
    if (n < 7) all[n++] = f;
  }
  all[n++] = {"msg", msg};
  sentinel::LogFieldList(LogLevel::kInfo, event, all, n);
}

bool ReadNode(const NodeData* data, NodeData* d) noexcept {
  const auto a = reinterpret_cast<std::uintptr_t>(data);
  return SafeRead(a, d, 0x20) && SafeRead(a + 0x20, &d->enable, sizeof d->enable);  // 0x24 bytes: never reads past the game's struct
}

// The stats line (#2359): every counter CurrentStats() returns, and the drone ends the D11 rule saw. The same record every combat module writes.
void LogStats(bool final) noexcept {
  const Stats s = CurrentStats();
  sentinel::LogFields(final ? LogLevel::kInfo : LogLevel::kDebug, "combat_dronekill_stats",
                      {{"enter_drone", s.enter_drone}, {"enter_other", s.enter_other}, {"fires", s.fires}, {"attacks", s.attacks},
                       {"ticks", s.ticks}, {"due_runs", s.due_runs}, {"capturing", s.capturing}, {"expired", s.expired},
                       {"explosions", s.explosions}, {"held", s.held}, {"final", final ? 1 : 0},
                       {"consequence", "the counts dronekill.h names (Stats): drone and other Enters in a combat map, drone shots, AttackTarget "
                                       "calls, frame ticks, ticks with work due, Enters in flight, drone ends, explosions run and offs held; over "
                                       "the run's time they are the hooks' call rates"}});
}

// ---- the explosion (#1737, dronekill.h "The explosion") ----
int OffOne(const Comp& c, const char** why) noexcept;

// The EC page's drone block word at `off` (ec_core.h); 0 = unreadable.
std::uint64_t PageWord(std::uint32_t off) noexcept {
  std::uint64_t w = 0;
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(sentinel::EcPage()) + ec::core::kDroneBlock + off, &w, sizeof w)) return 0;
  return w;
}

const char* HoldMsg(const char* step) noexcept {
  if (std::strcmp(step, "held") == 0) return "a drone's first-system off is held back: it goes off 2.5 s after the drone's explosion";
  if (std::strcmp(step, "let_go") == 0) return "no damageable off came within 2.5 s of a hold: the held component goes off now";
  if (std::strcmp(step, "released") == 0) return "a held component went off";
  if (std::strcmp(step, "off_next_tick") == 0) return "a held component goes off at the next tick";
  if (std::strcmp(step, "stays_on") == 0)
    return "a held component could not be turned off: it stays on, and nothing will turn it off (the game's Enter for it did not run)";
  if (std::strcmp(step, "end_open") == 0) return "a drone's first-system off ran (not held): its end explodes when its damageable goes off";
  if (std::strcmp(step, "end_not_open") == 0)
    return "a drone's first-system off ran (not held) and no single live drone has its actor: that drone's end will not explode";
  return "a drone's first-system off was not held: the game's own Enter ran at once";
}
void HoldLine(LogLevel level, const char* step, std::uint32_t index, int group, const char* why, int ctxMatch = -1) noexcept {
  const bool stays = std::strcmp(step, "stays_on") == 0;  // a Warning: its own budget, so the Debug lines cannot use it up
  if (stays ? !LineBudget(&g_stays_on_lines, kStaysOnLines, "dronekill", "hold_stays_on", "no further stays_on lines are written")
            : !LineBudget(&g_hold_lines, kHoldLines, "dronekill", "hold", "no further hold lines are written; the stats line counts the holds")) return;
  sentinel::LogFields(level, "combat_dronekill_hold",
                      {{"index", index & 0xffff}, {"group", group}, {"step", step}, {"why", why}, {"ctx_match", ctxMatch}, {"msg", HoldMsg(step)}});
}
void SkipLine(std::uint32_t index, int group, const char* reason, std::uintptr_t entry = 0) noexcept {
  if (!LineBudget(&g_fx_skip_lines, kFxSkipLines, "dronekill", "explosion_skipped",
                  "no further explosion_skipped lines are written; the stats line counts the explosions run")) return;
  char e[19];
  sentinel::LogFields(LogLevel::kWarn, "combat_dronekill_explosion_skipped",
                      {{"index", index & 0xffff}, {"group", group}, {"reason", reason}, {"entry", sentinel::HexString(e, entry)},
                       {"msg", "a dead drone's explosion was not run: the drone goes without it"}});
}

// The hold (dronekill.h): the first-system off of the drone whose group is the only one that can still run its end (SoleCandidate). True =
// held (the caller does not run the real Enter); false = not resolved (the caller runs the real Enter at once, as the game would). A hold is
// taken only when everything turning the component off later needs is in place now: the component, the system's DisableComponent shape and
// its game space's invoke-immediate word.
__attribute__((noinline)) bool TryHold(const NodeData& d) noexcept {
  int candidates = 0, number = 0;
  bool dead = false;
  std::uint32_t index = 0;
  bool already = false;
  std::uint64_t actor = 0, space = 0, gctx = 0;
  pthread_mutex_lock(&g_mu);
  if (const Group* g = g_table.SoleCandidate(&candidates, &dead)) {
    number = g->x.number;
    index = g->handle;
    already = g->x.holdOpen;
    actor = g->actor;
    space = g->n > 0 ? g->comps[0].x.space : 0;
    gctx = g->x.ctx;
  }
  pthread_mutex_unlock(&g_mu);
  if (number == 0) {
    HoldLine(LogLevel::kDebug, "not_held", 0, 0, candidates == 0 ? "no_live_group" : dead ? "dead_group" : "ambiguous");
    return false;
  }
  if (already) {
    HoldLine(LogLevel::kDebug, "not_held", index, number, "already_held");
    return false;
  }
  void* const system = space != 0 && g_fns.find != nullptr ? g_fns.find(reinterpret_cast<const void*>(space), d.system) : nullptr;
  if (system == nullptr) {
    HoldLine(LogLevel::kDebug, "not_held", index, number, "no_system");
    return false;
  }
  std::uintptr_t component = 0;
  if (!g_cmp_ok || !ComponentShape(system, &component)) {
    HoldLine(LogLevel::kDebug, "not_held", index, number, "no_shape");
    return false;
  }
  std::uintptr_t disable = 0;
  if (DisableShape(system, &disable) == 0) {
    HoldLine(LogLevel::kDebug, "not_held", index, number, "no_disable_shape");
    return false;
  }
  std::uintptr_t owner = 0;
  std::uint32_t flag = 0;
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(system) + kSysOwner, &owner, sizeof owner) || owner == 0 ||
      !SafeRead(owner + kOwnerFlag, &flag, sizeof flag)) {
    HoldLine(LogLevel::kDebug, "not_held", index, number, "owner_unreadable");
    return false;
  }
  const std::uint32_t handle = static_cast<std::uint32_t>(CallComponent(component, system, actor, d.component));
  if ((handle & 0xffff) == 0xffff) {
    HoldLine(LogLevel::kDebug, "not_held", index, number, "no_component");
    return false;
  }
  Comp c;
  c.system = reinterpret_cast<std::uint64_t>(system);
  c.handle = handle;
  c.at = NowMs();
  c.actor = actor;
  c.x.sysSym = d.system;
  c.x.compSym = d.component;
  c.x.space = space;
  c.x.owner = owner;
  bool held = false;
  pthread_mutex_lock(&g_mu);
  if (Group* g = g_table.ByNumber(number); g != nullptr && g->used && !g->dead && !g->x.holdOpen) {
    g->x.holdOpen = true;
    g->x.holdAt = c.at;
    g->x.held = c;
    g_any_due.store(true, std::memory_order_release);
    held = true;
  }
  pthread_mutex_unlock(&g_mu);
  if (!held) {
    HoldLine(LogLevel::kDebug, "not_held", index, number, "group_changed");
    return false;
  }
  g_held.fetch_add(1, std::memory_order_relaxed);
  HoldLine(LogLevel::kDebug, "held", index, number, "ok", PageWord(ec::core::kDroneCtx) == gctx ? 1 : 0);
  return true;
}

// A held component goes off now, from the caller (no lock held). Its outcome is one line: Debug released, or Warning stays_on (OffOne
// could not turn it off, or its game space defers the disable and nothing will retry it: why).
void HeldOffNow(const Comp& c, std::uint32_t index, int group, const char* why) noexcept {
  const char* r = "";
  const int k = OffOne(c, &r);
  if (k == 0) return HoldLine(LogLevel::kDebug, "released", index, group, why);
  HoldLine(LogLevel::kWarn, "stays_on", index, group, k == 2 ? "invoke_immediate_0" : r);
}

// What a damageable off or a 'K' decided about a group's explosion and hold (under g_mu); the lines are written after the lock.
struct FxOutcome {
  const char* skipped = nullptr;  // explosion_skipped's reason
  const char* released = nullptr; // the held component handed to the off list: why
  bool direct = false;            // the off list was full: the caller turns `held` off itself
  Comp held{};
  int group = 0;
  std::uint32_t index = 0;
};

// A held component goes to the off list, or (full) back to the caller.
void ReleaseLocked(Group& g, const char* why, std::uint64_t now, FxOutcome* out) noexcept {
  g.x.holdOpen = false;
  out->released = why;
  if (!g_offs.Add(g.x.held, g.x.number, g.handle, now)) {
    out->direct = true;
    out->held = g.x.held;
  }
  g_any_due.store(true, std::memory_order_release);
}

// The damageable's off (the drone's end), after the shared D11 rule marked the group dead: the expiry rule (dronekill_rules.h).
FxOutcome ExpiryLocked(std::uint64_t system, std::uint32_t handle, bool alive, std::uint64_t now) noexcept {
  FxOutcome out;
  const int s = g_table.t.Find(system, handle);
  if (s < 0) return out;
  Group& g = g_table.t.g[s];
  out.group = g.x.number;
  out.index = handle;
  g.x.ended = true;  // its end script ran: no longer a candidate for a first-system off (SoleCandidate)
  const bool held = g.x.holdOpen, open = held || g.x.endOpen;
  g.x.endOpen = false;
  if (!open) return out;  // no first-system off seen for it: the game's own end, no explosion (the expiry is not open)
  switch (::nevr_dronekill::OnDamageableOff(alive, open, g.x.ctx != 0)) {
    case ::nevr_dronekill::ExpiryAction::kQueue: {
      const FxHeld h{g.x.held, g.x.number, handle};
      if (g_fx.Queue(FxCtx{g.x.ctx, g.x.number, handle, false, held}, held ? &h : nullptr, held ? 1 : 0, now)) {
        g.x.holdOpen = false;
        g_any_due.store(true, std::memory_order_release);
        return out;
      }
      out.skipped = "queue_full";
      if (held) ReleaseLocked(g, "queue_full", now, &out);
      return out;
    }
    case ::nevr_dronekill::ExpiryAction::kOffNow:
      if (alive) out.skipped = "no_context";
      if (held) ReleaseLocked(g, alive ? "no_context" : "dead", now, &out);
      return out;
  }
  return out;
}

// A 'K' on a live group (marked dead by the shared kill rule): its explosion, holding nothing; an open hold goes off now.
FxOutcome KilledLocked(Group& g, std::uint64_t now) noexcept {
  FxOutcome out;
  out.group = g.x.number;
  out.index = g.handle;
  if (g.x.holdOpen) ReleaseLocked(g, "killed", now, &out);
  if (g.x.ctx == 0) {
    out.skipped = "no_context";
  } else if (g_fx.Queue(FxCtx{g.x.ctx, g.x.number, g.handle, true, false}, nullptr, 0, now)) {
    g_any_due.store(true, std::memory_order_release);
  } else {
    out.skipped = "queue_full";
  }
  return out;
}

void FxOutcomeLines(const FxOutcome& o) noexcept {
  if (o.skipped != nullptr) SkipLine(o.index, o.group, o.skipped);
  if (o.released == nullptr) return;
  if (o.direct) return HeldOffNow(o.held, o.index, o.group, "off_list_full");  // the off list was full: off now, from here
  HoldLine(LogLevel::kDebug, "off_next_tick", o.index, o.group, o.released);
}

using EntryFn = void (*)(void* ctx);

bool DladdrInfo(std::uintptr_t fn, EntryInfo* out) {
  Dl_info di{};
  if (dladdr(reinterpret_cast<void*>(fn), &di) == 0 || di.dli_fname == nullptr) return false;
  out->fname = di.dli_fname;
  out->fbase = reinterpret_cast<std::uintptr_t>(di.dli_fbase);
  return true;
}
bool EntryOk(std::uintptr_t fn, std::uint64_t offset) noexcept {
  bool (*info)(std::uintptr_t, EntryInfo*) = g_entryInfo.load(std::memory_order_acquire);
  if (info == nullptr) info = &DladdrInfo;
  EntryInfo e{nullptr, 0};
  return info(fn, &e) && EntryAt(fn, e.fname, e.fbase, ec::core::kDroneLib, offset);
}

// One explosion at the tick: the drone library's spawn and sound entries with the context kept at the release, its state word at +0x18
// zeroed round each and put back after both (PC SpawnDeathFx).
void RunExplosion(const FxCtx& f) noexcept {
  const std::uintptr_t spawn = PageWord(ec::core::kDroneSpawn), sound = PageWord(ec::core::kDroneSoundFn);
  if (spawn == 0 || sound == 0) return SkipLine(f.index, f.group, "no_entry");
  if (!EntryOk(spawn, ec::core::kDroneSpawnEntry)) return SkipLine(f.index, f.group, "foreign_entry", spawn);
  if (!EntryOk(sound, ec::core::kDroneSoundEntry)) return SkipLine(f.index, f.group, "foreign_entry", sound);
  std::uint64_t saved = 0;
  if (f.ctx == 0 || !SafeRead(static_cast<std::uintptr_t>(f.ctx) + 0x18, &saved, sizeof saved)) return SkipLine(f.index, f.group, "no_context");
  auto* const state = reinterpret_cast<volatile std::uint64_t*>(static_cast<std::uintptr_t>(f.ctx) + 0x18);
  void* const ctx = reinterpret_cast<void*>(static_cast<std::uintptr_t>(f.ctx));
  *state = 0;  // before the spawn entry
  reinterpret_cast<EntryFn>(spawn)(ctx);
  *state = 0;  // before the sound entry
  reinterpret_cast<EntryFn>(sound)(ctx);
  *state = saved;  // after both
  g_explosions.fetch_add(1, std::memory_order_relaxed);
  if (!LineBudget(&g_fx_lines, kFxLines, "dronekill", "explosion", "no further explosion lines are written; the stats line counts the explosions")) return;
  sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_explosion",
                      {{"index", f.index & 0xffff}, {"group", f.group}, {"reason", f.killed ? "killed" : "expired"},
                       {"sound_delay_ms", f.held ? static_cast<unsigned>(kFxSoundHoldMs) : 0u}, {"entries", 2},
                       {"msg", f.held ? "a drone that ran out exploded; its held component goes off 2.5 s from now"
                               : f.killed ? "a drone that was shot down exploded"
                                          : "a drone that ran out exploded (nothing was held: its first system went off at the game's own time)"}});
}

// CEnableComponentNode::Enter in a combat map (EnterHandler below is the hook).
__attribute__((noinline)) void EnterInScope(EnterThunk::Fn real, void* node, const NodeData* data) noexcept {
  if (!scope::LookIsFresh()) return real(node, data);  // the tick stopped: the answer is not believed (combat_scope.h)
  std::uint64_t actor = 0;
  if (data == nullptr || !SafeRead(reinterpret_cast<std::uintptr_t>(data), &actor, sizeof actor) || actor != kDroneActor) {
    g_enter_other.fetch_add(1, std::memory_order_relaxed);
    real(node, data);
    return;
  }
  NodeData d{};
  if (!ReadNode(data, &d)) {
    real(node, data);
    return;
  }
  const std::uint32_t drones = g_enter_drone.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((drones & (kStatsEvery - 1)) == 0) LogStats(false);
  if (g_hold && !d.enable && d.system == kNcaPhysics && TryHold(d)) return;  // held: the real Enter is not run (dronekill.h "The hold")
  Capture cap;
  Capture* const outer = CurrentCapture();  // a drone Enter inside a drone Enter keeps its own capture
  g_capturing.fetch_add(1, std::memory_order_relaxed);
  SetCapture(&cap);
  real(node, data);  // DK-3: the game's own work first, arguments untouched
  SetCapture(outer);
  g_capturing.fetch_sub(1, std::memory_order_relaxed);
  const std::uint32_t tid = Tid();
  if (!cap.have || !cap.ok || cap.space == nullptr) {
    Note(d, tid, "no actor (Enter found none, or Actor was not called)");
    return;
  }
  if (g_explosion && !d.enable && d.system == kNcaPhysics) {  // the first-system off ran (not held): its drone's expiry opens by its actor
    pthread_mutex_lock(&g_mu);
    const int opened = g_table.OpenEnd(cap.actor);
    pthread_mutex_unlock(&g_mu);
    HoldLine(LogLevel::kDebug, opened != 0 ? "end_open" : "end_not_open", 0, opened, opened != 0 ? "actor" : "no_single_live_group");
  }
  void* const system = g_fns.find != nullptr ? g_fns.find(cap.space, d.system) : nullptr;
  if (system == nullptr) {
    Note(d, tid, "no such system in the actor's game space");
    return;
  }
  std::uintptr_t component = 0;
  if (!g_cmp_ok || !ComponentShape(system, &component)) {
    Note(d, tid, "system not looked up (its Component() is not the read shape)");
    return;
  }
  const std::uint32_t handle = static_cast<std::uint32_t>(CallComponent(component, system, cap.actor, d.component));
  if ((handle & 0xffff) == 0xffff) {
    Note(d, tid, "no component under that name (Enter's alias retry is not followed)");
    return;
  }
  std::uintptr_t owner = 0;
  std::uint32_t flag = 0xffffffffu;  // ffffffff = unreadable
  if (SafeRead(reinterpret_cast<std::uintptr_t>(system) + kSysOwner, &owner, sizeof owner) && owner != 0) SafeRead(owner + kOwnerFlag, &flag, sizeof flag);
  if (g_lockon || g_gun || g_chase) {  // L5 / M4: a drone node in another game space = a level change
    pthread_mutex_lock(&g_mu);
    if (cap.space != g_aux_space) {
      g_touch.Clear();
      g_guns.Clear();
      g_chases.Clear();
      g_aux_space = cap.space;
    }
    pthread_mutex_unlock(&g_mu);
  }
  if (g_lockon && d.system == kR14Touch && !d.enable) {  // F3 L1: the script made it ungrabbable (a release)
    Comp c;
    c.system = reinterpret_cast<std::uint64_t>(system);
    c.handle = handle;
    c.at = NowMs();
    c.actor = cap.actor;
    c.x.sysSym = d.system;
    c.x.compSym = d.component;
    c.x.space = reinterpret_cast<std::uint64_t>(cap.space);
    c.x.owner = owner;
    pthread_mutex_lock(&g_mu);
    g_touch.Deployed(c, c.at);
    g_any_due.store(true, std::memory_order_release);
    pthread_mutex_unlock(&g_mu);
    F357Line("combat_dronekill_touch", "touch deployed: the drone stays grabbable until it locks on; on again at the tick",
             {{"step", "deployed"}, {"index", handle & 0xffff}, {"tid", tid}});
  }
  if (g_gun && d.system == kNcaModel && (d.component == kGun || d.component == kPort)) {  // F5
    std::uint32_t shown = 0;
    bool known = false;
    pthread_mutex_lock(&g_mu);
    if (d.component == kGun) g_guns.Set(d.actor, d.enable);
    else known = g_guns.Shown(d.actor, &shown);
    pthread_mutex_unlock(&g_mu);
    Capture* const saved = CurrentCapture();  // the Actor handler only passes through for our own extra Enters (M1)
    SetCapture(nullptr);
    if (d.component == kGun) {
      for (const std::uint64_t sym : {kSatellite, kPort}) {
        if ((static_cast<std::uint32_t>(CallComponent(component, system, cap.actor, sym)) & 0xffff) == 0xffff) {  // M3
          char s[19];
          F357Line("combat_dronekill_gun", "the drone has no such component, so it is not mirrored", {{"step", "missing"}, {"component", sentinel::HexString(s, sym)}});
          continue;
        }
        NodeData copy = d;  // E1: Enter reads data +0..+0x20 only and keeps no pointer to it
        copy.component = sym;
        real(node, &copy);
      }
      F357Line("combat_dronekill_gun", "gun mirrored: the satellite and the port follow the gun",
               {{"step", "mirrored"}, {"gun", d.enable ? "shown" : "hidden"}, {"tid", tid}});
    } else if (known && shown != d.enable) {  // M2: the port follows the gun, by effect
      NodeData copy = d;
      copy.enable = shown;
      real(node, &copy);
      F357Line("combat_dronekill_gun", "the port follows the gun, not the script",
               {{"step", "port_follows"}, {"script", d.enable ? "shown" : "hidden"}, {"gun", shown ? "shown" : "hidden"}, {"tid", tid}});
    }
    SetCapture(saved);
  }
  Joined j{0, 0, false, false};
  if (d.enable) {
    const std::uint64_t page_ctx = d.system == kDamageable ? PageWord(ec::core::kDroneCtx) : 0;  // the release's node context (explosion)
    FxHeld open[kGroups];  // the holds open before the call: a release may reuse their group's slot, a new game space clears the table
    int nopen = 0;
    FxHeld direct[kGroups];  // holds taken away that the off list has no room for: turned off from here, after the lock
    int ndirect = 0;
    pthread_mutex_lock(&g_mu);  // DK-5: table operations only
    for (const Group& g : g_table.t.g) {
      if (g.used && g.x.holdOpen) open[nopen++] = FxHeld{g.x.held, g.x.number, g.handle};
    }
    j = g_table.On(reinterpret_cast<std::uint64_t>(system), d.system, d.component, handle, NowMs(), cap.actor,
                   reinterpret_cast<std::uint64_t>(cap.space), owner);
    if (d.system == kDamageable) {
      if (Group* g = g_table.ByNumber(j.group)) g->x.ctx = page_ctx;
    }
    for (int i = 0; i < nopen; ++i) {  // a hold the call took away goes off at the next tick, or from here when the list is full
      const Group* g = g_table.ByNumber(open[i].group);
      if (g != nullptr && g->x.holdOpen) continue;
      if (g_offs.Add(open[i].c, open[i].group, open[i].index, NowMs())) g_any_due.store(true, std::memory_order_release);
      else direct[ndirect++] = open[i];
    }
    if (j.again && g_kill) g_any_due.store(true, std::memory_order_release);  // R3: off again at the next tick
    pthread_mutex_unlock(&g_mu);
    for (int i = 0; i < ndirect; ++i) HeldOffNow(direct[i].c, direct[i].index, direct[i].group, "off_list_full");
  }
  if (!d.enable && d.system == kDamageable) {  // D11 (#2359): the drone's damageable went off, which is the drone's end: its group is dead
    FxOutcome fx;
    pthread_mutex_lock(&g_mu);
    const int dead = g_table.Off(reinterpret_cast<std::uint64_t>(system), handle);
    if (g_explosion) fx = ExpiryLocked(reinterpret_cast<std::uint64_t>(system), handle, dead != 0, NowMs());
    pthread_mutex_unlock(&g_mu);
    FxOutcomeLines(fx);
    if (dead != 0) {
      g_expired.fetch_add(1, std::memory_order_relaxed);
      if (LineBudget(&g_expired_lines, kExpiredLines, "dronekill", "expired",
                     "no further expired lines are written; the stats line still counts every drone end")) {
        sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_expired",
                            {{"index", handle & 0xffff}, {"group", dead}, {"tid", tid},
                             {"msg", "a drone's damageable went off (its end): its group is dead, and a component of it that goes on again is held off"}});
      }
    }
  }
  char sys[19], comp[19], fl[19];
  if (j.again) {
    if (Allow(g_off_lines, kOffLineCap, "off")) {
      sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_reenabled",
                          {{"system", sentinel::HexString(sys, d.system)}, {"component", sentinel::HexString(comp, d.component)},
                           {"index", handle & 0xffff}, {"group", j.group}, {"action", g_kill ? "off_at_next_tick" : "none_no_kill_word"},
                           {"tid", tid}, {"msg", "a component of a dead drone group was enabled again"}});
    }
    return;
  }
  if (!Allow(g_lines, kLineCap, "enter")) return;
  if (d.enable && d.system == kDamageable) {
    sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_release",
                        {{"index", handle & 0xffff}, {"group", j.group}, {"pending_joined", j.pendingJoined},
                         {"flag368", sentinel::HexString(fl, flag)}, {"tid", tid}, {"msg", "a drone was released (its damageable went on): a new group"}});
    return;
  }
  char where[32];
  if (!d.enable) std::snprintf(where, sizeof where, "(off)");
  else if (j.pending) std::snprintf(where, sizeof where, "pending");
  else std::snprintf(where, sizeof where, "group %d", j.group);
  sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_component",
                      {{"system", sentinel::HexString(sys, d.system)}, {"component", sentinel::HexString(comp, d.component)},
                       {"index", handle & 0xffff}, {"enable", d.enable ? "on" : "off"}, {"flag368", sentinel::HexString(fl, flag)}, {"tid", tid},
                       {"to", where}});
}

// CEnableComponentNode::Enter's handler. Outside a combat map: the game's call, nothing else (a load, a test and a tail call: the work is
// EnterInScope's, so this path builds no frame).
void EnterHandler(EnterThunk::Fn real, void* node, const NodeData* data) noexcept {
  if (!g_inScope.load(std::memory_order_relaxed)) return real(node, data);
  EnterInScope(real, node, data);
}

// Stage 1, D2 / D4: the recorded component is still that one. Null with *why when not.
void* Revalidate(const Comp& c, std::uintptr_t* owner_out, const char** why) noexcept {
  std::uintptr_t owner = 0;
  if (c.x.space == 0 || c.x.owner == 0) {
    *why = "no game space recorded";
    return nullptr;
  }
  if (!SafeRead(static_cast<std::uintptr_t>(c.system) + kSysOwner, &owner, sizeof owner) || owner != c.x.owner) {
    *why = "system's game space changed";
    return nullptr;
  }
  void* const system = g_fns.find != nullptr ? g_fns.find(reinterpret_cast<const void*>(c.x.space), c.x.sysSym) : nullptr;
  if (system != reinterpret_cast<void*>(c.system)) {
    *why = "system not found again in its game space";
    return nullptr;
  }
  std::uintptr_t component = 0;
  if (!g_cmp_ok || !ComponentShape(system, &component)) {
    *why = "Component() not the read shape";
    return nullptr;
  }
  if (static_cast<std::uint32_t>(CallComponent(component, system, c.actor, c.x.compSym)) != c.handle) {
    *why = "handle changed";
    return nullptr;
  }
  *owner_out = owner;
  return system;
}

// One component off, as Enter itself would turn it off. 0 = done, 1 = skipped for good (why), 2 = wait (the invoke-immediate word is 0).
int OffOne(const Comp& c, const char** why) noexcept {
  std::uintptr_t owner = 0;
  std::uint32_t flag = 0;
  void* const system = Revalidate(c, &owner, why);
  if (system == nullptr) return 1;
  std::uintptr_t disable = 0;
  if (!DisableShape(system, &disable)) {
    *why = "DisableComponent not the read shape";
    return 1;
  }
  if (!SafeRead(owner + kOwnerFlag, &flag, sizeof flag)) {
    *why = "invoke-immediate word unreadable";
    return 1;
  }
  if (flag == 0) return 2;  // Enter would queue here: not done by us (the queue push races its drain), retried
  CallDisable(disable, system, c.handle, kDisCategory);
  return 0;
}

// F3 L2: the touch interact the script turned off, on again (EnableComponent, as Enter itself would): 0 = done, 1 = skipped for good (why),
// 2 = wait (the invoke-immediate word is 0, or the script's disable is not applied yet)
int OnOne(const Comp& c, const char** why) noexcept {
  std::uintptr_t owner = 0;
  std::uint32_t flag = 0;
  void* const system = Revalidate(c, &owner, why);
  if (system == nullptr) return 1;
  std::uintptr_t enable = 0;
  MaskAt at{};
  if (!EnableShape(system, &enable, &at)) {
    *why = "EnableComponent not the read shape";
    return 1;
  }
  if (!SafeRead(owner + kOwnerFlag, &flag, sizeof flag)) {
    *why = "invoke-immediate word unreadable";
    return 1;
  }
  if (flag == 0) return 2;
  const int bit = DisableBit(reinterpret_cast<std::uintptr_t>(system), c.handle, at);
  if (bit < 0) {
    *why = "disable mask unreadable";
    return 1;
  }
  if (bit == 0) return 2;  // the script's disable is still queued (drained at its game space's EndUpdate)
  CallEnable(enable, system, c.handle, kDisCategory);
  return 0;
}

// F3 L3 (EchoCombat LockedOn, df 805-827): a table operation only, on the shot's thread
void LockedOn(std::uint32_t at_handle) noexcept {
  pthread_mutex_lock(&g_mu);
  const int slot = g_touch.LockedOn(at_handle, NowMs());
  if (slot >= 0) g_any_due.store(true, std::memory_order_release);
  pthread_mutex_unlock(&g_mu);
}

// FireBulletLocal(cs, handle, C3Vector velocity by value, remote) (E4: AttackTarget remote 0, FireBulletNetwork remote 1). Outside a combat map:
// the game's call, nothing else (no shot frame, so dronemuzzle passes too).
__attribute__((noinline)) void FireInScope(FireThunk::Fn real, void* cs, std::uint32_t handle, DkC3 velocity, std::uint32_t remote) noexcept {
  if (!scope::LookIsFresh()) return real(cs, handle, velocity, remote);  // the tick stopped: the answer is not believed (combat_scope.h)
  g_fires.fetch_add(1, std::memory_order_relaxed);
  if (g_lockon) LockedOn(handle);
  const std::uintptr_t outer = g_t_shot.Get();  // a shot inside a shot keeps the outer one's after it returns
  ShotFrame frame{velocity, remote};
  g_t_shot.Set(reinterpret_cast<std::uintptr_t>(&frame));
  real(cs, handle, velocity, remote);
  g_t_shot.Set(outer);
}
void FireHandler(FireThunk::Fn real, void* cs, std::uint32_t handle, DkC3 velocity, std::uint32_t remote) noexcept {
  if (!g_inScope.load(std::memory_order_relaxed)) return real(cs, handle, velocity, remote);
  FireInScope(real, cs, handle, velocity, remote);
}

// F7 C3: true = the drone's R14NetPhysics is locally controlled, or the drone has no R14NetPhysics component (not replicated); false = another
// peer controls it, or anything is unreadable / unknown (fails closed: C2 writes nothing).
bool LocallyControlled(std::uintptr_t physics, std::uint32_t phys_h, const void* space) noexcept {
  if (g_fns.local == nullptr || space == nullptr || g_fns.find == nullptr) return false;
  std::uintptr_t idx = 0, owners = 0;
  std::uint16_t slot = 0;
  std::uint64_t actor = 0;
  if (!SafeRead(physics + kPhIdx, &idx, sizeof idx) || !idx || !SafeRead(idx + (phys_h & 0xffff) * 4ull, &slot, sizeof slot)) return false;
  if (!SafeRead(physics + kPhOwners, &owners, sizeof owners) || !owners || !SafeRead(owners + slot * 16ull, &actor, sizeof actor) || !actor) return false;
  void* const np = g_fns.find(space, kNetPhysics);
  std::uintptr_t np_phys = 0;
  if (np == nullptr || !SafeRead(reinterpret_cast<std::uintptr_t>(np) + kNetPhysicsPhys, &np_phys, sizeof np_phys) || np_phys != physics) return false;
  std::uintptr_t component = 0;
  if (!g_cmp_ok || !ComponentShape(np, &component)) return false;
  const std::uint32_t h = static_cast<std::uint32_t>(CallComponent(component, np, actor, kNetPhysics));
  if ((h & 0xffff) == 0xffff) return true;
  return g_fns.local(np, h) != 0;
}

// F7 C1 (EchoCombat ChaseTarget, df 1059-1095): after the real AttackTarget, on its thread, the velocity toward the target from the game's own
// loads-only reads; applied at the tick (C2)
void ChaseAfter(std::uintptr_t cs, std::uint32_t handle) noexcept {
  if (g_fns.l2w == nullptr || g_fns.primary == nullptr || g_fns.resolve == nullptr) return;
  std::uintptr_t idx = 0, recs = 0, transforms = 0, physics = 0;
  std::uint16_t slot = 0;
  if (!SafeRead(cs + kAtIdx, &idx, sizeof idx) || !idx || !SafeRead(idx + (handle & 0xffff) * 4ull, &slot, sizeof slot)) return;
  if (!SafeRead(cs + kAtRecs, &recs, sizeof recs) || !recs) return;
  const std::uintptr_t rec = recs + slot * kAtStride;
  std::uint8_t has = 0;
  std::uint32_t xf = 0, phys_h = 0;
  if (!SafeRead(rec + kAtHasTarget, &has, 1) || !SafeRead(rec + kAtTransform, &xf, 4) || !SafeRead(rec + kAtPhysIdx, &phys_h, 4)) return;
  if (!SafeRead(cs + kAtTransforms, &transforms, sizeof transforms) || !transforms) return;
  if (!SafeRead(cs + kAtPhysics, &physics, sizeof physics) || !physics) return;
  std::uintptr_t owner = 0, at_owner = 0;
  if (!SafeRead(physics + kSysOwner, &owner, sizeof owner) || !owner) return;
  if (!SafeRead(cs + kSysOwner, &at_owner, sizeof at_owner) || !at_owner) return;
  float v[3] = {}, dist = 0;
  bool moving = false;
  if (has) {
    const DkTransfQ me = g_fns.l2w(reinterpret_cast<void*>(transforms), xf);
    std::uintptr_t ts = 0, d_idx = 0, d_recs = 0, t_phys = 0, t_xf = 0;
    std::uint16_t t_h = 0, d_slot = 0;
    if (!SafeRead(rec + kAtTargetSys, &ts, sizeof ts) || !ts || !SafeRead(rec + kAtTargetIdx, &t_h, sizeof t_h)) return;
    if (!SafeRead(ts + kDmIdx, &d_idx, sizeof d_idx) || !d_idx || !SafeRead(d_idx + t_h * 4ull, &d_slot, sizeof d_slot)) return;
    if (!SafeRead(ts + kDmRecs, &d_recs, sizeof d_recs) || !d_recs) return;
    const std::uintptr_t drec = d_recs + d_slot * kDmStride;
    std::uint32_t body_h = 0, xf_h = 0;
    if (!SafeRead(drec + kDmPhysH, &body_h, 4) || !SafeRead(drec + kDmXfH, &xf_h, 4)) return;
    float q[3];
    void* body = nullptr;
    if (SafeRead(ts + kDmPhys, &t_phys, sizeof t_phys) && t_phys) {
      const DkPhTarget t = g_fns.primary(reinterpret_cast<void*>(t_phys), body_h);
      body = g_fns.resolve(&t);
    }
    if (body == nullptr || !SafeRead(reinterpret_cast<std::uintptr_t>(body) + kBodyPos, q, sizeof q)) {  // AttackTarget's fallback
      if (!SafeRead(ts + kDmXf, &t_xf, sizeof t_xf) || !t_xf) return;
      const DkTransfQ them = g_fns.l2w(reinterpret_cast<void*>(t_xf), xf_h);
      std::memcpy(q, them.p, sizeof q);
    }
    if (!ChaseVelocity(me.p, q, v, &dist)) return;  // on the target: EchoCombat writes nothing
    moving = true;
  }
  void* space = nullptr;
  pthread_mutex_lock(&g_mu);
  space = reinterpret_cast<void*>(g_table.SpaceOf(owner));  // DFC-1: a space seen with the live [physics+0x80], else none (= not local)
  Chase* c = moving ? g_chases.Take(cs, handle) : g_chases.Find(cs, handle);
  const bool was_moving = c != nullptr && c->moving;
  pthread_mutex_unlock(&g_mu);
  if (c == nullptr || (!moving && !was_moving)) return;  // no slot; or no target and resting
  const bool local = LocallyControlled(physics, phys_h, space);
  pthread_mutex_lock(&g_mu);
  if (c->cs == cs && c->handle == handle) {  // still ours (another drone may have taken a resting slot)
    c->physics = physics;
    c->owner = owner;
    c->atOwner = at_owner;
    c->phys = phys_h;
    std::memcpy(c->v, v, sizeof v);
    c->ms = NowMs();
    c->moving = moving;
    c->due = local;
    if (c->due) g_any_due.store(true, std::memory_order_release);
  }
  pthread_mutex_unlock(&g_mu);
  if (moving && Allow(g_chase_lines, 8, "chase")) {  // EchoCombat logs the first 5
    char d[32], s[32];
    sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_chase",
                        {{"drone", handle & 0xffff}, {"dist_m", Fmt(d, dist)}, {"speed_ms", Fmt(s, std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]))},
                         {"locally_controlled", local ? 1 : 0}, {"msg", "a drone chases its target; the velocity is set at the next tick when this peer controls it"}});
  }
}

// AttackTarget's handler: the game's call first; outside a combat map nothing after it.
__attribute__((noinline)) void AttackInScope(void* cs, std::uint32_t handle) noexcept {
  if (!scope::LookIsFresh()) return;  // the tick stopped: the answer is not believed (combat_scope.h); the game's call is made
  g_attacks.fetch_add(1, std::memory_order_relaxed);
  if (g_chase) ChaseAfter(reinterpret_cast<std::uintptr_t>(cs), handle);
}
void AttackHandler(AttackThunk::Fn real, void* cs, std::uint32_t handle) noexcept {
  real(cs, handle);
  if (!g_inScope.load(std::memory_order_relaxed)) return;
  AttackInScope(cs, handle);
}

NEVR_HOOK_RECORD(kActorHook, ActorThunk, &ActorHandler);
NEVR_HOOK_RECORD(kEnterHook, EnterThunk, &EnterHandler);
NEVR_HOOK_RECORD(kFireHook, FireThunk, &FireHandler);
NEVR_HOOK_RECORD(kAttackHook, AttackThunk, &AttackHandler);

// F3 L2 / L4 at the tick: RE-ON records on again, OFF-DUE ones off (only while the component is part of a live group: TouchLive). Engine calls
// with g_mu released (D5).
void TouchTick(std::uint64_t now, std::uint32_t tid) noexcept {
  int slots[kTouches];
  TouchRec recs[kTouches];
  pthread_mutex_lock(&g_mu);
  const int n = g_touch.TakeDue(now, slots, kTouches);
  for (int i = 0; i < n; ++i) recs[i] = g_touch.t[slots[i]];
  pthread_mutex_unlock(&g_mu);
  for (int i = 0; i < n; ++i) {
    const TouchRec& r = recs[i];
    const char* why = "";
    if (r.state == kTouchReOn) {
      const int k = OnOne(r.c, &why);
      if (k == 2) continue;
      pthread_mutex_lock(&g_mu);
      if (g_touch.t[slots[i]].state == kTouchReOn && g_touch.t[slots[i]].c.handle == r.c.handle) g_touch.ReOnDone(slots[i], k == 0);
      pthread_mutex_unlock(&g_mu);
      F357Line("combat_dronekill_touch", "touch on again: the drone is grabbable until it locks on",
               {{"step", "on_again"}, {"index", r.c.handle & 0xffff}, {"result", k == 0 ? "on" : why}, {"tid", tid}});
      continue;
    }
    pthread_mutex_lock(&g_mu);
    const bool live = g_table.LiveComp(r.c.system, r.c.handle) || g_table.LiveActor(r.c.actor);
    pthread_mutex_unlock(&g_mu);
    const int k = live ? OffOne(r.c, &why) : 1;
    if (!live) why = "belongs to no live drone, left alone";
    if (k == 2) continue;
    pthread_mutex_lock(&g_mu);
    if (g_touch.t[slots[i]].state == kTouchOffDue && g_touch.t[slots[i]].c.handle == r.c.handle) g_touch.OffDone(slots[i]);
    pthread_mutex_unlock(&g_mu);
    char a[19];
    F357Line("combat_dronekill_touch", "touch locked on: the drone is no longer grabbable",
             {{"step", "locked_on"}, {"index", r.c.handle & 0xffff}, {"at", r.atHandle & 0xffff}, {"result", k == 0 ? "off" : why},
              {"actor", sentinel::HexString(a, r.c.actor)}, {"tid", tid}});
  }
}

// AUD-7 DF-1: the recorded drone is still that one at the tick (up to kChaseMs after AttackTarget). Plain loads only.
bool HandleLive(std::uintptr_t system, std::uint64_t idx_off, std::uint32_t handle, std::uint16_t* slot_out) noexcept {
  std::uintptr_t idx = 0;
  std::uint16_t e[2] = {};
  if (!SafeRead(system + idx_off, &idx, sizeof idx) || !idx || !SafeRead(idx + (handle & 0xffff) * 4ull, e, sizeof e)) return false;
  if (e[1] != static_cast<std::uint16_t>(handle >> 16)) return false;
  *slot_out = e[0];
  return true;
}

bool ChaseStillOk(const Chase& c) noexcept {
  std::uintptr_t at_owner = 0, recs = 0, physics = 0;
  std::uint32_t phys_h = 0;
  std::uint16_t slot = 0, pslot = 0;
  if (!SafeRead(c.cs + kSysOwner, &at_owner, sizeof at_owner) || at_owner != c.atOwner) return false;
  if (!HandleLive(c.cs, kAtIdx, c.handle, &slot)) return false;
  if (!SafeRead(c.cs + kAtRecs, &recs, sizeof recs) || !recs) return false;
  if (!SafeRead(recs + slot * kAtStride + kAtPhysIdx, &phys_h, sizeof phys_h) || phys_h != c.phys) return false;
  if (!SafeRead(c.cs + kAtPhysics, &physics, sizeof physics) || physics != c.physics) return false;
  return HandleLive(c.physics, kPhIdx, c.phys, &pslot);
}

// F7 C2 at the tick: each due velocity, younger than kChaseMs, set directly while its physics system's game space is in the invoke-immediate
// window (the engine's own rule for SetWorldVelocity, E6)
void ChaseTick(std::uint64_t now) noexcept {
  Chase due[kChases];
  int n = 0;
  pthread_mutex_lock(&g_mu);
  for (auto& c : g_chases.c) {
    if (!c.due) continue;
    c.due = false;  // one write per AttackTarget (it runs every frame while the drone has a target)
    if (now - c.ms < kChaseMs) due[n++] = c;
    else c.moving = false;
  }
  pthread_mutex_unlock(&g_mu);
  for (int i = 0; i < n; ++i) {
    std::uintptr_t owner = 0;
    std::uint32_t flag = 0;
    if (!ChaseStillOk(due[i])) continue;  // DF-1
    if (!SafeRead(due[i].physics + kSysOwner, &owner, sizeof owner) || owner != due[i].owner) continue;
    if (!SafeRead(owner + kOwnerFlag, &flag, sizeof flag) || flag == 0) continue;  // outside the window: the next AttackTarget makes a new one
    if (g_fns.set_vel == nullptr) continue;
    g_fns.set_vel(reinterpret_cast<void*>(due[i].physics), due[i].phys, DkC3{due[i].v[0], due[i].v[1], due[i].v[2]});
  }
}

// The explosion's due work at the tick (#1737): holds that ran out go to the off list, the queue's explosions run and its held components go
// to the off list, then each held component on the list goes off (OffOne; a deferring game space is retried up to kDueMs).
void FxTick(std::uint64_t now) noexcept {
  FxCtx spawn[kFxSlots];
  FxHeld off[kFxSlots * kFxHeld];
  Comp let[kGroups];
  int letN[kGroups];
  std::uint32_t letI[kGroups];
  OffRetry work[kOffRetries + kFxSlots * kFxHeld + kGroups];
  int nwork = 0, noff = 0;
  pthread_mutex_lock(&g_mu);
  const int nlet = g_table.LetGo(now, let, letN, letI, kGroups);
  const int nspawn = g_fx.Due(now, spawn, kFxSlots, off, kFxSlots * kFxHeld, &noff);
  for (auto& r : g_offs.r) {
    if (r.used) work[nwork++] = r;
  }
  g_offs.Clear();
  pthread_mutex_unlock(&g_mu);
  for (int i = 0; i < nlet; ++i) {
    HoldLine(LogLevel::kDebug, "let_go", letI[i], letN[i], "no_damageable_off");
    work[nwork++] = OffRetry{let[i], letN[i], letI[i], now, true};
  }
  for (int i = 0; i < noff; ++i) work[nwork++] = OffRetry{off[i].c, off[i].group, off[i].index, now, true};
  for (int i = 0; i < nspawn; ++i) RunExplosion(spawn[i]);
  OffRetry keep[kOffRetries + kFxSlots * kFxHeld + kGroups];
  int nkeep = 0;
  for (int i = 0; i < nwork; ++i) {
    const char* why = "";
    const int k = OffOne(work[i].c, &why);
    if (k == 2 && now - work[i].since < kDueMs) {
      keep[nkeep++] = work[i];
      continue;
    }
    if (k == 0) HoldLine(LogLevel::kDebug, "released", work[i].index, work[i].group, "off");
    else HoldLine(LogLevel::kWarn, "stays_on", work[i].index, work[i].group, k == 2 ? "invoke_immediate_0" : why);
  }
  OffRetry lost[kOffRetries + kFxSlots * kFxHeld + kGroups];
  int nlost = 0;
  pthread_mutex_lock(&g_mu);
  for (int i = 0; i < nkeep; ++i) {
    if (!g_offs.Add(keep[i].c, keep[i].group, keep[i].index, keep[i].since)) lost[nlost++] = keep[i];
  }
  pthread_mutex_unlock(&g_mu);
  for (int i = 0; i < nlost; ++i) HoldLine(LogLevel::kWarn, "stays_on", lost[i].index, lost[i].group, "off_list_full");
}

// The due work (the reference build ran it at the end of each recvfrom on the game's own socket); here once per frame on the game thread.
// Nothing but one atomic load while nothing is due.
__attribute__((noinline)) void TickBody() noexcept {
  if (!g_any_due.load(std::memory_order_acquire) || g_t_in_tick.Get() != 0) return;
  g_t_in_tick.Set(1);
  g_due_runs.fetch_add(1, std::memory_order_relaxed);
  Due due[kGroupComps * 2];
  const std::uint64_t now = NowMs();
  pthread_mutex_lock(&g_mu);
  const std::uint64_t expired_before = g_table.expired;
  const int n = g_table.TakeDue(now, due, static_cast<int>(sizeof due / sizeof due[0]));
  const std::uint64_t expired = g_table.expired - expired_before;
  pthread_mutex_unlock(&g_mu);
  const std::uint32_t tid = Tid();
  int off = 0, skipped = 0, waiting = 0;
  for (int i = 0; i < n; ++i) {
    const char* why = "";
    const int r = OffOne(due[i].c, &why);  // engine calls with g_mu released (D5)
    if (r == 2) {
      ++waiting;
      continue;
    }
    pthread_mutex_lock(&g_mu);
    g_table.Done(due[i].number, due[i].idx);
    pthread_mutex_unlock(&g_mu);
    if (r == 0) ++off;
    else ++skipped;
    if (Allow(g_off_lines, kOffLineCap, "off")) {
      char sys[19], comp[19];
      sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_off",
                          {{"group", due[i].number}, {"system", sentinel::HexString(sys, due[i].c.x.sysSym)},
                           {"component", sentinel::HexString(comp, due[i].c.x.compSym)}, {"index", due[i].c.handle & 0xffff},
                           {"result", r == 0 ? "OFF" : why}, {"tid", tid}});
    }
  }
  if (g_lockon) TouchTick(now, tid);
  if (g_chase) ChaseTick(now);
  if (g_explosion) FxTick(now);
  pthread_mutex_lock(&g_mu);
  if (!g_table.AnyDue() && !g_touch.AnyDue() && !g_chases.AnyDue() && !g_fx.Any() && !g_offs.Any() && !g_table.AnyHold())
    g_any_due.store(false, std::memory_order_release);
  pthread_mutex_unlock(&g_mu);
  if ((expired != 0 || (waiting != 0 && off == 0 && skipped == 0)) && Allow(g_off_lines, kOffLineCap, "off")) {
    sentinel::LogFields(LogLevel::kWarn, "combat_dronekill_tick",
                        {{"tid", tid}, {"off", off}, {"skipped", skipped}, {"waiting_invoke_immediate_0", waiting}, {"expired", expired},
                         {"due_after_ms", kDueMs},
                         {"msg", "components that were due to go off did not: expired ones are dropped, waiting ones are tried at the next tick"}});
  }
  g_t_in_tick.Set(0);
}

// The thunks' call counts, in hook order.
void CallCounts(std::uint64_t (&out)[kHookCount]) noexcept {
  out[0] = ActorThunk::Calls();
  out[1] = EnterThunk::Calls();
  out[2] = FireThunk::Calls();
  out[3] = AttackThunk::Calls();
}

// The no-combat-map answers in a row while the scope is on (the tick thread only).
std::uint32_t g_leave_asks = 0;

// The scope, asked of the combat level answer (combat_scope.h: a combat map and no Echo Arena level, never a lobby: the drones are a combat map's). One
// record (LastLook) decides and names the level on the change line, so the two cannot be of two looks. A change is one line, with each hook's calls over
// the scope that ended; leaving the scope empties the tables, so nothing is due outside a combat map.
// Leaving waits for kLeaveAsks no-combat-map answers in a row. A look cannot tell a game it could not read from a game with no combat map (an unreadable
// table, row or game, 0 included, is no level: combat_scope.h "Fails closed"), so a single answer is no proof the map ended; and a drop with the map still
// loaded leaves the held components on for good. The answer's freshness rule does not replace this: it bounds a combat answer the tick stopped
// refreshing, not a look that missed the map (a stale answer is no combat map, counted like any other). Entering is immediate.
__attribute__((noinline)) void RefreshScope() noexcept {
  const scope::Look look = scope::LastLook();
  const bool in = scope::InCombatMap(look);
  const std::uint64_t level_id = look.level;
  const bool was = g_inScope.load(std::memory_order_relaxed);
  if (in || !was) g_leave_asks = 0;
  if (in == was) return;
  if (!in) {
    if (++g_leave_asks < kLeaveAsks) {
      sentinel::LogFields(LogLevel::kDebug, "combat_dronekill_scope_wait",
                          {{"no_level_asks", g_leave_asks}, {"needed", kLeaveAsks},
                           {"msg", "no combat map was read: dronekill stays in its scope until the answer has been the same for the asks it needs"}});
      return;
    }
    g_leave_asks = 0;
    pthread_mutex_lock(&g_mu);
    int dropped = 0;  // held components the level takes with it: open holds, queued explosions' and those waiting to go off
    for (const Group& g : g_table.t.g) dropped += g.used && g.x.holdOpen ? 1 : 0;
    for (const auto& x : g_fx.s) dropped += x.used ? x.nheld : 0;
    for (const OffRetry& r : g_offs.r) dropped += r.used ? 1 : 0;
    if (dropped != 0) {
      sentinel::LogFields(LogLevel::kWarn, "combat_dronekill_hold",
                          {{"step", "dropped"}, {"count", dropped}, {"no_level_asks", kLeaveAsks},
                           {"msg", "no combat map was read for several asks in a row: dronekill forgets its held components, and nothing turns them off, "
                                   "so they stay on (that the game space is gone is not established)"}});
    }
    g_table = Table();
    g_touch.Clear();
    g_guns.Clear();
    g_chases.Clear();
    g_aux_space = nullptr;
    g_fx.Clear();    // the scope is over: its explosions and holds with it
    g_offs.Clear();
    g_any_due.store(false, std::memory_order_release);
    pthread_mutex_unlock(&g_mu);
  }
  g_inScope.store(in, std::memory_order_relaxed);
  std::uint64_t calls[kHookCount];
  CallCounts(calls);
  const std::uint64_t now = NowMs();
  char lv[19];
  sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_scope",
                      {{"in_combat", in ? 1 : 0}, {"level", sentinel::HexString(lv, level_id)},
                       {"actor_calls", calls[0] - g_scopeSince[0]}, {"enter_calls", calls[1] - g_scopeSince[1]},
                       {"fire_calls", calls[2] - g_scopeSince[2]}, {"attack_calls", calls[3] - g_scopeSince[3]},
                       {"over_ms", g_scopeSinceMs != 0 ? now - g_scopeSinceMs : 0},
                       {"msg", in ? "a combat map is loaded: dronekill acts on the drones' nodes, shots and attacks"
                                  : "no combat map is loaded: every dronekill hook only calls the game's function"}});
  for (int i = 0; i < kHookCount; ++i) g_scopeSince[i] = calls[i];
  g_scopeSinceMs = now;
}

struct FnSpec {
  const char* name;
  std::uintptr_t vaddr;
};
constexpr FnSpec kFnSpecs[6] = {
    {"_ZNK10NRadEngine13CncaGameSpace21FindComponentSystemMTENS_9CSymbol64E", 0x1804a10},  // disassembly 0x1904a10, size 92 (the const one)
    {"_ZNK10NRadEngine12CTransformCS26ComputeLocalToWorldTransfQENS_18SCompactPoolHandleE", 0x16f6ef0},  // disassembly 0x17f6ef0, size 144
    {"_ZNK10NRadEngine10CPhysicsCS11PrimaryBodyENS_18SCompactPoolHandleE", 0x1677d34},                    // disassembly 0x1777d34, size 112
    {"_ZNK10NRadEngine9CPhTarget11ResolveBodyEv", 0x19a27ac},                                            // disassembly 0x1aa27ac, size 104
    {"_ZN10NRadEngine10CPhysicsCS16SetWorldVelocityENS_18SCompactPoolHandleENS_8C3VectorE", 0x167bec0},   // disassembly 0x177bec0, size 244
    {"_ZNK10NRadEngine8NRadGame16CR15NetPhysicsCS19IsLocallyControlledENS_18SCompactPoolHandleE", 0x2175630},  // disassembly 0x2275630, size 280
};

constexpr const char* kHookNames[kHookCount] = {"SComponentNodeUtils::Actor", "CEnableComponentNode::Enter",
                                                "CR15NetAutoTargetCS::FireBulletLocal", "CR15NetAutoTargetCS::AttackTarget"};

void LogHook(int i, sentinel::GotStatus status) noexcept {
  sentinel::LogFields(status == sentinel::GotStatus::kOk ? LogLevel::kInfo : LogLevel::kError, "combat_dronekill_hook",
                      {{"hook", kHookNames[i]}, {"status", status == sentinel::GotStatus::kOk ? "installed" : sentinel::GotStatusName(status)}});
}

bool CheckFnAddresses(std::uintptr_t base, const void* const p[6], Fns* out) noexcept {
  for (int i = 0; i < 6; ++i) {
    if (p[i] != nullptr && reinterpret_cast<std::uintptr_t>(p[i]) == base + kFnSpecs[i].vaddr) continue;
    char want[19], found[19];
    sentinel::LogFields(LogLevel::kError, "combat_dronekill_function",
                        {{"function", kFnSpecs[i].name}, {"want_vaddr", sentinel::HexString(want, kFnSpecs[i].vaddr)},
                         {"found", sentinel::HexString(found, reinterpret_cast<std::uintptr_t>(p[i]))},
                         {"msg", "a function dronekill calls is missing or not at its pinned address: not the pinned build, dronekill is not installed"}});
    return false;
  }
  Fns f;
  f.find = reinterpret_cast<void* (*)(const void*, std::uint64_t)>(const_cast<void*>(p[0]));
  f.l2w = reinterpret_cast<DkTransfQ (*)(const void*, std::uint32_t)>(const_cast<void*>(p[1]));
  f.primary = reinterpret_cast<DkPhTarget (*)(const void*, std::uint32_t)>(const_cast<void*>(p[2]));
  f.resolve = reinterpret_cast<void* (*)(const DkPhTarget*)>(const_cast<void*>(p[3]));
  f.set_vel = reinterpret_cast<void (*)(void*, std::uint32_t, DkC3)>(const_cast<void*>(p[4]));
  f.local = reinterpret_cast<std::uint32_t (*)(const void*, std::uint32_t)>(const_cast<void*>(p[5]));
  *out = f;
  return true;
}

Result Refuse(Result out, const char* token, const char* why) noexcept {
  out.ok = false;
  out.status = token;
  sentinel::LogFields(LogLevel::kError, "combat_dronekill_install",
                      {{"result", "refused"}, {"reason", token}, {"why", why},
                       {"consequence", "dronekill is not installed: the drones keep the game's own behaviour"}});
  return out;
}

}  // namespace

bool ResolveFnsFromAddressesForTest(std::uintptr_t base, const void* const addrs[6], Fns* out) noexcept { return CheckFnAddresses(base, addrs, out); }

bool ResolveFns(std::uintptr_t base, Fns* out) noexcept {
  void* h = dlopen(pinned::kLibR15, RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) {
    sentinel::LogFields(LogLevel::kError, "combat_dronekill_function",
                        {{"why", "libr15.so is not loaded"}, {"msg", "dronekill's functions are not resolved: dronekill is not installed"}});
    return false;
  }
  const void* p[6];
  for (int i = 0; i < 6; ++i) p[i] = dlsym(h, kFnSpecs[i].name);
  dlclose(h);
  return CheckFnAddresses(base, p, out);
}

Result InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Words& words, const Fns& fns) noexcept {
  Result out;
  if (g_installed.load(std::memory_order_acquire)) return Refuse(out, "already_ran", "a second step installs nothing");
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(pinned::kLibR15, &image)) return Refuse(out, "module_not_loaded", "libr15.so is not loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refuse(out, "no_build_id", "libr15.so has no build ID");
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refuse(out, "build_id_mismatch", "libr15.so is not the pinned build");
  const std::uintptr_t base = image.base;
  if (fns.find == nullptr) return Refuse(out, "no_find", "FindComponentSystemMT is not resolved");
  g_base = base;
  g_cmp_plt = pins.cmpPlt;
  g_fns = fns;
  g_kill = words.kill;
  g_lockon = words.lockon;
  g_gun = words.gun;
  g_chase = words.chase;
  g_shot = words.shot;
  g_explosion = words.explosion;
  g_hold = words.hold && words.explosion;
  if (g_chase && (fns.l2w == nullptr || fns.primary == nullptr || fns.resolve == nullptr || fns.set_vel == nullptr || fns.local == nullptr)) {
    sentinel::LogFields(LogLevel::kError, "combat_dronekill_chase_off",
                        {{"why", "a function chase calls is not resolved"}, {"consequence", "drones do not chase; the other rules install"}});
    g_chase = false;
  }
  bool rd_ok = true;
  const auto cmp = SafeRead<std::uintptr_t>(base + pins.cmpSlot, &rd_ok);
  g_cmp_ok = rd_ok && cmp == base + pins.cmpFn;
  if (!g_cmp_ok) {
    char have[19], want[19];
    sentinel::LogFields(LogLevel::kWarn, "combat_dronekill_comparator",
                        {{"slot_word", sentinel::HexString(have, cmp)}, {"want", sentinel::HexString(want, base + pins.cmpFn)},
                         {"consequence", "the comparator slot does not hold its function, so Component() look-ups are off"}});
  }

  const bool want_fire = g_lockon || g_shot, want_attack = g_chase;
  const std::uint64_t slot[kHookCount] = {pins.actorSlot, pins.enterSlot, pins.fireSlot, pins.attackSlot};
  const std::uint64_t fn[kHookCount] = {pins.actorFn, pins.enterFn, pins.fireFn, pins.attackFn};
  const char* const sym[kHookCount] = {kActorSymbol, kEnterSymbol, kFireSymbol, kAttackSymbol};
  sentinel::GotStatus* const status[kHookCount] = {&out.actor, &out.enter, &out.fire, &out.attack};
  const bool use[kHookCount] = {true, true, want_fire, want_attack};
  // every slot is checked before the first install (DK-6: all or none)
  for (int i = 0; i < kHookCount; ++i) {
    if (!use[i]) continue;
    bool ok = true;
    const auto word = SafeRead<std::uintptr_t>(base + slot[i], &ok);
    if (ok && word == base + fn[i]) continue;
    *status[i] = sentinel::GotStatus::kOriginalMismatch;
    LogHook(i, *status[i]);
    return Refuse(out, "slot_not_pinned_function", "a slot does not hold the pinned function");
  }
  // every check is done: the handlers' per-thread state is three pthread keys, made now so that a module refused above holds none
  int key_error = g_t_cap.Create();
  if (key_error == 0) key_error = g_t_shot.Create();
  if (key_error == 0) key_error = g_t_in_tick.Create();
  if (key_error != 0) {
    sentinel::LogFields(LogLevel::kError, "combat_dronekill_install",
                        {{"result", "refused"}, {"reason", "no_thread_key"}, {"error", key_error},
                         {"why", "no pthread key for the handlers' per-thread state"},
                         {"consequence", "dronekill is not installed: the drones keep the game's own behaviour"}});
    out.status = "no_thread_key";
    return out;
  }
  ActorThunk::Arm(kActorHook);
  EnterThunk::Arm(kEnterHook);
  FireThunk::Arm(kFireHook);
  AttackThunk::Arm(kAttackHook);
  // install order: Actor first (its handler alone only passes through; the capture is set by Enter's handler only), then Enter,
  // FireBulletLocal, AttackTarget
  for (int i = 0; i < kHookCount; ++i) {
    if (!use[i]) continue;
    const sentinel::GotTarget target = Target(sym[i], slot[i], base, fn[i]);
    sentinel::GotStatus got = sentinel::GotStatus::kBadArgument;
    switch (i) {
      case 0: got = sentinel::InstallThunk<ActorThunk>(g_hooks[0], target, lookup); break;
      case 1: got = sentinel::InstallThunk<EnterThunk>(g_hooks[1], target, lookup); break;
      case 2: got = sentinel::InstallThunk<FireThunk>(g_hooks[2], target, lookup); break;
      default: got = sentinel::InstallThunk<AttackThunk>(g_hooks[3], target, lookup); break;
    }
    *status[i] = got;
    LogHook(i, got);
    if (got == sentinel::GotStatus::kOk) continue;
    int not_removed = 0;
    for (int k = i - 1; k >= 0; --k) {
      if (!use[k]) continue;
      if (g_hooks[k].Remove() != sentinel::GotStatus::kOk) ++not_removed;
    }
    sentinel::LogFields(not_removed != 0 ? LogLevel::kError : LogLevel::kWarn, "combat_dronekill_rollback",
                        {{"failed_hook", kHookNames[i]}, {"hooks_not_removed", not_removed},
                         {"consequence", not_removed != 0 ? "an earlier hook could not be removed and stays: it only calls the game's function outside a combat map"
                                                          : "the earlier hooks were removed: dronekill is not installed"}});
    out.status = "hook_failed";
    return out;
  }
  g_installed.store(true, std::memory_order_release);
  g_shot_swapped.store(want_fire, std::memory_order_release);
  out.ok = true;
  out.status = "installed";
  out.comparatorOk = g_cmp_ok;
  out.needsTick = g_kill || g_lockon || g_chase || g_explosion;
  sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_install",
                      {{"result", "installed"}, {"reason", "ok"}, {"kill", g_kill ? 1 : 0}, {"lockon", g_lockon ? 1 : 0}, {"gun", g_gun ? 1 : 0},
                       {"chase", g_chase ? 1 : 0}, {"shot", g_shot ? 1 : 0}, {"explosion", g_explosion ? 1 : 0}, {"hold", g_hold ? 1 : 0}, {"fire_bullet_local", want_fire ? "hooked" : "not_hooked"},
                       {"attack_target", want_attack ? "hooked" : "not_hooked"}, {"component_lookups", g_cmp_ok ? "on" : "off"},
                       {"consequence", "the drone rules act while a combat map is loaded; in every other level each hook only calls the game's function"}});
  return out;
}

ModuleResult Install(bool on, const Words& words) noexcept {
  if (!on) {
    sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"consequence", "combat.dronekill is false: the drones keep the game's own behaviour"}});
    return {false, "off", true};  // off by its key: counted apart on the phase line, neither installed nor failed
  }
  if (g_installed.load(std::memory_order_acquire)) return {false, Refuse(Result{}, "already_ran", "a second step installs nothing").status};
  // The scope is the combat level answer (combat_scope.h): when it cannot change (the tick or the scope row's refresh is not installed) the module
  // would never act, so nothing is looked up or hooked.
  if (!scope::Live()) return {false, Refuse(Result{}, "scope_not_live", "the combat level answer cannot change: the tick or its refresh is not installed").status};
  const sentinel::ImageLookup override = g_lookup.load(std::memory_order_acquire);
  const sentinel::ImageLookup lookup = override != nullptr ? override : &sentinel::FindLoadedImage;
  const Pins* const pinsOverride = g_pinsOverride.load(std::memory_order_acquire);
  const Pins& pins = pinsOverride != nullptr ? *pinsOverride : kPins;
  Fns fns{};
  const Fns* const fnsOverride = g_fnsOverride.load(std::memory_order_acquire);
  if (fnsOverride != nullptr) {
    fns = *fnsOverride;
  } else {
    sentinel::ElfImage image;
    if (!lookup(pinned::kLibR15, &image)) return {false, Refuse(Result{}, "module_not_loaded", "libr15.so is not loaded").status};
    if (!ResolveFns(image.base, &fns)) return {false, Refuse(Result{}, "engine_export_missing", "a function dronekill calls is not at its pin").status};
  }
  // The tick asks the scope and runs the due work: without it the module could never act, so it is registered before any hook goes in.
  if (!tick::Add(&Tick)) return {false, Refuse(Result{}, "tick_table_full", "the combat tick has no room for the scope").status};
  const Result r = InstallAt(lookup, pins, words, fns);
  if (!r.ok) return {false, r.status};
  ec::OnDroneKilled(&OnKilled);
  sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_link",
                      {{"feed", "ec"}, {"msg", "the EC client's 'K' datagrams (a drone's health ran out on the server) reach dronekill's kill rule"}});
  return {true, r.status};
}

void Tick(std::uintptr_t /*game*/) noexcept {  // the game is the scope row's to read (combat_scope.h); this tick reads its answer
  if (!g_installed.load(std::memory_order_acquire)) return;
  const std::uint32_t n = g_ticks.fetch_add(1, std::memory_order_relaxed);
  if (n == 0) {
    sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_tick_first",
                        {{"msg", "dronekill's frame tick runs: the scope is asked every 32 frames and due work runs in a combat map"}});
  }
  if (n % kScopeFrames == 0) RefreshScope();
  if (!g_inScope.load(std::memory_order_relaxed)) return;
  TickBody();
}

bool InCombatScope() noexcept { return g_inScope.load(std::memory_order_relaxed); }

// The 'K' feed: the group is marked dead and due (kill) under g_mu, a table operation only. Out of scope it is dropped.
void OnKilled(std::uint32_t index) noexcept {
  if (!g_installed.load(std::memory_order_acquire) || !g_inScope.load(std::memory_order_relaxed) || !scope::LookIsFresh()) return;
  const std::uint32_t tid = Tid();
  int group = 0, comps = 0, live_n = 0;
  std::uint32_t live[kGroups];
  FxOutcome fx;
  pthread_mutex_lock(&g_mu);
  if (g_kill) {
    if (Group* g = g_table.Killed(index, NowMs(), true)) {
      group = g->x.number;
      comps = g->n;
      if (g->x.due) g_any_due.store(true, std::memory_order_release);
      if (g_gun) HideGunOnKill(g_guns);  // M4: EchoCombat Shutdown's SetGun(DRONE_ACTOR, 0): the drone symbol's state, hidden (D4)
      if (g_explosion) fx = KilledLocked(*g, NowMs());  // #1737: a shot-down drone explodes (a second 'K' finds no live group)
    }
  } else if (Group* g = g_table.Killed(index)) {
    group = g->x.number;
    comps = g->n;
  }
  live_n = g_table.Live(live, kGroups);
  pthread_mutex_unlock(&g_mu);
  FxOutcomeLines(fx);
  if (!Allow(g_k_lines, kKLineCap, "k")) return;
  sentinel::LogFields(LogLevel::kInfo, "combat_dronekill_k",
                      {{"index", index & 0xffff}, {"tid", tid}, {"group", group}, {"components", comps}, {"live_groups", live_n},
                       {"result", group == 0 ? "no_live_group" : g_kill ? "shut_down" : "would_shut_down"},
                       {"msg", group == 0 ? "the server says a drone died, and no live drone group has that index"
                               : g_kill ? "a drone group is shut down: its components go off at the tick"
                                        : "a drone group would be shut down, but the kill word is off"}});
}

const float* ShotVelocity(int* remote) noexcept {
  const auto* frame = reinterpret_cast<const ShotFrame*>(g_t_shot.Get());  // valid while the FireBulletLocal that set it runs
  if (frame == nullptr) return nullptr;
  if (remote != nullptr) *remote = static_cast<int>(frame->remote);
  return &frame->velocity.x;
}

bool ShotActive() noexcept { return g_shot_swapped.load(std::memory_order_acquire); }

Stats CurrentStats() noexcept {
  return {g_enter_drone.load(), g_enter_other.load(), g_fires.load(), g_attacks.load(), g_ticks.load(), g_due_runs.load(),
          static_cast<std::uint32_t>(g_capturing.load()), g_expired.load(), g_explosions.load(), g_held.load()};
}

void NoteEnd() noexcept {
  if (g_installed.load(std::memory_order_acquire)) LogStats(true);
}

void SetCallsForTest(const Calls* calls) noexcept { g_test_calls = calls; }
void SetClockForTest(std::uint64_t (*now_ms)()) noexcept { g_clock_ms = now_ms; }
void SetInstallForTest(sentinel::ImageLookup lookup, const Pins* pins, const Fns* fns) noexcept {
  g_lookup.store(lookup, std::memory_order_release);
  g_pinsOverride.store(pins, std::memory_order_release);
  g_fnsOverride.store(fns, std::memory_order_release);
}
void SetShotForTest(const float* v3, int remote) noexcept {
  g_t_shot.Create();  // a test that drives the accessor without installing has no key yet
  if (v3 == nullptr) {
    g_t_shot.Set(0);
    return;
  }
  g_test_shot = ShotFrame{DkC3{v3[0], v3[1], v3[2]}, static_cast<std::uint32_t>(remote)};
  g_t_shot.Set(reinterpret_cast<std::uintptr_t>(&g_test_shot));
}
void SetShotActiveForTest(bool active) noexcept { g_shot_swapped.store(active); }
void SetScopeForTest(bool inCombat) noexcept { g_inScope.store(inCombat); }
void SetEntryInfoForTest(bool (*info)(std::uintptr_t fn, EntryInfo* out)) noexcept { g_entryInfo.store(info, std::memory_order_release); }

void ResetForTest() noexcept {
  for (sentinel::GotHook& hook : g_hooks) hook.Forget();
  ActorThunk::Reset();
  EnterThunk::Reset();
  FireThunk::Reset();
  AttackThunk::Reset();
  g_base = g_cmp_plt = 0;
  g_fns = Fns{};
  g_cmp_ok = false;
  g_installed.store(false);
  g_kill = g_lockon = g_gun = g_chase = g_shot = g_explosion = g_hold = false;
  g_shot_swapped.store(false);
  g_inScope.store(false);
  g_capturing.store(0);
  pthread_mutex_lock(&g_mu);
  g_table = Table();
  g_touch = TouchTable();
  g_guns = GunTable();
  g_chases = ChaseTable();
  g_aux_space = nullptr;
  g_fx.Clear();
  g_offs.Clear();
  g_any_due.store(false);
  pthread_mutex_unlock(&g_mu);
  for (auto& c : g_vt) c.store(0);
  for (auto& c : g_vt_dis) c.store(0);
  for (auto& c : g_vt_en) c.store(0);
  SetCapture(nullptr);
  g_t_shot.Set(0);
  g_t_in_tick.Set(0);
  for (std::atomic<int>* n : {&g_lines, &g_notes, &g_off_lines, &g_k_lines, &g_f357_lines, &g_chase_lines}) n->store(0);
  for (std::atomic<std::uint32_t>* n : {&g_enter_drone, &g_enter_other, &g_fires, &g_attacks, &g_ticks, &g_due_runs, &g_expired}) n->store(0);
  g_expired_lines.store(0);
  g_explosions.store(0);
  g_held.store(0);
  for (std::atomic<unsigned>* n : {&g_fx_lines, &g_fx_skip_lines, &g_hold_lines, &g_stays_on_lines}) n->store(0);
  g_entryInfo.store(nullptr);
  for (std::uint64_t& s : g_scopeSince) s = 0;
  g_scopeSinceMs = 0;
  g_leave_asks = 0;
  g_clock_ms = nullptr;
  g_test_calls = nullptr;
  SetInstallForTest(nullptr, nullptr, nullptr);
}

}  // namespace nevr_quest::combat::dronekill

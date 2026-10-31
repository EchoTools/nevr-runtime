// Port of the source build's dronekill (rows R45 / R47 / R48 and the Actor slot). See dronekill.h and docs/quest/q15t/modules/dronekill.md.
// Data-page GOT swaps through InstallImportHook / SwapWord, reads through SafeRead, calls into libr15 only after its read shapes match.
#include "dronekill.h"

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

#include "dronekill_core.h"
#include "dronekill_log.h"
#include "got_swap.h"
#include "safe_read.h"

namespace q15t {

const DroneKillSlots kDroneKillPinned = {
    /*actor*/ 0x36e2080, 0x2059714,   // disassembly 0x2159714 "Actor" size 544
    /*enter*/ 0x36cacf8, 0x263b700,   // R45; disassembly 0x273b700 "Enter" size 424
    /*fire*/ 0x36c37a8, 0x210bff8,    // R48; disassembly 0x220bff8 "FireBulletLocal" size 1200
    /*attack*/ 0x36e6610, 0x210b8c4,  // R47; disassembly 0x220b8c4 "AttackTarget" size 1116
    /*cmp*/ 0x36f91f0, 0x1524cb4,     // comparator JUMP_SLOT -> disassembly 0x1624cb4 "operator()" size 488
    /*plt*/ 0xf43610};                // the comparator's PLT stub (disassembly 0x1043610: adrp x16 0x37f9000 / ldr x17,[x16,#0x1f0])

namespace {

using namespace dk;

constexpr char kUnexpectedSlot[] = "slot_word_not_the_pinned_function";

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
using FireFn = void (*)(void* cs, std::uint32_t handle, DkC3 velocity, std::uint32_t remote);
using AttackFn = void (*)(void* cs, std::uint32_t handle);
using EnterFn = void (*)(void* node, const NodeData* data);
using ActorFn = std::uint32_t (*)(const void* thread, const void* space, std::uint64_t actor_sym, void** space_out, void* actor_out,
                                  unsigned* flag_out);
using ComponentFn = std::uint64_t (*)(const void* system, std::uint64_t actor, std::uint64_t component_sym);
using DisableFn = void (*)(void* system, std::uint64_t handle, std::uint32_t category);
using EnableFn = void (*)(void* system, std::uint64_t handle, std::uint32_t category);

constexpr int kLineCap = 160, kNoteCap = 24, kOffLineCap = 64, kKLineCap = 48, kF357LineCap = 96;
constexpr int kVtCache = 16;

// ---- the real functions the swapped words held (set by InstallImportHook before each swap) ----
std::atomic<std::uintptr_t> g_actor_real{0}, g_enter_real{0}, g_fire_real{0}, g_attack_real{0};

// ---- set before the first swap, read by the wrappers ----
std::uintptr_t g_base = 0, g_cmp_plt = 0;
DroneKillFns g_fns{};
bool g_cmp_ok = false;
bool g_installed = false;
bool g_kill = false, g_lockon = false, g_gun = false, g_chase = false, g_shot = false;
std::atomic<bool> g_shot_swapped{false};

pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
Table g_table;           // under g_mu
TouchTable g_touch;      // under g_mu
GunTable g_guns;         // under g_mu
ChaseTable g_chases;     // under g_mu
void* g_aux_space = nullptr;  // under g_mu: the game space of the three tables above
std::atomic<bool> g_any_due{false};  // written under g_mu only, read without it

std::atomic<std::uintptr_t> g_vt[kVtCache] = {};      // vtables whose Component slots were checked: low bit 1 = the read shapes
std::atomic<std::uintptr_t> g_vt_dis[kVtCache] = {};  // the same for slot +0x210 DisableComponent
std::atomic<std::uintptr_t> g_vt_en[kVtCache] = {};   // the same for slot +0x208 EnableComponent

thread_local Capture* t_cap = nullptr;  // set only while a drone Enter runs on this thread
thread_local DkC3 t_shot;
thread_local std::uint32_t t_remote = 0;
thread_local bool t_in_shot = false;
thread_local bool t_in_tick = false;

std::atomic<int> g_lines{0}, g_notes{0}, g_off_lines{0}, g_k_lines{0}, g_f357_lines{0}, g_chase_lines{0};
std::atomic<std::uint32_t> g_enter_drone{0}, g_enter_other{0}, g_fires{0}, g_attacks{0}, g_ticks{0}, g_due_runs{0};

std::uint64_t (*g_clock_ms)() = nullptr;
const DroneKillCalls* g_test_calls = nullptr;

std::uint64_t NowMs() {
  if (g_clock_ms != nullptr) return g_clock_ms();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
}

std::uint32_t Tid() { return static_cast<std::uint32_t>(syscall(SYS_gettid)); }

// one capped line kind; the line that hits the cap says so once (a silent stop would look like the module went quiet)
bool Allow(std::atomic<int>& n, int cap, const char* what) {
  const int v = n.fetch_add(1, std::memory_order_relaxed);
  if (v < cap) return true;
  if (v == cap) LogKv().S("kind", what).U("cap", static_cast<unsigned long long>(cap)).Emit(NEVR_LOG_INFO, "dronekill: log cap reached, more are counted not written");
  return false;
}

// ---- calls into game code through vtable slots (the test replaces them) ----
std::uint64_t CallComponent(std::uintptr_t fn, const void* system, std::uint64_t actor, std::uint64_t sym) {
  if (g_test_calls != nullptr) return g_test_calls->component(fn, system, actor, sym);
  return reinterpret_cast<ComponentFn>(fn)(system, actor, sym);
}
void CallDisable(std::uintptr_t fn, void* system, std::uint64_t handle, std::uint32_t category) {
  if (g_test_calls != nullptr) return g_test_calls->disable(fn, system, handle, category);
  reinterpret_cast<DisableFn>(fn)(system, handle, category);
}
void CallEnable(std::uintptr_t fn, void* system, std::uint64_t handle, std::uint32_t category) {
  if (g_test_calls != nullptr) return g_test_calls->enable(fn, system, handle, category);
  reinterpret_cast<EnableFn>(fn)(system, handle, category);
}

// A vtable-keyed cache of "this vtable's slot holds the read shape": low bit 1 = yes. A full cache only means the words are read again.
int CachedShape(std::atomic<std::uintptr_t>* cache, std::uintptr_t vt, bool* hit) {
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
void CacheShape(std::atomic<std::uintptr_t>* cache, std::uintptr_t vt, bool ok) {
  for (int i = 0; i < kVtCache; ++i) {
    std::uintptr_t empty = 0;
    if (cache[i].compare_exchange_strong(empty, vt | (ok ? 1 : 0), std::memory_order_acq_rel)) break;
    if ((empty & ~static_cast<std::uintptr_t>(1)) == vt) break;
  }
}

bool ReadVtable(const void* system, std::uintptr_t* vt) {
  return SafeRead(reinterpret_cast<std::uintptr_t>(system), vt, sizeof *vt) && *vt != 0 && (*vt & 1) == 0;
}

// 1 = both slots hold the read shapes, 0 = not (or unreadable). Reads only. *fn = the Component() thunk's address.
int ComponentShape(const void* system, std::uintptr_t* fn) {
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
int DisableShape(const void* system, std::uintptr_t* fn) {
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
int EnableShape(const void* system, std::uintptr_t* fn, MaskAt* at) {
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
int DisableBit(std::uintptr_t system, std::uint32_t handle, const MaskAt& at) {
  std::uintptr_t idx = 0, masks = 0;
  std::uint16_t slot = 0, mask = 0;
  if (!SafeRead(system + at.idx, &idx, sizeof idx) || !idx) return -1;
  if (!SafeRead(idx + (handle & 0xffff) * 4ull, &slot, sizeof slot)) return -1;
  if (!SafeRead(system + at.maskArr, &masks, sizeof masks) || !masks) return -1;
  if (!SafeRead(masks + slot * 2ull, &mask, sizeof mask)) return -1;
  return mask & 1;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Actor's wrapper: the real call, untouched arguments and result; while a drone Enter runs on this thread, a copy of the two
// out-parameters (game space, SActor) the real call just wrote. The module never calls Actor itself.
std::uint32_t ActorWrap(const void* thread, const void* space, std::uint64_t actor_sym, void** space_out, void* actor_out,
                        unsigned* flag_out) {
  const auto real = reinterpret_cast<ActorFn>(g_actor_real.load(std::memory_order_acquire));
  if (real == nullptr) return 0;
  const std::uint32_t r = real(thread, space, actor_sym, space_out, actor_out, flag_out);
  Capture* const c = t_cap;
  // Enter passes [data+0] = the drone actor's symbol in x2: an Actor call for another actor that re-enters this slot inside the real
  // Actor returns first and must not be taken for Enter's own
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
  return r;
}

void Note(const NodeData& d, std::uint32_t tid, const char* why) {
  if (Allow(g_notes, kNoteCap, "note"))
    LogKv().X("system", d.system).X("component", d.component).S("enable", d.enable ? "on" : "off").U("tid", tid).S("why", why).Emit(NEVR_LOG_INFO, "dronekill: enter note");
}

#define F357_LOG(msg, ...)                                                                          \
  do {                                                                                              \
    if (Allow(g_f357_lines, kF357LineCap, "f357")) LogKv() __VA_ARGS__.Emit(NEVR_LOG_INFO, msg);     \
  } while (0)

bool ReadNode(const NodeData* data, NodeData* d) {
  const auto a = reinterpret_cast<std::uintptr_t>(data);
  return SafeRead(a, d, 0x20) && SafeRead(a + 0x20, &d->enable, sizeof d->enable);  // 0x24 bytes: never reads past the game's struct
}

void EnterWrap(void* node, const NodeData* data) {
  const auto real = reinterpret_cast<EnterFn>(g_enter_real.load(std::memory_order_acquire));
  if (real == nullptr) return;
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
  g_enter_drone.fetch_add(1, std::memory_order_relaxed);
  Capture cap;
  Capture* const outer = t_cap;  // a drone Enter inside a drone Enter keeps its own capture
  t_cap = &cap;
  real(node, data);  // DK-3: the game's own work first, arguments untouched
  t_cap = outer;
  const std::uint32_t tid = Tid();
  if (!cap.have || !cap.ok || cap.space == nullptr) {
    Note(d, tid, "no actor (Enter found none, or Actor was not called)");
    return;
  }
  void* const system = g_fns.find != nullptr ? g_fns.find(cap.space, d.system) : nullptr;
  if (system == nullptr) {
    Note(d, tid, "no such system in the actor's game space");
    return;
  }
  std::uintptr_t component = 0;
  if (!g_cmp_ok || !ComponentShape(system, &component)) {
    char why[96];
    std::snprintf(why, sizeof why, "system %016llx: not looked up (its Component() is not the read shape)", static_cast<unsigned long long>(d.system));
    Note(d, tid, why);
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
    Comp c = {reinterpret_cast<std::uint64_t>(system), d.system, d.component, handle, NowMs()};
    c.actor = cap.actor;
    c.space = reinterpret_cast<std::uint64_t>(cap.space);
    c.owner = owner;
    pthread_mutex_lock(&g_mu);
    g_touch.Deployed(c, c.ms);
    g_any_due.store(true, std::memory_order_release);
    pthread_mutex_unlock(&g_mu);
    F357_LOG("dronekill: touch deployed (grabbable until it locks on; on again at the tick)", .U("index", handle & 0xffff).U("tid", tid));
  }
  if (g_gun && d.system == kNcaModel && (d.component == kGun || d.component == kPort)) {  // F5
    std::uint32_t shown = 0;
    bool known = false;
    pthread_mutex_lock(&g_mu);
    if (d.component == kGun) g_guns.Set(cap.actor, d.enable);
    else known = g_guns.Shown(cap.actor, &shown);
    pthread_mutex_unlock(&g_mu);
    Capture* const saved = t_cap;  // ActorWrap only passes through for our own extra Enters (M1)
    t_cap = nullptr;
    if (d.component == kGun) {
      for (const std::uint64_t sym : {kSatellite, kPort}) {
        if ((static_cast<std::uint32_t>(CallComponent(component, system, cap.actor, sym)) & 0xffff) == 0xffff) {  // M3
          F357_LOG("dronekill: gun: drone has no such component, not mirrored", .X("component", sym));
          continue;
        }
        NodeData copy = d;  // E1: Enter reads data +0..+0x20 only and keeps no pointer to it
        copy.component = sym;
        real(node, &copy);
      }
      F357_LOG("dronekill: gun mirrored (satellite and port follow)", .S("gun", d.enable ? "shown" : "hidden").U("tid", tid));
    } else if (known && shown != d.enable) {  // M2: the port follows the gun, by effect
      NodeData copy = d;
      copy.enable = shown;
      real(node, &copy);
      F357_LOG("dronekill: port follows the gun", .S("script", d.enable ? "shown" : "hidden").S("gun", shown ? "shown" : "hidden").U("tid", tid));
    }
    t_cap = saved;
  }
  Joined j{0, 0, false, false};
  if (d.enable) {
    pthread_mutex_lock(&g_mu);  // DK-5: table operations only
    j = g_table.On(reinterpret_cast<std::uint64_t>(system), d.system, d.component, handle, NowMs(), cap.actor,
                   reinterpret_cast<std::uint64_t>(cap.space), owner);
    if (j.again && g_kill) g_any_due.store(true, std::memory_order_release);  // R3: off again at the next tick
    pthread_mutex_unlock(&g_mu);
  }
  if (j.again) {
    if (Allow(g_off_lines, kOffLineCap, "off"))
      LogKv().X("system", d.system).X("component", d.component).U("index", handle & 0xffff).U("group", static_cast<unsigned long long>(j.group))
          .S("action", g_kill ? "off again at the next tick" : "would be off again (no kill word)").U("tid", tid)
          .Emit(NEVR_LOG_INFO, "dronekill: re-enabled in a dead group");
    return;
  }
  if (!Allow(g_lines, kLineCap, "enter")) return;
  if (d.enable && d.system == kDamageable) {
    LogKv().U("index", handle & 0xffff).U("group", static_cast<unsigned long long>(j.group)).U("pending_joined", static_cast<unsigned long long>(j.pendingJoined))
        .X("flag368", flag).U("tid", tid).Emit(NEVR_LOG_INFO, "dronekill: release (damageable on)");
    return;
  }
  char where[32];
  if (!d.enable) std::snprintf(where, sizeof where, "(off)");
  else if (j.pending) std::snprintf(where, sizeof where, "pending");
  else std::snprintf(where, sizeof where, "group %d", j.group);
  LogKv().X("system", d.system).X("component", d.component).U("index", handle & 0xffff).S("enable", d.enable ? "on" : "off").X("flag368", flag)
      .U("tid", tid).S("to", where).Emit(NEVR_LOG_INFO, "dronekill: component");
}

// Stage 1, D2 / D4: one component off, as Enter itself would turn it off. 0 = done, 1 = skipped for good (why), 2 = wait (the game
// space's invoke-immediate word is 0). D4 (DK1-2): the recorded component is still that one.
void* Revalidate(const Comp& c, std::uintptr_t* owner_out, const char** why) {
  std::uintptr_t owner = 0;
  if (c.space == 0 || c.owner == 0) {
    *why = "no game space recorded";
    return nullptr;
  }
  if (!SafeRead(static_cast<std::uintptr_t>(c.system) + kSysOwner, &owner, sizeof owner) || owner != c.owner) {
    *why = "system's game space changed";
    return nullptr;
  }
  void* const system = g_fns.find != nullptr ? g_fns.find(reinterpret_cast<const void*>(c.space), c.sysSym) : nullptr;
  if (system != reinterpret_cast<void*>(c.system)) {
    *why = "system not found again in its game space";
    return nullptr;
  }
  std::uintptr_t component = 0;
  if (!g_cmp_ok || !ComponentShape(system, &component)) {
    *why = "Component() not the read shape";
    return nullptr;
  }
  if (static_cast<std::uint32_t>(CallComponent(component, system, c.actor, c.compSym)) != c.handle) {
    *why = "handle changed";
    return nullptr;
  }
  *owner_out = owner;
  return system;
}

int OffOne(const Comp& c, const char** why) {
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

// F3 L2: the touch interact the script turned off, on again (EnableComponent, as Enter itself would): 0 = done, 1 = skipped for good
// (why), 2 = wait (the invoke-immediate word is 0, or the script's disable is not applied yet)
int OnOne(const Comp& c, const char** why) {
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

// F3 L3 (upstream LockedOn): a table operation only, on the shot's thread
void LockedOn(std::uint32_t at_handle) {
  pthread_mutex_lock(&g_mu);
  const int slot = g_touch.LockedOn(at_handle, NowMs());
  if (slot >= 0) g_any_due.store(true, std::memory_order_release);
  pthread_mutex_unlock(&g_mu);
}

// FireBulletLocal(cs, handle, C3Vector velocity by value, remote) (E4: AttackTarget remote 0, FireBulletNetwork remote 1)
void FireWrap(void* cs, std::uint32_t handle, DkC3 velocity, std::uint32_t remote) {
  const auto real = reinterpret_cast<FireFn>(g_fire_real.load(std::memory_order_acquire));
  if (real == nullptr) return;
  g_fires.fetch_add(1, std::memory_order_relaxed);
  if (g_lockon) LockedOn(handle);
  const DkC3 saved_shot = t_shot;  // a shot inside a shot keeps the outer one's after it returns
  const std::uint32_t saved_remote = t_remote;
  const bool saved_in = t_in_shot;
  t_shot = velocity;
  t_remote = remote;
  t_in_shot = true;
  real(cs, handle, velocity, remote);
  t_shot = saved_shot;
  t_remote = saved_remote;
  t_in_shot = saved_in;
}

// F7 C3: true = the drone's R14NetPhysics is locally controlled, or the drone has no R14NetPhysics component (not replicated:
// upstream-exact); false = another peer controls it, or anything is unreadable / unknown (fails closed: C2 writes nothing).
bool LocallyControlled(std::uintptr_t physics, std::uint32_t phys_h, const void* space) {
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

// F7 C1 (upstream ChaseTarget): after the real AttackTarget, on its thread, the velocity toward the target from the game's
// own loads-only reads; applied at the tick (C2)
void ChaseAfter(std::uintptr_t cs, std::uint32_t handle) {
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
    if (!ChaseVelocity(me.p, q, v, &dist)) return;  // on the target: upstream writes nothing
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
  if (moving && Allow(g_chase_lines, 8, "chase"))  // upstream logs the first 5
    LogKv().U("drone", handle & 0xffff).D("dist_m", dist).D("speed_ms", std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]))
        .S("locally_controlled", local ? "1" : "0").Emit(NEVR_LOG_INFO, "dronekill: chase");
}

void AttackWrap(void* cs, std::uint32_t handle) {
  const auto real = reinterpret_cast<AttackFn>(g_attack_real.load(std::memory_order_acquire));
  if (real == nullptr) return;
  real(cs, handle);
  g_attacks.fetch_add(1, std::memory_order_relaxed);
  if (g_chase) ChaseAfter(reinterpret_cast<std::uintptr_t>(cs), handle);
}

// F3 L2 / L4 at the tick: RE-ON records on again, OFF-DUE ones off (only while the component is part of a live group: TouchLive).
// Engine calls with g_mu released (D5).
void TouchTick(std::uint64_t now, std::uint32_t tid) {
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
      F357_LOG("dronekill: touch on again", .U("index", r.c.handle & 0xffff).S("result", k == 0 ? "on again: grabbable until it locks on" : why).U("tid", tid));
      continue;
    }
    pthread_mutex_lock(&g_mu);
    const bool live = g_table.LiveComp(r.c.system, r.c.handle) || g_table.LiveActor(r.c.actor);
    pthread_mutex_unlock(&g_mu);
    const int k = live ? OffOne(r.c, &why) : 1;
    char nolive[64];
    if (!live) {
      std::snprintf(nolive, sizeof nolive, "belongs to no live drone (actor %016llx), left alone", static_cast<unsigned long long>(r.c.actor));
      why = nolive;
    }
    if (k == 2) continue;
    pthread_mutex_lock(&g_mu);
    if (g_touch.t[slots[i]].state == kTouchOffDue && g_touch.t[slots[i]].c.handle == r.c.handle) g_touch.OffDone(slots[i]);
    pthread_mutex_unlock(&g_mu);
    F357_LOG("dronekill: touch locked on", .U("index", r.c.handle & 0xffff).U("at", r.atHandle & 0xffff).S("result", k == 0 ? "off" : why).U("tid", tid));
  }
}

// AUD-7 DF-1: the recorded drone is still that one at the tick (up to kChaseMs after AttackTarget). Plain loads only.
bool HandleLive(std::uintptr_t system, std::uint64_t idx_off, std::uint32_t handle, std::uint16_t* slot_out) {
  std::uintptr_t idx = 0;
  std::uint16_t e[2] = {};
  if (!SafeRead(system + idx_off, &idx, sizeof idx) || !idx || !SafeRead(idx + (handle & 0xffff) * 4ull, e, sizeof e)) return false;
  if (e[1] != static_cast<std::uint16_t>(handle >> 16)) return false;
  *slot_out = e[0];
  return true;
}

bool ChaseStillOk(const Chase& c) {
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

// F7 C2 at the tick: each due velocity, younger than kChaseMs, set directly while its physics system's game space is in the
// invoke-immediate window (the engine's own rule for SetWorldVelocity, E6)
void ChaseTick(std::uint64_t now) {
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

// The source's net tick body (source-build ec called it at the end of each recvfrom on the game's own socket); here once per frame on the
// game thread. Nothing but one atomic load while nothing is due.
void TickBody() {
  if (!g_any_due.load(std::memory_order_acquire) || t_in_tick) return;
  t_in_tick = true;
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
    if (Allow(g_off_lines, kOffLineCap, "off"))
      LogKv().U("group", static_cast<unsigned long long>(due[i].number)).X("system", due[i].c.sysSym).X("component", due[i].c.compSym)
          .U("index", due[i].c.handle & 0xffff).S("result", r == 0 ? "OFF" : why).U("tid", tid).Emit(NEVR_LOG_INFO, "dronekill: component off");
  }
  if (g_lockon) TouchTick(now, tid);
  if (g_chase) ChaseTick(now);
  pthread_mutex_lock(&g_mu);
  if (!g_table.AnyDue() && !g_touch.AnyDue() && !g_chases.AnyDue()) g_any_due.store(false, std::memory_order_release);
  pthread_mutex_unlock(&g_mu);
  if ((expired != 0 || (waiting != 0 && off == 0 && skipped == 0)) && Allow(g_off_lines, kOffLineCap, "off"))
    LogKv().U("tid", tid).U("off", static_cast<unsigned long long>(off)).U("skipped", static_cast<unsigned long long>(skipped))
        .U("waiting_invoke_immediate_0", static_cast<unsigned long long>(waiting)).U("expired", expired).U("due_after_ms", kDueMs)
        .Emit(NEVR_LOG_WARNING, "dronekill: tick");
  t_in_tick = false;
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

void LogSwap(const char* hook, const InstallResult& r) {
  LogKv().S("hook", hook).S("status", StatusName(r.status)).S("reason", r.reason).Emit(r.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "dronekill hook install");
}

struct Hook {
  const char* name;
  std::uintptr_t slot, fn, wrapper;
  std::atomic<std::uintptr_t>* real;
  InstallResult* result;
};

}  // namespace


namespace {
bool CheckFnAddresses(std::uintptr_t base, const void* const p[6], DroneKillFns* out) {
  for (int i = 0; i < 6; ++i) {
    if (p[i] != nullptr && reinterpret_cast<std::uintptr_t>(p[i]) == base + kFnSpecs[i].vaddr) continue;
    LogKv().S("function", kFnSpecs[i].name).X("want_vaddr", kFnSpecs[i].vaddr).X("found", reinterpret_cast<std::uintptr_t>(p[i]))
        .Emit(NEVR_LOG_ERROR, "dronekill: a function is missing or not at its pinned address (not the pinned build?)");
    return false;
  }
  DroneKillFns f;
  f.find = reinterpret_cast<void* (*)(const void*, std::uint64_t)>(const_cast<void*>(p[0]));
  f.l2w = reinterpret_cast<DkTransfQ (*)(const void*, std::uint32_t)>(const_cast<void*>(p[1]));
  f.primary = reinterpret_cast<DkPhTarget (*)(const void*, std::uint32_t)>(const_cast<void*>(p[2]));
  f.resolve = reinterpret_cast<void* (*)(const DkPhTarget*)>(const_cast<void*>(p[3]));
  f.set_vel = reinterpret_cast<void (*)(void*, std::uint32_t, DkC3)>(const_cast<void*>(p[4]));
  f.local = reinterpret_cast<std::uint32_t (*)(const void*, std::uint32_t)>(const_cast<void*>(p[5]));
  *out = f;
  return true;
}
}  // namespace

bool ResolveDroneKillFnsFromAddressesForTest(std::uintptr_t base, const void* const addrs[6], DroneKillFns* out) { return CheckFnAddresses(base, addrs, out); }

bool ResolveDroneKillFns(std::uintptr_t base, DroneKillFns* out) {
  void* h = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) {
    LogKv().S("why", "libr15.so is not loaded").Emit(NEVR_LOG_ERROR, "dronekill: functions not resolved");
    return false;
  }
  const void* p[6];
  for (int i = 0; i < 6; ++i) p[i] = dlsym(h, kFnSpecs[i].name);
  dlclose(h);
  return CheckFnAddresses(base, p, out);
}

DroneKillInstall InstallDroneKill(std::uintptr_t base, const DroneKillWords& words, const DroneKillFns& fns, const DroneKillSlots& slots) {
  DroneKillInstall out;
  auto refuse = [&out](const char* why) {
    LogKv().S("why", why).Emit(NEVR_LOG_ERROR, "dronekill: OFF, nothing changed");
    return out;
  };
  if (base == 0) return refuse("no base address");
  if (g_installed) return refuse("already installed");
  if (fns.find == nullptr) return refuse("FindComponentSystemMT is not resolved");
  g_base = base;
  g_cmp_plt = slots.cmp_plt;
  g_fns = fns;
  g_kill = words.kill;
  g_lockon = words.lockon;
  g_gun = words.gun;
  g_chase = words.chase;
  g_shot = words.shot;
  if (g_chase && (fns.l2w == nullptr || fns.primary == nullptr || fns.resolve == nullptr || fns.set_vel == nullptr || fns.local == nullptr)) {
    LogKv().S("why", "a function chase calls is not resolved").Emit(NEVR_LOG_ERROR, "dronekill: chase OFF");
    g_chase = false;
  }
  bool rd_ok = true;
  const auto cmp = Rd<std::uintptr_t>(base + slots.cmp_slot, &rd_ok);
  g_cmp_ok = rd_ok && cmp == base + slots.cmp_fn;
  if (!g_cmp_ok) LogKv().X("slot_word", cmp).X("want", base + slots.cmp_fn).Emit(NEVR_LOG_WARNING, "dronekill: the comparator slot does not hold its function; Component() look-ups are off");

  const bool want_fire = g_lockon || g_shot, want_attack = g_chase;
  Hook hooks[4] = {
      {"SComponentNodeUtils::Actor", slots.actor_slot, slots.actor_fn, reinterpret_cast<std::uintptr_t>(&ActorWrap), &g_actor_real, &out.actor},
      {"CEnableComponentNode::Enter", slots.enter_slot, slots.enter_fn, reinterpret_cast<std::uintptr_t>(&EnterWrap), &g_enter_real, &out.enter},
      {"CR15NetAutoTargetCS::FireBulletLocal", slots.fire_slot, slots.fire_fn, reinterpret_cast<std::uintptr_t>(&FireWrap), &g_fire_real, &out.fire},
      {"CR15NetAutoTargetCS::AttackTarget", slots.attack_slot, slots.attack_fn, reinterpret_cast<std::uintptr_t>(&AttackWrap), &g_attack_real, &out.attack}};
  bool use[4] = {true, true, want_fire, want_attack};
  // every word is checked before the first swap (DK-6: all or none)
  for (int i = 0; i < 4; ++i) {
    if (!use[i]) continue;
    bool ok = true;
    const auto word = Rd<std::uintptr_t>(base + hooks[i].slot, &ok);
    if (ok && word == base + hooks[i].fn) continue;
    hooks[i].result->status = SwapStatus::kUnexpectedOriginal;
    hooks[i].result->reason = kUnexpectedSlot;
    LogSwap(hooks[i].name, *hooks[i].result);
    return refuse("a slot does not hold the pinned function");
  }
  // swap order: Actor first (its wrapper alone only passes through; t_cap is set by EnterWrap only), then Enter, FireBulletLocal, AttackTarget
  for (int i = 0; i < 4; ++i) {
    if (!use[i]) continue;
    *hooks[i].result = InstallImportHook(hooks[i].name, base, hooks[i].slot, hooks[i].wrapper, hooks[i].real, hooks[i].fn);
    LogSwap(hooks[i].name, *hooks[i].result);
    if (hooks[i].result->status == SwapStatus::kOk) continue;
    int not_restored = 0;
    const int last = hooks[i].result->status == SwapStatus::kRestoreFailed ? i : i - 1;  // that word was written even though the call failed
    for (int k = last; k >= 0; --k) {
      if (!use[k]) continue;
      const SwapResult back = SwapWord(base + hooks[k].slot, hooks[k].wrapper, base + hooks[k].fn);
      if (back.status != SwapStatus::kOk) ++not_restored;
    }
    LogKv().S("failed_hook", hooks[i].name).U("words_not_put_back", static_cast<unsigned long long>(not_restored))
        .Emit(not_restored != 0 ? NEVR_LOG_ERROR : NEVR_LOG_WARNING, "dronekill: a swap failed; the earlier words were put back");
    return out;
  }
  g_installed = true;
  g_shot_swapped.store(want_fire, std::memory_order_release);
  out.ok = true;
  out.comparator_ok = g_cmp_ok;
  out.needs_tick = g_kill || g_lockon || g_chase;
  LogKv().S("words", g_kill ? (g_lockon && g_gun && g_chase && g_shot ? "kill lockon gun chase shot" : "kill (+ some)") : "no kill")
      .S("fire_bullet_local", want_fire ? "wrapped" : "not wrapped").S("attack_target", want_attack ? "wrapped" : "not wrapped")
      .S("component_lookups", g_cmp_ok ? "on (read shapes only)" : "OFF").S("needs_tick", out.needs_tick ? "1" : "0")
      .S("k_feed", "none in the plugin: kill waits for DroneKillOnKilled").Emit(NEVR_LOG_INFO, "dronekill: on (QA)");
  return out;
}

void DroneKillTick(std::uintptr_t) {
  if (!g_installed) return;
  if (g_ticks.fetch_add(1, std::memory_order_relaxed) == 0) LogKv().Emit(NEVR_LOG_INFO, "dronekill: tick running (first call)");
  TickBody();
}

// The 'K' feed (the source's OnKilled + the module thread's line): the group is marked dead and due (kill) under g_mu, a table operation only.
void DroneKillOnKilled(std::uint32_t index) {
  if (!g_installed) return;
  const std::uint32_t tid = Tid();
  unsigned long long group = 0, comps = 0;
  unsigned long long live_n = 0;
  std::uint32_t live[kGroups];
  pthread_mutex_lock(&g_mu);
  if (g_kill) {
    if (Group* g = g_table.Killed(index, NowMs(), true)) {
      group = static_cast<unsigned long long>(g->number);
      comps = static_cast<unsigned long long>(g->n);
      if (g->due) g_any_due.store(true, std::memory_order_release);
      if (g_gun) g_guns.Set(g->actor, 0);  // M4: upstream Shutdown's SetGun(DRONE_ACTOR, 0)
    }
  } else if (Group* g = g_table.Killed(index)) {
    group = static_cast<unsigned long long>(g->number);
    comps = static_cast<unsigned long long>(g->n);
  }
  live_n = static_cast<unsigned long long>(g_table.Live(live, kGroups));
  pthread_mutex_unlock(&g_mu);
  if (!Allow(g_k_lines, kKLineCap, "k")) return;
  LogKv().U("index", index & 0xffff).U("tid", tid).U("group", group).U("components", comps).U("live_groups", live_n)
      .S("result", group == 0 ? "NO live group has that index" : g_kill ? "group shut down (its components go off at the tick)" : "would be shut down (nothing done)")
      .Emit(NEVR_LOG_INFO, "dronekill: K");
}

const float* DroneKillShotVelocity(int* remote) {
  if (!t_in_shot) return nullptr;
  if (remote != nullptr) *remote = static_cast<int>(t_remote);
  return &t_shot.x;
}

bool DroneKillShotActive() { return g_shot_swapped.load(std::memory_order_acquire); }

DroneKillStats GetDroneKillStats() {
  return {g_enter_drone.load(), g_enter_other.load(), g_fires.load(), g_attacks.load(), g_ticks.load(), g_due_runs.load()};
}

void SetDroneKillCallsForTest(const DroneKillCalls* calls) { g_test_calls = calls; }
void SetDroneKillClockForTest(std::uint64_t (*now_ms)()) { g_clock_ms = now_ms; }
void SetDroneKillShotForTest(const float* v3, int remote) {
  t_in_shot = v3 != nullptr;
  if (v3 != nullptr) t_shot = DkC3{v3[0], v3[1], v3[2]};
  t_remote = static_cast<std::uint32_t>(remote);
}

void SetDroneKillShotActiveForTest(bool active) { g_shot_swapped.store(active); }

void ResetDroneKillForTest() {
  g_actor_real.store(0);
  g_enter_real.store(0);
  g_fire_real.store(0);
  g_attack_real.store(0);
  g_base = g_cmp_plt = 0;
  g_fns = DroneKillFns{};
  g_cmp_ok = g_installed = false;
  g_kill = g_lockon = g_gun = g_chase = g_shot = false;
  g_shot_swapped.store(false);
  pthread_mutex_lock(&g_mu);
  g_table = Table();
  g_touch = TouchTable();
  g_guns = GunTable();
  g_chases = ChaseTable();
  g_aux_space = nullptr;
  g_any_due.store(false);
  pthread_mutex_unlock(&g_mu);
  for (auto& c : g_vt) c.store(0);
  for (auto& c : g_vt_dis) c.store(0);
  for (auto& c : g_vt_en) c.store(0);
  t_cap = nullptr;
  t_in_shot = t_in_tick = false;
  t_remote = 0;
  g_lines.store(0);
  g_notes.store(0);
  g_off_lines.store(0);
  g_k_lines.store(0);
  g_f357_lines.store(0);
  g_chase_lines.store(0);
  g_enter_drone.store(0);
  g_enter_other.store(0);
  g_fires.store(0);
  g_attacks.store(0);
  g_ticks.store(0);
  g_due_runs.store(0);
  g_clock_ms = nullptr;
  g_test_calls = nullptr;
}

}  // namespace q15t

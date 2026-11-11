// nEVR port: he_is_the_cat's plugin/dronefix.cpp from heisthecat31/EchoCombat (branch nevr-pack, c3ff4b54), compiled into
// nevr-runtime.dll as part of the combat module (runtime/combat/combat.cpp) nearly as he wrote it. Every change to his
// text is marked "nEVR port:". cat_compat.h maps Detours, EC_TRY/__except, his .ini reads and his logger onto the
// runtime; every write to the game passes runtime/combat/write_gate.h first. __try, __except and
// GetExceptionCode are spelled EC_TRY, EC_EXCEPT and EC_EXCEPTION_CODE (cat_compat.h).

// DroneFix: keeps the cut DRONE ordnance from crashing Echo VR.
//
// The drone's actor carries a CR15NetFollowPlayerCS component (it orbits its owner). That system
// follows ONE player, held on the system itself: a body-system pointer at +0x140 and the player's
// index at +0x190, filled in when a script runs R15FollowPlayerSetOrbitNode. Its update
//   +0xd65cb0  CR15NetFollowPlayerCS::Update(cs)
// walks every enabled follow component and reads that player's transform through +0x140 with no
// check. Once the drone runs the shared magazine script (fix_drone_script.py) its components are
// enabled as soon as it spawns in the pouch -- before any orbit target exists -- and the update
// dereferences a null +0x140 (crash at echovr+0x742aaa, GetCurrentTransform).
//
// The hook skips the update until a target is set, and contains an access violation inside it
// rather than letting it take the game down. The prologue is verified first; on a mismatch
// nothing is hooked (and nEVR unloads the plugin). A nEVR plugin (nevr_plugin.h): hooked from
// NvrPluginInitEx, unhooked in NvrPluginShutdown. Its lines go to nEVR's host log, tagged
// plugin=DroneFix; the per-shot and per-tick lines are Debug, and the extra diagnostic hooks
// and their lines are built with ECHOCOMBAT_LOG.
//
// It also fires the drone's bullets from its gun. CR15NetAutoTargetCS aims by turning the
// drone's ROOT transform toward the target (+Z at it) and spawns each bullet at that root:
//   +0xce2550  CR15NetAutoTargetCS::FireBulletLocal(cs, handle, velocity, remote)
//   +0xce27e3    call CTransformCS::get(transforms, &out, root)   -> the bullet's spawn pose
// which is the middle of the drone, not the gun on its top (tools/fix_drone_model.py). That
// one call is redirected to MuzzleTransform, which moves the pose by MUZZLE, the gun's
// muzzle in the drone, turned the way AttackTarget turns the drone for this shot: +Z along
// the bullet's velocity, +Y as near world up as it can be. (Not the root's rotation: that
// is set through a deferred queue, so at the shot it can still be the last one, and differ
// between the server and the clients.) FireBulletLocal is hooked only to hand its velocity
// to MuzzleTransform. Every auto-target component is the drone's, and both the server's and
// the clients' shots go through it. Each bullet starts a frame's travel (at most BACK_MAX) behind the muzzle,
// so it is first drawn there (see TickFrame). The first SHOT_LOG shots are logged.
// nEVR port: MUZZLE, BACK_MAX, the frame average and the move are runtime/patch/dronemuzzle_rules.h, the one
// definition the Quest's dronemuzzle.cpp calls too (#2337); this file keeps its clock, its pose read and its lines.
// nEVR port: the drone kill rules (the symbols, the chase velocity, the gun table, the group table) are
// runtime/patch/dronekill_rules.h, the one definition the Quest's dronekill.cpp calls too (#2359); this file
// keeps its hooks, its clock reads and its lines.

#include "runtime/combat/cat_compat.h"  // nEVR port: first, for winsock2.h before windows.h
#include "runtime/combat/cat_links.h"   // nEVR port: the other two files, in place of GetProcAddress
#include "runtime/combat/write_gate.h"  // nEVR port
#include "runtime/hook/process_memory.h"  // nEVR port: byte patches recorded
#include "runtime/patch/dronemuzzle_rules.h"  // nEVR port: the muzzle rule, shared with the Quest (#2337)
#include "runtime/patch/dronekill_rules.h"  // nEVR port: the drone kill rules, shared with the Quest (#2359)
#include "core/burst_then_count.h"  // nEVR port: repeated Warnings are counted, never silenced (#2349)
#include "runtime/patch/ecfx_rules.h"  // nEVR port: NEUTRON's effects drain, shared with the Quest (#2349)
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <atomic>  // nEVR port: the Warning budgets' pending flags (#2349)

// Lines go to nEVR's host log, tagged plugin=<config.yaml name> (docs/standards/logging.md, Rule 15).
namespace nevr_combat::dronefix {  // nEVR port: one namespace per file
constexpr const char *kCatModule = "dronefix";  // nEVR port
NEVR_DEFINE_PLUGIN_LOG("[DroneFix]")

constexpr const char *kLogName = "DroneFix";   // names the plugin in its log-cap Warnings

namespace {

constexpr uintptr_t FOLLOW_UPDATE_RVA = 0xd65cb0;
constexpr uint8_t FOLLOW_UPDATE_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9 };
constexpr uintptr_t TARGET_BODY_SYSTEM = 0x140;

using follow_update_fn = void(__fastcall *)(uintptr_t);
follow_update_fn Real_FollowUpdate = nullptr;

constexpr uintptr_t FIRE_GET_CALL_RVA = 0xce27e3;           // in FireBulletLocal
constexpr uintptr_t TRANSFORM_GET_RVA = 0x3c4470;           // CTransformCS::get
constexpr uint8_t FIRE_GET_CALL_BYTES[] = { 0xe8, 0x88, 0x1c, 0x6e, 0xff, 0x0f, 0x10, 0x45, 0x40 };
// nEVR port: the gun's muzzle in the drone (MUZZLE, fix_drone_model.py's) and BACK_MAX are dm::kMuzzle and dm::kBackMax
namespace dm = nevr_dronemuzzle;
namespace dk = nevr_dronekill;  // nEVR port: the drone kill rules (#2359)

constexpr uintptr_t FIRE_LOCAL_RVA = 0xce2550;              // CR15NetAutoTargetCS::FireBulletLocal
constexpr uint8_t FIRE_LOCAL_BYTES[] = { 0x44, 0x89, 0x4c, 0x24, 0x20, 0x55, 0x53, 0x57, 0x41, 0x54 };
constexpr LONG SHOT_LOG = 40;

// The drone's script shows / hides its gun with an EnableComponentNode on (ncaModel, GUN),
// but nothing hides the satellite (another model component, fix_drone_model.py), so it
// outlived the gun. Every such node on GUN is repeated for SATELLITE.
//   +0x12478d0  CEnableComponentNode::Enter(node, data): data = { actor, scope, system,
//               component, enable }
constexpr uintptr_t ENABLE_ENTER_RVA = 0x12478d0;
constexpr uint8_t ENABLE_ENTER_BYTES[] = { 0x40, 0x55, 0x56, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0x01 };
// the port on the body's front (fix_drone_model.py's PORT_COMP: a detonator part the drone's
// script shows once deployed; 0xA977.. is the one it shows only in the hand)
using enable_enter_fn = void(__fastcall *)(void *, const uint64_t *);
enable_enter_fn Real_EnableEnter = nullptr;
volatile LONG g_mirrored = 0;

// CTransformCS::get's output: rotation (x, y, z, w), then position
struct Pose { float q[4]; float p[3]; float s; };
using transform_get_fn = Pose *(__fastcall *)(void *, Pose *, uint32_t);
transform_get_fn Real_TransformGet = nullptr;
using fire_local_fn = void(__fastcall *)(void *, uint32_t, const float *, int);
fire_local_fn Real_FireLocal = nullptr;
thread_local const float *t_velocity = nullptr;             // the shot FireBulletLocal is making
thread_local int t_remote = 0;
void *volatile g_at_cs = nullptr;                               // the auto-target system (DroneHitsDue)
void *volatile g_bullet_cs = nullptr;                           // the bullet system
void NoteDroneShot(uint8_t *bullet_rec);
struct AtHandle { uint32_t handle; ULONGLONG seen; };
AtHandle g_at_handles[8];                                       // drones' auto-target handles, from their shots
SRWLOCK g_at_lock = SRWLOCK_INIT;
void NoteAtHandle(uint32_t handle) {
    AcquireSRWLockExclusive(&g_at_lock);
    AtHandle *slot = nullptr;
    for (auto &h : g_at_handles) if (h.handle == handle && h.seen) slot = &h;
    if (!slot) for (auto &h : g_at_handles) if (!slot || h.seen < slot->seen) slot = &h;
    *slot = { handle, GetTickCount64() };
    ReleaseSRWLockExclusive(&g_at_lock);
}

volatile LONG g_muzzle_shots = 0;
volatile LONG g_skipped = 0;
volatile LONG g_faults = 0;
SRWLOCK g_fault_lines_lock = SRWLOCK_INIT;            // nEVR port: the fault Warnings' budget (#2349)
nevr::BurstThenCount g_fault_lines{5, 10000};

// One line through nEVR's host log. Debug and Info are counted apart, so a burst of Debug lines
// never takes Info's room: Debug stops after kDebugCap lines and Info after kInfoCap. A line past
// either cap is counted, not written (nEVR port, #2349): the first says so in a Warning, then the
// count follows at most every 10 s, flushed with DRONE on and at Stop (nevr::BurstThenCount with no
// burst of its own: the cap is the burst). A Warning or an Error is always written.
constexpr LONG kDebugCap = 5000;
constexpr LONG kInfoCap = 20000;
volatile LONG g_logged_debug = 0;
volatile LONG g_logged_info = 0;
SRWLOCK g_dropped_lock = SRWLOCK_INIT;
nevr::BurstThenCount g_dropped_lines[2] = {{0, 10000}, {0, 10000}};  // [0] Debug, [1] Info
std::atomic<bool> g_dropped_pending{false};

void ReportDropped(bool debug, LONG cap, const nevr::BurstThenCount::Line &l) {
    using V = nevr::BurstThenCount::Verdict;
    char note[200];
    if (l.verdict == V::kCapped)
        snprintf(note, sizeof(note), "%s log cap reached: later %s lines are counted, not written (cap %ld); the count follows at most every 10 s",
                 kLogName, debug ? "Debug" : "Info", static_cast<long>(cap));
    else if (l.verdict == V::kCount)
        snprintf(note, sizeof(note), "%s %s lines over the cap: %llu not written in all (%llu since the last line)", kLogName,
                 debug ? "Debug" : "Info", static_cast<unsigned long long>(l.total), static_cast<unsigned long long>(l.sinceLine));
    else
        return;
    PluginLogFields(NEVR_LOG_WARNING, note, {});
}

void DroppedLine(bool debug, LONG cap) {
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_dropped_lock);
    const nevr::BurstThenCount::Line l = g_dropped_lines[debug ? 0 : 1].Event(now);
    g_dropped_pending.store(g_dropped_lines[0].Pending() || g_dropped_lines[1].Pending(), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_dropped_lock);
    ReportDropped(debug, cap, l);
}

// The dropped-line counts no line covers yet: once none has come for 10 s, or (final) at once.
void FlushDropped(bool final) {
    if (!final && !g_dropped_pending.load(std::memory_order_relaxed)) return;
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_dropped_lock);
    const nevr::BurstThenCount::Line debugLine = final ? g_dropped_lines[0].Final(now) : g_dropped_lines[0].Flush(now);
    const nevr::BurstThenCount::Line infoLine = final ? g_dropped_lines[1].Final(now) : g_dropped_lines[1].Flush(now);
    g_dropped_pending.store(g_dropped_lines[0].Pending() || g_dropped_lines[1].Pending(), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_dropped_lock);
    ReportDropped(true, kDebugCap, debugLine);
    ReportDropped(false, kInfoCap, infoLine);
}

void LogLine(uint32_t level, const char *fmt, va_list args) {
    if (level < NEVR_LOG_WARNING) {
        bool debug = level == NEVR_LOG_DEBUG;
        LONG cap = debug ? kDebugCap : kInfoCap;
        LONG n = InterlockedIncrement(debug ? &g_logged_debug : &g_logged_info);
        if (n > cap) {
            DroppedLine(debug, cap);
            return;
        }
    }
    char line[1024];
    vsnprintf(line, sizeof(line), fmt, args);
    PluginLogFields(level, line, {});
}

void Log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogLine(NEVR_LOG_INFO, fmt, args);
    va_end(args);
}

void LogDebug(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogLine(NEVR_LOG_DEBUG, fmt, args);
    va_end(args);
}

void LogWarning(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogLine(NEVR_LOG_WARNING, fmt, args);
    va_end(args);
}

void LogError(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogLine(NEVR_LOG_ERROR, fmt, args);
    va_end(args);
}

bool CallGuarded(uintptr_t cs) {
    EC_TRY {
        Real_FollowUpdate(cs);
        return true;
    } EC_EXCEPT(EC_EXCEPTION_CODE() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
}

// The game's frame time, from the follow-player update (once a frame): a bullet is first
// drawn after its first step, a frame's travel from where it is spawned (at 120 m/s and
// 45-90 fps, 1.3-2.7 m out), so it is spawned a frame behind the muzzle.
LARGE_INTEGER g_qpf, g_last_frame;
volatile float g_frame_dt = 1.0f / 90.0f;

void TickFrame() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_last_frame.QuadPart) {
        float dt = (float)(now.QuadPart - g_last_frame.QuadPart) / (float)g_qpf.QuadPart;
        g_frame_dt = dm::FrameDt(g_frame_dt, dt);                // nEVR port: the shared rule (#2337)
    }
    g_last_frame = now;
}

// shared by the death and grab rules below
using disable_fn = void(__fastcall *)(void *system, uint32_t index, uint16_t flag);
disable_fn Real_Disable = nullptr;

// --- the drone dies when its health runs out --------------------------------------------
// fix_drone_health.py gives the drone's damageable real health, and ProcessDamageServer kills
// it at 0 (the dead flag, then the death routine DEATH_RVA), but for anything that is not a
// player nothing happens after that. So: at each release, every component the drone's script
// turns on (CEnableComponentNode::Enter -> ENABLE_COMP_RVA (component system, index)) goes
// into that drone's group, its damageable first (a new life: health back to full and the dead
// flag off, as nothing else ever revives an ordnance). When that damageable's death routine
// runs (on the server; VeilNet.dll then tells the clients, DroneFix_Killed), the group's
// components are turned off, and kept off whatever the script does, until the next release:
// the drone is gone, and its life and cooldown run out as they would.
constexpr uintptr_t ENABLE_COMP_RVA = 0x160070;                  // (component system, index, flag): enable
constexpr uint8_t ENABLE_COMP_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x41, 0x56 };
constexpr uintptr_t DEATH_RVA = 0xccf3a0;                        // CR15NetDamageableCS death (cs, index)
constexpr uint8_t DEATH_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10 };
//: the drone death token (fix_drone_fx.py's DEATH_TOKEN_RVA): +00 count, +08 expiry, +10 shown
constexpr uintptr_t DEATH_TOKEN_RVA = 0x2104D60;
// the system the drone's script turns off first when its lifetime ends (all of it): dk::kNcaPhysics, which upstream calls its
// sound (nEVR port: the shared constant, #1737)
// (test) each deployed drone killed on the server this long after release, as a hit to 0
// health does (own-team bullets can't hit own ordnance, so it can't be shot alone); 0 = off
constexpr ULONGLONG KILL_TEST_MS = 0;
using enable_comp_fn = void(__fastcall *)(void *system, uint32_t index, uint16_t flag);
using death_fn = void(__fastcall *)(void *cs, uint32_t index);
enable_comp_fn Real_EnableComp = nullptr;
death_fn Real_Death = nullptr;
death_fn Death = nullptr;                                         // the hooked routine (test kill)
thread_local int t_drone_enable = 0;                              // 1: a drone component on, 2: its damageable
thread_local void *t_node = nullptr;                              // the drone script node in Hooked_EnableEnter

// A node context for the drone whose script node `node` is (its explosion's). The drone
// script's dispatcher leaves the context of the last node ANY drone ran at DEATH_TOKEN_RVA
// +20, so with two drones out it can be the other one's (and that one got the explosion).
// A context's first field is its script instance, one per drone, as a node's is: the slot's
// context is taken only when it is this node's drone's, and kept per instance for when it
// is not.
struct InstanceCtx { void *instance; void *ctx; };
InstanceCtx g_ctxs[16];
SRWLOCK g_ctxs_lock = SRWLOCK_INIT;
volatile LONG g_ctx_misses = 0;

void *DroneCtx(void *node, bool quiet = false) {
    void *instance = nullptr, *last = nullptr, *last_instance = nullptr;
    EC_TRY {
        instance = *(void **)node;
        last = *(void *volatile *)((uint8_t *)GetModuleHandleW(nullptr) + DEATH_TOKEN_RVA + 0x20);
        if (last) last_instance = *(void **)last;
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    if (!instance) return nullptr;
    void *ctx = nullptr;
    AcquireSRWLockExclusive(&g_ctxs_lock);
    InstanceCtx *slot = nullptr;
    for (auto &e : g_ctxs) if (e.instance == instance) { slot = &e; break; }
    if (last && last_instance == instance) {
        if (!slot) for (auto &e : g_ctxs) if (!e.instance) { slot = &e; break; }
        if (!slot) slot = &g_ctxs[(uintptr_t)instance / 16 % 16];
        *slot = { instance, last };
        ctx = last;
    } else if (slot) {
        ctx = slot->ctx;
    } else {
        ctx = last;                                            // (none of its own yet: the last one, as before)
    }
    ReleaseSRWLockExclusive(&g_ctxs_lock);
#ifdef ECHOCOMBAT_LOG
    static volatile LONG dumps = 0;
    if (!quiet && last && InterlockedIncrement(&dumps) <= 6)
        EC_TRY {                                                // which field of a context names its drone?
            const uint64_t *c = (const uint64_t *)last, *n = (const uint64_t *)node;
            LogDebug("[test] node %p: %llx %llx %llx %llx | slot ctx %p: %llx %llx %llx %llx %llx %llx %llx %llx",
                node, n[0], n[1], n[2], n[3], last, c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
        } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        }
#endif
    if (!quiet && ctx != last && InterlockedIncrement(&g_ctx_misses) <= 10)
        LogDebug("drone script %p: the last node was another drone's (%p); %s", instance, last_instance,
            ctx != last ? "its own context kept from before" : "none of its own yet: the last one");
    return ctx;
}

// The actor a component belongs to (its system's vtable +1e0: (system, &out, handle) -> the
// actor's handle). A drone runs two scripts (the shared magazine script and its own), each
// its own instance, so its parts are told apart by their actor, not by the node's script.
uint64_t ActorOf(void *system, uint32_t index) {
    EC_TRY {
        using fn = uint64_t *(__fastcall *)(void *, uint64_t *, uint32_t);
        uint64_t out[2] = {};
        fn get = (*(fn **)system)[0x1e0 / 8];
        return *get(system, out, index);
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// each drone's explosion context by actor, learnt whenever one of the drone script's own
// nodes turns a component of it on or off (DroneCtx: the slot's context is that node's)
struct ActorCtx { uint64_t actor; void *ctx; };
ActorCtx g_actor_ctx[16];

void LearnCtx(void *system, uint32_t index) {
    // off: DroneCtx never finds a context of the node's own (a context's first field is not
    // its drone's script instance), so this only learnt whatever ran last -- with two drones
    // out both got the same explosion. A drone's explosion is the context kept at its release.
    (void)system; (void)index;
    return;
    if (!t_node) return;
    void *ctx = DroneCtx(t_node, true);
    if (!ctx) return;
    uint64_t actor = ActorOf(system, index);
    if (!actor) return;
    AcquireSRWLockExclusive(&g_ctxs_lock);
    ActorCtx *slot = nullptr;
    for (auto &e : g_actor_ctx) if (e.actor == actor) { slot = &e; break; }
    if (!slot) for (auto &e : g_actor_ctx) if (!e.actor) { slot = &e; break; }
    if (!slot) slot = &g_actor_ctx[actor % 16];
    *slot = { actor, ctx };
    ReleaseSRWLockExclusive(&g_ctxs_lock);
}

void *CtxOfActor(uint64_t actor) {
    void *ctx = nullptr;
    AcquireSRWLockShared(&g_ctxs_lock);
    for (auto &e : g_actor_ctx) if (actor && e.actor == actor) ctx = e.ctx;
    ReleaseSRWLockShared(&g_ctxs_lock);
    return ctx;
}
bool g_is_server = false;
bool g_drone_on = false;  // nEVR port: combat.drone; off leaves only the follow crash guard
struct Comp { void *system; uint32_t index; };

// Explosions due, run on the follow update (the game thread, outside any script node: run
// from inside one -- the drone script's own disable node, as the expiry first did -- a
// node's Execute overwrites the running node's per-thread storage, and every client in a
// match crashed when a drone's lifetime ran out). An expiry also holds back the drone's
// sound system going off (`sound`) until its explosion's sound has played.
// nEVR port: the queue and its timing are the shared rule (runtime/patch/dronekill_rules.h FxQueue: dk::kFxSlots slots,
// dk::kFxHeld held components each, dk::kFxSoundHoldMs), which the Quest's dronekill.cpp runs too (#1737).
dk::FxQueue<void *, Comp> g_fx;
SRWLOCK g_fx_lock = SRWLOCK_INIT;

void QueueDeathFx(void *ctx, const Comp *sound, int nsound) {
    AcquireSRWLockExclusive(&g_fx_lock);
    const bool queued = g_fx.Queue(ctx, sound, nsound, GetTickCount64());
    ReleaseSRWLockExclusive(&g_fx_lock);
    if (!queued) {                                                // (8 at once: no explosion, sound off now)
        for (int i = 0; i < nsound; ++i) Real_Disable(sound[i].system, sound[i].index, 1);
        LogWarning("drone explosion queue full: skipped");
    }
}

// fix_drone_fx.py's spawn and sound stubs (their addresses left by the drone script) with a
// dead drone's node context: the detonator's explosion and POINT's sound on that drone. Its
// node state kept round each, as the translocator's dispatcher hook does.
void SpawnDeathFx(void *ctx) {
    using stub_fn = void(__fastcall *)(void *);
    auto slots = (stub_fn volatile *)((uint8_t *)GetModuleHandleW(nullptr) + DEATH_TOKEN_RVA + 0x28);
    if (!slots[0]) { LogWarning("drone explosion: no spawn stub (drone script unpatched?)"); return; }
    EC_TRY {
        auto state = (uint64_t *)((uint8_t *)ctx + 0x18);
        uint64_t saved = *state;
        for (int i = 0; i < 2; ++i)
            if (stub_fn stub = slots[i]) { *state = 0; stub(ctx); }
        *state = saved;
        Log("drone explosion spawned (ctx %p)", ctx);
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        LogWarning("drone explosion faulted and was contained (ctx %p)", ctx);
    }
}

// --- NEUTRON's effects, run here instead of inside its wall's node ---
// tools/veil_fx.py (defer_stub): where FX_DEFER_RVA is set (here, on clients), the NEUTRON
// wall's node queues its charge-up indicator, burst, stun explosion and sound -- {ctx, stub |
// kind} -- instead of running another node's Execute inside its own (what crashed clients).
// Run on the follow update: one per wall node per frame, lowest kind first (charge, burst,
// sound; the stun explosion is left to the server), one sound per wall (both its nodes may
// queue one), the node's own state kept round each, as for the drone's explosion.
constexpr uintptr_t FX_QUEUE_RVA = 0x2104DA0;                    // veil_fx.FX_QUEUE_RVA
constexpr uintptr_t FX_DEFER_RVA = 0x2104E20;                    // veil_fx.FX_DEFER_RVA
// nEVR port: the queue's length and kinds, SoundAgain and the drain's rules are runtime/patch/ecfx_rules.h, the one
// definition the Quest's ecfx.cpp uses too (#2349). The drain runs each entry as it reaches its slot, as EC's WallFxDue does.
namespace fx = nevr_ecfx;
fx::Drain g_wall_drain;                                          // the follow update only

uint64_t WallOf(uint64_t ctx) {                                   // the wall's script data: [[ctx]+68h]; 0 = unreadable
    EC_TRY {
        return *reinterpret_cast<const uint64_t *>(*reinterpret_cast<uint8_t *const *>(ctx) + 0x68);
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void RunWallFx(void *ctx, uintptr_t stub) {
    using stub_fn = void(__fastcall *)(void *);
    EC_TRY {
        auto state = (uint64_t *)((uint8_t *)ctx + 0x18);
        uint64_t saved = *state;
        *state = 0;
        ((stub_fn)stub)(ctx);
        *state = saved;
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        LogWarning("NEUTRON effect faulted and was contained (ctx %p)", ctx);
    }
}

// nEVR port: the drain is fx::Drain::Plan with no exact-entry check (PC resolves no entry address, as EC did not, #2349 D1).
// A word with no entry or a stray flag bit, or a sound whose wall could not be read, is freed and not run (D2, D3; EC ran
// each inside __try). A drain that freed one is a Warning: the first 16 each, then one that says the later ones are counted,
// then the count at most every 10 s, and once no such drain has come for 10 s (nevr::BurstThenCount, core/burst_then_count.h).
SRWLOCK g_wall_lines_lock = SRWLOCK_INIT;
nevr::BurstThenCount g_wall_lines{16, 10000};
uint64_t g_wall_refused_total = 0, g_wall_nowall_total = 0;
std::atomic<bool> g_wall_lines_pending{false};

void ReportWallRefusals(const nevr::BurstThenCount::Line &l, const fx::Counts &c, uint64_t refused, uint64_t nowall) {
    using V = nevr::BurstThenCount::Verdict;
    if (l.verdict == V::kWrite)
        LogWarning("NEUTRON effects freed and not run in one drain: refused=%d (no entry or a stray flag bit) nowall=%d (a sound whose wall could not be read) drain=%llu of the first 16 logged (the wall content wrote a word its defer stub never writes, or the wall is gone)",
                   c.refused, c.nowall, static_cast<unsigned long long>(l.total));
    else if (l.verdict == V::kCapped)
        LogWarning("NEUTRON effects freed and not run: drains=%llu refused=%llu nowall=%llu so far; later drains are counted, not logged, and the count follows at most every 10 s",
                   static_cast<unsigned long long>(l.total), static_cast<unsigned long long>(refused), static_cast<unsigned long long>(nowall));
    else if (l.verdict == V::kCount)
        LogWarning("NEUTRON effects freed and not run: drains=%llu (%llu since the last line, counted, not logged) refused=%llu nowall=%llu in all",
                   static_cast<unsigned long long>(l.total), static_cast<unsigned long long>(l.sinceLine), static_cast<unsigned long long>(refused),
                   static_cast<unsigned long long>(nowall));
}

void WallFxDue() {
    if (g_is_server) return;
    auto q = reinterpret_cast<uint64_t *>(reinterpret_cast<uint8_t *>(GetModuleHandleW(nullptr)) + FX_QUEUE_RVA);
    fx::Env env;
    env.wallOf = &WallOf;
    fx::Counts c;
    const ULONGLONG now = GetTickCount64();
    // The stun explosion is the server's alone: its explosion is the one that stuns (with
    // none there, nobody was). Activated here too, a client it stunned crashed -- only
    // the team the NEUTRON's owner wasn't on ever did. (nEVR port: fx::Drain frees it unrun.)
    g_wall_drain.Plan(q, env, now, [](const fx::Run &r) {
        RunWallFx(reinterpret_cast<void *>(r.ctx), static_cast<uintptr_t>(r.fn));
    }, &c);
    const bool bad = c.refused || c.nowall;
    if (!bad && !g_wall_lines_pending.load(std::memory_order_relaxed)) return;
    AcquireSRWLockExclusive(&g_wall_lines_lock);
    if (bad) {
        g_wall_refused_total += static_cast<uint64_t>(c.refused);
        g_wall_nowall_total += static_cast<uint64_t>(c.nowall);
    }
    const nevr::BurstThenCount::Line l = bad ? g_wall_lines.Event(now) : g_wall_lines.Flush(now);
    g_wall_lines_pending.store(g_wall_lines.Pending(), std::memory_order_relaxed);
    const uint64_t refused = g_wall_refused_total, nowall = g_wall_nowall_total;
    ReleaseSRWLockExclusive(&g_wall_lines_lock);
    ReportWallRefusals(l, c, refused, nowall);
}

void DeathFxDue() {
    void *spawn[dk::kFxSlots];
    Comp off[dk::kFxSlots * dk::kFxHeld];
    int noff = 0;
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_fx_lock);
    const int nspawn = g_fx.Due(now, spawn, dk::kFxSlots, off, dk::kFxSlots * dk::kFxHeld, &noff);   // nEVR port: shared rule
    ReleaseSRWLockExclusive(&g_fx_lock);
    for (int i = 0; i < nspawn; ++i) SpawnDeathFx(spawn[i]);
    for (int i = 0; i < noff; ++i) Real_Disable(off[i].system, off[i].index, 1);
}
// nEVR port: the drone groups and their pending components are the shared table (runtime/patch/dronekill_rules.h, #2359).
// A component's system is keyed by its address (SysKey); PC keeps two things per group: its node context and the test kill.
uint64_t SysKey(const void *p) { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p)); }
void *SysPtr(uint64_t k) { return reinterpret_cast<void *>(static_cast<uintptr_t>(k)); }
struct PcGroup { void *ctx = nullptr; bool test_killed = false; };   // ctx: a node context of its script (its explosion)
dk::Groups<dk::None, PcGroup> g_groups;
SRWLOCK g_drones_lock = SRWLOCK_INIT;
// the gun table: each drone's last gun state by actor symbol, so the port follows it whatever the drone's script does with
// the port's component (the shared table, #2359). Declared here because Shutdown hides it.
dk::GunTable g_guns;
SRWLOCK g_guns_lock = SRWLOCK_INIT;

uint8_t *DamageRecord(uint8_t *cs, uint32_t index) {
    uint16_t slot = *(uint16_t *)(*(uint8_t **)(cs + 0xd0) + (uint64_t)(index & 0xffff) * 4);
    return *(uint8_t **)(cs + 0x100) + slot * 0xb8ull;
}

void Revive(uint8_t *cs, uint32_t index) {                        // health back to full, not dead
    EC_TRY {
        uint8_t *rec = DamageRecord(cs, index);
        uint8_t *cd = **(uint8_t ***)(cs + 0xc0) + *(uint16_t *)rec * 0x38ull;
        *(float *)(rec + 0x20) = *(float *)(cd + 0x24);
        rec[2] &= ~4;
        Log("drone %u deployed: health %.0f", index & 0xffff, *(float *)(rec + 0x20));
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        LogWarning("drone %u deployed: its damageable record is unreadable", index & 0xffff);
    }
}

// how long after its release a dead drone's script can still try to turn its parts on (the
// stock lifetime is 20 s); the next one is out no sooner (cooldown 30 s)
// when a drone last died (0: none yet): an enable that isn't a drone's own goes straight
// through, no lock, unless one did within dk::kSwallowMs (every component enable in the
// game comes through Hooked_EnableComp)
volatile ULONGLONG g_last_death = 0;
volatile ULONGLONG g_last_deploy = 0;                            // when a drone was last released

void __fastcall Hooked_EnableComp(void *system, uint32_t index, uint16_t flag) {
    int what = t_drone_enable;
    uint64_t actor = what ? ActorOf(system, index) : 0;
    if (what) LearnCtx(system, index);
    if (!what) {
        ULONGLONG died = g_last_death;
        if (!died || GetTickCount64() - died > dk::kSwallowMs) { Real_EnableComp(system, index, flag); return; }
    }
    AcquireSRWLockExclusive(&g_drones_lock);
    const ULONGLONG now = GetTickCount64();                       // nEVR port: one clock read per call (#2359)
    bool swallow = false;
    if (what == 2) {                                              // a release: this drone's new life
        const dk::Verdict r = g_groups.Release(SysKey(system), index, actor, 0, now);   // nEVR port: shared rule (#2359)
        g_last_deploy = now;
        // this enable is the drone script's own node: the context the dispatcher just left
        g_groups.g[r.group].x.ctx = t_node ? DroneCtx(t_node) : nullptr;
    } else {
        // dead: the script can't bring it back; else a drone component joins its group or waits (nEVR port: shared rule, #2359)
        swallow = g_groups.Enable(SysKey(system), index, actor, 0, now, what == 1).swallowed;
    }
    ReleaseSRWLockExclusive(&g_drones_lock);
    if (what == 2) Revive((uint8_t *)system, index);
    if (!swallow) Real_EnableComp(system, index, flag);
}

// a touch interact the grab rule held back: part of the newest drone too
void AddTouch(void *system, uint32_t index) {
    uint64_t actor = ActorOf(system, index);
    AcquireSRWLockExclusive(&g_drones_lock);
    const ULONGLONG now = GetTickCount64();
    const int g = g_groups.Newest(actor, now);                    // nEVR port: shared rule (#2359)
    if (g >= 0) g_groups.Add(g, SysKey(system), index, actor, now);
    ReleaseSRWLockExclusive(&g_drones_lock);
}


// Turns the drone whose damageable is (dcs, didx) off; dcs null: the one whose damageable
// index is didx (a client told by the server).
bool Shutdown(uint8_t *dcs, uint32_t didx, const char *why) {
    Comp comps[dk::kGroupComps];
    int n = 0;
    void *ctx = nullptr;
    AcquireSRWLockExclusive(&g_drones_lock);
    int g = -1;
    if (dcs) {
        const dk::DeadMark m = g_groups.MarkDead(SysKey(dcs), didx);   // nEVR port: shared rule (#2359, D11)
        if (m.wasAlive) g = m.group;
    } else {
        g = g_groups.KillIndex(didx);                                  // nEVR port: shared rule (#2359, D9)
    }
    if (g >= 0) {
        const auto &d = g_groups.g[g];
        g_last_death = GetTickCount64();
        n = d.n;
        ctx = CtxOfActor(d.actor);
        if (!ctx) ctx = d.x.ctx;
        for (int i = 0; i < n; ++i) comps[i] = { SysPtr(d.comps[i].system), d.comps[i].handle };
    }
    ReleaseSRWLockExclusive(&g_drones_lock);
    if (!n || !Real_Disable) return g >= 0;                            // (unhooked: VeilNet's 'K' after shutdown)
    AcquireSRWLockExclusive(&g_guns_lock);
    dk::HideGunOnKill(g_guns);                                         // the port's override: hidden (EC 621)
    ReleaseSRWLockExclusive(&g_guns_lock);
    for (int i = 0; i < n; ++i) Real_Disable(comps[i].system, comps[i].index, 1);
    // the explosion, on the next follow update (the game thread). Clients only: a particle
    // spawn on the headless server crashed it.
    if (!g_is_server && ctx) QueueDeathFx(ctx, nullptr, 0);
    Log("drone %u %s: shut down (%d components off)%s", didx & 0xffff, why, n,
        !g_is_server && ctx ? ", explosion" : "");
    return true;
}

// The drone's own end (its lifetime, 20 s): its script turns off its sound system
// (dk::kNcaPhysics) first, then its damageable and the rest, on the script's thread. The sound
// system's disables are held back (t_expiry, Hooked_Disable); the damageable's names the
// drone (ExpiredDamageable): still alive -> its explosion is queued, with the held-back
// sound system to go off after it; already dead (shot down: its explosion was then) -> the
// sound system goes off now. Every client runs the script; the server gets no explosion.
struct Expiry { void *ctx; int nsound; Comp sound[8]; bool open; };
thread_local Expiry t_expiry = {};
thread_local int t_expiry_step = 0;                               // 1: sound system going off, 2: damageable

void ExpiryHold(void *system, uint32_t index) {                   // (Hooked_Disable, t_expiry_step 1)
    if (t_expiry.nsound < 8) t_expiry.sound[t_expiry.nsound++] = { system, index };
    else Real_Disable(system, index, 1);
}

void ExpiredDamageable(void *system, uint32_t index) {            // (Hooked_Disable, t_expiry_step 2)
    bool alive = false;
    ULONGLONG age = 0;
    AcquireSRWLockExclusive(&g_drones_lock);
    const dk::DeadMark m = g_groups.MarkDead(SysKey(system), index);   // nEVR port: shared rule (#2359, D11)
    if (m.group >= 0) {
        alive = m.wasAlive;
        g_last_death = GetTickCount64();
        age = GetTickCount64() - g_groups.g[m.group].opened;
    }
    ReleaseSRWLockExclusive(&g_drones_lock);
    Expiry e = t_expiry;
    t_expiry = {};
    {                                                              // this drone's, kept at its release
        AcquireSRWLockShared(&g_drones_lock);
        const int g = g_groups.Find(SysKey(system), index);
        if (g >= 0 && g_groups.g[g].x.ctx) e.ctx = g_groups.g[g].x.ctx;
        ReleaseSRWLockShared(&g_drones_lock);
    }
    if (dk::OnDamageableOff(alive && !g_is_server, e.open, e.ctx != nullptr) == dk::ExpiryAction::kQueue) {   // nEVR port: shared rule
        QueueDeathFx(e.ctx, e.sound, e.nsound);
        Log("drone %u expired after %.1f s, explosion", index & 0xffff, age / 1000.0);
    } else {
        for (int i = 0; i < e.nsound; ++i) Real_Disable(e.sound[i].system, e.sound[i].index, 1);
        if (alive) Log("drone %u expired after %.1f s", index & 0xffff, age / 1000.0);
    }
}

void __fastcall Hooked_Death(void *cs, uint32_t index) {
    Real_Death(cs, index);
    if (!Shutdown((uint8_t *)cs, index, "destroyed")) return;   // nEVR port: not a live drone (#2359)
    if (g_is_server) {
        using killed_fn = void(__cdecl *)(uint32_t);
        static killed_fn tell = nullptr;
        if (!tell)
            if (::nevr_combat::veilnet::Started())                // nEVR port: was GetProcAddress(VeilNet.dll)
                tell = &::nevr_combat::veilnet::DroneKilled;
        if (tell) tell(index & 0xffff);
        else LogWarning("drone %u: no VeilNet.dll, the clients won't see it go", index & 0xffff);
    }
}

void __cdecl DroneFix_Killed(uint32_t index) {   // VeilNet: the server says
    if (Real_Disable) Shutdown(nullptr, index, "destroyed on the server");
}

void KillTestDue() {                                              // (test) kill as a 0-health hit does
    if (!KILL_TEST_MS || !g_is_server || !Death) return;
    uint8_t *cs = nullptr;
    uint32_t index = 0;
    AcquireSRWLockExclusive(&g_drones_lock);
    for (auto &d : g_groups.g)
        if (d.used && !d.dead && !d.x.test_killed && GetTickCount64() - d.opened >= KILL_TEST_MS) {
            d.x.test_killed = true;
            cs = static_cast<uint8_t *>(SysPtr(d.system));
            index = d.handle;
            break;
        }
    ReleaseSRWLockExclusive(&g_drones_lock);
    if (!cs) return;
    EC_TRY {
        uint8_t *rec = DamageRecord(cs, index);
        if (rec[2] & 4) return;
        *(float *)(rec + 0x20) = 0.0f;
        rec[2] |= 4;
        Log("[test] drone %u: health 0, killing it", index & 0xffff);
        Death(cs, index);
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        LogWarning("[test] killing drone %u faulted", index & 0xffff);
    }
}

void WatchDue();
void GrabKeyDue();
void DroneHitsDue();

// nEVR port: the follow update's fault Warnings (#2349). The first 5 faults are each a Warning, then one that says the later
// ones are counted, then the count at most every 10 s and once none has come for 10 s (nevr::BurstThenCount); his `<= 5` stopped
// without a count. The follow update runs every frame in every level, arena included, so its call sites enter FaultLines only on a
// fault or with a count pending: with neither, the follow update pays one relaxed load and one branch.
std::atomic<bool> g_fault_lines_pending{false};

void ReportFaults(const nevr::BurstThenCount::Line &l, uintptr_t cs) {
    using V = nevr::BurstThenCount::Verdict;
    if (l.verdict == V::kWrite)
        LogWarning("follow update faulted and was contained (cs %p, target system %p, index %u)", (void *)cs,
            *(void **)(cs + TARGET_BODY_SYSTEM), *(uint16_t *)(cs + 0x190));
    else if (l.verdict == V::kCapped)
        LogWarning("follow update faults contained: %llu so far; later ones are counted, not logged, and the count follows at most every 10 s",
                   static_cast<unsigned long long>(l.total));
    else if (l.verdict == V::kCount)
        LogWarning("follow update faults contained: %llu in all (%llu since the last line, counted, not logged)",
                   static_cast<unsigned long long>(l.total), static_cast<unsigned long long>(l.sinceLine));
}

void FaultLines(bool faulted, uintptr_t cs) {
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_fault_lines_lock);
    const nevr::BurstThenCount::Line l = faulted ? g_fault_lines.Event(now) : g_fault_lines.Flush(now);
    g_fault_lines_pending.store(g_fault_lines.Pending(), std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_fault_lines_lock);
    ReportFaults(l, cs);
}

void __fastcall Hooked_FollowUpdate(uintptr_t cs) {
    if (g_drone_on) {                                             // nEVR port: the drone's per-frame work, with DRONE on
    TickFrame();
    WatchDue();
    GrabKeyDue();
    DroneHitsDue();
    KillTestDue();
    DeathFxDue();
    WallFxDue();
    FlushDropped(false);                                          // nEVR port: a pending dropped-line count goes out (#2349)
    }
    if (*(uintptr_t *)(cs + TARGET_BODY_SYSTEM) == 0) {
        if (InterlockedIncrement(&g_skipped) == 1)
            Log("follow update skipped: no orbit target set yet (cs %p)", (void *)cs);
        if (g_fault_lines_pending.load(std::memory_order_relaxed)) FaultLines(false, cs);  // nEVR port: a pending fault count still goes out
        return;
    }
    const bool faulted = !CallGuarded(cs);
    if (faulted) InterlockedIncrement(&g_faults);
    if (faulted || g_fault_lines_pending.load(std::memory_order_relaxed)) FaultLines(faulted, cs);
}

// Each drone's last gun state (by actor), so the port follows it whatever the drone's
// script does with the port's component: that script is the detonator's, and shows / hides
// its parts on its own (the part PORT is on is one it shows once the drone is deployed).
void SetGun(uint64_t actor, uint32_t shown) {
    AcquireSRWLockExclusive(&g_guns_lock);
    g_guns.Set(actor, shown);                                     // nEVR port: the shared table (#2359, D3, D4)
    ReleaseSRWLockExclusive(&g_guns_lock);
}

bool GunShown(uint64_t actor, uint32_t *shown) {
    AcquireSRWLockShared(&g_guns_lock);
    const bool found = g_guns.Shown(actor, shown);
    ReleaseSRWLockShared(&g_guns_lock);
    return found;
}

// --- the drone stays grabbable until it locks on ------------------------------------------
// A moment after release the drone's script turns its R14TouchInteract component off, which
// is what stops it being grabbed. CEnableComponentNode::Enter resolves the component and asks
// for that through DISABLE_RVA (component system, component index); while capturing (the
// drone's touch interact, from the hook below) that call is held back and the component kept,
// with this drone's record reset (a new life). At the drone's first shot, the drone that has
// not fired yet and was deployed last is the one shooting: its touch interact is turned off
// then, so a drone that has locked onto someone can't be grabbed. The next release starts again.
constexpr uintptr_t DISABLE_RVA = 0x9cb780;                      // (component system, index, flag): disable
constexpr uint8_t DISABLE_BYTES[] = { 0x48, 0x83, 0xec, 0x78, 0x48, 0x8b, 0xc1 };
thread_local bool t_capture = false;

struct TouchRec { void *system; uint32_t index; uint32_t handle; ULONGLONG deployed; bool locked; };
TouchRec g_touch[16];
SRWLOCK g_touch_lock = SRWLOCK_INIT;

void ExpiryHold(void *system, uint32_t index);
void ExpiredDamageable(void *system, uint32_t index);

void __fastcall Hooked_Disable(void *system, uint32_t index, uint16_t flag) {
    if (t_node) LearnCtx(system, index);
    if (t_expiry_step == 1 && !g_is_server) { ExpiryHold(system, index); return; }
    if (t_expiry_step == 2) {
        Real_Disable(system, index, flag);
        ExpiredDamageable(system, index);
        return;
    }
    if (!t_capture) {
        Real_Disable(system, index, flag);
        return;
    }
    AcquireSRWLockExclusive(&g_touch_lock);
    int slot = -1, oldest = 0;
    for (int i = 0; i < 16 && slot < 0; ++i)
        if (g_touch[i].system == system && g_touch[i].index == index) slot = i;
    for (int i = 0; i < 16 && slot < 0; ++i)
        if (!g_touch[i].system) slot = i;
    for (int i = 1; i < 16; ++i)
        if (g_touch[i].deployed < g_touch[oldest].deployed) oldest = i;
    if (slot < 0) slot = oldest;
    g_touch[slot] = { system, index, 0, GetTickCount64(), false };   // deployed, not locked on
    ReleaseSRWLockExclusive(&g_touch_lock);
    AddTouch(system, index);                                 // goes when the drone does
    Log("drone deployed: stays grabbable until it locks on (touch interact %p #%u)", system, index);
}

// The touch interact is still part of a drone that is alive. A record outlives its drone (an
// expired or destroyed drone's is never cleared, and a new drone's first shot can pick it up),
// and disabling a component the game has since freed crashed clients (echovr+0x198b2d reads
// through a stale component, null +0x388, from DISABLE_RVA).
bool TouchLive(void *system, uint32_t index) {
    AcquireSRWLockExclusive(&g_drones_lock);
    const bool live = g_groups.LiveComp(SysKey(system), index);   // nEVR port: shared rule (#2359)
    ReleaseSRWLockExclusive(&g_drones_lock);
    return live;
}

bool SafeDisable(void *system, uint32_t index) {
    EC_TRY {
        Real_Disable(system, index, 1);
        return true;
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// At a drone's shot: the first one locks that drone on, and turns its touch interact off.
void LockedOn(uint32_t handle) {
    if (!Real_Disable) return;
    void *system = nullptr;
    uint32_t index = 0;
    AcquireSRWLockExclusive(&g_touch_lock);
    int mine = -1, newest = -1;
    for (int i = 0; i < 16; ++i) {
        if (!g_touch[i].system) continue;
        if (g_touch[i].handle == handle) mine = i;
        else if (!g_touch[i].handle && !g_touch[i].locked
                 && GetTickCount64() - g_touch[i].deployed < 120000
                 && (newest < 0 || g_touch[i].deployed > g_touch[newest].deployed)) newest = i;
    }
    if (mine < 0 && newest >= 0) {
        mine = newest;
        g_touch[mine].handle = handle;
    }
    if (mine >= 0 && !g_touch[mine].locked) {
        g_touch[mine].locked = true;
        system = g_touch[mine].system;
        index = g_touch[mine].index;
    }
    ReleaseSRWLockExclusive(&g_touch_lock);
    if (system && !TouchLive(system, index)) {
        Log("drone %u locked on: its touch interact %p #%u belongs to no live drone, left alone", handle, system, index);
    } else if (system) {
        bool ok = SafeDisable(system, index);
        if (ok) Log("drone %u locked on: no longer grabbable (touch interact %p #%u)", handle, system, index);
        else LogWarning("drone %u locked on: disabling touch interact %p #%u faulted, contained", handle, system, index);
    }
}

// For Hooked_EnableComp: what the drone's script is turning on in this call (restored after).
struct EnableTag {
    int saved;
    explicit EnableTag(int v) : saved(t_drone_enable) { t_drone_enable = v; }
    ~EnableTag() { t_drone_enable = saved; }
};

#ifdef ECHOCOMBAT_LOG
volatile LONG g_dbg_enables = 0;
#endif

struct NodeTag {
    void *saved;
    explicit NodeTag(void *n) : saved(t_node) { t_node = n; }
    ~NodeTag() { t_node = saved; }
};

void __fastcall Hooked_EnableEnter(void *node, const uint64_t *data) {
    NodeTag node_tag(data[0] == dk::kDroneActor ? node : t_node);
    if (data[0] == dk::kDroneActor) DroneCtx(node, true);       // each drone node: learn its drone's context
#ifdef ECHOCOMBAT_LOG
    if (data[0] == dk::kDroneActor && InterlockedIncrement(&g_dbg_enables) <= 400)
        LogDebug("[test] drone script: %s %016llx/%016llx", (uint32_t)data[4] ? "on " : "off", data[2], data[3]);
#endif
    if (data[0] == dk::kDroneActor && !(uint32_t)data[4] && Real_EnableComp && Real_Disable
        && (data[2] == dk::kNcaPhysics || data[2] == dk::kDamageable)) {
        if (data[2] == dk::kNcaPhysics) {                       // the end begins: this node's context
            for (int i = 0; i < t_expiry.nsound; ++i)        // (an unfinished one before: let it go)
                Real_Disable(t_expiry.sound[i].system, t_expiry.sound[i].index, 1);
            t_expiry = {};
            t_expiry.open = true;
            t_expiry.ctx = DroneCtx(node);
        }
        t_expiry_step = data[2] == dk::kNcaPhysics ? 1 : 2;
        Real_EnableEnter(node, data);
        t_expiry_step = 0;
        return;
    }
    EnableTag tag(data[0] == dk::kDroneActor && (uint32_t)data[4] ? (data[2] == dk::kDamageable ? 2 : 1) : 0);
    if (data[0] == dk::kDroneActor && data[2] == dk::kTouchInteract && !(uint32_t)data[4] && Real_Disable) {
        t_capture = true;                                   // the engine resolves it; Hooked_Disable keeps it
        Real_EnableEnter(node, data);
        t_capture = false;
        return;
    }
    if (data[2] == dk::kNcaModel && data[3] == dk::kPort) {          // the script's own say: the gun's instead
        uint32_t shown;
        if (GunShown(data[0], &shown) && shown != (uint32_t)data[4]) {
            uint64_t copy[5];
            memcpy(copy, data, sizeof copy);
            copy[4] = shown;
            Real_EnableEnter(node, copy);
            return;
        }
    }
    Real_EnableEnter(node, data);
    if (data[2] == dk::kNcaModel && data[3] == dk::kGun) {
        SetGun(data[0], (uint32_t)data[4]);
        uint64_t copy[5];                                   // actor, scope, system, component, enable
        memcpy(copy, data, sizeof copy);
        copy[3] = dk::kSatellite;
        Real_EnableEnter(node, copy);
        copy[3] = dk::kPort;
        Real_EnableEnter(node, copy);
        if (InterlockedIncrement(&g_mirrored) <= 5)
            LogDebug("drone gun %s: body and port too", (uint32_t)data[4] ? "shown" : "hidden");
    }
}

void __fastcall Hooked_FireLocal(void *cs, uint32_t handle, const float *velocity, int remote) {
    g_at_cs = cs;
    NoteAtHandle(handle);
    LockedOn(handle);
    t_velocity = velocity;
    t_remote = remote;
    Real_FireLocal(cs, handle, velocity, remote);
    t_velocity = nullptr;
}

Pose *__fastcall MuzzleTransform(void *transforms, Pose *out, uint32_t index) {
    Real_TransformGet(transforms, out, index);
    const float *v = t_velocity;
    if (!v) return out;
    // nEVR port: AttackTarget's basis (Z = aim, X = (z, 0, -x) / |..|, Y = Z x X), the clamped frame and the
    // BACK_MAX cap (at 30 fps a frame is 4 m, which put the start inside the wall behind a drone backed into a
    // corner, and through the drone itself) are dm::MuzzleMove. A velocity below 1e-4 or NaN leaves the pose (#2337).
    const float root[3] = { out->p[0], out->p[1], out->p[2] };
    const float frame = g_frame_dt;
    if (!dm::MuzzleMove(root, v, frame, out->p)) return out;
    const float dt = dm::ClampDt(frame);                        // the frame time the move used, for the line below
    LONG n = InterlockedIncrement(&g_muzzle_shots);
    if (n <= SHOT_LOG)
        LogDebug("[pid %lu] shot %ld%s: root (%.3f %.3f %.3f) spawn (%.3f %.3f %.3f) velocity (%.2f %.2f %.2f) "
            "frame %.1f ms", GetCurrentProcessId(), n, t_remote ? " (remote)" : "", root[0], root[1], root[2],
            out->p[0], out->p[1], out->p[2], v[0], v[1], v[2], dt * 1000.0f);
    return out;
}

// --- each drone bullet's tracer starts at the drone -----------------------------------------
// The bullet system's spawn (BULLET_SPAWN_RVA, from FireBulletLocal) is a player gun's: it
// stores the shooter's velocity (+0x11c) and position (+0x128) in the bullet's record, read
// from the system's own player, and the tracer effect is drawn from that position
// (+0xcd90a0 hands both to it). A drone bullet got some player's instead of the drone's, so
// a second streak ran off to another spot (0,0,0). For a drone shot (t_velocity set) the
// record gets the spawn point and no shooter velocity.
constexpr uintptr_t BULLET_SPAWN_RVA = 0xce2020;
constexpr uint8_t BULLET_SPAWN_BYTES[] = { 0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
using bullet_spawn_fn = void(__fastcall *)(uint8_t *, uint64_t, void *, float, const Pose *, const float *, int, uint32_t);
bullet_spawn_fn Real_BulletSpawn = nullptr;
volatile LONG g_tracers = 0;

// (logging builds) the first WATCH_TOTAL drone bullets, logged each frame for 1.5 s from the
// follow update: where each one goes and what state it is in (record +0xb4 flags: 8 hit,
// 0x40 hit a damageable; +0xd0 metres to the hit the spawn raycast found; +0x104 direction)
#ifdef ECHOCOMBAT_LOG
constexpr int WATCH_TOTAL = 10;
struct Watch { uint8_t *sys; uint32_t index; ULONGLONG since; int n; float last[3]; };
Watch g_watch[4];
int g_watched = 0;
SRWLOCK g_watch_lock = SRWLOCK_INIT;

void WatchBullet(uint8_t *sys, uint32_t index) {
    AcquireSRWLockExclusive(&g_watch_lock);
    if (g_watched < WATCH_TOTAL)
        for (auto &w : g_watch)
            if (!w.sys) { w = { sys, index, GetTickCount64(), g_watched++, {} }; break; }
    ReleaseSRWLockExclusive(&g_watch_lock);
}

void WatchDue() {
    if (!g_watched || !Real_TransformGet) return;
    AcquireSRWLockExclusive(&g_watch_lock);
    for (auto &w : g_watch) {
        if (!w.sys) continue;
        ULONGLONG age = GetTickCount64() - w.since;
        EC_TRY {
            uint16_t slot = *(uint16_t *)(*(uint8_t **)(w.sys + 0xc8) + (w.index & 0xffff) * 4);
            uint8_t *rec = *(uint8_t **)(w.sys + 0xf8) + (size_t)slot * 0x298;
            Pose p;
            Real_TransformGet(*(void **)(rec + 0x30), &p, *(uint32_t *)(rec + 0x38));
            const float *dir = (const float *)(rec + 0x104), *sp = (const float *)(rec + 0x128);
            LogDebug("  bullet %d (#%u slot %u) %llu ms: pos (%.2f %.2f %.2f) flags %x to-hit %.2f dir (%.2f %.2f %.2f) "
                "hits %llu shooter (%.2f %.2f %.2f)", w.n, w.index & 0xffff, slot, age, p.p[0], p.p[1], p.p[2],
                *(uint32_t *)(rec + 0xb4), *(float *)(rec + 0xd0), dir[0], dir[1], dir[2],
                *(unsigned long long *)(rec + 0xd8), sp[0], sp[1], sp[2]);
        } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
            LogDebug("  bullet %d: record gone", w.n);
            w.sys = nullptr;
        }
        if (age > 1500) w.sys = nullptr;
    }
    ReleaseSRWLockExclusive(&g_watch_lock);
}
#else
void WatchBullet(uint8_t *, uint32_t) {}
void WatchDue() {}
#endif

void __fastcall Hooked_BulletSpawn(uint8_t *sys, uint64_t index, void *ref, float speed, const Pose *pose,
                                   const float *velocity, int remote, uint32_t flags) {
    Real_BulletSpawn(sys, index, ref, speed, pose, velocity, remote, flags);
    g_bullet_cs = sys;
    if (!t_velocity || !pose) return;
    EC_TRY {
        uint16_t slot = *(uint16_t *)(*(uint8_t **)(sys + 0xc8) + (index & 0xffff) * 4);
        float *rec = (float *)(*(uint8_t **)(sys + 0xf8) + (size_t)slot * 0x298);
        float *shooter_vel = (float *)((uint8_t *)rec + 0x11c), *shooter_pos = (float *)((uint8_t *)rec + 0x128);
        for (int i = 0; i < 3; ++i) { shooter_vel[i] = 0.0f; shooter_pos[i] = pose->p[i]; }
        NoteDroneShot((uint8_t *)rec);
        if (InterlockedIncrement(&g_tracers) <= 3)
            LogDebug("drone bullet tracer starts at the drone (%.2f %.2f %.2f)", pose->p[0], pose->p[1], pose->p[2]);
        WatchBullet(sys, (uint32_t)index);
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
    }
}

// --- the drone chases the enemy it is shooting ---------------------------------------------
// CR15NetAutoTargetCS's update runs, per drone, FindTarget (+0xd89070: the nearest enemy in
// max_target_distance it can see; record +0x63 set, target at (+0x88 system, +0x80 index))
// then AttackTarget (ATTACK_RVA), which turns the drone at it, zeroes the drone body's spin
// and fires. Nothing ever moves the drone. After AttackTarget, a drone with a target gets a
// velocity toward it, through the same deferred physics queue (so after AttackTarget's own
// calls): CHASE_SPEED at most, easing to a stop CHASE_DISTANCE from the target, a little
// above it. When it loses its target it stops where it is.
//   autotarget record (stride 0x90, cs+0xf8 / index table cs+0xc8): +0x63 has target,
//   +0x7c transform, +0x80 target index, +0x84 physics index, +0x88 target system
//   cs+0x130 transforms, cs+0x138 physics (deferred queue: [physics+0x80]+0x170)
constexpr uintptr_t ATTACK_RVA = 0xcaf9b0;
constexpr uint8_t ATTACK_BYTES[] = { 0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
constexpr uintptr_t TARGET_POS_RVA = 0xd5fe40;               // (target system, &pose, index): its pose
constexpr uintptr_t DEFER_RVA = 0x198c40;                    // CDeferredMethodQueue (queue, sys, fn, index, &vec3)
constexpr uintptr_t SET_VELOCITY_RVA = 0x469cb0;             // CPhysicsCS (sys, index, &world velocity)
using attack_fn = void(__fastcall *)(uint8_t *, uint32_t);
attack_fn Real_Attack = nullptr;
using target_pos_fn = Pose *(__fastcall *)(void *, Pose *, uint32_t);
target_pos_fn TargetPos = nullptr;
using defer_fn = void(__fastcall *)(uintptr_t, void *, void *, uint32_t, const float *);
defer_fn Defer = nullptr;
void *g_set_velocity = nullptr;
struct Chase { uint8_t *cs; uint16_t index; bool moving; };
Chase g_chase[16];                                            // (the auto-target update's thread)
volatile LONG g_chases = 0;

void SetDroneVelocity(uint8_t *cs, uint8_t *rec, const float *v) {
    auto physics = *(uint8_t **)(cs + 0x138);
    Defer(*(uintptr_t *)(physics + 0x80) + 0x170, physics, g_set_velocity, *(uint32_t *)(rec + 0x84), v);
}

void NoteDronePos(uint8_t *cs, uint16_t index, const float *p);

void ChaseTarget(uint8_t *cs, uint32_t handle) {
    uint16_t slot = *(uint16_t *)(*(uint8_t **)(cs + 0xc8) + (handle & 0xffff) * 4);
    uint8_t *rec = *(uint8_t **)(cs + 0xf8) + (size_t)slot * 0x90;
    Chase *c = nullptr;
    for (auto &e : g_chase)
        if (e.cs == cs && e.index == (uint16_t)handle) { c = &e; break; }
    bool target = rec[0x63] != 0;
    Pose me;
    Real_TransformGet(*(void **)(cs + 0x130), &me, *(uint32_t *)(rec + 0x7c));
    NoteDronePos(cs, (uint16_t)handle, me.p);
    if (!target) {
        if (c && c->moving) {
            const float zero[3] = {};
            SetDroneVelocity(cs, rec, zero);
            c->moving = false;
        }
        return;
    }
    if (!c) {
        for (auto &e : g_chase)
            if (!e.cs || !e.moving) { c = &e; break; }
        if (!c) return;
        *c = { cs, (uint16_t)handle, false };
    }
    Pose them;
    TargetPos(*(void **)(rec + 0x88), &them, *(uint16_t *)(rec + 0x80));
    float v[3], dist = 0.0f, speed = 0.0f;
    if (!dk::ChaseVelocity(me.p, them.p, v, &dist, &speed)) return;   // nEVR port: shared rule (#2359)
    SetDroneVelocity(cs, rec, v);
    c->moving = true;
    if (InterlockedIncrement(&g_chases) <= 5)
        LogDebug("drone %u chasing its target: %.1f m away, %.1f m/s", handle & 0xffff, dist, speed);
}

void __fastcall Hooked_Attack(uint8_t *cs, uint32_t handle) {
    g_at_cs = cs;
    Real_Attack(cs, handle);
    EC_TRY {
        ChaseTarget(cs, handle);
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
    }
}

// (logging builds) can the drone be shot? Every bullet impact within HIT_LOG_RANGE of a drone
// (positions from AttackTarget, every frame) and every weapon hit on a drone's damageable.
//   +0xceab90  bullet impact (bullet system, bullet, &hit damageable handle: +40 system, +48 index)
//   +0xceb2d0  ApplyWeaponDamageToTarget (damageable cs, index, ...): a bullet's hit on it
#ifdef ECHOCOMBAT_LOG
constexpr uintptr_t IMPACT_RVA = 0xceab90;
constexpr uint8_t IMPACT_BYTES[] = { 0x40, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
constexpr uintptr_t WEAPON_HIT_RVA = 0xceb2d0;
constexpr uint8_t WEAPON_HIT_BYTES[] = { 0x4c, 0x89, 0x44, 0x24, 0x18, 0x55, 0x53, 0x56, 0x41, 0x54 };
constexpr float HIT_LOG_RANGE = 3.0f;
using impact_fn = void(__fastcall *)(uint8_t *, uint32_t, uint8_t *);
using weapon_hit_fn = void(__fastcall *)(uint8_t *, uint32_t, void *, void *, void *, void *);
impact_fn Real_Impact = nullptr;
weapon_hit_fn Real_WeaponHit = nullptr;
struct DronePos { uint8_t *cs; uint16_t index; ULONGLONG at; float p[3]; };
DronePos g_drone_pos[8];
volatile LONG g_impact_logs = 0, g_hit_logs = 0;

void NoteDronePos(uint8_t *cs, uint16_t index, const float *p) {
    DronePos *d = nullptr;
    for (auto &e : g_drone_pos) if (e.cs == cs && e.index == index) { d = &e; break; }
    if (!d) for (auto &e : g_drone_pos) if (!d || e.at < d->at) d = &e;
    *d = { cs, index, GetTickCount64(), { p[0], p[1], p[2] } };
}

void __fastcall Hooked_Impact(uint8_t *sys, uint32_t index, uint8_t *hit) {
    EC_TRY {
        uint16_t slot = *(uint16_t *)(*(uint8_t **)(sys + 0xc8) + (index & 0xffff) * 4);
        uint8_t *rec = *(uint8_t **)(sys + 0xf8) + (size_t)slot * 0x298;
        Pose p;
        Real_TransformGet(*(void **)(rec + 0x30), &p, *(uint32_t *)(rec + 0x38));
        ULONGLONG now = GetTickCount64();
        for (auto &d : g_drone_pos) {
            if (!d.cs || now - d.at > 500) continue;
            float dx = p.p[0] - d.p[0], dy = p.p[1] - d.p[1], dz = p.p[2] - d.p[2];
            float dist = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist < HIT_LOG_RANGE && InterlockedIncrement(&g_impact_logs) <= 60)
                LogDebug("bullet impact %.2f m from drone %u: at (%.2f %.2f %.2f), hit damageable %p #%u, bullet flags %x",
                    dist, d.index, p.p[0], p.p[1], p.p[2], hit ? *(void **)(hit + 0x40) : nullptr,
                    hit ? *(uint16_t *)(hit + 0x48) : 0xffff, *(uint32_t *)(rec + 0xb4));
        }
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
    }
    Real_Impact(sys, index, hit);
}

void __fastcall Hooked_WeaponHit(uint8_t *cs, uint32_t index, void *a, void *b, void *c, void *d) {
    bool drone = false;
    AcquireSRWLockShared(&g_drones_lock);
    for (auto &g : g_groups.g) if (g.used && g.system == SysKey(cs) && (g.handle & 0xffff) == (index & 0xffff)) drone = true;
    ReleaseSRWLockShared(&g_drones_lock);
    EC_TRY {
        float hp = *(float *)(DamageRecord(cs, index) + 0x20);
        // the drone, or another ordnance (they all sit near 9999: how much an arc mine takes)
        if ((drone || hp > 9000.0f) && InterlockedIncrement(&g_hit_logs) <= 120)
            LogDebug("weapon hit on %s %u: health %.1f", drone ? "drone" : "ordnance", index & 0xffff, hp);
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
    }
    Real_WeaponHit(cs, index, a, b, c, d);
}

void InstallHitLog(uintptr_t base) {
    auto impact = (uint8_t *)(base + IMPACT_RVA), hit = (uint8_t *)(base + WEAPON_HIT_RVA);
    if (memcmp(impact, IMPACT_BYTES, sizeof IMPACT_BYTES) || memcmp(hit, WEAPON_HIT_BYTES, sizeof WEAPON_HIT_BYTES)) {
        LogWarning("bullet impact / weapon hit prologue mismatch: no hit log");
        return;
    }
    Real_Impact = (impact_fn)impact;
    Real_WeaponHit = (weapon_hit_fn)hit;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)Real_Impact, Hooked_Impact);
    DetourAttach(&(PVOID &)Real_WeaponHit, Hooked_WeaponHit);
    if (DetourTransactionCommit() == NO_ERROR) Log("[test] logging bullet impacts near drones and hits on them");
    else LogWarning("[test] hit log hooks failed");
}
#else
void NoteDronePos(uint8_t *, uint16_t, const float *) {}
void InstallHitLog(uintptr_t) {}
#endif

// --- (logging builds) Shift+F6: take the selected ordnance out of the pouch --------------
// For testing from a windowed client, which has no hands to reach the pouch with. The pouch
// update (POUCH_UPDATE_RVA, local players' pouches only) asks per hand: in the pouch's grab
// zone (IN_ZONE_RVA), not blocked (BLOCKED_RVA), grip (GRIP_RVA: only this update reads it)
// going from released to pressed -> it asks the server for the ordnance in that hand. The
// key makes that true for the right hand over two pouch updates (grip released, then
// pressed); the windowed grip is never held, so the ordnance drops at once and deploys.
#ifdef ECHOCOMBAT_LOG
constexpr bool GRAB_KEY = false;                               // off: true to test from a windowed client again
constexpr uintptr_t POUCH_UPDATE_RVA = 0xd66e40, IN_ZONE_RVA = 0xd08e80, BLOCKED_RVA = 0xd09920, GRIP_RVA = 0x73fc10;
constexpr uint8_t POUCH_UPDATE_BYTES[] = { 0x4c, 0x8b, 0xdc, 0x55, 0x56, 0x41, 0x56, 0x49, 0x8d, 0xab };
constexpr uint8_t IN_ZONE_BYTES[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x68, 0x10 };
constexpr uint8_t BLOCKED_BYTES[] = { 0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18 };
constexpr uint8_t GRIP_BYTES[] = { 0x48, 0x8b, 0x81, 0xc8, 0x00, 0x00, 0x00, 0x44, 0x0f, 0xb7, 0xca };
constexpr uint32_t GRAB_HAND = 1;                              // the right hand
constexpr uintptr_t NO_HANDS_RVA = 0x20a0478;
constexpr uint32_t NO_HANDS_BIT = 0x100000;
using pouch_update_fn = void(__fastcall *)(void *);
using hand_check_fn = int(__fastcall *)(void *, uint32_t, uint32_t);
using grip_fn = float(__fastcall *)(void *, uint32_t, uint32_t);
pouch_update_fn Real_PouchUpdate = nullptr;
hand_check_fn Real_InZone = nullptr, Real_Blocked = nullptr;
grip_fn Real_Grip = nullptr;
volatile LONG g_grab_stage = 0;                                // 1: grip released, 2: pressed
thread_local bool t_in_pouch = false;
volatile LONG g_grab_zone = 0, g_grab_blocked = 0, g_grab_grip = 0, g_grab_pouches = 0;

bool OurWindowFocused() {                                      // the key is the focused game's only
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void GrabKeyDue() {                                            // (once a frame)
    static bool was = false;
    bool down = (GetAsyncKeyState(VK_F6) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000) && OurWindowFocused();
    if (down && !was && Real_PouchUpdate && InterlockedCompareExchange(&g_grab_stage, 1, 0) == 0) {
        g_grab_zone = g_grab_blocked = g_grab_grip = g_grab_pouches = 0;
        LogDebug("[test] [pid %lu] Shift+F6: taking the ordnance out of the pouch (right hand)", GetCurrentProcessId());
    }
    was = down;
}

int __fastcall Hooked_InZone(void *cs, uint32_t index, uint32_t hand) {
    if (t_in_pouch && g_grab_stage && hand == GRAB_HAND) {
        if (InterlockedIncrement(&g_grab_zone) == 1) {
            EC_TRY {                                            // the pouch record: +10 magazine, +70 flags
                uint8_t *rec = *(uint8_t **)((uint8_t *)cs + 0x100)
                               + *(uint16_t *)(*(uint8_t **)((uint8_t *)cs + 0xd0) + (index & 0xffff) * 4) * 0x180ull;
                LogDebug("[test] pouch %u: +10 %llx, flags +70 %x, magazine system %p", index & 0xffff,
                    *(unsigned long long *)(rec + 0x10), *(uint32_t *)(rec + 0x70), *(void **)((uint8_t *)cs + 0x188));
            } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        return 1;
    }
    return Real_InZone(cs, index, hand);
}

int __fastcall Hooked_Blocked(void *cs, uint32_t index, uint32_t hand) {
    if (t_in_pouch && g_grab_stage && hand == GRAB_HAND) { InterlockedIncrement(&g_grab_blocked); return 0; }
    return Real_Blocked(cs, index, hand);
}

float __fastcall Hooked_Grip(void *body, uint32_t index, uint32_t hand) {
    if (t_in_pouch && g_grab_stage && hand == GRAB_HAND) {
        InterlockedIncrement(&g_grab_grip);
        return g_grab_stage == 2 ? 1.0f : 0.0f;
    }
    return Real_Grip(body, index, hand);
}

void __fastcall Hooked_PouchUpdate(void *cs) {
    t_in_pouch = true;
    if (g_grab_stage) {
        InterlockedIncrement(&g_grab_pouches);
        if (g_grab_pouches == 1)
            EC_TRY {
                auto c = (uint8_t *)cs;
                uint8_t *rec = *(uint8_t **)(c + 0x100);                     // the first pouch
                uint8_t *net = *(uint8_t **)(c + 0xb8);
                uint8_t *g = *(uint8_t **)((uint8_t *)GetModuleHandleW(nullptr) + NO_HANDS_RVA);
                LogDebug("[test] pouch update: %u pouches; first: flags +70 %x (10 = ours), +10 %llx; net state %llx "
                    "(bit 4 skips); global +7ae0 %x (bit 0x100000 skips)", *(uint16_t *)(c + 0xfc),
                    *(uint32_t *)(rec + 0x70), *(unsigned long long *)(rec + 0x10),
                    **(unsigned long long **)(net + 0x2da0), g ? *(uint32_t *)(g + 0x7ae0) : 0xffffffffu);
            } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
                LogDebug("[test] pouch update: unreadable");
            }
    }
    // a windowed client sets bit 0x100000 of [NO_HANDS_RVA]+7ae0 (no headset?) and the pouch
    // update then skips every hand: off for the two forced updates, back after
    uint32_t *flags = nullptr, saved = 0;
    if (g_grab_stage)
        EC_TRY {
            uint8_t *g = *(uint8_t **)((uint8_t *)GetModuleHandleW(nullptr) + NO_HANDS_RVA);
            if (g && (*(uint32_t *)(g + 0x7ae0) & NO_HANDS_BIT)) {
                flags = (uint32_t *)(g + 0x7ae0);
                saved = *flags;
                *flags = saved & ~NO_HANDS_BIT;
            }
        } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
            flags = nullptr;
        }
    Real_PouchUpdate(cs);
    if (flags) *flags = (*flags & ~NO_HANDS_BIT) | (saved & NO_HANDS_BIT);
    t_in_pouch = false;
    LONG stage = g_grab_stage;
    if (stage == 1) InterlockedExchange(&g_grab_stage, 2);
    else if (stage == 2) {
        InterlockedExchange(&g_grab_stage, 0);
        LogDebug("[test] Shift+F6 done: pouch updates %ld, in-zone %ld, blocked %ld, grip %ld (0 = that check never ran)",
            g_grab_pouches, g_grab_zone, g_grab_blocked, g_grab_grip);
    }
}

void InstallGrabKey(uintptr_t base) {
    if (!GRAB_KEY || g_is_server) return;
    auto pu = (uint8_t *)(base + POUCH_UPDATE_RVA), iz = (uint8_t *)(base + IN_ZONE_RVA),
         bl = (uint8_t *)(base + BLOCKED_RVA), gr = (uint8_t *)(base + GRIP_RVA);
    if (memcmp(pu, POUCH_UPDATE_BYTES, sizeof POUCH_UPDATE_BYTES) || memcmp(iz, IN_ZONE_BYTES, sizeof IN_ZONE_BYTES)
        || memcmp(bl, BLOCKED_BYTES, sizeof BLOCKED_BYTES) || memcmp(gr, GRIP_BYTES, sizeof GRIP_BYTES)) {
        LogWarning("[test] pouch prologue mismatch: no Shift+F6 grab");
        return;
    }
    Real_PouchUpdate = (pouch_update_fn)pu;
    Real_InZone = (hand_check_fn)iz;
    Real_Blocked = (hand_check_fn)bl;
    Real_Grip = (grip_fn)gr;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)Real_PouchUpdate, Hooked_PouchUpdate);
    DetourAttach(&(PVOID &)Real_InZone, Hooked_InZone);
    DetourAttach(&(PVOID &)Real_Blocked, Hooked_Blocked);
    DetourAttach(&(PVOID &)Real_Grip, Hooked_Grip);
    if (DetourTransactionCommit() == NO_ERROR) Log("[test] Shift+F6 takes the selected ordnance out of the pouch");
    else { Real_PouchUpdate = nullptr; LogWarning("[test] pouch hooks failed: no Shift+F6 grab"); }
}
#else
void GrabKeyDue() {}
void InstallGrabKey(uintptr_t) {}
#endif

// --- the drone's hitbox: a sphere round the CubeSat, tested on the server ---------------
// The drone's only collision is the ordnance shell's: a flat 19 cm disc at its origin, where
// the sidearm is, so the CubeSat beside it (fix_drone_model.py BODY_OFFSET) could not be
// shot. On the server, every frame, each bullet's path since the last frame is tested
// against a sphere (HIT_RADIUS) round the body's centre (HIT_CENTRE, turned with the drone);
// one of another team that crosses it takes its damage off the drone's health, and at 0 the
// drone dies as a 0-health hit kills it (Hooked_Death: shut down, explosion on the clients).
//   autotarget cs (from AttackTarget / FireBulletLocal): enabled list +0xd8 (u16 slots,
//     count +0xf4), handle table +0xc8, records +0xf8 (0x90), transforms +0x130
//   bullet cs (from the bullet spawn): records +0xf8 (0x298, dense, count +0xf4, slots +0xd8),
//     +0x20/+0x28 its team component, +0x30/+0x38 transform, +0xb4 flags (8: hit),
//     +0xc8 spawn time, +0x134 start point
//   team component system: records +0x100 (0xc: +4 flags, bit 1 team 0, bit 2 team 1)
constexpr float HIT_CENTRE[3] = { 0.064f, -0.024f, 0.039f };    // the CubeSat's middle, drone space
constexpr float HIT_RADIUS = 0.18f;                             // covers its 18 x 21 x 18 cm
constexpr uintptr_t BULLET_DAMAGE_RVA = 0xcc02f0;               // (bullet cs, handle) -> its damage
using bullet_damage_fn = float(__fastcall *)(void *, uint32_t);
bullet_damage_fn BulletDamage = nullptr;
struct BulletSeen { float spawned; float p[3]; bool done; bool drone; };
BulletSeen g_seen[1024];                                        // by dense record index
struct DroneShot { void *rec; float spawned; };
DroneShot g_drone_shots[64];                                    // the drones' own bullets
LONG g_drone_shot_next = 0;
volatile LONG g_sphere_hits = 0;
#ifdef ECHOCOMBAT_LOG
struct HitDiag { ULONGLONG at; int drones, matched, bullets, fresh, checked; float closest; float c[3]; } g_hd;
#endif

void NoteDroneShot(uint8_t *bullet_rec) {                       // (the bullet spawn, drone shots)
    LONG i = InterlockedIncrement(&g_drone_shot_next) % 64;
    g_drone_shots[i] = { bullet_rec, *(float *)(bullet_rec + 0xc8) };
}

uint32_t TeamFlags(uint8_t *team_sys, uint32_t index) {         // 0: unknown
    if (!team_sys) return 0;
    uint16_t slot = *(uint16_t *)(*(uint8_t **)(team_sys + 0xd0) + (index & 0xffff) * 4);
    uint32_t f = *(uint32_t *)(*(uint8_t **)(team_sys + 0x100) + 4 + slot * 0xcull);
    return f == 0xffffffffu ? 0 : f & 3;
}

uint32_t ActorTeam(uint8_t *team_sys, uint64_t actor) {         // the team of an actor, 0: unknown
    if (!team_sys || !actor) return 0;
    using fn = uint32_t *(__fastcall *)(void *, uint32_t *, uint64_t, uint64_t);
    uint32_t out = 0xffffffffu;
    uint32_t idx = *(*(fn **)team_sys)[0x1a8 / 8](team_sys, &out, actor, ~0ull);
    if ((idx & 0xffff) == 0xffff) return 0;
    return TeamFlags(team_sys, idx);
}

void Rotate(const float *q, const float *v, float *out) {       // q (x, y, z, w) applied to v
    float t[3] = { 2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0]) };
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

float SegmentDistance(const float *a, const float *b, const float *c) {   // from c to segment ab
    float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, ac[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
    float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = l2 > 1e-8f ? (ac[0] * ab[0] + ac[1] * ab[1] + ac[2] * ab[2]) / l2 : 0.0f;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float d[3] = { ac[0] - ab[0] * t, ac[1] - ab[1] * t, ac[2] - ab[2] * t };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

void DroneHitsDue() {                                           // (server, once a frame)
    if (!g_is_server || !Death || !BulletDamage || !Real_TransformGet) return;
    auto at = (uint8_t *)g_at_cs, bs = (uint8_t *)g_bullet_cs;
    if (!at || !bs) {
#ifdef ECHOCOMBAT_LOG
        static ULONGLONG said = 0;
        if (GetTickCount64() - said > 2000 && g_last_deploy && GetTickCount64() - g_last_deploy < 30000) {
            said = GetTickCount64();
            LogDebug("[test] hit sphere: no %s system seen yet", !at ? "auto-target" : "bullet");
        }
#endif
        return;
    }
    struct Target { float c[3]; uint8_t *dcs; uint32_t didx; uint32_t team; };
    Target targets[8];
    int nt = 0;
    EC_TRY {
        // the drones' auto-target components: not enabled on the server (its list is empty
        // there), so each live drone's own (a component of its group on the auto-target system)
        uint8_t *team_sys = nullptr;
        if (*(uint16_t *)(bs + 0xf4)) team_sys = *(uint8_t **)(*(uint8_t **)(bs + 0xf8) + 0x20);
        struct Live { uint8_t *dcs; uint32_t didx; uint64_t actor; uint32_t handle; } live[8];
        int nl = 0;
        AtHandle hs[8];
        AcquireSRWLockShared(&g_at_lock);
        memcpy(hs, g_at_handles, sizeof hs);
        ReleaseSRWLockShared(&g_at_lock);
        ULONGLONG now = GetTickCount64();
        AcquireSRWLockShared(&g_drones_lock);
        int alive = 0;
        const dk::Group<dk::None, PcGroup> *only = nullptr;
        for (auto &d : g_groups.g) if (d.used && !d.dead) { ++alive; only = &d; }
        for (auto &h : hs) {
            if (!h.seen || now - h.seen > 25000 || nl == 8) continue;
            uint64_t actor = ActorOf(at, h.handle);
            const dk::Group<dk::None, PcGroup> *g = nullptr;
            for (auto &d : g_groups.g) if (d.used && !d.dead && actor && d.actor == actor) g = &d;
            if (!g && alive == 1 && only->opened <= h.seen) g = only;   // (one drone out: its shots are its)
            if (g) live[nl++] = { static_cast<uint8_t *>(SysPtr(g->system)), g->handle, g->actor, h.handle };
        }
        ReleaseSRWLockShared(&g_drones_lock);
#ifdef ECHOCOMBAT_LOG
        g_hd.drones = nl;
#endif
        for (int i = 0; i < nl && nt < 8; ++i) {
            uint32_t handle = live[i].handle;
            uint8_t *tbl = *(uint8_t **)(at + 0xc8);
            uint8_t *rec = *(uint8_t **)(at + 0xf8) + *(uint16_t *)(tbl + (handle & 0xffff) * 4ull) * 0x90ull;
            uint8_t *dcs = live[i].dcs;
            uint32_t didx = live[i].didx;
            uint64_t actor = live[i].actor;
            if (!dcs) continue;
            Pose p;
            Real_TransformGet(*(void **)(at + 0x130), &p, *(uint32_t *)(rec + 0x7c));
            float off[3];
            Rotate(p.q, HIT_CENTRE, off);
            targets[nt++] = { { p.p[0] + off[0], p.p[1] + off[1], p.p[2] + off[2] }, dcs, didx, ActorTeam(team_sys, actor) };
#ifdef ECHOCOMBAT_LOG
            memcpy(g_hd.c, targets[nt - 1].c, sizeof g_hd.c);
#endif
        }
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
#ifdef ECHOCOMBAT_LOG
    g_hd.matched = nt;
    if (GetTickCount64() - g_hd.at > 1000) {
        if (g_hd.drones)
            LogDebug("[test] hit sphere: %d auto-targets, %d drones matched, %d bullet records, %d new, %d tested, "
                "closest %.2f m (centre %.2f %.2f %.2f)", g_hd.drones, g_hd.matched, g_hd.bullets, g_hd.fresh,
                g_hd.checked, g_hd.closest, g_hd.c[0], g_hd.c[1], g_hd.c[2]);
        g_hd.at = GetTickCount64();
        g_hd.fresh = g_hd.checked = 0;
        g_hd.closest = 1e9f;
    }
#endif
    EC_TRY {
        uint16_t n = *(uint16_t *)(bs + 0xf4);
#ifdef ECHOCOMBAT_LOG
        g_hd.bullets = n;
#endif
        for (uint16_t i = 0; i < n && i < 1024; ++i) {
            uint8_t *rec = *(uint8_t **)(bs + 0xf8) + i * 0x298ull;
            BulletSeen &s = g_seen[i];
            float spawned = *(float *)(rec + 0xc8);
            Pose p;
            Real_TransformGet(*(void **)(rec + 0x30), &p, *(uint32_t *)(rec + 0x38));
            if (s.spawned != spawned) {                         // a new bullet in this record
                s = { spawned, { ((float *)(rec + 0x134))[0], ((float *)(rec + 0x134))[1], ((float *)(rec + 0x134))[2] },
                      false, false };
                for (auto &d : g_drone_shots) if (d.rec == rec && d.spawned == spawned) s.drone = true;
#ifdef ECHOCOMBAT_LOG
                if (!s.drone) ++g_hd.fresh;
#endif
            }
            float from[3] = { s.p[0], s.p[1], s.p[2] };
            memcpy(s.p, p.p, sizeof s.p);
            if (s.done || s.drone || !nt || (*(uint32_t *)(rec + 0xb4) & 8)) continue;
            uint32_t team = TeamFlags(*(uint8_t **)(rec + 0x20), *(uint16_t *)(rec + 0x28));
            for (int t = 0; t < nt; ++t) {
#ifdef ECHOCOMBAT_LOG
                ++g_hd.checked;
                float dd = SegmentDistance(from, p.p, targets[t].c);
                if (dd < g_hd.closest) g_hd.closest = dd;
                if (dd < 1.0f && team && targets[t].team && (team & targets[t].team))
                    LogDebug("[test] hit sphere: a team %u bullet %.2f m from the team %u drone: skipped (same team)",
                        team, dd, targets[t].team);
#endif
                if (team && targets[t].team && (team & targets[t].team)) continue;   // its own team's
                if (SegmentDistance(from, p.p, targets[t].c) > HIT_RADIUS) continue;
                s.done = true;
                uint16_t bslot = *(uint16_t *)(*(uint8_t **)(bs + 0xd8) + i * 2ull);
                uint32_t handle = bslot | (uint32_t)*(uint16_t *)(*(uint8_t **)(bs + 0xc8) + 2 + bslot * 4ull) << 16;
                float dmg = BulletDamage(bs, handle);
                uint8_t *dr = DamageRecord(targets[t].dcs, targets[t].didx);
                if (dr[2] & 4) break;
                float hp = *(float *)(dr + 0x20) - dmg;
                *(float *)(dr + 0x20) = hp > 0 ? hp : 0.0f;
                if (InterlockedIncrement(&g_sphere_hits) <= 40)
                    LogDebug("drone %u hit (team %u bullet, drone team %u): %.1f damage, health %.1f",
                        targets[t].didx & 0xffff, team, targets[t].team, dmg, hp > 0 ? hp : 0.0f);
                if (hp <= 0) {
                    dr[2] |= 4;
                    Death(targets[t].dcs, targets[t].didx);
                }
                break;
            }
        }
    } EC_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
    }
}

void InstallChase(uintptr_t base) {
    auto attack = (uint8_t *)(base + ATTACK_RVA), spawn = (uint8_t *)(base + BULLET_SPAWN_RVA);
    if (memcmp(attack, ATTACK_BYTES, sizeof ATTACK_BYTES) || memcmp(spawn, BULLET_SPAWN_BYTES, sizeof BULLET_SPAWN_BYTES)) {
        LogWarning("AttackTarget / bullet spawn prologue mismatch -- the drone stays put, tracers as stock");
        return;
    }
    Real_TransformGet = (transform_get_fn)(base + TRANSFORM_GET_RVA);
    TargetPos = (target_pos_fn)(base + TARGET_POS_RVA);
    Defer = (defer_fn)(base + DEFER_RVA);
    g_set_velocity = (void *)(base + SET_VELOCITY_RVA);
    BulletDamage = (bullet_damage_fn)(base + BULLET_DAMAGE_RVA);
    Real_Attack = (attack_fn)attack;
    Real_BulletSpawn = (bullet_spawn_fn)spawn;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)Real_Attack, Hooked_Attack);
    DetourAttach(&(PVOID &)Real_BulletSpawn, Hooked_BulletSpawn);
    LONG rc = DetourTransactionCommit();
    if (rc == NO_ERROR)
        Log("hooked AttackTarget and bullet spawn: a drone chases its target (%.0f m/s, holds %.0f m)", dk::kChaseSpeed, dk::kChaseDistance);
    else
        LogWarning("AttackTarget / bullet spawn hook failed");
}

// The call's rel32 has to reach MuzzleTransform: through a jump stub allocated within 2 GB.
void *StubNear(uintptr_t site, void *target) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t at = (site & ~(gran - 1)) - gran; at > site - 0x7ff00000; at -= gran) {
        auto stub = (uint8_t *)VirtualAlloc((void *)at, 16, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!stub) continue;
        const uint8_t jmp[] = { 0xff, 0x25, 0, 0, 0, 0 };     // jmp [rip+0]
        memcpy(stub, jmp, sizeof jmp);
        memcpy(stub + sizeof jmp, &target, sizeof target);
        return stub;
    }
    return nullptr;
}

bool g_hooked = false;
bool g_fire_hooked = false;
bool g_enable_hooked = false;
uint8_t *g_fire_site = nullptr;

void InstallMuzzle(uintptr_t base) {
    auto site = (uint8_t *)(base + FIRE_GET_CALL_RVA);
    if (!EchoCombatBytesMatch(site, FIRE_GET_CALL_BYTES, sizeof(FIRE_GET_CALL_BYTES))) {
        LogWarning("FireBulletLocal call mismatch at +0x%llx -- wrong echovr build, bullets stay at the root",
            (unsigned long long)FIRE_GET_CALL_RVA);
        return;
    }
    auto fire = (uint8_t *)(base + FIRE_LOCAL_RVA);
    if (memcmp(fire, FIRE_LOCAL_BYTES, sizeof(FIRE_LOCAL_BYTES)) != 0) {
        LogWarning("FireBulletLocal prologue mismatch at +0x%llx -- bullets stay at the root", (unsigned long long)FIRE_LOCAL_RVA);
        return;
    }
    Real_FireLocal = (fire_local_fn)fire;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)Real_FireLocal, Hooked_FireLocal);
    if (DetourTransactionCommit() != NO_ERROR) {
        LogWarning("FireBulletLocal hook failed; bullets stay at the root");
        return;
    }
    g_fire_hooked = true;
    Real_TransformGet = (transform_get_fn)(base + TRANSFORM_GET_RVA);
    void *stub = StubNear((uintptr_t)site, (void *)MuzzleTransform);
    if (!stub) {
        LogWarning("no memory near echovr for the muzzle stub; bullets stay at the root");
        return;
    }
    int32_t rel = (int32_t)((uintptr_t)stub - ((uintptr_t)site + 5));
    // Checked patch: the call must still be the bytes we expect, and the page must be
    // writable, or nothing is written.
    if (!EchoCombatBytesMatch(site, FIRE_GET_CALL_BYTES, sizeof(FIRE_GET_CALL_BYTES))) {
        LogWarning("muzzle call site changed before patching at +0x%llx -- not patched, bullets stay at the root",
            (unsigned long long)FIRE_GET_CALL_RVA);
        return;
    }
    // nEVR port: the patch and its stub pass the write gate, or nothing is written.
    if (!::nevr_combat::GateWrite(site + 1, 4, "DroneFix muzzle call rel32 (FireBulletLocal +0x293)") ||
        !::nevr_combat::GateWrite(stub, 16, "DroneFix muzzle stub")) {
        LogWarning("muzzle patch refused at +0x%llx: it overlaps a range the runtime owns -- bullets stay at the root",
            (unsigned long long)FIRE_GET_CALL_RVA);
        return;
    }
    // nEVR port: written through ProcessMemcpy, which records the range in the runtime's registry (write_ranges.h).
    DWORD error = 0;
    if (!ProcessMemcpy(site + 1, &rel, 4, &error, "DroneFix muzzle call rel32")) {
        LogWarning("muzzle patch failed (error %lu) at +0x%llx -- bullets stay at the root", error,
            (unsigned long long)FIRE_GET_CALL_RVA);
        return;
    }
    g_fire_site = site;
    Log("drone bullets fire from the muzzle (%.4f %.4f %.4f) (+0x%llx)", dm::kMuzzle[0], dm::kMuzzle[1], dm::kMuzzle[2],
        (unsigned long long)FIRE_GET_CALL_RVA);
}

void InstallEnableMirror(uintptr_t base) {
    auto enter = (uint8_t *)(base + ENABLE_ENTER_RVA);
    if (memcmp(enter, ENABLE_ENTER_BYTES, sizeof(ENABLE_ENTER_BYTES)) != 0) {
        LogWarning("EnableComponentNode prologue mismatch at +0x%llx -- the satellite won't hide with the gun",
            (unsigned long long)ENABLE_ENTER_RVA);
        return;
    }
    Real_EnableEnter = (enable_enter_fn)enter;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)Real_EnableEnter, Hooked_EnableEnter);
    LONG rc = DetourTransactionCommit();
    g_enable_hooked = rc == NO_ERROR;
    if (rc == NO_ERROR)
        Log("hooked EnableComponentNode: the satellite shows / hides with the gun (+0x%llx)", (unsigned long long)ENABLE_ENTER_RVA);
    else
        LogWarning("EnableComponentNode hook failed (+0x%llx)", (unsigned long long)ENABLE_ENTER_RVA);
}

void InstallGrabRule(uintptr_t base) {
    auto target = (uint8_t *)(base + DISABLE_RVA);
    if (memcmp(target, DISABLE_BYTES, sizeof(DISABLE_BYTES)) != 0) {
        LogWarning("component disable prologue mismatch at +0x%llx -- the drone can't be grabbed after release, as stock",
            (unsigned long long)DISABLE_RVA);
        return;
    }
    disable_fn real = (disable_fn)target;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)real, Hooked_Disable);
    LONG rc = DetourTransactionCommit();
    if (rc == NO_ERROR) Real_Disable = real;
    if (rc == NO_ERROR)
        Log("hooked component disable: a drone stays grabbable until it locks on (+0x%llx)", (unsigned long long)DISABLE_RVA);
    else
        LogWarning("component disable hook failed (+0x%llx)", (unsigned long long)DISABLE_RVA);
}

void InstallDeathRule(uintptr_t base) {
    g_is_server = strstr(GetCommandLineA(), "-server") != nullptr;
    auto en = (uint8_t *)(base + ENABLE_COMP_RVA), death = (uint8_t *)(base + DEATH_RVA);
    if (memcmp(en, ENABLE_COMP_BYTES, sizeof(ENABLE_COMP_BYTES)) || memcmp(death, DEATH_BYTES, sizeof(DEATH_BYTES))) {
        LogWarning("component enable / death prologue mismatch -- the drone can't be shot down");
        return;
    }
    enable_comp_fn real_en = (enable_comp_fn)en;
    death_fn real_death = (death_fn)death;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)real_en, Hooked_EnableComp);
    DetourAttach(&(PVOID &)real_death, Hooked_Death);
    LONG rc = DetourTransactionCommit();
    if (rc != NO_ERROR) {
        LogWarning("component enable / death hook failed -- the drone can't be shot down");
        return;
    }
    Real_EnableComp = real_en;
    Real_Death = real_death;
    Death = (death_fn)death;                                // through the hook (the test kill)
    Log("hooked component enable and damageable death: a drone is destroyed when its health runs out (%s)",
        g_is_server ? "server" : "client");
    if (KILL_TEST_MS && g_is_server)
        Log("[test] each deployed drone is killed %llu ms after release", KILL_TEST_MS);
}

bool Install() {
    if (g_hooked) return true;
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    if (g_drone_on) {                                             // nEVR port: DRONE (combat.drone)
    InstallDeathRule(base);
    InstallMuzzle(base);
    InstallChase(base);
    InstallHitLog(base);
    InstallGrabKey(base);
    InstallEnableMirror(base);
    InstallGrabRule(base);
    }
    auto target = (uint8_t *)(base + FOLLOW_UPDATE_RVA);
    if (memcmp(target, FOLLOW_UPDATE_BYTES, sizeof(FOLLOW_UPDATE_BYTES)) != 0) {
        LogError("prologue mismatch at +0x%llx -- wrong echovr build, not hooking", (unsigned long long)FOLLOW_UPDATE_RVA);
        return false;
    }
    QueryPerformanceFrequency(&g_qpf);
    Real_FollowUpdate = (follow_update_fn)target;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)Real_FollowUpdate, Hooked_FollowUpdate);
    LONG rc = DetourTransactionCommit();
    if (rc == NO_ERROR)
        Log("hooked CR15NetFollowPlayerCS update (+0x%llx)", (unsigned long long)FOLLOW_UPDATE_RVA);
    else
        LogError("hook failed (+0x%llx)", (unsigned long long)FOLLOW_UPDATE_RVA);
    if (rc == NO_ERROR && !g_is_server && g_drone_on) {          // NEUTRON's effects come here now (nEVR port: DRONE on)
        *(volatile LONG *)(base + FX_DEFER_RVA) = 1;
        Log("NEUTRON's effects run on the follow update, not inside its wall's node");
    }
    g_hooked = rc == NO_ERROR;
    return g_hooked;
}

} // namespace

// nEVR port: the plugin's exports and DllMain are replaced by these, which runtime/combat/combat.cpp calls.
bool Start(bool drone_on) {
    g_drone_on = drone_on;
    return Install();
}

bool Started() { return g_hooked; }

void Killed(uint32_t index) { DroneFix_Killed(index); }

void Stop() {
    if (g_fire_site) {                                      // the call back to CTransformCS::get
        DWORD old;
        VirtualProtect(g_fire_site + 1, 4, PAGE_EXECUTE_READWRITE, &old);
        memcpy(g_fire_site + 1, FIRE_GET_CALL_BYTES + 1, 4);
        VirtualProtect(g_fire_site + 1, 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), g_fire_site, 5);
        g_fire_site = nullptr;
    }
    if (!g_hooked && !g_fire_hooked && !g_enable_hooked && !Real_Disable && !Real_EnableComp) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (g_hooked) DetourDetach(&(PVOID &)Real_FollowUpdate, Hooked_FollowUpdate);
    if (g_fire_hooked) DetourDetach(&(PVOID &)Real_FireLocal, Hooked_FireLocal);
    if (g_enable_hooked) DetourDetach(&(PVOID &)Real_EnableEnter, Hooked_EnableEnter);
    if (Real_Disable) DetourDetach(&(PVOID &)Real_Disable, Hooked_Disable);       // the grab rule
    if (Real_EnableComp) {                                                       // the death rule
        DetourDetach(&(PVOID &)Real_EnableComp, Hooked_EnableComp);
        DetourDetach(&(PVOID &)Real_Death, Hooked_Death);
    }
    DetourTransactionCommit();
    g_hooked = g_fire_hooked = g_enable_hooked = false;
    Real_Disable = nullptr;
    Real_EnableComp = nullptr;
    Real_Death = nullptr;
    // nEVR port (#2349): every count no line covers yet goes out now, then the totals, in a line the Info cap cannot drop
    const ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_fault_lines_lock);
    ReportFaults(g_fault_lines.Final(now), 0);
    g_fault_lines_pending.store(false, std::memory_order_relaxed);
    ReleaseSRWLockExclusive(&g_fault_lines_lock);
    AcquireSRWLockExclusive(&g_wall_lines_lock);
    const nevr::BurstThenCount::Line wall = g_wall_lines.Final(now);
    g_wall_lines_pending.store(false, std::memory_order_relaxed);
    const uint64_t refused = g_wall_refused_total, nowall = g_wall_nowall_total;
    ReleaseSRWLockExclusive(&g_wall_lines_lock);
    ReportWallRefusals(wall, fx::Counts(), refused, nowall);
    FlushDropped(true);
    char line[256];
    snprintf(line, sizeof(line), "unhooked (%ld updates skipped, %ld faults contained, %llu NEUTRON effects refused, %llu sounds with no wall)",
             static_cast<long>(g_skipped), static_cast<long>(g_faults), static_cast<unsigned long long>(refused),
             static_cast<unsigned long long>(nowall));
    PluginLogFields(NEVR_LOG_INFO, line, {});
}

}  // namespace nevr_combat::dronefix

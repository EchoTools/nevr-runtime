#pragma once
// The drone kill rules, for both builds (#2359, unit 5 of #2187): the drone's symbols, its chase velocity, the gun table the port
// follows, and the group table (a release opens a group, the drone's other components join it, a pending list holds the ones that come
// first, a dead group's component is held off for the swallow window, a kill picks a group by its damageable index, the drone's own end
// marks its group dead). It reads no memory and calls nothing of the game's, so it needs no Env: the inputs a port supplies are the
// arguments (its clock in ms, its actor id, its game space, its own per-component and per-group payload). Each caller keeps its own lock
// around every call, its own hooks, its own enable, disable and swallow mechanism and its own log lines: runtime/combat/dronefix.cpp on
// PC (Hooked_EnableComp, AddTouch, Shutdown, ExpiredDamageable, Hooked_Death, SetGun, GunShown, TouchLive, ChaseTarget) and
// quest/combat/dronekill.cpp on Quest (EnterInScope, OnKilled, ChaseAfter; the due bits, the touch table and the chase slot table stay in
// quest/combat/dronekill_core.h). The PC GTest (runtime/tests/test_dronekill_rules.cpp) and the Quest host test
// (quest/tests/dronekill_test.cpp) both compile this one definition.
//
// Origin. EchoCombat's `plugin/dronefix.cpp` (he_is_the_cat, heisthecat31/EchoCombat), as released in v2.0.0 (nevr-pack 4d1f0024, whose
// dronefix.cpp is c3ff4b54's; "EC" lines below are that file). PC ports c3ff4b54; the Quest ported c14c3e0a, whose rule lines are the same
// text, through the reference build's `runtime/native/afkxr/afk_dronekill_core.h` (kotorvr's echoquest-combat). Rule for rule: the symbols
// EC 75-80, 196, 214, 785; the groups EC 480-488; Newest EC 507-515; AddComp EC 517-521; the swallow window EC 525 and InDeadGroup
// EC 532-539; the release and the pending list EC 552-573; the join EC 576-582; the kill pick EC 606-616; the drone's own end EC 645-653;
// the gun table EC 749-775 and the kill's gun state EC 621; the chase EC 1083-1086, 1131-1136.
// Both builds hold these forms (#2359):
//   - Every rule the two ports shared is EC's.
//   - D1: a chase whose distance is below 1e-3f gives no velocity (EC 1133). The Quest's `dist2 < 1e-6f` gives the same answer for every
//     float (each non-negative finite float was checked), so the form is EC's and nothing changes there.
//   - D2: a drone or target position that is not finite, or a distance that is not, gives no velocity. EC writes NaN for a NaN or
//     infinite input, and (0, 0, 0) for a finite distance too large for a float.
//   - D3: gun key 0 is ignored by Set and unknown to Shown. In EC, key 0 is written into a slot that still reads as free.
//   - D4: the gun table takes a key; both callers pass the node's actor symbol, and a kill hides kDroneActor's state (EC 621).
//   - D5, D6: 16 pending components; a full list is emptied whole before the new one is added (EC 580).
//   - D7, D9: the first live group in table order for an actor and for a kill; release time, first in table order on a tie, for the
//     newest and the oldest (EC 509-513, 555, 608-609).
//   - D8: the swallow window includes 25000 ms (EC 534: `> 25000` ends it).
//   - D10: a non-zero game space unlike the table's clears the groups and the pending list; 0 is unknown and clears nothing. PC passes 0.
//   - D11: one dead-marking call by (system, handle) (EC 645-653); PC calls it at the drone's expiry and its server death, the Quest when
//     a drone node turns kDamageable off.
// The non-finite guard assumes IEEE 754 floating point: building with -ffast-math or -ffinite-math-only removes it.
#include <math.h>
#include <stdint.h>

#include <cmath>

namespace nevr_dronekill {

// ---- the symbols (CSymbol64)
constexpr uint64_t kDroneActor = 0x10E40287A9BF5A13ull;     // the drone actor (EC 196)
constexpr uint64_t kDamageable = 0xC9DC08CFC2F5920Full;     // R14NetDamageable: its ON is a release (EC 214)
constexpr uint64_t kTouchInteract = 0x2E40131422F7B1FCull;  // R14TouchInteract (EC 785)
constexpr uint64_t kNcaModel = 0x38EE951A26FB816Aull;       // ncaModel (EC 75)
constexpr uint64_t kGun = 0x72F91FFFA17C3C8Cull;            // its gun (EC 76)
constexpr uint64_t kSatellite = 0x106C63A2927B7A4Dull;      // its satellite (EC 77)
constexpr uint64_t kPort = 0x45B09F0FA9F7FA33ull;           // its port (EC 80)
// CSymbol64("ncaPhysics") (docs/quest/combat/drone_explosion.md): the first system the drone script turns off at its end; upstream
// labels it the sound system (EC's DRONE_SOUND). Which of the two the drone's graph uses it for is not established.
constexpr uint64_t kNcaPhysics = 0x5B8CC538E22AD937ull;

// ---- the chase (EC 1083-1086)
constexpr float kChaseSpeed = 7.0f;      // m/s at most
constexpr float kChaseDistance = 7.0f;   // m from the target it holds
constexpr float kChaseGain = 2.0f;       // speed per metre off that distance
constexpr float kChaseAbove = 0.5f;      // m above the target's position
constexpr float kChaseMinDist = 1e-3f;   // closer than this: no velocity (EC 1133)

// p: the drone's position, q: the target's (both world). v: the velocity toward the point kChaseAbove above q, (dist - kChaseDistance) *
// kChaseGain clamped to +-kChaseSpeed (EC 1131-1136). False, with v, distOut and speedOut left as they were, when dist < kChaseMinDist or
// dist is not finite (a NaN or infinite input gives a NaN or infinite dist).
inline bool ChaseVelocity(const float* p, const float* q, float* v, float* distOut = nullptr, float* speedOut = nullptr) {
    const float d[3] = { q[0] - p[0], q[1] + kChaseAbove - p[1], q[2] - p[2] };
    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!std::isfinite(dist)) return false;
    if (dist < kChaseMinDist) return false;
    float speed = (dist - kChaseDistance) * kChaseGain;
    speed = speed > kChaseSpeed ? kChaseSpeed : speed < -kChaseSpeed ? -kChaseSpeed : speed;
    for (int i = 0; i < 3; ++i) v[i] = d[i] / dist * speed;
    if (distOut) *distOut = dist;
    if (speedOut) *speedOut = speed;
    return true;
}

// ---- the gun table (EC 749-775): the last gun state per key; full, the oldest goes
constexpr int kGuns = 32;
struct GunTable {
    struct Entry { uint64_t key = 0; uint32_t shown = 0; };
    Entry e[kGuns] = {};
    void Set(uint64_t key, uint32_t shown) {
        if (!key) return;                                                   // D3
        int slot = -1;
        for (int i = 0; i < kGuns && slot < 0; ++i) if (e[i].key == key) slot = i;
        for (int i = 0; i < kGuns && slot < 0; ++i) if (!e[i].key) slot = i;
        if (slot < 0) {                                                     // full: forget the oldest
            for (int i = 0; i + 1 < kGuns; ++i) e[i] = e[i + 1];
            slot = kGuns - 1;
        }
        e[slot].key = key;
        e[slot].shown = shown;
    }
    bool Shown(uint64_t key, uint32_t* shown) const {
        if (!key) return false;                                             // D3
        for (const Entry& x : e) if (x.key == key) { *shown = x.shown; return true; }
        return false;
    }
    void Clear() { for (Entry& x : e) x = Entry{}; }
};

// a kill hides the port: the drone symbol's state (EC 621), whichever drone died (D4)
inline void HideGunOnKill(GunTable& t) { t.Set(kDroneActor, 0); }

// ---- the groups (EC 480-488)
constexpr int kGroups = 8, kGroupComps = 20, kPending = 16;
constexpr uint64_t kPendingMs = 3000;   // a release takes pending components younger than this (EC 567)
constexpr uint64_t kJoinMs = 5000;      // a component joins the newest live group younger than this (EC 513)
constexpr uint64_t kSwallowMs = 25000;  // a dead group's component is held off up to and including this long after its release (EC 525, 534)

struct None {};

// a component: (system, handle), when it was seen and the actor it was seen with (0 unknown); x: the caller's own
template <class CompX = None> struct Comp {
    uint64_t system = 0;
    uint32_t handle = 0;
    uint64_t at = 0, actor = 0;
    CompX x{};
};
// a drone's group: its damageable (system, handle) is comps[0]; opened: its release time; x: the caller's own
template <class CompX = None, class GroupX = None> struct Group {
    bool used = false, dead = false;
    uint64_t system = 0;
    uint32_t handle = 0;
    uint64_t opened = 0, actor = 0;
    int n = 0;
    Comp<CompX> comps[kGroupComps];
    GroupX x{};
};

// group: the slot (-1 none). comp: the component's index in that group (-1: not in it; for a swallow, the one held off).
// pendingJoined: pending components a release took. full: components not added because the group was full.
struct Verdict { int group = -1, comp = -1, pendingJoined = 0, full = 0; bool pending = false, swallowed = false, spaceCleared = false; };
struct DeadMark { int group = -1; bool wasAlive = false; };

template <class CompX = None, class GroupX = None> struct Groups {
    using C = Comp<CompX>;
    using G = Group<CompX, GroupX>;
    G g[kGroups];
    C pending[kPending];
    int pendingN = 0;
    uint64_t pendingDropped = 0;  // pending components forgotten when the list was full
    uint64_t space = 0;           // the game space of the table (0: none yet)
    uint32_t spaceClears = 0;

    void Clear() {
        for (G& x : g) x = G{};
        pendingN = 0;
    }
    // D10: a non-zero space unlike the table's empties it; true when it did
    bool ObserveSpace(uint64_t spaceNow) {
        bool cleared = false;
        if (spaceNow && space && spaceNow != space) { Clear(); ++spaceClears; cleared = true; }
        if (spaceNow) space = spaceNow;
        return cleared;
    }
    // the group whose damageable is (system, handle), any state; -1 none
    int Find(uint64_t system, uint32_t handle) const {
        for (int i = 0; i < kGroups; ++i) if (g[i].used && g[i].system == system && g[i].handle == handle) return i;
        return -1;
    }
    // EC Newest: this actor's first live group in table order (any age), else the live group released last and less than kJoinMs ago,
    // the first in table order on a tie; -1 none
    int Newest(uint64_t actor, uint64_t now) const {
        if (actor)
            for (int i = 0; i < kGroups; ++i) if (g[i].used && !g[i].dead && g[i].actor == actor) return i;
        int best = -1;
        for (int i = 0; i < kGroups; ++i)
            if (g[i].used && !g[i].dead && now - g[i].opened < kJoinMs && (best < 0 || g[i].opened > g[best].opened)) best = i;
        return best;
    }
    // EC AddComp: once per (system, handle). The component's index, or -1 when the group is full.
    int Add(int group, uint64_t system, uint32_t handle, uint64_t actor, uint64_t now, const CompX& x = CompX{}) {
        G& gr = g[group];
        for (int i = 0; i < gr.n; ++i) if (gr.comps[i].system == system && gr.comps[i].handle == handle) return i;
        if (gr.n >= kGroupComps) return -1;
        C& c = gr.comps[gr.n];
        c.system = system;
        c.handle = handle;
        c.at = now;
        c.actor = actor;
        c.x = x;
        return gr.n++;
    }
    // EC InDeadGroup: the first dead group in table order, released no more than kSwallowMs ago, that holds (system, handle); -1 none
    int Swallowing(uint64_t system, uint32_t handle, uint64_t now, int* comp) const {
        for (int i = 0; i < kGroups; ++i) {
            if (!g[i].used || !g[i].dead || now - g[i].opened > kSwallowMs) continue;
            for (int k = 0; k < g[i].n; ++k)
                if (g[i].comps[k].system == system && g[i].comps[k].handle == handle) {
                    if (comp) *comp = k;
                    return i;
                }
        }
        return -1;
    }
    // A release (the damageable goes on): the group of the same (system, handle), else the first free one, else the one released first
    // (the first in table order on a tie) starts again with the damageable, and takes the pending components younger than kPendingMs
    // whose actor or its own is unknown or which agree; older ones are forgotten, the others stay pending (EC 552-573).
    Verdict Release(uint64_t system, uint32_t handle, uint64_t actor, uint64_t spaceNow, uint64_t now, const CompX& x = CompX{}) {
        Verdict r;
        r.spaceCleared = ObserveSpace(spaceNow);
        int s = Find(system, handle);
        if (s < 0) for (int i = 0; i < kGroups && s < 0; ++i) if (!g[i].used) s = i;
        if (s < 0) { s = 0; for (int i = 1; i < kGroups; ++i) if (g[i].opened < g[s].opened) s = i; }
        g[s] = G{};
        g[s].used = true;
        g[s].system = system;
        g[s].handle = handle;
        g[s].opened = now;
        g[s].actor = actor;
        Add(s, system, handle, actor, now, x);
        int kept = 0;
        for (int i = 0; i < pendingN; ++i) {
            const C p = pending[i];
            if (now - p.at >= kPendingMs) continue;
            if (!actor || !p.actor || p.actor == actor) {
                if (Add(s, p.system, p.handle, p.actor, p.at, p.x) < 0) ++r.full;
                ++r.pendingJoined;
            } else {
                pending[kept++] = p;
            }
        }
        pendingN = kept;
        r.group = s;
        r.comp = 0;
        return r;
    }
    // Any other component going on. A dead group's (Swallowing) is swallowed. Else, for a drone's (drone true), it joins Newest(actor)
    // when the actors agree (either unknown, or equal), else it waits in the pending list, which is emptied first when full (EC 575-582).
    // drone false: the swallow check only (PC: an enable that is not the drone script's).
    Verdict Enable(uint64_t system, uint32_t handle, uint64_t actor, uint64_t spaceNow, uint64_t now, bool drone, const CompX& x = CompX{}) {
        Verdict r;
        r.spaceCleared = ObserveSpace(spaceNow);
        int comp = -1;
        const int dead = Swallowing(system, handle, now, &comp);
        if (dead >= 0) {
            r.group = dead;
            r.comp = comp;
            r.swallowed = true;
            return r;
        }
        if (!drone) return r;
        const int s = Newest(actor, now);
        if (s >= 0 && (!actor || !g[s].actor || g[s].actor == actor)) {
            r.group = s;
            r.comp = Add(s, system, handle, actor, now, x);
            if (r.comp < 0) r.full = 1;
            return r;
        }
        if (pendingN == kPending) {   // D6: full, every pending component goes
            pendingDropped += static_cast<uint64_t>(pendingN);
            pendingN = 0;
        }
        C& c = pending[pendingN++];
        c.system = system;
        c.handle = handle;
        c.at = now;
        c.actor = actor;
        c.x = x;
        r.pending = true;
        return r;
    }
    // D11, EC ExpiredDamageable: the group whose damageable is (system, handle) is dead, whatever it was. wasAlive: it was not dead.
    DeadMark MarkDead(uint64_t system, uint32_t handle) {
        DeadMark m;
        m.group = Find(system, handle);
        if (m.group < 0) return m;
        m.wasAlive = !g[m.group].dead;
        g[m.group].dead = true;
        return m;
    }
    // EC Shutdown(nullptr, index): the first live group in table order whose damageable index (the low 16 bits) is index's is dead; -1 none
    int KillIndex(uint32_t index) {
        for (int i = 0; i < kGroups; ++i)
            if (g[i].used && !g[i].dead && (g[i].handle & 0xffff) == (index & 0xffff)) {
                g[i].dead = true;
                return i;
            }
        return -1;
    }
    // EC TouchLive: (system, handle) is a component of a group that is not dead
    bool LiveComp(uint64_t system, uint32_t handle) const {
        for (const G& x : g) {
            if (!x.used || x.dead) continue;
            for (int k = 0; k < x.n; ++k) if (x.comps[k].system == system && x.comps[k].handle == handle) return true;
        }
        return false;
    }
};

// ---- the drone's explosion (EC's DeathFx queue; PC dronefix.cpp QueueDeathFx / DeathFxDue / ExpiredDamageable, the Quest's dronekill.cpp)
// A dead drone's explosion runs at the next per-frame update, outside any script node. An expiry (the drone's own end) holds the first
// system its script turns off (kNcaPhysics) back, and turns it off kFxSoundHoldMs after the explosion was spawned; a shot-down drone's
// explosion holds nothing.
constexpr int kFxSlots = 8;                  // explosions queued at once (EC's g_fx[8])
constexpr int kFxHeld = 8;                   // held components per explosion (EC's DeathFx::sound[8])
constexpr uint64_t kFxSoundHoldMs = 2500;    // the explosion's sound, then the held components off (EC's FX_SOUND_HOLD_MS)

// Ctx: the caller's explosion context (PC: the node context; Quest: the context, its group and index). Held: a held component.
template <class Ctx, class Held> struct FxQueue {
    struct Slot { bool used = false, spawned = false; uint64_t at = 0; Ctx ctx{}; int nheld = 0; Held held[kFxHeld]; };
    Slot s[kFxSlots];

    // the first free slot takes it (at most kFxHeld held components kept); false: all kFxSlots are taken, nothing was queued and the
    // caller turns its held components off now
    bool Queue(const Ctx& ctx, const Held* held, int nheld, uint64_t now) {
        for (Slot& x : s) {
            if (x.used) continue;
            x = Slot{};
            x.used = true;
            x.ctx = ctx;
            x.at = now;
            x.nheld = nheld < 0 ? 0 : nheld < kFxHeld ? nheld : kFxHeld;
            for (int i = 0; i < x.nheld; ++i) x.held[i] = held[i];
            return true;
        }
        return false;
    }
    // What is due at now: each explosion not yet spawned goes to spawn (its clock restarts at now); a slot with nothing held, or spawned
    // kFxSoundHoldMs or more ago, hands its held components to off and is freed. Returns the explosions written to spawn (at most
    // spawnCap); *nOff = the components written to off (at most offCap; the rest of a full off list are dropped with the slot).
    int Due(uint64_t now, Ctx* spawn, int spawnCap, Held* off, int offCap, int* nOff) {
        int n = 0, m = 0;
        for (Slot& x : s) {
            if (!x.used) continue;
            if (!x.spawned) {
                if (n >= spawnCap) continue;
                spawn[n++] = x.ctx;
                x.spawned = true;
                x.at = now;
            }
            if (now - x.at >= kFxSoundHoldMs || !x.nheld) {
                for (int i = 0; i < x.nheld && m < offCap; ++i) off[m++] = x.held[i];
                x = Slot{};
            }
        }
        *nOff = m;
        return n;
    }
    bool Any() const {
        for (const Slot& x : s) if (x.used) return true;
        return false;
    }
    void Clear() { for (Slot& x : s) x = Slot{}; }
};

// The expiry: the first-system off opens it; the damageable's off closes it. Then a drone that was alive, with an explosion context, has
// its explosion queued with the held components; any other (already dead: shot down, its explosion then; or no context) has them off
// now. An expiry no damageable off closed within kFxSoundHoldMs of its opening lets them go (off) all the same: nothing stays held.
enum class ExpiryAction { kQueue, kOffNow };
inline ExpiryAction OnDamageableOff(bool alive, bool open, bool haveCtx) {
    return alive && open && haveCtx ? ExpiryAction::kQueue : ExpiryAction::kOffNow;
}
inline bool ExpiryLetGo(bool open, uint64_t opened, uint64_t now) { return open && now - opened >= kFxSoundHoldMs; }

}  // namespace nevr_dronekill

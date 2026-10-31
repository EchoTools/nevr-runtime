// Ported from the source build's dronekill_core (<source build>/the source build's dronekill_core,
// RE-33 E-N1 stage 0, EC-NATIVES stage 1): the platform-free part of
// dronekill.cpp: the libr15 shape checks (vtable thunks / bodies), the drone group table (rules R1' / R2 / R3), the F3 touch
// table, the F5 gun table and the F7 chase table. Nothing here calls the game or reads game memory. Verbatim from the source
// build except: the namespace (q15t::dk) and the 'K' ring (the source's recvfrom thread pushes into it; here DroneKillOnKilled
// does the table work itself, see dronekill.cpp) and Comp's members, which carry initializers so that the tables are constant-initialised
// (the repack refuses a plugin with a static constructor). The shape tables were compared with the disassembly (docs/quest/q15t/modules/dronekill.md).
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

namespace q15t {
namespace dk {

constexpr uint64_t kDroneActor = 0x10e40287a9bf5a13ull;    // the drone actor's symbol (upstream)
constexpr uint64_t kDamageable = 0xc9dc08cfc2f5920full;    // R14NetDamageable: its ON = a drone release = a new group

// libr15 4987566 (both md5 pins; a source-build test reads every component system's vtable in both files):
// vtable slot +0x1b8 = Component(SActor, CSymbol64), 7 words, byte-identical in 286 of 287 systems: it calls slot
// +0x1b0 and masks the result to 32 bits.
constexpr int kThunkWords = 7;
constexpr uint32_t kThunk[kThunkWords] = { 0xf81f0ffe, 0xf9400008, 0xf940d908, 0xd63f0100, 0x92407c00, 0xf84107fe, 0xd65f03c0 };
// vtable slot +0x1b0 = Component(SComponentID), 54 words in 283 systems: a binary search over the system's own key
// table (loads only; the frame is its only store) whose one call is the BinarySearch comparator's PLT stub
// (r15+kCmpPlt -> JUMP_SLOT r15+kCmpSlot -> r15+kCmpFn: 122 words, loads and compares only). The systems differ in
// six `ldr xN, [x19, #imm]` words (kBodyLdr: the imm12 field is masked) and in the bl's displacement (word kBodyBl).
constexpr int kBodyWords = 54, kBodyBl = 27;
constexpr int kBodyLdr[6] = { 8, 13, 21, 42, 44, 48 };
constexpr uint32_t kLdrImmMask = 0x003ffc00, kBlMask = 0x03ffffff;
constexpr uint32_t kBody[kBodyWords] = {
    0xd10143ff, 0xa9025bf7, 0xa90353f5, 0xa9047bf3, 0xb100045f, 0xaa0003f3, 0x540000a0, 0xa9008be1, 0xf9409275,
    0xb50000d5, 0x14000018, 0xf9401662, 0xa9008be1, 0xf9409275, 0xb4000295, 0xaa1f03f6, 0x52800314, 0x14000004,
    0xeb1602ff, 0xaa1703f5, 0x540001c9, 0xf9408e68, 0xcb1602a9, 0x8b4906d7, 0x910063e0, 0x9b1422e1, 0x910023e2,
    0x94000000, 0xf100001f, 0x54fffeac, 0xb6f80180, 0x910006f6, 0xaa1503f7, 0x17fffff1, 0xaa1f03e9, 0x529fffe8,
    0xa9447bf3, 0xa94353f5, 0xa9425bf7, 0xaa090100, 0x910143ff, 0xd65f03c0, 0xf9408e68, 0x52800309, 0xf940726a,
    0xd503201f, 0x9b0922e8, 0x79402108, 0xf9406a69, 0x78687948, 0x8b080929, 0x79400529, 0xd370bd29, 0x17ffffef };
constexpr uint64_t kCmpPlt = 0xf43610, kCmpSlot = 0x36f91f0, kCmpFn = 0x1524cb4;
constexpr uint64_t kVtComponentId = 0x1b0, kVtComponent = 0x1b8;
constexpr uint64_t kSysOwner = 0x80, kOwnerFlag = 0x368;   // Enter's "direct call or queue" word: [[system+0x80]+0x368]

inline bool ThunkOk(const uint32_t* w) { return memcmp(w, kThunk, sizeof kThunk) == 0; }

// w = the 54 words at bodyVa (an address in the same space as pltVa = the comparator's PLT stub).
inline bool BodyOk(const uint32_t* w, uint64_t bodyVa, uint64_t pltVa) {
    for (int i = 0; i < kBodyWords; ++i) {
        uint32_t mask = 0xffffffffu;
        if (i == kBodyBl) mask = ~kBlMask;
        for (int k : kBodyLdr) if (i == k) mask = ~kLdrImmMask;
        if ((w[i] & mask) != (kBody[i] & mask)) return false;
    }
    int64_t d = w[kBodyBl] & kBlMask;
    if (d & 0x02000000) d -= 0x04000000;
    return bodyVa + kBodyBl * 4 + static_cast<uint64_t>(d * 4) == pltVa;
}

// vtable slot +0x210 = <system>::DisableComponent(SCompactPoolHandle, EComponentEnableCategory): named in all 287
// systems; 282 share this 48-word body (CR15NetDamageableCS r15+0x2211868 is the reference; EC-NATIVES read
// 2026-10-16, a source-build test reads every one on the three libr15 files). No lock, no TLS, no script call:
// the disable mask |= category, CCompactPool::SwapEntry (the bl, word kDisBl: its own template per system) past the
// active count, then a tail call of vtable +0x60 OnComponentDisabled. The systems differ in the imm12 of nine
// ldr / ldrh / strh / add words (kDisImm) and in the bl's displacement.
constexpr uint64_t kVtDisable = 0x210;
constexpr int kDisWords = 48, kDisBl = 23;
constexpr int kDisImm[9] = { 3, 6, 8, 17, 20, 24, 26, 31, 32 };
constexpr uint32_t kDis[kDisWords] = {
    0xa9bd5bf7, 0xa90153f5, 0xa9027bf3, 0xf9406808, 0x92403c29, 0xd37ef536, 0x7941f817, 0x78766915, 0xf9407408,
    0xaa0003f3, 0xaa0103f4, 0xd37ffaa9, 0x7869690a, 0x2a02014a, 0x72003d5f, 0x7829690a, 0x54000160, 0x7941fa68,
    0x6b0802bf, 0x54000102, 0x91032260, 0x51000502, 0x2a1503e1, 0x97b4afdb, 0x7941fa68, 0x51000508, 0x7901fa68,
    0x6b1702bf, 0x54000102, 0xb940ae68, 0x350000c8, 0xf9406a68, 0x7941fa69, 0x78766908, 0x6b09011f, 0x540000a2,
    0xa9427bf3, 0xa94153f5, 0xa8c35bf7, 0xd65f03c0, 0xf9400268, 0x92407e81, 0xaa1303e0, 0xa9427bf3, 0xf9403102,
    0xa94153f5, 0xa8c35bf7, 0xd61f0040 };
constexpr uint32_t kDisCategory = 1;   // what CEnableComponentNode::Enter passes (w2 = 1)

inline bool DisableOk(const uint32_t* w) {
    for (int i = 0; i < kDisWords; ++i) {
        uint32_t mask = 0xffffffffu;
        if (i == kDisBl) mask = ~kBlMask;
        for (int k : kDisImm) if (i == k) mask = ~kLdrImmMask;
        if ((w[i] & mask) != (kDis[i] & mask)) return false;
    }
    return true;
}

// F3 (the source build's tools E2, EC-NATIVES read 2026-10-17 on the three libr15 files): vtable slot +0x208
// = <system>::EnableComponent(SCompactPoolHandle, EComponentEnableCategory); 282 of the 287 systems share this
// 47-word body (CAbilityCS r15+0x13e8cdc is the reference; the same 282 as kDis; CR15TouchInteractCS r15+0x1d1f17c is
// one): the disable mask &= ~category, SwapEntry (the bl, word kEnBl) into the active count, then a tail call of vtable
// +0x58 OnComponentEnabled. The systems differ in the imm12 of nine words (kEnImm) and the bl's displacement.
constexpr uint64_t kVtEnable = 0x208;
constexpr int kEnWords = 47, kEnBl = 23;
constexpr int kEnImm[9] = { 3, 8, 9, 19, 22, 24, 26, 30, 31 };
constexpr uint32_t kEn[kEnWords] = {
    0xf81d0ff6, 0xa90153f5, 0xa9027bf3, 0xf9406808, 0x92403c29, 0xd37ef535, 0xaa0103f3, 0x78756901, 0xf9407408,
    0x7941f809, 0xaa0003f4, 0xd37ff82a, 0x786a690b, 0x6b09003f, 0x1a9f27f6, 0x0a220169, 0x72003d3f, 0x782a6909,
    0x54000121, 0x7941fa82, 0x6b02003f, 0x540000c3, 0x91032280, 0x97ed3dae, 0x7941fa88, 0x11000508, 0x7901fa88,
    0xb940ae88, 0x2a160108, 0x350001c8, 0xf9406a88, 0x7941fa89, 0x78756908, 0x6b09011f, 0x54000122, 0xf9400288,
    0x92407e61, 0xaa1403e0, 0xa9427bf3, 0xf9402d02, 0xa94153f5, 0xf84307f6, 0xd61f0040, 0xa9427bf3, 0xa94153f5,
    0xf84307f6, 0xd65f03c0 };

inline bool EnableOk(const uint32_t* w) {
    for (int i = 0; i < kEnWords; ++i) {
        uint32_t mask = 0xffffffffu;
        if (i == kEnBl) mask = ~kBlMask;
        for (int k : kEnImm) if (i == k) mask = ~kLdrImmMask;
        if ((w[i] & mask) != (kEn[i] & mask)) return false;
    }
    return true;
}

// The per-component disable mask, read the way both bodies read it (plain loads): slot = u16 [[sys+idx] + (handle &
// 0xffff) * 4], mask = u16 [[sys+maskArr] + slot * 2] (bit 0 = category 1 set = off). The offsets come from the
// system's own Enable body: idx = word 3's imm12 * 8, maskArr = word 8's imm12 * 8 (CR15TouchInteractCS 0xc8 / 0xe0).
struct MaskAt { uint32_t idx, maskArr; };
inline uint32_t Imm12(uint32_t w) { return (w >> 10) & 0xfff; }
inline MaskAt MaskOffsets(const uint32_t* en) { return MaskAt{ Imm12(en[3]) * 8, Imm12(en[8]) * 8 }; }

// Rule R1' (EC-NATIVES stage 1, the source build's tools):
// the damageable going ON opens a group (the slot of the same (system, handle) is reused, C2) and takes the pending
// components younger than kPendingMs whose actor is unknown or the release's (C1; other actors' stay pending); any
// other component going ON joins this actor's live group, else the newest live group younger than kJoinMs when the
// actors agree (or one is unknown), else it waits in the pending list. A drone Enter in another game space than the
// table's clears the table first (DK1-2: a level change). R2: a 'K' marks the newest live group with that index dead
// and, with `due`, every component of it due for a disable. R3: a component of a dead group younger than kSwallowMs
// that goes ON again is due again (it is disabled after the game's own Enter: by effect, DK-3 kept). Due bits expire
// kDueMs after they were set (DK1-2). The caller holds its own lock around every call (DK-5: never across a game call).
constexpr uint64_t kJoinMs = 5000, kPendingMs = 3000, kSwallowMs = 25000, kDueMs = 2000;
constexpr int kGroupComps = 20, kGroups = 8, kPending = 20;

// owner = [system+0x80] read at the Enter that recorded it; space / actor = Actor()'s out-parameters there (0 = unknown)
struct Comp { uint64_t system = 0, sysSym = 0, compSym = 0; uint32_t handle = 0; uint64_t ms = 0; uint64_t actor = 0, space = 0, owner = 0; };
struct Group {
    uint64_t system = 0;   // the damageable system
    uint32_t handle = 0;   // its handle (index = the low 16 bits)
    uint64_t ms = 0, actor = 0;
    int n = 0, overflow = 0, number = 0;   // number 0 = a free slot
    bool dead = false;     // a 'K' named it
    uint32_t due = 0;      // bit i = comps[i] is to be disabled
    uint64_t dueMs = 0;
    Comp comps[kGroupComps];
};
// group 0 = none (the component went to the pending list); again = R3: a dead group's component, due again
struct Joined { int group; int pendingJoined; bool pending; bool again; };
struct Due { Comp c; int number; int idx; };

inline int Bits(uint32_t v) { int n = 0; for (; v; v &= v - 1) ++n; return n; }

struct Table {
    Group groups[kGroups];
    int made = 0;          // groups so far = the last group's number
    Comp pending[kPending];
    int pendingN = 0;
    uint64_t pendingDropped = 0;
    uint64_t space = 0;    // the game space of the table's components (0 = none yet)
    uint32_t spaceClears = 0;
    uint64_t expired = 0;  // due bits that ran out (kDueMs)

    void Clear() {
        for (auto& g : groups) g = Group();
        pendingN = 0;
    }
    Group* Newest() {   // the last group made (any state; tests and lines)
        Group* best = nullptr;
        for (auto& g : groups) if (g.number && (!best || g.number > best->number)) best = &g;
        return best;
    }
    Group* ByNumber(int number) {
        for (auto& g : groups) if (number && g.number == number) return &g;
        return nullptr;
    }
    // upstream Newest(actor): this actor's live group, else the newest live one younger than kJoinMs
    Group* Newest(uint64_t actor, uint64_t nowMs) {
        Group* best = nullptr;
        if (actor)
            for (auto& g : groups)
                if (g.number && !g.dead && g.actor == actor && (!best || g.number > best->number)) best = &g;
        if (best) return best;
        for (auto& g : groups)
            if (g.number && !g.dead && nowMs - g.ms < kJoinMs && (!best || g.number > best->number)) best = &g;
        return best;
    }
    static void Add(Group& g, const Comp& c) {   // upstream AddComp: once per (system, handle)
        for (int i = 0; i < g.n; ++i) if (g.comps[i].system == c.system && g.comps[i].handle == c.handle) return;
        if (g.n < kGroupComps) g.comps[g.n++] = c; else ++g.overflow;
    }

    Joined On(uint64_t system, uint64_t sysSym, uint64_t compSym, uint32_t handle, uint64_t nowMs, uint64_t actor = 0,
              uint64_t spaceNow = 0, uint64_t owner = 0) {
        if (spaceNow && space && spaceNow != space) { Clear(); ++spaceClears; }
        if (spaceNow) space = spaceNow;
        Comp c = { system, sysSym, compSym, handle, nowMs };
        c.actor = actor;
        c.space = spaceNow;
        c.owner = owner;
        if (sysSym == kDamageable) {
            Group* g = nullptr;
            for (auto& x : groups) if (x.number && x.system == system && x.handle == handle) { g = &x; break; }   // C2
            if (!g) for (auto& x : groups) if (!x.number) { g = &x; break; }
            if (!g) { g = &groups[0]; for (auto& x : groups) if (x.number < g->number) g = &x; }   // the oldest
            *g = Group();
            g->system = system;
            g->handle = handle;
            g->ms = nowMs;
            g->actor = actor;
            g->number = ++made;
            g->comps[g->n++] = c;
            int joined = 0, kept = 0;
            for (int i = 0; i < pendingN; ++i) {
                const Comp& p = pending[i];
                if (nowMs - p.ms >= kPendingMs) continue;
                if (!actor || !p.actor || p.actor == actor) { Add(*g, p); ++joined; }
                else pending[kept++] = p;                       // another drone's, released next
            }
            pendingN = kept;
            return { g->number, joined, false, false };
        }
        for (auto& g : groups) {   // R3
            if (!g.number || !g.dead || nowMs - g.ms >= kSwallowMs) continue;
            for (int i = 0; i < g.n; ++i)
                if (g.comps[i].system == system && g.comps[i].handle == handle) {
                    g.due |= 1u << i;
                    g.dueMs = nowMs;
                    return { g.number, 0, false, true };
                }
        }
        Group* g = Newest(actor, nowMs);
        if (g && (!actor || !g->actor || g->actor == actor)) {
            Add(*g, c);
            return { g->number, 0, false, false };
        }
        if (pendingN == kPending) {   // drop the oldest
            memmove(pending, pending + 1, sizeof(Comp) * (kPending - 1));
            --pendingN;
            ++pendingDropped;
        }
        pending[pendingN++] = c;
        return { 0, 0, true, false };
    }

    // 'K' for a damageable index: the newest live group whose index's low 16 bits match (upstream's compare). Marks it
    // dead (and with `due` every component due) and returns it; null = no live group has that index.
    Group* Killed(uint32_t index, uint64_t nowMs = 0, bool due = false) {
        Group* best = nullptr;
        for (auto& g : groups)
            if (g.number && !g.dead && (g.handle & 0xffff) == (index & 0xffff) && (!best || g.number > best->number)) best = &g;
        if (!best) return nullptr;
        best->dead = true;
        if (due && best->n) {
            best->due = best->n >= 32 ? ~0u : (1u << best->n) - 1;
            best->dueMs = nowMs;
        }
        return best;
    }

    // the due components (at most cap), oldest group first is not needed: any order; expired bits are dropped
    int TakeDue(uint64_t nowMs, Due* out, int cap) {
        int n = 0;
        for (auto& g : groups) {
            if (!g.number || !g.due) continue;
            if (nowMs - g.dueMs >= kDueMs) { expired += Bits(g.due); g.due = 0; continue; }
            for (int i = 0; i < g.n && n < cap; ++i)
                if (g.due & (1u << i)) out[n++] = { g.comps[i], g.number, i };
        }
        return n;
    }
    void Done(int number, int idx) {
        if (Group* g = ByNumber(number)) g->due &= ~(1u << idx);
    }
    bool AnyDue() const {
        for (const auto& g : groups) if (g.number && g.due) return true;
        return false;
    }

    // "live: #<index> ..." for the K line; returns the number of live groups
    int Live(uint32_t* indexes, int cap) const {
        int n = 0;
        for (const auto& g : groups)
            if (g.number && !g.dead && n < cap) indexes[n++] = g.handle & 0xffff;
        return n;
    }
    // F7 (AUD-7 DFC-1): the game space recorded at the Enter of a table component whose system's [+0x80] was owner
    // (0 = none: the caller does not use a space it has not seen with that owner)
    uint64_t SpaceOf(uint64_t owner) const {
        if (!owner) return 0;
        for (const auto& g : groups)
            for (int i = 0; g.number && i < g.n; ++i) if (g.comps[i].owner == owner && g.comps[i].space) return g.comps[i].space;
        for (int i = 0; i < pendingN; ++i) if (pending[i].owner == owner && pending[i].space) return pending[i].space;
        return 0;
    }
    // F3 TouchLive (upstream, stricter: the full handle with its generation): (system, handle) is
    // a component of a group that is not dead
    bool LiveComp(uint64_t system, uint32_t handle) const {
        for (const auto& g : groups) {
            if (!g.number || g.dead) continue;
            for (int i = 0; i < g.n; ++i) if (g.comps[i].system == system && g.comps[i].handle == handle) return true;
        }
        return false;
    }
    // F3-1 (EC-DRONE2 drone L1 read): a touch record never joins a group (the script's Enter disables it and the RE-ON
    // enable is a direct EnableComponent), so its liveness is its actor's: a live (not dead) group of the same actor
    bool LiveActor(uint64_t actor) const {
        if (!actor) return false;
        for (const auto& g : groups) if (g.number && !g.dead && g.actor == actor) return true;
        return false;
    }
};

// F3 (the source build's tools section 2; upstream): the drone's R14TouchInteract stays on until
// the drone's first shot. By effect: the script's own disable runs (Enter untouched), the record is RE-ON (on again at
// the net tick), then HELD; the drone's first shot (any FireBulletLocal, local or remote) binds the HELD record
// deployed last (< kLockMs) to its auto-target handle and makes it OFF-DUE (off at the net tick). Due states expire
// kDueMs after they were set (as the group table's bits). The caller holds its lock around every call.
constexpr uint64_t kR14Touch = 0x2E40131422F7B1FCull;      // CSymbol64("R14TouchInteract") = CR15TouchInteractCS
constexpr uint64_t kLockMs = 120000;
constexpr int kTouches = 16;
enum TouchState : uint8_t { kTouchFree = 0, kTouchReOn, kTouchHeld, kTouchOffDue, kTouchDone };
struct TouchRec { Comp c; uint32_t atHandle = 0; uint64_t deployed = 0, dueMs = 0; TouchState state = kTouchFree; };
struct TouchTable {
    TouchRec t[kTouches];
    uint64_t expired = 0;
    void Clear() { for (auto& r : t) r = TouchRec(); }
    // the script turned it off (a release): a new life for that (system, handle), else a free slot, else the
    // oldest deployed. Returns the slot.
    int Deployed(const Comp& c, uint64_t nowMs) {
        int slot = -1, oldest = 0;
        for (int i = 0; i < kTouches && slot < 0; ++i) if (t[i].state && t[i].c.system == c.system && t[i].c.handle == c.handle) slot = i;
        for (int i = 0; i < kTouches && slot < 0; ++i) if (!t[i].state) slot = i;
        for (int i = 1; i < kTouches; ++i) if (t[i].deployed < t[oldest].deployed) oldest = i;
        if (slot < 0) slot = oldest;
        t[slot] = TouchRec();
        t[slot].c = c;
        t[slot].deployed = nowMs;
        t[slot].dueMs = nowMs;
        t[slot].state = kTouchReOn;
        return slot;
    }
    // upstream LockedOn's pick: the record of this handle, else the HELD one with no handle deployed last
    // and < kLockMs ago takes it. Returns the slot made OFF-DUE, or -1 (none, or that drone is already locked on).
    int LockedOn(uint32_t atHandle, uint64_t nowMs) {
        int mine = -1, newest = -1;
        for (int i = 0; i < kTouches; ++i) {
            if (!t[i].state) continue;
            if (t[i].atHandle == atHandle) mine = i;
            else if (!t[i].atHandle && t[i].state == kTouchHeld && nowMs - t[i].deployed < kLockMs
                     && (newest < 0 || t[i].deployed > t[newest].deployed)) newest = i;
        }
        if (mine < 0 && newest >= 0) { mine = newest; t[mine].atHandle = atHandle; }
        if (mine < 0 || t[mine].state != kTouchHeld) return -1;
        t[mine].state = kTouchOffDue;
        t[mine].dueMs = nowMs;
        return mine;
    }
    bool AnyDue() const {
        for (const auto& r : t) if (r.state == kTouchReOn || r.state == kTouchOffDue) return true;
        return false;
    }
    // the due records (slots into out, at most cap); expired ones go to DONE (RE-ON expired = left off = the game's own
    // rule; OFF-DUE expired = left on, counted)
    int TakeDue(uint64_t nowMs, int* out, int cap) {
        int n = 0;
        for (int i = 0; i < kTouches; ++i) {
            if (t[i].state != kTouchReOn && t[i].state != kTouchOffDue) continue;
            if (nowMs - t[i].dueMs >= kDueMs) { ++expired; t[i].state = kTouchDone; continue; }
            if (n < cap) out[n++] = i;
        }
        return n;
    }
    void ReOnDone(int slot, bool ok) { if (t[slot].state == kTouchReOn) t[slot].state = ok ? kTouchHeld : kTouchDone; }
    void OffDone(int slot) { if (t[slot].state == kTouchOffDue) t[slot].state = kTouchDone; }
};

// F5 (section 3; upstream): each drone's last gun state, keyed by the captured SActor (upstream: the actor
// symbol, one state for every drone). Full = the oldest dropped.
constexpr uint64_t kNcaModel = 0x38EE951A26FB816Aull, kGun = 0x72F91FFFA17C3C8Cull, kSatellite = 0x106C63A2927B7A4Dull,
                   kPort = 0x45B09F0FA9F7FA33ull;
constexpr int kGuns = 32;
struct GunTable {
    struct G { uint64_t actor; uint32_t shown; };
    G g[kGuns] = {};
    void Clear() { for (auto& x : g) x = G{ 0, 0 }; }
    void Set(uint64_t actor, uint32_t shown) {
        if (!actor) return;
        int slot = -1;
        for (int i = 0; i < kGuns && slot < 0; ++i) if (g[i].actor == actor) slot = i;
        for (int i = 0; i < kGuns && slot < 0; ++i) if (!g[i].actor) slot = i;
        if (slot < 0) { memmove(g, g + 1, sizeof g - sizeof g[0]); slot = kGuns - 1; }
        g[slot] = G{ actor, shown };
    }
    bool Shown(uint64_t actor, uint32_t* shown) const {
        if (!actor) return false;
        for (const auto& x : g) if (x.actor == actor) { *shown = x.shown; return true; }
        return false;
    }
};

// F7 (section 4; upstream): the velocity toward the target, computed after AttackTarget, applied at the
// net tick while it is younger than kChaseMs. p = the drone's position, q = the target's (both world).
constexpr uint64_t kNetPhysics = 0x8FEE6557F995E4A3ull;    // CSymbol64("R14NetPhysics") = CR15NetPhysicsCS (r15+0x2ba5e40)
constexpr uint64_t kNetPhysicsPhys = 0x170;                // CR15NetPhysicsCS +0x170 = its CPhysicsCS (Initialize r15+0x21773c4)
// the auto-target record (stride 0x90; cs+0xc8 index table, cs+0xf8 records) and the systems it names (E5, = PC)
constexpr uint64_t kAtIdx = 0xc8, kAtRecs = 0xf8, kAtStride = 0x90, kAtTransforms = 0x130, kAtPhysics = 0x138;
constexpr uint64_t kAtHasTarget = 0x63, kAtTransform = 0x7c, kAtTargetIdx = 0x80, kAtPhysIdx = 0x84, kAtTargetSys = 0x88;
// the target's damageable (CR15NetDamageableCS): [ts+0xd0] index table, [ts+0x100] records stride 0xb8; record +0xc =
// its physics handle on [ts+0x140], +8 = its transform handle on [ts+0x138]; a CPhBody's position at +0x8f8
constexpr uint64_t kDmIdx = 0xd0, kDmRecs = 0x100, kDmStride = 0xb8, kDmPhysH = 0xc, kDmXfH = 0x8, kDmPhys = 0x140,
                   kDmXf = 0x138, kBodyPos = 0x8f8;
// CPhysicsCS: [phys+0xc8] index table, [phys+0x100] the owners (16 bytes per slot, the SActor first)
constexpr uint64_t kPhIdx = 0xc8, kPhOwners = 0x100;
constexpr float kChaseSpeed = 7.0f, kChaseDistance = 7.0f, kChaseGain = 2.0f, kChaseAbove = 0.5f;
constexpr uint64_t kChaseMs = 100;
constexpr int kChases = 16;
// false = no velocity (the two positions coincide: upstream returns without writing)
inline bool ChaseVelocity(const float* p, const float* q, float* v, float* distOut = nullptr) {
    const float d[3] = { q[0] - p[0], q[1] + kChaseAbove - p[1], q[2] - p[2] };
    const float dist2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    if (dist2 < 1e-6f) return false;
    const float dist = sqrtf(dist2);
    float speed = (dist - kChaseDistance) * kChaseGain;
    speed = speed > kChaseSpeed ? kChaseSpeed : speed < -kChaseSpeed ? -kChaseSpeed : speed;
    for (int i = 0; i < 3; ++i) v[i] = d[i] / dist * speed;
    if (distOut) *distOut = dist;
    return true;
}
struct Chase { uint64_t cs = 0, physics = 0, owner = 0, atOwner = 0; uint32_t handle = 0, phys = 0; float v[3] = {}; uint64_t ms = 0;
               bool moving = false, due = false; };
struct ChaseTable {
    Chase c[kChases];
    void Clear() { for (auto& x : c) x = Chase(); }
    Chase* Find(uint64_t cs, uint32_t handle) {
        for (auto& x : c) if (x.cs == cs && x.handle == handle) return &x;
        return nullptr;
    }
    // upstream ChaseTarget's slot pick: a free or a resting slot
    Chase* Take(uint64_t cs, uint32_t handle) {
        if (Chase* x = Find(cs, handle)) return x;
        for (auto& x : c)
            if (!x.cs || (!x.moving && !x.due)) { x = Chase(); x.cs = cs; x.handle = handle; return &x; }
        return nullptr;
    }
    bool AnyDue() const {
        for (const auto& x : c) if (x.due) return true;
        return false;
    }
};

}  // namespace dk
}  // namespace q15t

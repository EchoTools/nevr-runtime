#pragma once
// POWER BOOST's cone and link rules, for both builds (#2339): the marker decode, the cone geometry (Rotate, AngleTo), the angle a bubble
// carries in its radius and which bubble it names, the cone decision with its bubble table, and the link / unlink marker a client sends.
// It reads no memory and calls nothing of the game's, so it needs no Env. The hooks that feed it are runtime/combat/powerboost.cpp on PC
// (Hooked_Heal, LinkState, BubbleFor, ConeFilter) and quest/combat/powerboost.cpp on Quest (the heal wrappers, ConeAt, NetTick); each
// keeps its own reads of the bubble, the owner and the pose, its own clock, its own hooks and its own log lines. PC's server half (AddBuff,
// EndBuff, the damage reduction's default) and the Quest's link line filter (quest/combat/powerboost_core.h) are not rules of this header.
// The PC GTest (runtime/tests/test_powerboost_rules.cpp) and the Quest host test (quest/tests/powerboost_test.cpp) both compile this one
// definition.
//
// Origin. EchoCombat's `plugin/powerboost.cpp` (he_is_the_cat, heisthecat31/EchoCombat), as released in v2.0.0 (nevr-pack 4d1f0024, whose
// powerboost.cpp is c3ff4b54's; "EC" lines below are that file). PC ports c3ff4b54 (runtime/combat/powerboost.cpp); the Quest ported
// c14c3e0a through the reference build's `runtime/native/afkxr/afk_powerboost_core.h` (kotorvr's q15t-upstream). Rule for rule: the decode
// EC 381-393, Rotate and AngleTo EC 531-544, LeftSeconds EC 554-557, LinkMarker EC 565-567, the slack and the send interval EC 673-674,
// the bubble table EC 681-687, the angle and the bands EC 707-709, the owner's 0.3 m EC 731, the cone decision EC 751-769.
// Both builds hold these forms (#2339):
//   - The decode is EC's: the rez flag is subtracted, then the value past the band's base is rounded (`+ 0.5f`), so 1,020,009.75 is
//     v 20,010. The script's heal node writes whole amounts, on which rounding and truncating agree.
//   - A use or link amount whose rounding carries it out of its band (1,999,999.75 gives v 1,000,000) is declined (kDeclined): no buff, no
//     boost, no self-damage and no link, and it is not passed on as a heal. That is EC's effective outcome (dr 1.0 for 0 s does nothing),
//     stated as a decision. An unlink amount needs no v; it is never declined.
//   - dr_permille is kept as the integer it is. EC keeps `(v / 1000) / 1000.0f` and re-encodes `(unsigned)(dr * 1000.0f + 0.5f)`; for every
//     dr_permille a decode can give (0..999) that returns the same integer (the PC GTest checks all 1000), so the link marker is EC's.
//   - A radius that is not finite names no bubble. EC converts `(r - floorf(r)) * 100000` to int with no check, which for a NaN or an
//     infinite radius is undefined (C++17 [conv.fpint]/1; x86-64 gives INT_MIN, AArch64 0).
// The bands are EC's: an angle of 1..180 is POWER BOOST's bubble, 501..590 MEDIGUN's (PC's MediCone; the Quest has no MEDIGUN and leaves
// such a bubble to the game's own radius check), anything else neither.
//
// A NaN direction, distance or quaternion fails closed: AngleTo's clamp passes a NaN cosine through to acosf, so the angle is NaN, and
// `off <= angle + slack` and `dist <= range` are both false: the target is outside the cone and takes no new link. The NaN and non-finite
// guards in this header assume IEEE 754 floating point: building it with -ffast-math or -ffinite-math-only removes them.
#include <math.h>
#include <stdint.h>

#include <cmath>

namespace nevr_powerboost {

// ---- the markers. A heal amount of 1,000,000 or more that falls in a band is not a heal: base + v, v = dr_permille * 1000 + seconds.
//   [1e6, 2e6) use "I used POWER BOOST"     [3e6, 4e6) use + rez (the rez flag, 2e6, on top)
//   [2e6, 3e6) link "you are linked"        [4e6, 5e6) link + rez
//   [6e6, 7e6) unlink "this one left my cone", its buff ends: checked before the other bands, no rez, no dr; v = what is past its base
// Anything else (below 1e6, [5e6, 6e6), 7e6 and above, NaN, the infinities) is a heal.
constexpr float kMarkerMin = 1000000.0f;   // the least amount a marker can be
constexpr float kUseBase = 1000000.0f, kUseEnd = 2000000.0f;
constexpr float kLinkBase = 2000000.0f, kLinkEnd = 3000000.0f;
constexpr float kRezFlag = 2000000.0f;
constexpr float kUnlinkBase = 6000000.0f, kUnlinkEnd = 7000000.0f;
constexpr uint32_t kValueMax = 999999;     // the largest v a use or link band holds

// kDeclined: a use or link amount whose rounded value is outside its band (see Decode). It is not a heal and carries no buff.
enum MarkerKind : int { kHeal = 0, kUse = 1, kLink = 2, kUnlink = 3, kDeclined = 4 };

struct Marker { int kind; int rez; uint32_t v, drPermille, seconds; };

// What is past a band's base, rounded. `past` is in [0, 1e6) (the band compares run first), so the conversion is always in range; the
// result is at most 1,000,000.
inline uint32_t BandValue(float past) { return static_cast<uint32_t>(past + 0.5f); }

// The compares run before any conversion: a NaN fails every one, so no out-of-range float is converted. rez is 0 unless the kind is use
// or link. A use or link whose rounded v exceeds kValueMax is kDeclined (rez kept, v, dr and seconds 0).
inline Marker Decode(float amount) {
    Marker m{ kHeal, 0, 0, 0, 0 };
    if (amount >= kUnlinkBase && amount < kUnlinkEnd) {
        m.kind = kUnlink;
        m.v = BandValue(amount - kUnlinkBase);
        return m;
    }
    const bool rez = amount >= kUseBase + kRezFlag;
    const float a = rez ? amount - kRezFlag : amount;
    if (a >= kUseBase && a < kUseEnd) m.kind = kUse;
    else if (a >= kLinkBase && a < kLinkEnd) m.kind = kLink;
    else return m;
    m.rez = rez ? 1 : 0;
    const uint32_t v = BandValue(a - (m.kind == kUse ? kUseBase : kLinkBase));
    if (v > kValueMax) {
        m.kind = kDeclined;
        return m;
    }
    m.v = v;
    m.drPermille = v / 1000u;
    m.seconds = v % 1000u;
    return m;
}

// ---- the cone geometry

// q (x, y, z, w) applied to v
inline void Rotate(const float* q, const float* v, float* out) {
    const float t[3] = { 2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0]) };
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

// degrees between the owner's axis (turned by q) and d, which is dist long
inline float AngleTo(const float* q, const float* axis, const float* d, float dist) {
    float f[3];
    Rotate(q, axis, f);
    const float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    const float c = (f[0] * d[0] + f[1] * d[1] + f[2] * d[2]) / (len * dist);
    return acosf(c < -1 ? -1 : c > 1 ? 1 : c) * 57.29578f;
}

// The angle a bubble carries in its radius (whole metres + angle / 100000; 12.00037 is 12 m, 37 degrees). A radius that is not finite
// carries none (0).
inline int ConeAngle(float radius) {
    if (!std::isfinite(radius)) return 0;
    return static_cast<int>((radius - floorf(radius)) * 100000.0f + 0.5f);
}

// Which bubble an angle names: 1..180 POWER BOOST's, 501..590 MEDIGUN's (its cone angle + 500), anything else neither (Repair Matrix's
// whole radius, among others): the cone leaves that one to the game.
enum class Bubble { kNeither, kPowerBoost, kMedigun };
inline Bubble BubbleOf(int angle) {
    if (angle > 500 && angle <= 590) return Bubble::kMedigun;
    if (angle >= 1 && angle <= 180) return Bubble::kPowerBoost;
    return Bubble::kNeither;
}

// The cone's reach: the bubble's whole metres (the tether reaches no further).
inline float ConeRange(float radius) { return floorf(radius); }

constexpr float kConeSlackDeg = 5.0f;     // a linked target leaves only this far past the cone's angle
constexpr uint64_t kLinkMinMs = 250;      // a target's change is sent at most this often (the latest state, once the time is up)
constexpr uint64_t kBubbleReuseMs = 2000; // a bubble record unseen this long is a new bubble (records are reused)
constexpr float kOwnerMinDist = 0.3f;     // closer than this to the aim's origin is the owner: never linked
constexpr uint64_t kUseWindowMs = 30000;  // a client sends link markers only within this long of its own use
constexpr int kBubbles = 8, kLinksPerBubble = 8;

struct ConeLink { uintptr_t dcs; uint32_t index; bool inside, sent; uint64_t sentAt; };   // sent: what the server has
struct BubbleLinks { uintptr_t bubble; uint64_t first, last; int count; ConeLink linked[kLinksPerBubble]; };

// The bubbles the cone has seen, each with its targets. Plain fields: the caller keeps it under its own lock.
struct ConeTable {
    BubbleLinks b[kBubbles] = {};

    // the bubble's entry; a new bubble takes the one seen longest ago
    BubbleLinks& For(uintptr_t bubble, uint64_t now) {
        BubbleLinks* oldest = &b[0];
        for (BubbleLinks& x : b) {
            if (x.bubble == bubble && now - x.last < kBubbleReuseMs) { x.last = now; return x; }
            if (x.last < oldest->last) oldest = &x;
        }
        *oldest = BubbleLinks{};
        oldest->bubble = bubble;
        oldest->first = oldest->last = now;
        return *oldest;
    }
};

struct ConeVerdict { bool ok, send; };

// The cone decision for a target dist m away and off degrees from the aim, in a bubble with this radius and angle (the owner already
// excluded: dist >= kOwnerMinDist). ok: linked now. send: tell the server (a change, at most every kLinkMinMs).
inline ConeVerdict ConeDecide(ConeTable& t, uintptr_t bubble, uintptr_t dcs, uint32_t index, float radius, int angle, float dist,
                              float off, uint64_t now) {
    const float range = ConeRange(radius);
    BubbleLinks& bl = t.For(bubble, now);
    int picked = -1;
    for (int i = 0; i < bl.count; ++i)
        if (bl.linked[i].dcs == dcs && (bl.linked[i].index & 0xffff) == (index & 0xffff)) picked = i;
    const bool was = picked >= 0 && bl.linked[picked].inside;   // linked: the cone is a little wider to leave
    bool ok = off <= static_cast<float>(angle) + (was ? kConeSlackDeg : 0.0f) && dist <= range;
    if (picked >= 0) {                                           // one of the bubble's targets: follows the cone
        bl.linked[picked].inside = ok;
    } else if (ok && bl.count < kLinksPerBubble) {               // a new target in the cone
        picked = bl.count++;
        bl.linked[picked] = ConeLink{ dcs, index, true, false, 0 };
    } else {
        ok = false;
    }
    bool send = false;
    if (picked >= 0 && bl.linked[picked].sent != ok && now - bl.linked[picked].sentAt >= kLinkMinMs) {
        bl.linked[picked].sent = ok;
        bl.linked[picked].sentAt = now;
        send = true;
    }
    return ConeVerdict{ ok, send };
}

// ---- the link marker

// this client's last POWER BOOST use (recorded where the use marker is sent; usedAt 0: none)
struct UseState { uint64_t usedAt; uint32_t drPermille, seconds; bool rez; };

// the use's remaining seconds, rounded up; the full seconds once the use has run out; at least 1
inline uint32_t LeftSeconds(const UseState& u, uint64_t now) {
    uint32_t left = u.seconds;
    if (u.usedAt && now - u.usedAt < u.seconds * 1000ull)
        left = static_cast<uint32_t>((u.seconds * 1000ull - (now - u.usedAt) + 999) / 1000);
    return left < 1 ? 1 : left;
}

// The marker a client sends for a link made (linked) or broken, or 0: nothing to send (no use of ours, or none within kUseWindowMs).
inline float LinkMarker(const UseState& u, bool linked, uint64_t now) {
    if (u.usedAt == 0 || now - u.usedAt > kUseWindowMs) return 0.0f;
    if (!linked) return kUnlinkBase;
    return kLinkBase + static_cast<float>(u.drPermille * 1000u + LeftSeconds(u, now)) + (u.rez ? kRezFlag : 0.0f);
}

}  // namespace nevr_powerboost

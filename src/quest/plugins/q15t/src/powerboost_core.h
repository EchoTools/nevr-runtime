// powerboost_core.h: the platform-free part of powerboost.cpp. A port, rule for rule, of the source build's
// <source build>/the source build's powerboost_core (RE-39 E-N2; the source build's tools,
// audit items PC-1..PC-10; stage 2 PB-1..PB-9): the marker
// decode of EC's POWER BOOST (upstream rules U1 / U2), the link line's repeat filter, the cone
// and the link / unlink markers. Tested by src/quest/tests/q15t/powerboost_test.cpp. Nothing here calls the game.
#pragma once
#include <math.h>
#include <stdint.h>

namespace q15t {
namespace pbcore {

// A heal amount of 1,000,000 or more is never a heal: base + v, v = dr_permille * 1000 + seconds.
//   [1e6, 2e6) kind 1 "I used POWER BOOST"       [3e6, 4e6) kind 1 + rez
//   [2e6, 3e6) kind 2 "you are linked"           [4e6, 5e6) kind 2 + rez
//   [6e6, 7e6) kind 3 "unlinked" (1.1.7: "this one left my cone", its buff ends; checked before
//              the other bands, no rez, no value: the sender puts the base alone; v = what is past it)
// Anything else (below 1e6, [5e6, 6e6), 7e6 and above, NaN, infinities) is kind 0.
constexpr float kMarkerMin = 1000000.0f, kMarkerEnd = 5000000.0f;
constexpr float kUnlinkMin = 6000000.0f, kUnlinkEnd = 7000000.0f;

struct Marker { int kind; int rez; uint32_t v, drPermille, seconds; };

// The compare runs before the conversion: a NaN fails both tests, so no out-of-range float is ever converted.
// Every integer below 2^24 is exact in a float and the ranges end at 7e6, so the truncation loses only a fraction.
inline Marker Decode(float amount) {
    Marker m{ 0, 0, 0, 0, 0 };
    if (amount >= kUnlinkMin && amount < kUnlinkEnd) {
        m.kind = 3;
        m.v = static_cast<uint32_t>(amount) - 6000000u;
        return m;
    }
    if (!(amount >= kMarkerMin) || !(amount < kMarkerEnd)) return m;
    const uint32_t a = static_cast<uint32_t>(amount);
    const uint32_t band = a / 1000000u;   // 1..4
    m.rez = band >= 3 ? 1 : 0;
    m.kind = (band == 1 || band == 3) ? 1 : 2;
    m.v = a - band * 1000000u;
    m.drPermille = m.v / 1000u;
    m.seconds = m.v % 1000u;
    return m;
}

// The link call can come every update of a bubble: one line per (bubble, target) change, or per kLinkRepeatMs for the
// same pair. Plain fields: the caller keeps it under its own lock (table operation only).
constexpr uint64_t kLinkRepeatMs = 1000;
struct LinkFilter {
    uint64_t key = ~0ull, atMs = 0;
    bool have = false;
    bool Show(uint32_t bubble, uint32_t target, uint64_t nowMs) {
        const uint64_t k = (static_cast<uint64_t>(bubble) << 32) | target;
        if (have && k == key && nowMs - atMs < kLinkRepeatMs) return false;
        have = true;
        key = k;
        atMs = nowMs;
        return true;
    }
};

// ---- stage 2 (P4, PB-1..PB-9): the cone filter and the
// link / unlink markers, upstream "pb" rule for rule. Plain fields: the caller keeps the
// table under its own lock (table operation only, PB-4).

// q (x, y, z, w) applied to v
inline void Rotate(const float* q, const float* v, float* out) {
    const float t[3] = { 2 * (q[1] * v[2] - q[2] * v[1]), 2 * (q[2] * v[0] - q[0] * v[2]), 2 * (q[0] * v[1] - q[1] * v[0]) };
    out[0] = v[0] + q[3] * t[0] + (q[1] * t[2] - q[2] * t[1]);
    out[1] = v[1] + q[3] * t[1] + (q[2] * t[0] - q[0] * t[2]);
    out[2] = v[2] + q[3] * t[2] + (q[0] * t[1] - q[1] * t[0]);
}

// degrees between the owner's axis (turned by q) and d
inline float AngleTo(const float* q, const float* axis, const float* d, float dist) {
    float f[3];
    Rotate(q, axis, f);
    const float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    const float c = (f[0] * d[0] + f[1] * d[1] + f[2] * d[2]) / (len * dist);
    return acosf(c < -1 ? -1 : c > 1 ? 1 : c) * 57.29578f;
}

// the angle a POWER BOOST bubble carries in its radius (metres + angle / 100000); outside 1..180 = not
// POWER BOOST's bubble (Repair Matrix etc.): the cone leaves it alone
inline int ConeAngle(float radius) { return static_cast<int>((radius - floorf(radius)) * 100000.0f + 0.5f); }

constexpr float kConeSlackDeg = 5.0f;
constexpr uint64_t kLinkMinMs = 250;
constexpr uint64_t kBubbleReuseMs = 2000; // a bubble record unseen for 2 s is a new bubble
constexpr float kOwnerMinDist = 0.3f;
constexpr uint64_t kUseWindowMs = 30000;
constexpr float kLinkBase = 2000000.0f, kUnlinkBase = 6000000.0f, kRezFlag = 2000000.0f;   // REZ_FLAG

struct ConeLink { uintptr_t dcs; uint32_t index; bool inside, sent; uint64_t sentAt; };
struct BubbleLinks { uintptr_t bubble; uint64_t first, last; int count; ConeLink linked[8]; };

struct ConeTable {
    BubbleLinks b[8] = {};

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

// the cone decision for a target at dist m, off degrees from the aim, of a bubble with this radius / angle (dist >= 0.3)
inline ConeVerdict ConeDecide(ConeTable& t, uintptr_t bubble, uintptr_t dcs, uint32_t index, float radius, int angle, float dist,
                              float off, uint64_t now) {
    const float range = floorf(radius);
    BubbleLinks& bl = t.For(bubble, now);
    int picked = -1;
    for (int i = 0; i < bl.count; ++i)
        if (bl.linked[i].dcs == dcs && (bl.linked[i].index & 0xffff) == (index & 0xffff)) picked = i;
    const bool was = picked >= 0 && bl.linked[picked].inside;   // linked: the cone is a little wider to leave
    bool ok = off <= static_cast<float>(angle) + (was ? kConeSlackDeg : 0.0f) && dist <= range;
    if (picked >= 0) {
        bl.linked[picked].inside = ok;
    } else if (ok && bl.count < 8) {
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

// this client's last POWER BOOST use (recorded where the use marker is sent)
struct UseState { uint64_t usedAt; uint32_t drPermille, seconds; bool rez; };

// the use's remaining whole seconds, at least 1
inline uint32_t LeftSeconds(const UseState& u, uint64_t now) {
    uint32_t left = u.seconds;
    if (u.usedAt && now - u.usedAt < u.seconds * 1000ull)
        left = static_cast<uint32_t>((u.seconds * 1000ull - (now - u.usedAt) + 999) / 1000);
    return left < 1 ? 1 : left;
}

// the marker LinkState sends, or 0 = nothing (no use of ours within 30 s)
inline float LinkMarker(const UseState& u, bool linked, uint64_t now) {
    if (u.usedAt == 0 || now - u.usedAt > kUseWindowMs) return 0.0f;
    if (!linked) return kUnlinkBase;
    return kLinkBase + static_cast<float>(u.drPermille * 1000u + LeftSeconds(u, now)) + (u.rez ? kRezFlag : 0.0f);
}

}  // namespace pbcore
}  // namespace q15t

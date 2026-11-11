// The drone kill rules (patch/dronekill_rules.h, #2359): the symbols, the chase velocity, the gun table and the group table. The hooks
// that feed them are runtime/combat/dronefix.cpp (PC) and quest/combat/dronekill.cpp (Quest); what they read from the game and how they
// turn a component on or off need the game and are proven by a live run. The Quest host test (quest/tests/dronekill_test.cpp) compiles
// this same header. "EC <n>" is plugin/dronefix.cpp of EchoCombat v2.0.0 (nevr-pack 4d1f0024), the line each case pins.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "runtime/patch/dronekill_rules.h"

namespace {

namespace dk = nevr_dronekill;
using Table = dk::Groups<>;

// systems and handles: a damageable system, a model system, a sound system; the handles' low 16 bits are the index
constexpr uint64_t kDmg = 0xd0, kModel = 0xa0, kSound = 0xb0;
constexpr uint64_t kA = 0xaaa1, kB = 0xbbb2;   // two actors
constexpr uint64_t kT0 = 100000;               // a clock far from 0, so no age wraps

// the slot of the group whose damageable is (kDmg, handle)
int Slot(const Table& t, uint32_t handle) { return t.Find(kDmg, handle); }

bool InGroup(const Table& t, int slot, uint64_t system, uint32_t handle) {
  if (slot < 0) return false;
  for (int i = 0; i < t.g[slot].n; ++i)
    if (t.g[slot].comps[i].system == system && t.g[slot].comps[i].handle == handle) return true;
  return false;
}

bool Pending(const Table& t, uint64_t system, uint32_t handle) {
  for (int i = 0; i < t.pendingN; ++i)
    if (t.pending[i].system == system && t.pending[i].handle == handle) return true;
  return false;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the symbols

TEST(DroneKillRules, TheSymbolsAreEchoCombats) {
  EXPECT_EQ(dk::kNcaModel, 0x38EE951A26FB816Aull);       // EC 75
  EXPECT_EQ(dk::kGun, 0x72F91FFFA17C3C8Cull);            // EC 76
  EXPECT_EQ(dk::kSatellite, 0x106C63A2927B7A4Dull);      // EC 77
  EXPECT_EQ(dk::kPort, 0x45B09F0FA9F7FA33ull);           // EC 80
  EXPECT_EQ(dk::kDroneActor, 0x10E40287A9BF5A13ull);     // EC 196
  EXPECT_EQ(dk::kDamageable, 0xC9DC08CFC2F5920Full);     // EC 214
  EXPECT_EQ(dk::kTouchInteract, 0x2E40131422F7B1FCull);  // EC 785
}

TEST(DroneKillRules, TheSizesAndWindowsAreEchoCombats) {
  EXPECT_EQ(dk::kGroups, 8);              // EC 483
  EXPECT_EQ(dk::kGroupComps, 20);         // EC 480, 520
  EXPECT_EQ(dk::kPending, 16);            // EC 487, 580 (D5)
  EXPECT_EQ(dk::kPendingMs, 3000u);       // EC 567
  EXPECT_EQ(dk::kJoinMs, 5000u);          // EC 513
  EXPECT_EQ(dk::kSwallowMs, 25000u);      // EC 525
  EXPECT_EQ(dk::kGuns, 32);               // EC 750
  EXPECT_EQ(dk::kChaseSpeed, 7.0f);       // EC 1083
  EXPECT_EQ(dk::kChaseDistance, 7.0f);    // EC 1084
  EXPECT_EQ(dk::kChaseGain, 2.0f);        // EC 1085
  EXPECT_EQ(dk::kChaseAbove, 0.5f);       // EC 1086
  EXPECT_EQ(dk::kChaseMinDist, 1e-3f);    // EC 1133 (D1)
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the chase (EC 1131-1136). Every expected value below is the float v2.0.0's arithmetic gives, computed by its own lines outside this
// header (each within one ulp of the real-number formula); the triangles are chosen so that the distance is exact.

struct ChaseCase { const char* name; float p[3], q[3]; float dist, speed, v[3]; };

TEST(DroneKillRules, ChaseVelocityIsEchoCombatsFormula) {
  const ChaseCase cases[] = {
      // the issue's own: the point 0.5 m above the target, 10 m away: 6 m/s toward it
      {"10 m: 6 m/s", {0, 0, 0}, {10, -0.5f, 0}, 10.0f, 6.0f, {6.0f, 0.0f, 0.0f}},
      {"7 m: holds", {0, 0, 0}, {0, -0.5f, 7}, 7.0f, 0.0f, {0.0f, 0.0f, 0.0f}},
      {"2 m: backs off at -7", {0, 0, 0}, {0, -0.5f, 2}, 2.0f, -7.0f, {0.0f, 0.0f, -7.0f}},
      {"20 m: 7 at most", {0, 0, 0}, {0, -0.5f, 20}, 20.0f, 7.0f, {0.0f, 0.0f, 7.0f}},
      {"7.75 m: 1.5", {0, 0, 0}, {0, -0.5f, 7.75f}, 7.75f, 1.5f, {0.0f, 0.0f, 1.5f}},
      // d = (1, 4, 8): 9 m, 4 m/s
      {"(1,4,8)", {1, 2, 3}, {2, 5.5f, 11}, 9.0f, 4.0f, {0x1.c71c72p-2f, 0x1.c71c72p+0f, 0x1.c71c72p+1f}},
      // d = (1, 2, 2): 3 m, -8 clamped to -7
      {"(1,2,2)", {1, 2, 3}, {2, 3.5f, 5}, 3.0f, -7.0f, {-0x1.2aaaacp+1f, -0x1.2aaaacp+2f, -0x1.2aaaacp+2f}},
      // d = (2, 3, 6): 7 m, 0
      {"(2,3,6)", {1, 2, 3}, {3, 4.5f, 9}, 7.0f, 0.0f, {0.0f, 0.0f, 0.0f}},
      // d = (2, 6, 9): 11 m, 8 clamped to 7
      {"(2,6,9)", {1, 2, 3}, {3, 7.5f, 12}, 11.0f, 7.0f, {0x1.45d174p+0f, 0x1.e8ba3p+1f, 0x1.6e8ba2p+2f}},
      // d = (4, 4, 7): 9 m, 4
      {"(4,4,7)", {-1, -2, -3}, {3, 1.5f, 4}, 9.0f, 4.0f, {0x1.c71c72p+0f, 0x1.c71c72p+0f, 0x1.8e38e4p+1f}},
      // d = (2, 4, 4): 6 m, -2
      {"(2,4,4)", {-1, -2, -3}, {1, 1.5f, 1}, 6.0f, -2.0f, {-0x1.555556p-1f, -0x1.555556p+0f, -0x1.555556p+0f}},
  };
  for (const ChaseCase& c : cases) {
    float v[3] = {99, 99, 99}, dist = 0, speed = 0;
    ASSERT_TRUE(dk::ChaseVelocity(c.p, c.q, v, &dist, &speed)) << c.name;
    EXPECT_EQ(dist, c.dist) << c.name;
    EXPECT_EQ(speed, c.speed) << c.name;
    EXPECT_EQ(v[0], c.v[0]) << c.name;
    EXPECT_EQ(v[1], c.v[1]) << c.name;
    EXPECT_EQ(v[2], c.v[2]) << c.name;
  }
}

TEST(DroneKillRules, TheAimPointIsHalfAMetreAboveTheTarget) {
  // EC 1131: d[1] = q[1] + 0.5 - p[1]. A target level with the drone, 20 m away: the velocity climbs.
  const float p[3] = {0, 0, 0}, q[3] = {0, 0, 20};
  float v[3] = {}, dist = 0;
  ASSERT_TRUE(dk::ChaseVelocity(p, q, v, &dist));
  EXPECT_EQ(dist, sqrtf(0.25f + 400.0f));
  EXPECT_EQ(v[1], 0.5f / dist * 7.0f);
  EXPECT_GT(v[1], 0.0f);
}

TEST(DroneKillRules, NoVelocityLeavesTheOutputsAlone) {
  const float p[3] = {0, 0, 0}, q[3] = {0, -0.5f, 0};   // on the aim point
  float v[3] = {1, 2, 3}, dist = 4, speed = 5;
  EXPECT_FALSE(dk::ChaseVelocity(p, q, v, &dist, &speed));
  EXPECT_EQ(v[0], 1.0f);
  EXPECT_EQ(v[1], 2.0f);
  EXPECT_EQ(v[2], 3.0f);
  EXPECT_EQ(dist, 4.0f);
  EXPECT_EQ(speed, 5.0f);
}

// D1: `dist < 1e-3f` on the distance (EC 1133). 0.5 mm: none; 2 mm: one; at 1e-3f exactly one, and at the float below it none.
TEST(DroneKillRules, D1TheCoincidentThresholdIsAMillimetreOnTheDistance) {
  const float p[3] = {0, 0, 0};
  float v[3] = {}, dist = 0;
  const float halfMm[3] = {0.0005f, -0.5f, 0};
  EXPECT_FALSE(dk::ChaseVelocity(p, halfMm, v)) << "0.5 mm from the aim point";
  const float twoMm[3] = {0.002f, -0.5f, 0};
  ASSERT_TRUE(dk::ChaseVelocity(p, twoMm, v, &dist)) << "2 mm from the aim point";
  EXPECT_EQ(dist, 0.002f);
  EXPECT_EQ(v[0], -7.0f);
  const float at[3] = {1e-3f, -0.5f, 0};
  ASSERT_TRUE(dk::ChaseVelocity(p, at, v, &dist)) << "exactly 1e-3f: not below it";
  EXPECT_EQ(dist, 1e-3f);
  EXPECT_EQ(v[0], -7.0f);
  const float below[3] = {std::nextafter(1e-3f, 0.0f), -0.5f, 0};   // 0x1.0624dcp-10
  EXPECT_FALSE(dk::ChaseVelocity(p, below, v)) << "the float below 1e-3f";
}

// D2: a NaN or infinite position, or a distance too large for a float, gives no velocity. v2.0.0 writes NaN (and (0,0,0) for the last).
// The inputs pass through volatile, so the compiler cannot fold them.
TEST(DroneKillRules, D2ANonFinitePositionOrDistanceGivesNoVelocity) {
  volatile float nan = std::numeric_limits<float>::quiet_NaN();
  volatile float inf = std::numeric_limits<float>::infinity();
  float v[3] = {1, 2, 3};
  {
    const float p[3] = {nan, 0, 0}, q[3] = {10, -0.5f, 0};
    EXPECT_FALSE(dk::ChaseVelocity(p, q, v)) << "a NaN drone position";
  }
  {
    const float p[3] = {0, 0, 0}, q[3] = {0, nan, 10};
    EXPECT_FALSE(dk::ChaseVelocity(p, q, v)) << "a NaN target";
  }
  {
    const float p[3] = {0, 0, 0}, q[3] = {inf, -0.5f, 0};
    EXPECT_FALSE(dk::ChaseVelocity(p, q, v)) << "an infinite target (v2.0.0: inf / inf * 7 = NaN)";
  }
  {
    const float p[3] = {0, 0, -inf}, q[3] = {0, -0.5f, 0};
    EXPECT_FALSE(dk::ChaseVelocity(p, q, v)) << "an infinite drone position";
  }
  {
    const float p[3] = {inf, 0, 0}, q[3] = {inf, -0.5f, 0};
    EXPECT_FALSE(dk::ChaseVelocity(p, q, v)) << "both infinite (d = inf - inf = NaN)";
  }
  {
    volatile float big = 3e38f;
    const float p[3] = {-big, 0, 0}, q[3] = {big, -0.5f, 0};
    EXPECT_FALSE(dk::ChaseVelocity(p, q, v)) << "finite positions whose difference overflows (v2.0.0: NaN)";
  }
  {
    volatile float big = 2e38f;
    const float p[3] = {0, 0, 0}, q[3] = {big, big, 0};
    EXPECT_FALSE(dk::ChaseVelocity(p, q, v)) << "a finite d whose distance overflows (v2.0.0: (0, 0, 0))";
  }
  EXPECT_EQ(v[0], 1.0f);
  EXPECT_EQ(v[1], 2.0f);
  EXPECT_EQ(v[2], 3.0f);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the gun table (EC 749-775)

TEST(DroneKillRules, AGunStateIsUpdatedInPlace) {
  dk::GunTable t;
  uint32_t shown = 9;
  EXPECT_FALSE(t.Shown(dk::kDroneActor, &shown));
  EXPECT_EQ(shown, 9u) << "an unknown key leaves the output";
  t.Set(dk::kDroneActor, 1);
  t.Set(0x77, 1);
  t.Set(dk::kDroneActor, 0);
  ASSERT_TRUE(t.Shown(dk::kDroneActor, &shown));
  EXPECT_EQ(shown, 0u);
  EXPECT_EQ(t.e[0].key, dk::kDroneActor) << "updated where it was (EC 756-757), not appended";
  EXPECT_EQ(t.e[1].key, 0x77u);
  EXPECT_EQ(t.e[2].key, 0u);
}

TEST(DroneKillRules, The33rdKeyDropsTheOldest) {
  dk::GunTable t;
  for (uint64_t k = 1; k <= 32; ++k) t.Set(0x100 + k, static_cast<uint32_t>(k & 1));
  t.Set(0x101, 0);   // an update keeps 0x101 where it is: still the oldest
  t.Set(0x200, 1);   // EC 760-762: full, the oldest goes and the rest move down
  uint32_t shown = 9;
  EXPECT_FALSE(t.Shown(0x101, &shown));
  ASSERT_TRUE(t.Shown(0x102, &shown));
  EXPECT_EQ(shown, 0u);
  ASSERT_TRUE(t.Shown(0x200, &shown));
  EXPECT_EQ(shown, 1u);
  EXPECT_EQ(t.e[0].key, 0x102u);
  EXPECT_EQ(t.e[31].key, 0x200u);
  t.Set(0x201, 0);
  EXPECT_FALSE(t.Shown(0x102, &shown)) << "and the next oldest next";
  EXPECT_TRUE(t.Shown(0x103, &shown));
}

// D3: key 0 is ignored by Set and unknown to Shown. In v2.0.0 Set(0, x) writes into the first free slot, and Shown(0) reads it.
TEST(DroneKillRules, D3KeyZeroIsIgnored) {
  dk::GunTable t;
  t.Set(0, 1);
  uint32_t shown = 9;
  EXPECT_FALSE(t.Shown(0, &shown)) << "a fresh table: Shown(0) must not read a free slot";
  EXPECT_EQ(shown, 9u);
  for (const auto& e : t.e) {
    EXPECT_EQ(e.key, 0u);
    EXPECT_EQ(e.shown, 0u) << "Set(0, 1) wrote into a free slot";
  }
  t.Set(0x55, 1);
  t.Set(0, 0);
  ASSERT_TRUE(t.Shown(0x55, &shown));
  EXPECT_EQ(shown, 1u);
  EXPECT_EQ(t.e[1].shown, 0u);
  // a full table: Set(0, x) forgets nobody
  dk::GunTable f;
  for (uint64_t k = 1; k <= 32; ++k) f.Set(k, 1);
  f.Set(0, 0);
  EXPECT_TRUE(f.Shown(1, &shown)) << "Set(0) dropped the oldest";
  EXPECT_FALSE(f.Shown(0, &shown));
}

// D4: one gun state per key, and both callers pass the node's actor symbol: two drones share kDroneActor's state, and a kill hides it
// (EC 621: SetGun(DRONE_ACTOR, 0)) whichever drone died.
TEST(DroneKillRules, D4OneStatePerActorSymbolAndAKillHidesTheDroneSymbols) {
  dk::GunTable t;
  t.Set(dk::kDroneActor, 1);   // drone 1 shows its gun (EC 940: SetGun(data[0], ...))
  t.Set(dk::kDroneActor, 0);   // drone 2 hides its: the one state follows
  uint32_t shown = 9;
  ASSERT_TRUE(t.Shown(dk::kDroneActor, &shown));
  EXPECT_EQ(shown, 0u);
  t.Set(dk::kDroneActor, 1);
  t.Set(0x77, 1);              // another actor's state
  dk::HideGunOnKill(t);
  ASSERT_TRUE(t.Shown(dk::kDroneActor, &shown));
  EXPECT_EQ(shown, 0u) << "a kill hides the drone symbol's state";
  ASSERT_TRUE(t.Shown(0x77, &shown));
  EXPECT_EQ(shown, 1u) << "and only that key's";
  dk::GunTable fresh;
  dk::HideGunOnKill(fresh);
  ASSERT_TRUE(fresh.Shown(dk::kDroneActor, &shown)) << "a kill with no state yet records hidden";
  EXPECT_EQ(shown, 0u);
}

TEST(DroneKillRules, ClearForgetsEveryGunState) {
  dk::GunTable t;
  t.Set(dk::kDroneActor, 1);
  t.Clear();
  uint32_t shown = 9;
  EXPECT_FALSE(t.Shown(dk::kDroneActor, &shown));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the groups: a release (EC 552-573)

TEST(DroneKillRules, AReleaseOpensAGroupWithItsDamageableFirst) {
  Table t;
  const dk::Verdict r = t.Release(kDmg, 0x50011, kA, 0, kT0);
  ASSERT_EQ(r.group, 0);
  EXPECT_EQ(r.comp, 0);
  EXPECT_FALSE(r.pending);
  EXPECT_FALSE(r.swallowed);
  const auto& g = t.g[0];
  EXPECT_TRUE(g.used);
  EXPECT_FALSE(g.dead);
  EXPECT_EQ(g.system, kDmg);
  EXPECT_EQ(g.handle, 0x50011u);
  EXPECT_EQ(g.opened, kT0);
  EXPECT_EQ(g.actor, kA);
  ASSERT_EQ(g.n, 1);
  EXPECT_EQ(g.comps[0].system, kDmg);
  EXPECT_EQ(g.comps[0].handle, 0x50011u);
}

TEST(DroneKillRules, ASecondReleaseOfTheSameDamageableReusesItsSlot) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  t.Release(kDmg, 0x12, kB, 0, kT0 + 10);
  t.Enable(kSound, 6, kA, 0, kT0 + 20, true);   // joins kA's group
  ASSERT_EQ(t.g[0].n, 2);
  ASSERT_EQ(t.KillIndex(0x11), 0);
  const dk::Verdict r = t.Release(kDmg, 0x11, kA, 0, kT0 + 40000);   // EC 553: the same (system, index)
  EXPECT_EQ(r.group, 0);
  EXPECT_FALSE(t.g[0].dead) << "a new life";
  EXPECT_EQ(t.g[0].n, 1) << "its components start again";
  EXPECT_EQ(t.g[0].opened, kT0 + 40000);
  EXPECT_EQ(Slot(t, 0x12), 1);
  EXPECT_EQ(Slot(t, 0x11), 0);
}

TEST(DroneKillRules, ANewReleaseTakesTheFirstFreeSlot) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  t.Release(kDmg, 0x12, kA, 0, kT0 + 1);
  t.Release(kDmg, 0x13, kA, 0, kT0 + 2);
  EXPECT_EQ(Slot(t, 0x11), 0);
  EXPECT_EQ(Slot(t, 0x12), 1);
  EXPECT_EQ(Slot(t, 0x13), 2);
}

// EC 555: with every slot used, the one released first goes, even a live one
TEST(DroneKillRules, TheNinthReleaseReplacesTheOldest) {
  Table t;
  const uint64_t opened[8] = {kT0 + 50, kT0 + 10, kT0 + 70, kT0 + 30, kT0 + 80, kT0 + 60, kT0 + 20, kT0 + 40};
  for (int i = 0; i < 8; ++i) t.Release(kDmg, 0x100u + static_cast<uint32_t>(i), kA, 0, opened[i]);
  const dk::Verdict r = t.Release(kDmg, 0x200, kB, 0, kT0 + 90);
  EXPECT_EQ(r.group, 1) << "slot 1 was released first";
  EXPECT_EQ(Slot(t, 0x101), -1);
  EXPECT_EQ(Slot(t, 0x200), 1);
  t.Release(kDmg, 0x201, kB, 0, kT0 + 91);
  EXPECT_EQ(Slot(t, 0x201), 6) << "then slot 6 (kT0 + 20)";
}

// D7: two slots released in the same millisecond, the oldest: the first in table order
TEST(DroneKillRules, D7TheOldestOnATieIsTheFirstInTableOrder) {
  Table t;
  for (int i = 0; i < 8; ++i) t.Release(kDmg, 0x100u + static_cast<uint32_t>(i), kA, 0, i == 3 || i == 5 ? kT0 : kT0 + 100);
  EXPECT_EQ(t.Release(kDmg, 0x200, kA, 0, kT0 + 200).group, 3);
  EXPECT_EQ(t.Release(kDmg, 0x201, kA, 0, kT0 + 201).group, 5);
  // all eight in the same millisecond: slot 0
  Table s;
  for (int i = 0; i < 8; ++i) s.Release(kDmg, 0x100u + static_cast<uint32_t>(i), kA, 0, kT0);
  EXPECT_EQ(s.Release(kDmg, 0x200, kA, 0, kT0).group, 0);
}

TEST(DroneKillRules, ADamageableIsInOneGroupOnly) {
  Table t;
  for (int round = 0; round < 3; ++round)
    for (uint32_t h = 0; h < 12; ++h) t.Release(kDmg, 0x10 + h % 5, kA, 0, kT0 + round * 100 + h);
  for (int i = 0; i < dk::kGroups; ++i)
    for (int k = i + 1; k < dk::kGroups; ++k)
      if (t.g[i].used && t.g[k].used) {
        EXPECT_FALSE(t.g[i].system == t.g[k].system && t.g[i].handle == t.g[k].handle) << i << " " << k;
      }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the pending list at a release (EC 565-573)

TEST(DroneKillRules, AReleaseTakesPendingYoungerThan3000Ms) {
  Table t;
  t.Enable(kModel, 1, kA, 0, kT0, true);              // 3000 ms before the release
  t.Enable(kModel, 2, kA, 0, kT0 + 1, true);          // 2999 ms
  ASSERT_EQ(t.pendingN, 2);
  const dk::Verdict r = t.Release(kDmg, 0x11, kA, 0, kT0 + 3000);
  EXPECT_EQ(r.pendingJoined, 1);
  EXPECT_TRUE(InGroup(t, r.group, kModel, 2)) << "2999 ms joins";
  EXPECT_FALSE(InGroup(t, r.group, kModel, 1)) << "3000 ms does not (EC 567: `>= 3000` continues)";
  EXPECT_FALSE(Pending(t, kModel, 1)) << "and it is forgotten, not kept";
  EXPECT_EQ(t.pendingN, 0);
}

TEST(DroneKillRules, APendingComponentOfAnotherActorStaysPending) {
  Table t;
  t.Enable(kModel, 1, kA, 0, kT0, true);
  t.Enable(kModel, 2, kB, 0, kT0, true);
  t.Enable(kModel, 3, 0, 0, kT0, true);               // actor unknown
  const dk::Verdict r = t.Release(kDmg, 0x11, kA, 0, kT0 + 100);
  EXPECT_EQ(r.pendingJoined, 2);
  EXPECT_TRUE(InGroup(t, r.group, kModel, 1)) << "its own actor's";
  EXPECT_TRUE(InGroup(t, r.group, kModel, 3)) << "an unknown actor's (EC 568)";
  EXPECT_FALSE(InGroup(t, r.group, kModel, 2));
  ASSERT_EQ(t.pendingN, 1);
  EXPECT_EQ(t.pending[0].handle, 2u) << "another drone's, released next (EC 571)";
  const dk::Verdict rb = t.Release(kDmg, 0x12, kB, 0, kT0 + 200);
  EXPECT_TRUE(InGroup(t, rb.group, kModel, 2));
  EXPECT_EQ(t.pendingN, 0);
}

TEST(DroneKillRules, AReleaseOfAnUnknownActorTakesEveryYoungPending) {
  Table t;
  t.Enable(kModel, 1, kA, 0, kT0, true);
  t.Enable(kModel, 2, kB, 0, kT0, true);
  const dk::Verdict r = t.Release(kDmg, 0x11, 0, 0, kT0 + 100);
  EXPECT_EQ(r.pendingJoined, 2);
  EXPECT_EQ(t.pendingN, 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// a component joins (EC 507-521, 575-582)

TEST(DroneKillRules, AComponentJoinsItsActorsGroupAtAnyAge) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  t.Release(kDmg, 0x12, kB, 0, kT0 + 100);
  const dk::Verdict r = t.Enable(kSound, 6, kA, 0, kT0 + 9000, true);   // EC 509-510: no time limit for its own
  EXPECT_EQ(r.group, Slot(t, 0x11));
  EXPECT_FALSE(r.pending);
  EXPECT_EQ(r.comp, 1);
}

TEST(DroneKillRules, WithNoGroupOfItsActorItJoinsTheNewestLiveGroupYoungerThan5000Ms) {
  Table t;
  t.Release(kDmg, 0x11, 0, 0, kT0);         // a group of an unknown actor
  t.Release(kDmg, 0x12, 0, 0, kT0 + 100);   // the newest
  dk::Verdict r = t.Enable(kSound, 6, kA, 0, kT0 + 5099, true);
  EXPECT_EQ(r.group, Slot(t, 0x12)) << "4999 ms after the newest release";
  r = t.Enable(kSound, 7, kA, 0, kT0 + 5100, true);
  EXPECT_TRUE(r.pending) << "5000 ms: no longer (EC 513: `< 5000`)";
  EXPECT_EQ(r.group, -1);
  EXPECT_TRUE(Pending(t, kSound, 7));
}

TEST(DroneKillRules, AGroupOfAnotherActorDoesNotTakeIt) {
  Table t;
  t.Release(kDmg, 0x11, kB, 0, kT0);
  const dk::Verdict r = t.Enable(kSound, 6, kA, 0, kT0 + 10, true);   // EC 578: the actors disagree
  EXPECT_TRUE(r.pending);
  EXPECT_FALSE(InGroup(t, Slot(t, 0x11), kSound, 6));
  const dk::Verdict u = t.Enable(kSound, 7, 0, 0, kT0 + 10, true);    // an unknown actor joins it
  EXPECT_EQ(u.group, Slot(t, 0x11));
}

TEST(DroneKillRules, ADeadGroupTakesNoComponent) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  t.KillIndex(0x11);
  const dk::Verdict r = t.Enable(kSound, 6, kA, 0, kT0 + 10, true);
  EXPECT_TRUE(r.pending);
}

TEST(DroneKillRules, AComponentIsInAGroupOnceAndThe21stIsDropped) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  dk::Verdict r = t.Enable(kSound, 1, kA, 0, kT0 + 1, true);
  EXPECT_EQ(r.comp, 1);
  r = t.Enable(kSound, 1, kA, 0, kT0 + 2, true);
  EXPECT_EQ(r.comp, 1) << "once (EC 518-519)";
  EXPECT_EQ(t.g[0].n, 2);
  for (uint32_t h = 2; h <= 19; ++h) t.Enable(kSound, h, kA, 0, kT0 + 3, true);
  ASSERT_EQ(t.g[0].n, 20);
  r = t.Enable(kSound, 20, kA, 0, kT0 + 4, true);   // the 21st
  EXPECT_EQ(r.group, 0);
  EXPECT_EQ(r.comp, -1);
  EXPECT_EQ(r.full, 1);
  EXPECT_FALSE(r.pending) << "dropped, not pending (EC 520)";
  EXPECT_EQ(t.g[0].n, 20);
  EXPECT_FALSE(InGroup(t, 0, kSound, 20));
}

TEST(DroneKillRules, AReleaseCountsThePendingAGroupHadNoRoomFor) {
  Table t;
  for (uint32_t h = 1; h <= 16; ++h) t.Enable(kModel, h, kA, 0, kT0, true);
  const dk::Verdict r = t.Release(kDmg, 0x11, kA, 0, kT0 + 1);
  EXPECT_EQ(r.pendingJoined, 16);
  EXPECT_EQ(t.g[r.group].n, 17);
  EXPECT_EQ(r.full, 0);
}

// D7: two live groups of one actor: its first in table order; two newest released in the same millisecond: the first in table order
TEST(DroneKillRules, D7TwoLiveGroupsOfOneActorTheFirstInTableOrder) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0 + 500);   // slot 0, the newer
  t.Release(kDmg, 0x12, kA, 0, kT0);         // slot 1
  const dk::Verdict r = t.Enable(kSound, 6, kA, 0, kT0 + 600, true);
  EXPECT_EQ(r.group, 0) << "EC 509-510 returns the first match, not the newest";
  Table u;
  u.Release(kDmg, 0x11, kA, 0, kT0);         // slot 0
  u.Release(kDmg, 0x12, kA, 0, kT0 + 500);   // slot 1, the newer
  EXPECT_EQ(u.Enable(kSound, 6, kA, 0, kT0 + 600, true).group, 0) << "still the first in table order";
}

TEST(DroneKillRules, D7TheNewestByReleaseTimeThenTableOrder) {
  Table t;
  t.Release(kDmg, 0x11, 0, 0, kT0 + 200);    // slot 0, newest
  t.Release(kDmg, 0x12, 0, 0, kT0 + 100);    // slot 1
  EXPECT_EQ(t.Enable(kSound, 6, kA, 0, kT0 + 300, true).group, 0) << "release time, not table position";
  Table s;
  s.Release(kDmg, 0x11, 0, 0, kT0 + 100);    // slot 0
  s.Release(kDmg, 0x12, 0, 0, kT0 + 200);    // slot 1, newest
  EXPECT_EQ(s.Enable(kSound, 6, kA, 0, kT0 + 300, true).group, 1);
  Table tie;
  tie.Release(kDmg, 0x11, 0, 0, kT0 + 50);
  tie.Release(kDmg, 0x12, 0, 0, kT0 + 100);   // slot 1
  tie.Release(kDmg, 0x13, 0, 0, kT0 + 100);   // slot 2, the same millisecond
  EXPECT_EQ(tie.Enable(kSound, 6, kA, 0, kT0 + 300, true).group, 1) << "a tie: the first in table order (EC 513: `>`)";
  EXPECT_EQ(tie.Newest(kA, kT0 + 300), 1);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// D5, D6: the pending list holds 16; a component that finds it full empties it whole, then is added (EC 580)

TEST(DroneKillRules, D5D6The17thPendingComponentEmptiesTheListThenIsAdded) {
  Table t;
  for (uint32_t h = 1; h <= 16; ++h) {
    const dk::Verdict r = t.Enable(kModel, h, kA, 0, kT0 + h, true);
    EXPECT_TRUE(r.pending) << h;
  }
  ASSERT_EQ(t.pendingN, 16) << "16 fit";
  EXPECT_EQ(t.pendingDropped, 0u);
  const dk::Verdict r = t.Enable(kModel, 17, kA, 0, kT0 + 17, true);
  EXPECT_TRUE(r.pending);
  ASSERT_EQ(t.pendingN, 1) << "every pending component went";
  EXPECT_EQ(t.pending[0].handle, 17u);
  EXPECT_EQ(t.pendingDropped, 16u);
  for (uint32_t h = 1; h <= 16; ++h) EXPECT_FALSE(Pending(t, kModel, h)) << h;
  for (uint32_t h = 18; h <= 32; ++h) t.Enable(kModel, h, kA, 0, kT0 + h, true);
  ASSERT_EQ(t.pendingN, 16);
  t.Enable(kModel, 33, kA, 0, kT0 + 33, true);
  EXPECT_EQ(t.pendingN, 1) << "and again at the next full list";
  EXPECT_EQ(t.pendingDropped, 32u);
  // a release then takes only what is left
  const dk::Verdict rel = t.Release(kDmg, 0x11, kA, 0, kT0 + 40);
  EXPECT_EQ(rel.pendingJoined, 1);
  EXPECT_TRUE(InGroup(t, rel.group, kModel, 33));
  EXPECT_FALSE(InGroup(t, rel.group, kModel, 32));
}

TEST(DroneKillRules, D6AFullListOf16ThatAReleaseEmptiedTakesTheNextWithoutDropping) {
  Table t;
  for (uint32_t h = 1; h <= 16; ++h) t.Enable(kModel, h, kB, 0, kT0, true);
  t.Release(kDmg, 0x11, kB, 0, kT0 + 1);
  ASSERT_EQ(t.pendingN, 0);
  t.Enable(kModel, 99, kA, 0, kT0 + 2, true);
  EXPECT_EQ(t.pendingN, 1);
  EXPECT_EQ(t.pendingDropped, 0u);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// D8: a dead group's component that goes on again is swallowed up to and including 25000 ms after the release (EC 534: `> 25000` ends it)

TEST(DroneKillRules, D8TheSwallowWindowIncludes25000Ms) {
  for (const uint64_t age : {uint64_t{0}, uint64_t{1}, uint64_t{24999}, uint64_t{25000}}) {
    Table t;
    t.Release(kDmg, 0x11, kA, 0, kT0);
    t.Enable(kSound, 6, kA, 0, kT0 + 10, true);
    ASSERT_GE(t.KillIndex(0x11), 0);
    int comp = -9;
    EXPECT_EQ(t.Swallowing(kSound, 6, kT0 + age, &comp), 0) << age;
    EXPECT_EQ(comp, 1) << age;
    const dk::Verdict r = t.Enable(kSound, 6, kA, 0, kT0 + age, true);
    EXPECT_TRUE(r.swallowed) << age << " ms after the release";
    EXPECT_EQ(r.group, 0) << age;
    EXPECT_EQ(r.comp, 1) << age;
    EXPECT_FALSE(r.pending) << age;
  }
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  t.Enable(kSound, 6, kA, 0, kT0 + 10, true);
  t.KillIndex(0x11);
  int comp = -9;
  EXPECT_EQ(t.Swallowing(kSound, 6, kT0 + 25001, &comp), -1);
  const dk::Verdict r = t.Enable(kSound, 6, kA, 0, kT0 + 25001, true);
  EXPECT_FALSE(r.swallowed) << "25001 ms: let back";
  EXPECT_TRUE(r.pending) << "and, its group dead, it waits";
}

TEST(DroneKillRules, D8TheWindowIsCountedFromTheReleaseNotTheDeath) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  t.Enable(kSound, 6, kA, 0, kT0 + 10, true);
  t.Enable(kSound, 6, kA, 0, kT0 + 24000, true);   // still live
  t.KillIndex(0x11);                               // dies at 24000 ms
  EXPECT_FALSE(t.Enable(kSound, 6, kA, 0, kT0 + 25001, true).swallowed);
}

TEST(DroneKillRules, ALiveGroupsComponentIsNotSwallowed) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  t.Enable(kSound, 6, kA, 0, kT0 + 10, true);
  const dk::Verdict r = t.Enable(kSound, 6, kA, 0, kT0 + 20, true);
  EXPECT_FALSE(r.swallowed);
  EXPECT_EQ(r.group, 0);
}

TEST(DroneKillRules, AnEnableThatIsNotTheDroneScriptsIsOnlySwallowed) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  t.Enable(kSound, 6, kA, 0, kT0 + 10, true);
  dk::Verdict r = t.Enable(kSound, 99, 0, 0, kT0 + 20, false);   // PC: what == 0
  EXPECT_FALSE(r.swallowed);
  EXPECT_FALSE(r.pending);
  EXPECT_EQ(r.group, -1);
  EXPECT_FALSE(InGroup(t, 0, kSound, 99)) << "it joins nothing (EC 576: only what == 1)";
  t.KillIndex(0x11);
  r = t.Enable(kSound, 6, 0, 0, kT0 + 30, false);
  EXPECT_TRUE(r.swallowed) << "a dead group's component, whoever turns it on (EC 575)";
}

TEST(DroneKillRules, AReleaseIsNeverSwallowed) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  t.KillIndex(0x11);
  const dk::Verdict r = t.Release(kDmg, 0x11, kA, 0, kT0 + 100);   // EC 551: the release path has no swallow
  EXPECT_FALSE(r.swallowed);
  EXPECT_FALSE(t.g[r.group].dead);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the kill pick (EC 606-616)

TEST(DroneKillRules, AKillMarksTheLiveGroupWithThatIndexsLow16Bits) {
  Table t;
  t.Release(kDmg, 0x50011, kA, 0, kT0);
  t.Release(kDmg, 0x22, kB, 0, kT0 + 1);
  EXPECT_EQ(t.KillIndex(0xabc0011), 0) << "the low 16 bits of both";
  EXPECT_TRUE(t.g[0].dead);
  EXPECT_FALSE(t.g[1].dead);
  EXPECT_EQ(t.KillIndex(0x11), -1) << "a second kill of the same index finds none";
  EXPECT_EQ(t.KillIndex(0x33), -1);
}

// D9: two live groups with one index: the first in table order
TEST(DroneKillRules, D9TwoLiveGroupsWithOneIndexTheFirstInTableOrder) {
  Table t;
  t.Release(kDmg, 0x10011, kA, 0, kT0 + 500);   // slot 0
  t.Release(kDmg, 0x20011, kB, 0, kT0);         // slot 1, the older
  EXPECT_EQ(t.KillIndex(0x11), 0);
  EXPECT_EQ(t.KillIndex(0x11), 1) << "then the other";
  EXPECT_EQ(t.KillIndex(0x11), -1);
  Table u;
  u.Release(kDmg, 0x10011, kA, 0, kT0);         // slot 0, the older
  u.Release(kDmg, 0x20011, kB, 0, kT0 + 500);   // slot 1, the newer
  EXPECT_EQ(u.KillIndex(0x11), 0) << "table order, not the newest";
}

// ---------------------------------------------------------------------------------------------------------------------------------
// D11: the drone's own end marks its group dead (EC 645-653), by (system, handle)

TEST(DroneKillRules, D11MarkDeadByDamageable) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  t.Release(kDmg, 0x12, kA, 0, kT0 + 1);
  dk::DeadMark m = t.MarkDead(kDmg, 0x12);
  EXPECT_EQ(m.group, 1);
  EXPECT_TRUE(m.wasAlive);
  EXPECT_TRUE(t.g[1].dead);
  EXPECT_FALSE(t.g[0].dead);
  m = t.MarkDead(kDmg, 0x12);
  EXPECT_EQ(m.group, 1) << "a dead group is still found (EC 650: no liveness test)";
  EXPECT_FALSE(m.wasAlive);
  m = t.MarkDead(kDmg, 0x99);
  EXPECT_EQ(m.group, -1);
  EXPECT_EQ(t.MarkDead(kModel, 0x11).group, -1) << "the system is part of the key";
  EXPECT_EQ(t.MarkDead(kDmg, 0x10011).group, -1) << "the whole handle is the key, not its index";
}

TEST(DroneKillRules, D11AnExpiredGroupIsDead) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  t.Enable(kSound, 6, kA, 0, kT0 + 10, true);
  t.MarkDead(kDmg, 0x11);   // its lifetime ends at 20 s
  dk::Verdict r = t.Enable(kSound, 7, kA, 0, kT0 + 20100, true);
  EXPECT_TRUE(r.pending) << "a later component of its actor does not join it";
  r = t.Enable(kSound, 6, kA, 0, kT0 + 20200, true);
  EXPECT_TRUE(r.swallowed) << "its component is held off within 25000 ms of its release";
  EXPECT_EQ(t.KillIndex(0x11), -1) << "a kill of its index finds none";
  EXPECT_FALSE(t.LiveComp(kSound, 6));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// D10: a game space unlike the table's empties it; 0 clears nothing (PC passes 0)

TEST(DroneKillRules, D10GameSpaceZeroClearsNothing) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0x5000, kT0);
  t.Enable(kModel, 1, kB, 0x5000, kT0 + 1, true);
  dk::Verdict r = t.Enable(kSound, 6, kA, 0, kT0 + 2, true);
  EXPECT_FALSE(r.spaceCleared);
  EXPECT_EQ(r.group, 0);
  r = t.Release(kDmg, 0x12, kA, 0, kT0 + 3);
  EXPECT_FALSE(r.spaceCleared);
  EXPECT_EQ(Slot(t, 0x11), 0);
  EXPECT_EQ(t.space, 0x5000u) << "0 does not overwrite the table's space";
  EXPECT_EQ(t.spaceClears, 0u);
  // a PC table: every call passes 0, nothing is ever cleared
  Table pc;
  for (uint32_t h = 0; h < 6; ++h) pc.Release(kDmg, 0x20 + h, kA, 0, kT0 + h);
  pc.Enable(kModel, 1, kB, 0, kT0 + 10, true);
  EXPECT_EQ(pc.spaceClears, 0u);
  EXPECT_EQ(pc.pendingN, 1);
  EXPECT_EQ(Slot(pc, 0x25), 5);
}

TEST(DroneKillRules, D10AnotherGameSpaceEmptiesTheGroupsAndThePendingList) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0x5000, kT0);
  t.Enable(kModel, 1, kB, 0x5000, kT0 + 1, true);
  ASSERT_EQ(t.pendingN, 1);
  dk::Verdict r = t.Enable(kSound, 6, kA, 0x5000, kT0 + 2, true);
  EXPECT_FALSE(r.spaceCleared) << "the same space";
  r = t.Enable(kSound, 7, kA, 0x7000, kT0 + 3, true);
  EXPECT_TRUE(r.spaceCleared);
  EXPECT_EQ(Slot(t, 0x11), -1);
  EXPECT_TRUE(r.pending) << "cleared first, then the component waits";
  ASSERT_EQ(t.pendingN, 1);
  EXPECT_EQ(t.pending[0].handle, 7u);
  EXPECT_EQ(t.space, 0x7000u);
  EXPECT_EQ(t.spaceClears, 1u);
  r = t.Release(kDmg, 0x12, kA, 0x9000, kT0 + 4);
  EXPECT_TRUE(r.spaceCleared) << "a release observes the space too";
  EXPECT_EQ(r.pendingJoined, 0);
  EXPECT_EQ(t.spaceClears, 2u);
}

TEST(DroneKillRules, D10TheFirstSpaceClearsNothing) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0, kT0);
  const dk::Verdict r = t.Enable(kSound, 6, kA, 0x5000, kT0 + 1, true);
  EXPECT_FALSE(r.spaceCleared) << "the table had no space yet";
  EXPECT_EQ(r.group, 0);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// liveness for the touch rule (EC TouchLive)

TEST(DroneKillRules, LiveCompIsAComponentOfAGroupThatIsNotDead) {
  Table t;
  t.Release(kDmg, 0x5a, kA, 0, kT0);
  t.Enable(kSound, 6, kA, 0, kT0 + 1, true);
  EXPECT_TRUE(t.LiveComp(kDmg, 0x5a));
  EXPECT_TRUE(t.LiveComp(kSound, 6));
  EXPECT_FALSE(t.LiveComp(kSound, 7));
  EXPECT_FALSE(t.LiveComp(kModel, 6));
  t.KillIndex(0x5a);
  EXPECT_FALSE(t.LiveComp(kSound, 6));
  EXPECT_FALSE(t.LiveComp(kDmg, 0x5a));
}

TEST(DroneKillRules, ClearEmptiesTheGroupsAndThePendingList) {
  Table t;
  t.Release(kDmg, 0x11, kA, 0x5000, kT0);
  t.Enable(kModel, 1, kB, 0x5000, kT0, true);
  t.Clear();
  EXPECT_EQ(Slot(t, 0x11), -1);
  EXPECT_EQ(t.pendingN, 0);
  for (const auto& g : t.g) EXPECT_FALSE(g.used);
}

// the caller's own payloads ride with the component and the group
struct QComp { uint64_t owner = 0; };
struct QGroup { int number = 0; };
TEST(DroneKillRules, TheCallersPayloadsAreKept) {
  dk::Groups<QComp, QGroup> t;
  t.Enable(kModel, 1, kA, 0, kT0, true, QComp{0x6000});
  const dk::Verdict r = t.Release(kDmg, 0x11, kA, 0, kT0 + 1, QComp{0x6001});
  t.g[r.group].x.number = 7;
  t.Enable(kSound, 6, kA, 0, kT0 + 2, true, QComp{0x6002});
  ASSERT_EQ(t.g[r.group].n, 3);
  EXPECT_EQ(t.g[r.group].comps[0].x.owner, 0x6001u);
  EXPECT_EQ(t.g[r.group].comps[1].x.owner, 0x6000u);
  EXPECT_EQ(t.g[r.group].comps[1].at, kT0) << "a pending component keeps when it was seen";
  EXPECT_EQ(t.g[r.group].comps[2].x.owner, 0x6002u);
  EXPECT_EQ(t.g[r.group].x.number, 7);
  t.Release(kDmg, 0x11, kA, 0, kT0 + 3);
  EXPECT_EQ(t.g[r.group].x.number, 0) << "a release starts the group's payload again";
}

}  // namespace

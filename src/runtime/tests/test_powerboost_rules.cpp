// POWER BOOST's cone and link rules (patch/powerboost_rules.h, #2339). The hooks that feed them are runtime/combat/powerboost.cpp (PC:
// Hooked_Heal, LinkState, BubbleFor, ConeFilter) and quest/combat/powerboost.cpp (Quest); the reads of the bubble, the owner and the pose
// need the game and are proven by a live run. The Quest host test (quest/tests/powerboost_test.cpp) compiles this same header.
#include <gtest/gtest.h>

#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>

#include "runtime/patch/powerboost_rules.h"

namespace {

namespace pbr = nevr_powerboost;
using pbr::Bubble;

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

// The constants the ports read: the least marker (PC's hit boost skips anything from it up; the Quest's heal wrappers look at nothing
// below it) and the owner's distance (closer to the aim's origin than this is the owner, never linked).
TEST(PowerBoostRules, TheConstantsAreV200s) {
  EXPECT_EQ(pbr::kMarkerMin, 1000000.0f);
  EXPECT_EQ(pbr::kOwnerMinDist, 0.3f);
  EXPECT_EQ(pbr::kConeSlackDeg, 5.0f);
  EXPECT_EQ(pbr::kLinkMinMs, 250u);
  EXPECT_EQ(pbr::kBubbleReuseMs, 2000u);
  EXPECT_EQ(pbr::kUseWindowMs, 30000u);
}

// ---- Rotate and AngleTo

TEST(PowerBoostRules, TheIdentityQuaternionLeavesAVector) {
  const float q[4] = {0, 0, 0, 1};
  const float v[3] = {1.0f, -2.0f, 3.0f};
  float out[3] = {};
  pbr::Rotate(q, v, out);
  EXPECT_EQ(out[0], 1.0f);
  EXPECT_EQ(out[1], -2.0f);
  EXPECT_EQ(out[2], 3.0f);
}

TEST(PowerBoostRules, AQuarterTurnAboutYTakesPlusZToPlusX) {
  const float s = std::sqrt(0.5f);
  const float q[4] = {0, s, 0, s};  // (x, y, z, w): 90 degrees about +y
  const float z[3] = {0, 0, 1};
  float out[3] = {};
  pbr::Rotate(q, z, out);
  EXPECT_NEAR(out[0], 1.0f, 1e-6f);
  EXPECT_NEAR(out[1], 0.0f, 1e-6f);
  EXPECT_NEAR(out[2], 0.0f, 1e-6f);
}

// A third of a turn about (1, 1, 1) takes x to y, y to z and z to x; every quaternion matches its rotation matrix.
TEST(PowerBoostRules, EveryTurnMatchesTheRotationMatrix) {
  const float third[4] = {0.5f, 0.5f, 0.5f, 0.5f};
  const float x[3] = {1, 0, 0}, y[3] = {0, 1, 0}, z[3] = {0, 0, 1};
  float out[3] = {};
  pbr::Rotate(third, x, out);
  EXPECT_NEAR(out[0], 0.0f, 1e-6f);
  EXPECT_NEAR(out[1], 1.0f, 1e-6f);
  EXPECT_NEAR(out[2], 0.0f, 1e-6f);
  pbr::Rotate(third, y, out);
  EXPECT_NEAR(out[0], 0.0f, 1e-6f);
  EXPECT_NEAR(out[1], 0.0f, 1e-6f);
  EXPECT_NEAR(out[2], 1.0f, 1e-6f);
  pbr::Rotate(third, z, out);
  EXPECT_NEAR(out[0], 1.0f, 1e-6f);
  EXPECT_NEAR(out[1], 0.0f, 1e-6f);
  EXPECT_NEAR(out[2], 0.0f, 1e-6f);
  const float qs[][4] = {{0.1f, 0.7f, -0.3f, 0.6f}, {-0.5f, 0.2f, 0.4f, 0.74f}, {0.9f, -0.1f, 0.2f, 0.37f}};
  const float v[3] = {0.3f, -1.2f, 2.5f};
  for (const auto& raw : qs) {
    double n = 0;
    for (const float r : raw) n += static_cast<double>(r) * r;
    n = std::sqrt(n);
    float q[4];
    for (int i = 0; i < 4; ++i) q[i] = static_cast<float>(raw[i] / n);
    const double a = q[0], b = q[1], c = q[2], w = q[3];
    const double m[3][3] = {{1 - 2 * (b * b + c * c), 2 * (a * b - c * w), 2 * (a * c + b * w)},
                            {2 * (a * b + c * w), 1 - 2 * (a * a + c * c), 2 * (b * c - a * w)},
                            {2 * (a * c - b * w), 2 * (b * c + a * w), 1 - 2 * (a * a + b * b)}};
    pbr::Rotate(q, v, out);
    for (int i = 0; i < 3; ++i) EXPECT_NEAR(out[i], m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2], 1e-5) << i;
  }
}

// The axis need not be a unit vector: AngleTo divides its length out.
TEST(PowerBoostRules, TheAxisLengthIsDividedOut) {
  const float q[4] = {0, 0, 0, 1};
  const float axis[3] = {0, 0, 2};
  const float at60[3] = {4.0f * std::sin(1.0471976f), 0, 4.0f * std::cos(1.0471976f)};
  EXPECT_NEAR(pbr::AngleTo(q, axis, at60, 4.0f), 60.0f, 1e-3f);
}

TEST(PowerBoostRules, TheAngleIsZeroOnTheAxis180OnItsOppositeAnd90Across) {
  const float q[4] = {0, 0, 0, 1};
  const float axis[3] = {0, 0, 1};
  const float ahead[3] = {0, 0, 4};
  const float behind[3] = {0, 0, -4};
  const float across[3] = {4, 0, 0};
  EXPECT_NEAR(pbr::AngleTo(q, axis, ahead, 4.0f), 0.0f, 1e-3f);
  EXPECT_NEAR(pbr::AngleTo(q, axis, behind, 4.0f), 180.0f, 1e-3f);
  EXPECT_NEAR(pbr::AngleTo(q, axis, across, 4.0f), 90.0f, 1e-3f);
  const float s = std::sqrt(0.5f);
  const float turned[4] = {0, s, 0, s};  // the axis turned to +x: the target across is now ahead
  EXPECT_NEAR(pbr::AngleTo(turned, axis, across, 4.0f), 0.0f, 1e-2f);
}

TEST(PowerBoostRules, ACosineJustPastOneIsClamped) {
  const float q[4] = {0, 0, 0, 1};
  const float axis[3] = {0, 0, 1};
  const float ahead[3] = {0, 0, 4};
  EXPECT_EQ(pbr::AngleTo(q, axis, ahead, 3.9999f), 0.0f) << "a cosine above 1 is 1, not NaN";
  const float behind[3] = {0, 0, -4};
  EXPECT_NEAR(pbr::AngleTo(q, axis, behind, 3.9999f), 180.0f, 1e-3f) << "a cosine below -1 is -1";
}

// ---- the angle in the radius, and which bubble it names

TEST(PowerBoostRules, TheRadiusCarriesTheConeAngle) {
  EXPECT_EQ(pbr::ConeAngle(12.00037f), 37);
  EXPECT_EQ(pbr::ConeAngle(12.0f), 0) << "a whole radius carries no angle";
  EXPECT_EQ(pbr::ConeAngle(15.0018f), 180);
  EXPECT_EQ(pbr::ConeAngle(8.00545f), 545) << "MEDIGUN's tag: 45 degrees + 500";
  EXPECT_EQ(pbr::ConeRange(12.00037f), 12.0f);
}

TEST(PowerBoostRules, TheBandsAreV200s) {
  EXPECT_EQ(pbr::BubbleOf(1), Bubble::kPowerBoost);
  EXPECT_EQ(pbr::BubbleOf(37), Bubble::kPowerBoost);
  EXPECT_EQ(pbr::BubbleOf(180), Bubble::kPowerBoost);
  EXPECT_EQ(pbr::BubbleOf(501), Bubble::kMedigun);
  EXPECT_EQ(pbr::BubbleOf(545), Bubble::kMedigun);
  EXPECT_EQ(pbr::BubbleOf(590), Bubble::kMedigun);
  for (const int angle : {INT_MIN, -1, 0, 181, 500, 591, 99999})
    EXPECT_EQ(pbr::BubbleOf(angle), Bubble::kNeither) << angle;
}

// A radius that is not finite names no bubble (#2339). Through a volatile, so the compiler cannot fold the conversion: without the guard
// it is undefined, and x86-64's cvttss2si gives INT_MIN.
TEST(PowerBoostRules, ARadiusThatIsNotFiniteNamesNoBubble) {
  for (const float bad : {kNaN, kInf, -kInf}) {
    volatile float radius = bad;
    const int angle = pbr::ConeAngle(radius);
    EXPECT_EQ(angle, 0) << bad;
    EXPECT_EQ(pbr::BubbleOf(angle), Bubble::kNeither) << bad;
  }
}

// ---- the cone decision

constexpr uintptr_t kBubble = 0x1000, kDcs = 0x2000;

TEST(PowerBoostRules, ANewTargetInsideTheConeIsLinkedAndSent) {
  pbr::ConeTable t;
  const pbr::ConeVerdict v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 58.0f, 1000);
  EXPECT_TRUE(v.ok);
  EXPECT_TRUE(v.send);
}

TEST(PowerBoostRules, TheSlackIsForALinkedTargetOnly) {
  pbr::ConeTable t;
  pbr::ConeVerdict v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 62.0f, 1000);
  EXPECT_FALSE(v.ok) << "a new target 2 degrees past the cone: no slack";
  EXPECT_FALSE(v.send);
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 58.0f, 1100);
  EXPECT_TRUE(v.ok);
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 64.9f, 1400);
  EXPECT_TRUE(v.ok) << "linked: kept up to 5 degrees past the cone";
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 65.1f, 1700);
  EXPECT_FALSE(v.ok) << "past the slack: it leaves";
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 62.0f, 2000);
  EXPECT_FALSE(v.ok) << "once out it is a target outside again: no slack";
}

TEST(PowerBoostRules, TheRangeIsTheRadiusWholeMetres) {
  pbr::ConeTable t;
  EXPECT_FALSE(pbr::ConeDecide(t, kBubble, kDcs, 3, 15.9f, 60, 15.5f, 0.0f, 1000).ok) << "15.5 m is past floor(15.9) = 15";
  EXPECT_TRUE(pbr::ConeDecide(t, kBubble, kDcs, 4, 15.9f, 60, 15.0f, 0.0f, 1000).ok) << "15 m is inside";
}

TEST(PowerBoostRules, TheNinthTargetOfABubbleIsRefused) {
  pbr::ConeTable t;
  for (uint32_t i = 0; i < 8; ++i) EXPECT_TRUE(pbr::ConeDecide(t, kBubble, kDcs, i, 15.0f, 60, 5.0f, 0.0f, 1000).ok) << i;
  const pbr::ConeVerdict v = pbr::ConeDecide(t, kBubble, kDcs, 8, 15.0f, 60, 5.0f, 0.0f, 1000);
  EXPECT_FALSE(v.ok);
  EXPECT_FALSE(v.send);
  EXPECT_TRUE(pbr::ConeDecide(t, kBubble, kDcs, 7, 15.0f, 60, 5.0f, 0.0f, 1000).ok) << "the eighth is kept";
}

TEST(PowerBoostRules, ATargetIsKnownByItsDcsAndItsLow16Bits) {
  pbr::ConeTable t;
  EXPECT_TRUE(pbr::ConeDecide(t, kBubble, kDcs, 0x10003, 15.0f, 60, 5.0f, 0.0f, 1000).ok);
  EXPECT_TRUE(pbr::ConeDecide(t, kBubble, kDcs, 0x20003, 15.0f, 60, 5.0f, 62.0f, 1100).ok) << "the same target: slack";
  EXPECT_FALSE(pbr::ConeDecide(t, kBubble, kDcs + 8, 3, 15.0f, 60, 5.0f, 62.0f, 1100).ok) << "another damageable system: a new target";
}

TEST(PowerBoostRules, AChangeIsSentAtMostEvery250MsWithTheLatestState) {
  pbr::ConeTable t;
  EXPECT_TRUE(pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 0.0f, 1000).send);
  pbr::ConeVerdict v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 90.0f, 1100);
  EXPECT_FALSE(v.ok);
  EXPECT_FALSE(v.send) << "100 ms after the last send";
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 90.0f, 1249);
  EXPECT_FALSE(v.send) << "249 ms after the last send";
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 90.0f, 1250);
  EXPECT_FALSE(v.ok);
  EXPECT_TRUE(v.send) << "250 ms: the unlink goes out";
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 0.0f, 1300);
  EXPECT_TRUE(v.ok);
  EXPECT_FALSE(v.send);
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 90.0f, 1400);
  EXPECT_FALSE(v.send);
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 90.0f, 1600);
  EXPECT_FALSE(v.send) << "out again, and the server already has out: nothing to send";
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 0.0f, 1700);
  EXPECT_TRUE(v.ok);
  EXPECT_TRUE(v.send) << "the latest state, in, once the time is up";
}

TEST(PowerBoostRules, ABubbleUnseenFor2000MsIsANewBubble) {
  pbr::ConeTable t;
  pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 0.0f, 1000);
  EXPECT_TRUE(pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 62.0f, 2999).ok) << "seen 1999 ms ago: the same bubble, slack";
  EXPECT_FALSE(pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 62.0f, 4999).ok) << "unseen for 2000 ms: a new bubble, no slack";
  EXPECT_EQ(t.For(kBubble, 5000).first, 4999u) << "the new bubble starts at the call that renewed it";
}

TEST(PowerBoostRules, EachSightingRenewsTheBubble) {
  pbr::ConeTable t;
  pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 0.0f, 1000);
  EXPECT_TRUE(pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 62.0f, 2500).ok);
  EXPECT_TRUE(pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 10.0f, 62.0f, 4000).ok)
      << "seen 1500 ms ago, 3000 ms after it was made: the same bubble";
}

TEST(PowerBoostRules, TheOldestOfEightBubblesIsReplaced) {
  pbr::ConeTable t;
  for (int i = 0; i < 8; ++i) pbr::ConeDecide(t, kBubble + i, kDcs, 3, 15.0f, 60, 10.0f, 0.0f, 1000 + i);
  pbr::ConeDecide(t, kBubble + 8, kDcs, 3, 15.0f, 60, 10.0f, 0.0f, 1100);
  bool first = false, second = false, ninth = false;
  for (const pbr::BubbleLinks& b : t.b) {
    first = first || b.bubble == kBubble;
    second = second || b.bubble == kBubble + 1;
    ninth = ninth || b.bubble == kBubble + 8;
  }
  EXPECT_FALSE(first) << "the bubble seen longest ago gave up its entry";
  EXPECT_TRUE(second);
  EXPECT_TRUE(ninth);
}

// A NaN direction or distance fails closed: outside the cone, no new link (passes on PC's old lines too).
TEST(PowerBoostRules, ANaNDirectionOrDistanceLeavesTheTargetOutside) {
  const float q[4] = {0, 0, 0, 1};
  const float axis[3] = {0, 0, 1};
  const float d[3] = {0, kNaN, 4};
  const float off = pbr::AngleTo(q, axis, d, 4.0f);
  EXPECT_TRUE(std::isnan(off));
  pbr::ConeTable t;
  pbr::ConeVerdict v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, 4.0f, off, 1000);
  EXPECT_FALSE(v.ok);
  EXPECT_FALSE(v.send);
  const float ahead[3] = {0, 0, 4};
  const float offNaNDist = pbr::AngleTo(q, axis, ahead, kNaN);
  EXPECT_TRUE(std::isnan(offNaNDist));
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, kNaN, offNaNDist, 1000);
  EXPECT_FALSE(v.ok);
  v = pbr::ConeDecide(t, kBubble, kDcs, 3, 15.0f, 60, kNaN, 0.0f, 1000);
  EXPECT_FALSE(v.ok) << "a NaN distance is never inside the range";
  EXPECT_EQ(t.For(kBubble, 1000).count, 0) << "no link was made";
}

// ---- LeftSeconds and LinkMarker

TEST(PowerBoostRules, TheSecondsLeftAreRoundedUpAndAtLeastOne) {
  const pbr::UseState u{1000, 20, 10, false};
  EXPECT_EQ(pbr::LeftSeconds(u, 1000), 10u);
  EXPECT_EQ(pbr::LeftSeconds(u, 3500), 8u) << "7.5 s left is 8";
  EXPECT_EQ(pbr::LeftSeconds(u, 3000), 8u) << "8 s left is 8";
  EXPECT_EQ(pbr::LeftSeconds(u, 10999), 1u) << "1 ms left is 1";
  EXPECT_EQ(pbr::LeftSeconds(u, 11000), 10u) << "run out: the full seconds";
  EXPECT_EQ(pbr::LeftSeconds(u, 50000), 10u);
  EXPECT_EQ(pbr::LeftSeconds(pbr::UseState{0, 20, 10, false}, 5000), 10u) << "no use: the full seconds";
  EXPECT_EQ(pbr::LeftSeconds(pbr::UseState{1000, 20, 0, false}, 1000), 1u) << "at least 1";
  EXPECT_EQ(pbr::LeftSeconds(pbr::UseState{0, 20, 0, false}, 1000), 1u);
}

TEST(PowerBoostRules, NoMarkerWithoutAUseOrPast30s) {
  const pbr::UseState none{0, 20, 10, false};
  EXPECT_EQ(pbr::LinkMarker(none, true, 5000), 0.0f);
  EXPECT_EQ(pbr::LinkMarker(none, false, 5000), 0.0f);
  const pbr::UseState u{1000, 20, 10, false};
  EXPECT_EQ(pbr::LinkMarker(u, true, 31001), 0.0f) << "30,001 ms after the use";
  EXPECT_EQ(pbr::LinkMarker(u, false, 31001), 0.0f);
  EXPECT_NE(pbr::LinkMarker(u, true, 31000), 0.0f) << "exactly 30,000 ms after the use";
  EXPECT_NE(pbr::LinkMarker(u, false, 31000), 0.0f);
}

TEST(PowerBoostRules, TheMarkersAreUnlinkLinkAndLinkPlusRez) {
  const pbr::UseState u{1000, 20, 10, false};
  EXPECT_EQ(pbr::LinkMarker(u, false, 2000), 6000000.0f);
  EXPECT_EQ(pbr::LinkMarker(u, true, 3500), 2000000.0f + 20.0f * 1000.0f + 8.0f) << "dr 20, 8 s left";
  const pbr::UseState rez{1000, 500, 10, true};
  EXPECT_EQ(pbr::LinkMarker(rez, true, 4000), 2000000.0f + 500000.0f + 7.0f + 2000000.0f);
  EXPECT_EQ(pbr::LinkMarker(rez, false, 4000), 6000000.0f) << "an unlink carries no rez";
}

// v2.0.0 keeps dr as (v / 1000) / 1000.0f and re-encodes (unsigned)(dr * 1000.0f + 0.5f); the header keeps the integer. They agree for
// every dr_permille a decode can give.
TEST(PowerBoostRules, TheIntegerDrIsV200sFloatDrForEveryPermille) {
  for (uint32_t p = 0; p <= 999; ++p) {
    const float dr = static_cast<float>(p) / 1000.0f;
    ASSERT_EQ(static_cast<uint32_t>(dr * 1000.0f + 0.5f), p) << p;
    const pbr::UseState u{1000, p, 10, false};
    const float v200 = 2000000.0f + static_cast<float>(static_cast<uint32_t>(dr * 1000.0f + 0.5f) * 1000u + 10u);
    ASSERT_EQ(pbr::LinkMarker(u, true, 1000), v200) << p;
    const pbr::Marker m = pbr::Decode(1000000.0f + static_cast<float>(p * 1000u + 10u));
    ASSERT_EQ(m.drPermille, p);
    ASSERT_EQ(m.seconds, 10u);
  }
}

// ---- the marker decode

struct Row {
  float amount;
  int kind, rez;
  uint32_t v, dr, s;
};

void ExpectDecode(const Row& c) {
  const pbr::Marker m = pbr::Decode(c.amount);
  EXPECT_EQ(m.kind, c.kind) << c.amount;
  EXPECT_EQ(m.rez, c.rez) << c.amount;
  EXPECT_EQ(m.v, c.v) << c.amount;
  EXPECT_EQ(m.drPermille, c.dr) << c.amount;
  EXPECT_EQ(m.seconds, c.s) << c.amount;
}

TEST(PowerBoostRules, TheDecodeTableOfTheQuestHostTest) {
  const Row rows[] = {
      {1250030.0f, 1, 0, 250030, 250, 30}, {2000045.0f, 2, 0, 45, 0, 45}, {3500007.0f, 1, 1, 500007, 500, 7},
      {4000001.0f, 2, 1, 1, 0, 1},         {6000000.0f, 3, 0, 0, 0, 0},   {6000123.0f, 3, 0, 123, 0, 0},
      {999999.0f, 0, 0, 0, 0, 0},          {5000000.0f, 0, 0, 0, 0, 0},   {5999999.0f, 0, 0, 0, 0, 0},
      {7000000.0f, 0, 0, 0, 0, 0},         {1e9f, 0, 0, 0, 0, 0},         {-1250030.0f, 0, 0, 0, 0, 0},
      {kNaN, 0, 0, 0, 0, 0},               {kInf, 0, 0, 0, 0, 0},         {1000000.0f, 1, 0, 0, 0, 0},
  };
  for (const Row& c : rows) ExpectDecode(c);
  ExpectDecode({-kInf, 0, 0, 0, 0, 0});
  ExpectDecode({1020010.0f, 1, 0, 20010, 20, 10});
  ExpectDecode({2000000.0f, 2, 0, 0, 0, 0});
  ExpectDecode({4000000.0f, 2, 1, 0, 0, 0});
  ExpectDecode({2999999.0f, 2, 0, 999999, 999, 999});
  ExpectDecode({3000000.0f, 1, 1, 0, 0, 0});
  ExpectDecode({4999999.0f, 2, 1, 999999, 999, 999});
}

// The decode rounds what is past the base (+ 0.5f), as v2.0.0 does.
TEST(PowerBoostRules, AFractionalAmountIsRounded) {
  ExpectDecode({1020009.75f, 1, 0, 20010, 20, 10});
  ExpectDecode({3020009.75f, 1, 1, 20010, 20, 10});
  ExpectDecode({1020009.25f, 1, 0, 20009, 20, 9});
  ExpectDecode({2000045.5f, 2, 0, 46, 0, 46});
}

// An amount whose rounding carries it out of its band (1,999,999.75 rounds to v 1,000,000: dr 1.0 for 0 s) is declined: kDeclined, no
// buff data. The rez flag is kept. An amount that rounds to the band's top (999,999) is still a marker.
TEST(PowerBoostRules, AnAmountThatRoundsOutOfItsBandIsDeclined) {
  ExpectDecode({1999999.75f, pbr::kDeclined, 0, 0, 0, 0});
  ExpectDecode({1999999.5f, pbr::kDeclined, 0, 0, 0, 0});
  ExpectDecode({2999999.75f, pbr::kDeclined, 0, 0, 0, 0});
  ExpectDecode({3999999.75f, pbr::kDeclined, 1, 0, 0, 0});
  ExpectDecode({4999999.5f, pbr::kDeclined, 1, 0, 0, 0});
  ExpectDecode({1999999.25f, pbr::kUse, 0, 999999, 999, 999});
  ExpectDecode({2999999.0f, pbr::kLink, 0, 999999, 999, 999});
}

// An unlink needs no v: the amount is in its band, so the buff ends, whatever it rounds to.
TEST(PowerBoostRules, AnUnlinkIsNeverDeclined) {
  ExpectDecode({6999999.5f, pbr::kUnlink, 0, 1000000, 0, 0});
  ExpectDecode({6999999.0f, pbr::kUnlink, 0, 999999, 0, 0});
}

}  // namespace

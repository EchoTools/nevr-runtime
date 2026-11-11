// Where a drone's bullet starts (patch/dronemuzzle_rules.h, #2337). The hooks that feed the rule are runtime/combat/dronefix.cpp (PC:
// TickFrame, MuzzleTransform) and quest/combat/dronemuzzle.cpp (Quest); the game's pose read and its clock need the game and are proven by a
// live run. The Quest host test (quest/tests/dronemuzzle_test.cpp) compiles this same header.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include "runtime/patch/dronemuzzle_rules.h"

namespace {

using nevr_dronemuzzle::ClampDt;
using nevr_dronemuzzle::FrameDt;
using nevr_dronemuzzle::kBackMax;
using nevr_dronemuzzle::kMuzzle;
using nevr_dronemuzzle::MuzzleMove;

constexpr float kRoot[3] = {1.5f, -2.0f, 3.25f};
constexpr float kTol = 1e-5f;

// The spawn the rule promises, from the aim's yaw and pitch rather than from the rule's own arithmetic: Z = (cos p sin y, sin p,
// cos p cos y), X = (cos y, 0, -sin y) (horizontal, AttackTarget's row 0), Y = Z x X = (-sin p sin y, cos p, -sin p cos y) (as near world
// up as it can be), spawn = root + kMuzzle in (X, Y, Z) - min(speed dt, kBackMax) Z, dt clamped to [1/120, 1/30].
void Expected(double yaw, double pitch, double speed, double frameDt, double* out) {
  const double Z[3] = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw)};
  const double X[3] = {std::cos(yaw), 0.0, -std::sin(yaw)};
  const double Y[3] = {-std::sin(pitch) * std::sin(yaw), std::cos(pitch), -std::sin(pitch) * std::cos(yaw)};
  const double dt = frameDt < 1.0 / 120.0 ? 1.0 / 120.0 : frameDt > 1.0 / 30.0 ? 1.0 / 30.0 : frameDt;
  const double back = speed * dt > 0.25 ? 0.25 : speed * dt;
  for (int k = 0; k < 3; ++k)
    out[k] = kRoot[k] + kMuzzle[0] * X[k] + kMuzzle[1] * Y[k] + kMuzzle[2] * Z[k] - back * Z[k];
}

void Velocity(double yaw, double pitch, double speed, float* v) {
  v[0] = static_cast<float>(speed * std::cos(pitch) * std::sin(yaw));
  v[1] = static_cast<float>(speed * std::sin(pitch));
  v[2] = static_cast<float>(speed * std::cos(pitch) * std::cos(yaw));
}

TEST(DroneMuzzleRules, TheConstantsAreTheReferenceBuilds) {
  EXPECT_EQ(kMuzzle[0], 0.000024f);
  EXPECT_EQ(kMuzzle[1], 0.075634f);
  EXPECT_EQ(kMuzzle[2], 0.147283f);
  EXPECT_EQ(kBackMax, 0.25f);
}

// TickFrame's running average: 0.9 of the last average and 0.1 of the frame, taken only for a frame in (1/240, 0.1) s.
TEST(DroneMuzzleRules, AFrameInsideTheGateMovesTheAverageByATenth) {
  const float avg = 1.0f / 90.0f;
  EXPECT_EQ(FrameDt(avg, 1.0f / 60.0f), avg * 0.9f + (1.0f / 60.0f) * 0.1f);
  EXPECT_NEAR(FrameDt(0.0f, 0.05f), 0.005f, 1e-7f);
  EXPECT_NEAR(FrameDt(0.05f, 0.05f), 0.05f, 1e-7f);
}

TEST(DroneMuzzleRules, AFrameOutsideTheGateLeavesTheAverage) {
  const float avg = 1.0f / 90.0f;
  EXPECT_EQ(FrameDt(avg, 1.0f / 240.0f), avg) << "1/240 s is outside: the gate is open";
  EXPECT_NE(FrameDt(avg, 1.0f / 230.0f), avg) << "just above 1/240 s is inside";
  EXPECT_EQ(FrameDt(avg, 0.1f), avg) << "0.1 s is outside: the gate is open";
  EXPECT_NE(FrameDt(avg, 0.099f), avg) << "just below 0.1 s is inside";
  EXPECT_EQ(FrameDt(avg, 0.0f), avg);
  EXPECT_EQ(FrameDt(avg, -0.016f), avg);
  EXPECT_EQ(FrameDt(avg, 0.5f), avg) << "a hitch is not averaged in";
  EXPECT_EQ(FrameDt(avg, std::numeric_limits<float>::quiet_NaN()), avg);
}

TEST(DroneMuzzleRules, TheAverageSettlesOnASteadyFrameRate) {
  float avg = 1.0f / 90.0f;  // TickFrame's start
  for (int i = 0; i < 200; ++i) avg = FrameDt(avg, 1.0f / 60.0f);
  EXPECT_NEAR(avg, 1.0f / 60.0f, 1e-6f);
}

// MuzzleTransform's clamp: the frame time used for the back-off is held to [1/120, 1/30] s.
TEST(DroneMuzzleRules, TheFrameTimeIsClampedTo120To30PerSecond) {
  EXPECT_EQ(ClampDt(0.001f), 1.0f / 120.0f);
  EXPECT_EQ(ClampDt(0.0f), 1.0f / 120.0f);
  EXPECT_EQ(ClampDt(1.0f / 120.0f), 1.0f / 120.0f);
  EXPECT_EQ(ClampDt(0.02f), 0.02f);
  EXPECT_EQ(ClampDt(1.0f / 30.0f), 1.0f / 30.0f);
  EXPECT_EQ(ClampDt(0.04f), 1.0f / 30.0f);
  EXPECT_EQ(ClampDt(1.0f), 1.0f / 30.0f);
}

// The basis: Z along the shot, X horizontal ((z, 0, -x) / |..|), Y = Z x X.
TEST(DroneMuzzleRules, AShotAlongPlusZTakesTheMuzzleOffsetAsIs) {
  const float v[3] = {0.0f, 0.0f, 10.0f};
  float out[3] = {};
  ASSERT_TRUE(MuzzleMove(kRoot, v, 1.0f / 90.0f, out));
  const float back = 10.0f / 90.0f;  // below the cap: one frame's travel
  EXPECT_NEAR(out[0], kRoot[0] + kMuzzle[0], kTol);
  EXPECT_NEAR(out[1], kRoot[1] + kMuzzle[1], kTol);
  EXPECT_NEAR(out[2], kRoot[2] + kMuzzle[2] - back, kTol);
}

TEST(DroneMuzzleRules, AShotAlongPlusXTurnsTheOffset) {
  const float v[3] = {10.0f, 0.0f, 0.0f};  // Z = +x, X = (0, 0, -1), Y = +y
  float out[3] = {};
  ASSERT_TRUE(MuzzleMove(kRoot, v, 1.0f / 90.0f, out));
  const float back = 10.0f / 90.0f;
  EXPECT_NEAR(out[0], kRoot[0] + kMuzzle[2] - back, kTol);
  EXPECT_NEAR(out[1], kRoot[1] + kMuzzle[1], kTol);
  EXPECT_NEAR(out[2], kRoot[2] - kMuzzle[0], kTol);
}

TEST(DroneMuzzleRules, AShotStraightUpTakesXAsPlusX) {
  const float v[3] = {0.0f, 10.0f, 0.0f};  // |(z, 0, -x)| is 0: X = (1, 0, 0), Y = Z x X = (0, 0, -1)
  float out[3] = {};
  ASSERT_TRUE(MuzzleMove(kRoot, v, 1.0f / 90.0f, out));
  const float back = 10.0f / 90.0f;
  EXPECT_NEAR(out[0], kRoot[0] + kMuzzle[0], kTol);
  EXPECT_NEAR(out[1], kRoot[1] + kMuzzle[2] - back, kTol);
  EXPECT_NEAR(out[2], kRoot[2] - kMuzzle[1], kTol);
}

TEST(DroneMuzzleRules, EveryAimMatchesTheYawPitchOracle) {
  const double kPi = 3.14159265358979323846;
  int cases = 0;
  for (double yaw = -kPi; yaw < kPi; yaw += kPi / 7.0) {
    for (double pitch = -1.4; pitch <= 1.4; pitch += 0.35) {
      for (double speed : {0.5, 10.0, 120.0}) {
        for (double frame : {0.004, 1.0 / 90.0, 0.02, 0.2}) {
          float v[3];
          Velocity(yaw, pitch, speed, v);
          float got[3] = {};
          ASSERT_TRUE(MuzzleMove(kRoot, v, static_cast<float>(frame), got));
          double want[3];
          Expected(yaw, pitch, speed, frame, want);
          for (int k = 0; k < 3; ++k)
            EXPECT_NEAR(got[k], want[k], 2e-5) << "yaw " << yaw << " pitch " << pitch << " speed " << speed << " frame " << frame;
          ++cases;
        }
      }
    }
  }
  EXPECT_GT(cases, 500);
}

// BACK_MAX: a bullet starts a frame's travel behind the muzzle, but never more than 0.25 m.
TEST(DroneMuzzleRules, AFastShotStartsAtMostBackMaxBehindTheMuzzle) {
  const float v[3] = {0.0f, 0.0f, 120.0f};  // 120 m/s at 30 fps is 4 m a frame
  float out[3] = {};
  ASSERT_TRUE(MuzzleMove(kRoot, v, 1.0f / 30.0f, out));
  EXPECT_NEAR(out[2], kRoot[2] + kMuzzle[2] - kBackMax, kTol);
}

TEST(DroneMuzzleRules, TheBackOffUsesTheClampedFrameTime) {
  const float v[3] = {0.0f, 0.0f, 1.0f};  // 1 m/s: never at the cap
  float out[3] = {};
  ASSERT_TRUE(MuzzleMove(kRoot, v, 0.5f, out));
  EXPECT_NEAR(out[2], kRoot[2] + kMuzzle[2] - 1.0f / 30.0f, kTol) << "a long frame is one 1/30 s frame back";
  ASSERT_TRUE(MuzzleMove(kRoot, v, 0.001f, out));
  EXPECT_NEAR(out[2], kRoot[2] + kMuzzle[2] - 1.0f / 120.0f, kTol) << "a short frame is one 1/120 s frame back";
}

// The small-velocity case: below 1e-4 m/s there is no aim, and the pose stays as the game read it.
TEST(DroneMuzzleRules, ASmallVelocityLeavesThePose) {
  for (const float speed : {0.0f, 1e-5f, 9e-5f}) {
    const float v[3] = {speed, 0.0f, 0.0f};
    float out[3] = {7.0f, 8.0f, 9.0f};
    EXPECT_FALSE(MuzzleMove(kRoot, v, 1.0f / 90.0f, out)) << speed;
    EXPECT_EQ(out[0], 7.0f);
    EXPECT_EQ(out[1], 8.0f);
    EXPECT_EQ(out[2], 9.0f);
  }
  const float v[3] = {2e-4f, 0.0f, 0.0f};
  float out[3] = {};
  EXPECT_TRUE(MuzzleMove(kRoot, v, 1.0f / 90.0f, out)) << "2e-4 m/s is an aim";
}

// A NaN velocity is bad input: the pose stays as the game read it (#2337), never moved by NaN.
TEST(DroneMuzzleRules, ANaNVelocityLeavesThePose) {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float cases[3][3] = {{nan, 0.0f, 0.0f}, {10.0f, nan, 0.0f}, {0.0f, 0.0f, nan}};
  for (const auto& v : cases) {
    float out[3] = {7.0f, 8.0f, 9.0f};
    EXPECT_FALSE(MuzzleMove(kRoot, v, 1.0f / 90.0f, out));
    EXPECT_EQ(out[0], 7.0f);
    EXPECT_EQ(out[1], 8.0f);
    EXPECT_EQ(out[2], 9.0f);
  }
}

}  // namespace

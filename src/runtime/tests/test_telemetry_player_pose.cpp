/* SYNTHESIS -- custom tool code, not from binary */

// The telemetry stream's player poses (#2099), read the way echovr.exe 34.4.631547.1's /session player writer
// (0x1401b2ea0) reads them. The offsets below are written out from the disassembly, not taken from the shipped
// headers, so a wrong constant there fails here:
//   - the entity: the 16 bytes at net game + 0x178 + i * 0x250 + 0x08 go to the resolver *(net game + 0x08),
//     which may rewrite them; their first 8 then go to the entity lookup on the same object, whatever the
//     resolver returned (0x1401b3eb0-0x1401b3ee1). No entity, no pose;
//   - each part: GetComponent<T>(out, entity, handle[1] as the resolver left it, -1), then the component's own
//     check, its vfunc at +0x1C8, with the index GetComponent wrote; a part is read only when both hold;
//   - head: reader(component, out, index), out = quaternion x y z w, then position; body: reader(component,
//     index) returns the same shape;
//   - hands and velocity, only for *(u16*)(net game + 0xE0) <= i < *(u16*)(net game + 0xDC): the
//     R14RemotePlayer component's row = *(component + 0x100) + table[index] * 0x340, the table at
//     *(component + 0xD0) with a u16 every 4 bytes; left quaternion at row + 0x58, right at row + 0x68;
//     positions from the two hand readers (component, out, index); velocity = the vector the reader on
//     *(component + 0x138) gives for row[+0x0C], turned by the quaternion the reader on *(component + 0x140)
//     gives for row[+0x04], as the game computes it (t = 2 q.xyz x v; v + w t + q.xyz x t, no normalising).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

#include "runtime/server/telemetry_player_pose.h"

namespace {

using nevr_telemetry_pose::ComponentRef;
using nevr_telemetry_pose::PoseFunctions;
using nevr_telemetry_pose::PoseResult;

constexpr uint64_t kEntitySystem = 0x08;
constexpr uint64_t kEntrants = 0x178;
constexpr uint64_t kEntrantStride = 0x250;
constexpr uint64_t kEntrantHandle = 0x08;
constexpr uint64_t kFirstRemote = 0xE0;
constexpr uint64_t kRemoteEnd = 0xDC;

constexpr uint64_t kCheckSlot = 0x1C8 / 8;
constexpr uint64_t kRowTable = 0xD0;
constexpr uint64_t kRows = 0x100;
constexpr uint64_t kRowStride = 0x340;
constexpr uint64_t kRowTransformHandle = 0x04;
constexpr uint64_t kRowVelocityHandle = 0x0C;
constexpr uint64_t kRowLeftRotation = 0x58;
constexpr uint64_t kRowRightRotation = 0x68;
constexpr uint64_t kVelocitySystem = 0x138;
constexpr uint64_t kTransformSystem = 0x140;

// Where a fake component keeps what its fake readers hand back, and whether its check accepts it: bytes no
// real offset above uses.
constexpr uint64_t kFakeValues = 0x10;     // 8 floats
constexpr uint64_t kFakeLeftHand = 0x30;   // 3 floats
constexpr uint64_t kFakeRightHand = 0x40;  // 3 floats
constexpr uint64_t kFakeAccept = 0x1F0;

using CheckFn = int(__fastcall*)(void*, uint32_t);

template <typename T>
void PutAt(uint8_t* base, uint64_t off, T value) {
  std::memcpy(base + off, &value, sizeof(T));
}

struct Call2 {
  void* object;
  uint32_t index;
};
struct GetCall {
  void* entity;
  int64_t extra;
  int64_t any;
};

struct Recorder {
  int resolveResult = 1;
  int resolveCalls = 0;
  void* resolver = nullptr;
  int64_t resolveIn[2] = {};
  std::vector<std::pair<void*, int64_t>> lookups;
  std::map<int64_t, void*> entities;
  std::vector<GetCall> headGets, bodyGets, remoteGets;
  std::vector<Call2> checks, headReads, bodyReads, leftReads, rightReads, velocityReads, transformReads;
  float localVelocity[3] = {};
  float turn[4] = {0, 0, 0, 1};
};
Recorder g;

class Component {
 public:
  Component() {
    std::memset(raw_, 0, sizeof(raw_));
    const CheckFn* vtable = Vtable();
    std::memcpy(raw_, &vtable, sizeof(vtable));
    Accept(true);
  }
  void* Ptr() { return raw_; }
  void Accept(bool accept) { PutAt<uint32_t>(raw_, kFakeAccept, accept ? 1 : 0); }
  void SetValues(const std::vector<float>& values) {
    std::memcpy(raw_ + kFakeValues, values.data(), values.size() * sizeof(float));
  }
  void SetHands(const float left[3], const float right[3]) {
    std::memcpy(raw_ + kFakeLeftHand, left, 3 * sizeof(float));
    std::memcpy(raw_ + kFakeRightHand, right, 3 * sizeof(float));
  }
  template <typename T>
  void Put(uint64_t off, T value) {
    PutAt<T>(raw_, off, value);
  }

  static int __fastcall Check(void* component, uint32_t index) {
    g.checks.push_back({component, index});
    uint32_t accept;
    std::memcpy(&accept, static_cast<uint8_t*>(component) + kFakeAccept, sizeof(accept));
    return static_cast<int>(accept);
  }

 private:
  static const CheckFn* Vtable() {
    static CheckFn vtable[64] = {};
    vtable[kCheckSlot] = &Check;
    return vtable;
  }
  alignas(16) uint8_t raw_[0x200];
};

// What GetComponent<T> finds on a fake entity.
struct Entity {
  Component* head = nullptr;
  uint32_t headIndex = 0;
  Component* body = nullptr;
  uint32_t bodyIndex = 0;
  Component* remote = nullptr;
  uint32_t remoteIndex = 0;
};

int __fastcall FakeResolve(void* resolver, int64_t* handle) {
  g.resolveCalls++;
  g.resolver = resolver;
  g.resolveIn[0] = handle[0];
  g.resolveIn[1] = handle[1];
  handle[0] += 0x1000;  // the resolver rewrites the id; the lookup must get the rewritten one
  handle[1] += 0x20;    // and GetComponent the second half as it is after the call
  return g.resolveResult;
}

void* __fastcall FakeLookup(void* system, int64_t id) {
  g.lookups.push_back({system, id});
  const auto found = g.entities.find(id);
  return found == g.entities.end() ? nullptr : found->second;
}

void Fill(ComponentRef* out, Component* component, uint32_t index) {
  out->component = component ? component->Ptr() : nullptr;
  out->index = index;
}
void __fastcall FakeHeadComponent(ComponentRef* out, void* entity, int64_t extra, int64_t any) {
  g.headGets.push_back({entity, extra, any});
  const auto* e = static_cast<Entity*>(entity);
  Fill(out, e->head, e->headIndex);
}
void __fastcall FakeBodyComponent(ComponentRef* out, void* entity, int64_t extra, int64_t any) {
  g.bodyGets.push_back({entity, extra, any});
  const auto* e = static_cast<Entity*>(entity);
  Fill(out, e->body, e->bodyIndex);
}
void __fastcall FakeRemoteComponent(ComponentRef* out, void* entity, int64_t extra, int64_t any) {
  g.remoteGets.push_back({entity, extra, any});
  const auto* e = static_cast<Entity*>(entity);
  Fill(out, e->remote, e->remoteIndex);
}

float* __fastcall FakeReadHead(void* component, float* out, uint32_t index) {
  g.headReads.push_back({component, index});
  std::memcpy(out, static_cast<uint8_t*>(component) + kFakeValues, 8 * sizeof(float));
  return out;
}
const float* __fastcall FakeReadBody(void* component, uint32_t index) {
  g.bodyReads.push_back({component, index});
  return reinterpret_cast<const float*>(static_cast<uint8_t*>(component) + kFakeValues);
}
float* __fastcall FakeReadLeft(void* component, float* out, uint32_t index) {
  g.leftReads.push_back({component, index});
  std::memcpy(out, static_cast<uint8_t*>(component) + kFakeLeftHand, 3 * sizeof(float));
  return out;
}
float* __fastcall FakeReadRight(void* component, float* out, uint32_t index) {
  g.rightReads.push_back({component, index});
  std::memcpy(out, static_cast<uint8_t*>(component) + kFakeRightHand, 3 * sizeof(float));
  return out;
}
float* __fastcall FakeReadVelocity(void* system, float* out, uint32_t handle) {
  g.velocityReads.push_back({system, handle});
  std::memcpy(out, g.localVelocity, sizeof(g.localVelocity));
  return out;
}
float* __fastcall FakeReadTransform(void* system, float* out, uint32_t handle) {
  g.transformReads.push_back({system, handle});
  std::memcpy(out, g.turn, sizeof(g.turn));
  for (int k = 4; k < 8; k++) out[k] = 99.0f;  // the position, which the velocity does not use
  return out;
}

PoseFunctions Fakes() {
  PoseFunctions fn;
  fn.resolveHandle = &FakeResolve;
  fn.lookupEntity = &FakeLookup;
  fn.headComponent = &FakeHeadComponent;
  fn.bodyComponent = &FakeBodyComponent;
  fn.remotePlayerComponent = &FakeRemoteComponent;
  fn.readHead = &FakeReadHead;
  fn.readBody = &FakeReadBody;
  fn.readLeftHand = &FakeReadLeft;
  fn.readRightHand = &FakeReadRight;
  fn.readLocalVelocity = &FakeReadVelocity;
  fn.readWorldTransform = &FakeReadTransform;
  return fn;
}

void ExpectFloats(const float* actual, const std::vector<float>& expected, const char* what) {
  for (size_t k = 0; k < expected.size(); k++) EXPECT_FLOAT_EQ(actual[k], expected[k]) << what << "[" << k << "]";
}
bool AllZero(const float* values, size_t count) {
  for (size_t k = 0; k < count; k++) {
    if (values[k] != 0.0f) return false;
  }
  return true;
}

class TelemetryPlayerPose : public ::testing::Test {
 protected:
  void SetUp() override {
    g = Recorder{};
    netGame_.assign(0x3000, 0);
    PutAt<void*>(netGame_.data(), kEntitySystem, &entitySystem_);
    SetRemoteRange(0, 16);
    head_.SetValues({0.1f, 0.2f, 0.3f, 0.9f, 1.0f, 2.0f, 3.0f, 77.0f});
    body_.SetValues({0.4f, 0.5f, 0.6f, 0.7f, 4.0f, 5.0f, 6.0f});
    entity_.head = &head_;
    entity_.headIndex = 7;
    entity_.body = &body_;
    entity_.bodyIndex = 9;
  }

  void SetHandle(uint16_t entrant, int64_t id, int64_t extra) {
    const uint64_t at = kEntrants + entrant * kEntrantStride + kEntrantHandle;
    PutAt<int64_t>(netGame_.data(), at, id);
    PutAt<int64_t>(netGame_.data(), at + 8, extra);
  }
  void SetRemoteRange(uint16_t first, uint16_t end) {
    PutAt<uint16_t>(netGame_.data(), kFirstRemote, first);
    PutAt<uint16_t>(netGame_.data(), kRemoteEnd, end);
  }
  // Entrant `entrant` has entity_, found by the id its handle holds after the resolver.
  void PlaceEntity(uint16_t entrant, int64_t id, Entity* entity) {
    SetHandle(entrant, id, 0x55);
    g.entities[id + 0x1000] = entity;
  }

  // An R14RemotePlayer component whose instance `index` uses row 2.
  void SetUpRemote(uint32_t index) {
    rowTable_.assign(64, 0);
    rows_.assign(4 * kRowStride, 0);
    PutAt<uint16_t>(rowTable_.data(), (index & 0xFFFF) * 4, 2);
    uint8_t* row = rows_.data() + 2 * kRowStride;
    const float left[4] = {0.0f, 0.0f, 0.6f, 0.8f};
    const float right[4] = {0.8f, 0.0f, 0.0f, 0.6f};
    std::memcpy(row + kRowLeftRotation, left, sizeof(left));
    std::memcpy(row + kRowRightRotation, right, sizeof(right));
    PutAt<uint32_t>(row, kRowTransformHandle, 0x44);
    PutAt<uint32_t>(row, kRowVelocityHandle, 0xCC);
    remote_.Put<uint8_t*>(kRowTable, rowTable_.data());
    remote_.Put<uint8_t*>(kRows, rows_.data());
    remote_.Put<void*>(kVelocitySystem, &velocitySystem_);
    remote_.Put<void*>(kTransformSystem, &transformSystem_);
    const float leftPos[3] = {10.0f, 11.0f, 12.0f};
    const float rightPos[3] = {20.0f, 21.0f, 22.0f};
    remote_.SetHands(leftPos, rightPos);
    entity_.remote = &remote_;
    entity_.remoteIndex = index;
  }

  PoseResult Read(uint16_t entrant, TelemetrySnapshot::PlayerTransform* out) {
    return nevr_telemetry_pose::ReadPlayerPose(netGame_.data(), entrant, Fakes(), out);
  }

  std::vector<uint8_t> netGame_;
  std::vector<uint8_t> rowTable_, rows_;
  int entitySystem_ = 0, velocitySystem_ = 0, transformSystem_ = 0;
  Component head_, body_, remote_;
  Entity entity_;
};

TEST_F(TelemetryPlayerPose, TheEntityIsLookedUpFromTheEntrantsSixteenBytesThroughTheEntitySystemAtPlus8) {
  SetHandle(3, 0x1111, 0x2222);
  g.entities[0x1111 + 0x1000] = &entity_;
  TelemetrySnapshot::PlayerTransform pose{};

  const PoseResult result = Read(3, &pose);

  EXPECT_TRUE(result.entity);
  EXPECT_EQ(g.resolveCalls, 1);
  EXPECT_EQ(g.resolver, &entitySystem_);
  EXPECT_EQ(g.resolveIn[0], 0x1111);  // the entrant's bytes, not the account id
  EXPECT_EQ(g.resolveIn[1], 0x2222);
  ASSERT_EQ(g.lookups.size(), 1u);
  EXPECT_EQ(g.lookups[0].first, &entitySystem_);  // the same object, not *(*(net game + 0x2B08) + 8)
  EXPECT_EQ(g.lookups[0].second, 0x1111 + 0x1000);
}

TEST_F(TelemetryPlayerPose, TheResolversResultDoesNotDecideTheLookup) {
  PlaceEntity(1, 0x30, &entity_);
  g.resolveResult = 0;
  TelemetrySnapshot::PlayerTransform pose{};

  EXPECT_TRUE(Read(1, &pose).entity);
  ASSERT_EQ(g.lookups.size(), 1u);
  EXPECT_EQ(g.lookups[0].second, 0x30 + 0x1000);
}

TEST_F(TelemetryPlayerPose, WithNoEntityThereIsNoPoseAndNoComponentIsAskedFor) {
  SetHandle(2, 0x40, 0x41);  // no entity behind it
  SetUpRemote(1);
  TelemetrySnapshot::PlayerTransform pose{};

  const PoseResult result = Read(2, &pose);

  EXPECT_FALSE(result.entity);
  EXPECT_FALSE(result.head || result.body || result.hands);
  EXPECT_TRUE(g.headGets.empty() && g.bodyGets.empty() && g.remoteGets.empty());
  EXPECT_TRUE(AllZero(&pose.bodyPos[0], sizeof(pose) / sizeof(float)));
}

TEST_F(TelemetryPlayerPose, WithNoEntitySystemNothingIsCalled) {
  PlaceEntity(0, 0x50, &entity_);
  PutAt<void*>(netGame_.data(), kEntitySystem, nullptr);
  TelemetrySnapshot::PlayerTransform pose{};

  EXPECT_FALSE(Read(0, &pose).entity);
  EXPECT_EQ(g.resolveCalls, 0);
  EXPECT_TRUE(g.lookups.empty());
}

TEST_F(TelemetryPlayerPose, TheHeadIsReadThroughItsComponentAndReaderIntoHead) {
  PlaceEntity(4, 0x60, &entity_);
  TelemetrySnapshot::PlayerTransform pose{};

  EXPECT_TRUE(Read(4, &pose).head);

  ASSERT_EQ(g.headGets.size(), 1u);
  EXPECT_EQ(g.headGets[0].entity, &entity_);
  EXPECT_EQ(g.headGets[0].extra, 0x55 + 0x20);  // the handle's second half as the resolver left it
  EXPECT_EQ(g.headGets[0].any, -1);
  ASSERT_EQ(g.headReads.size(), 1u);
  EXPECT_EQ(g.headReads[0].object, head_.Ptr());
  EXPECT_EQ(g.headReads[0].index, 7u);
  ExpectFloats(pose.headRot, {0.1f, 0.2f, 0.3f, 0.9f}, "headRot");
  ExpectFloats(pose.headPos, {1.0f, 2.0f, 3.0f}, "headPos");
}

TEST_F(TelemetryPlayerPose, TheBodyIsReadThroughItsComponentAndReaderIntoBody) {
  PlaceEntity(4, 0x60, &entity_);
  TelemetrySnapshot::PlayerTransform pose{};

  EXPECT_TRUE(Read(4, &pose).body);

  ASSERT_EQ(g.bodyGets.size(), 1u);
  EXPECT_EQ(g.bodyGets[0].entity, &entity_);
  EXPECT_EQ(g.bodyGets[0].extra, 0x55 + 0x20);
  EXPECT_EQ(g.bodyGets[0].any, -1);
  ASSERT_EQ(g.bodyReads.size(), 1u);
  EXPECT_EQ(g.bodyReads[0].object, body_.Ptr());
  EXPECT_EQ(g.bodyReads[0].index, 9u);
  ExpectFloats(pose.bodyRot, {0.4f, 0.5f, 0.6f, 0.7f}, "bodyRot");
  ExpectFloats(pose.bodyPos, {4.0f, 5.0f, 6.0f}, "bodyPos");
}

TEST_F(TelemetryPlayerPose, APartWhoseComponentItsCheckRefusesIsNotRead) {
  PlaceEntity(4, 0x60, &entity_);
  head_.Accept(false);
  TelemetrySnapshot::PlayerTransform pose{};

  const PoseResult result = Read(4, &pose);

  EXPECT_FALSE(result.head);
  EXPECT_TRUE(result.body);
  ASSERT_FALSE(g.checks.empty());
  EXPECT_EQ(g.checks[0].object, head_.Ptr());  // the component's own check, with the index GetComponent wrote
  EXPECT_EQ(g.checks[0].index, 7u);
  EXPECT_TRUE(g.headReads.empty());
  EXPECT_TRUE(AllZero(pose.headRot, 4) && AllZero(pose.headPos, 3));
}

TEST_F(TelemetryPlayerPose, APartWithNoComponentIsNotReadAndNotChecked) {
  PlaceEntity(4, 0x60, &entity_);
  entity_.body = nullptr;
  TelemetrySnapshot::PlayerTransform pose{};

  const PoseResult result = Read(4, &pose);

  EXPECT_TRUE(result.head);
  EXPECT_FALSE(result.body);
  EXPECT_TRUE(g.bodyReads.empty());
  for (const Call2& check : g.checks) EXPECT_NE(check.object, nullptr);
  EXPECT_TRUE(AllZero(pose.bodyRot, 4) && AllZero(pose.bodyPos, 3));
}

TEST_F(TelemetryPlayerPose, ARemoteEntrantsHandsAndVelocityAreReadThroughTheRemotePlayerComponent) {
  SetRemoteRange(1, 9);
  PlaceEntity(5, 0x70, &entity_);
  SetUpRemote(0x10003);  // the row table is indexed by the low 16 bits
  g.localVelocity[0] = 1.0f;
  g.localVelocity[1] = 2.0f;
  g.localVelocity[2] = 3.0f;
  const float h = 0.70710678f;  // a quarter turn about Y: (x, y, z) goes to (z, y, -x)
  g.turn[0] = 0.0f;
  g.turn[1] = h;
  g.turn[2] = 0.0f;
  g.turn[3] = h;
  TelemetrySnapshot::PlayerTransform pose{};

  EXPECT_TRUE(Read(5, &pose).hands);

  ASSERT_EQ(g.remoteGets.size(), 1u);
  EXPECT_EQ(g.remoteGets[0].extra, 0x55 + 0x20);
  EXPECT_EQ(g.remoteGets[0].any, -1);
  ExpectFloats(pose.leftHandRot, {0.0f, 0.0f, 0.6f, 0.8f}, "leftHandRot");
  ExpectFloats(pose.rightHandRot, {0.8f, 0.0f, 0.0f, 0.6f}, "rightHandRot");
  ASSERT_EQ(g.leftReads.size(), 1u);
  EXPECT_EQ(g.leftReads[0].object, remote_.Ptr());
  EXPECT_EQ(g.leftReads[0].index, 0x10003u);
  ASSERT_EQ(g.rightReads.size(), 1u);
  EXPECT_EQ(g.rightReads[0].index, 0x10003u);
  ExpectFloats(pose.leftHandPos, {10.0f, 11.0f, 12.0f}, "leftHandPos");
  ExpectFloats(pose.rightHandPos, {20.0f, 21.0f, 22.0f}, "rightHandPos");
  ASSERT_EQ(g.velocityReads.size(), 1u);
  EXPECT_EQ(g.velocityReads[0].object, &velocitySystem_);
  EXPECT_EQ(g.velocityReads[0].index, 0xCCu);
  ASSERT_EQ(g.transformReads.size(), 1u);
  EXPECT_EQ(g.transformReads[0].object, &transformSystem_);
  EXPECT_EQ(g.transformReads[0].index, 0x44u);
  EXPECT_NEAR(pose.velocity[0], 3.0f, 1e-5f);
  EXPECT_NEAR(pose.velocity[1], 2.0f, 1e-5f);
  EXPECT_NEAR(pose.velocity[2], -1.0f, 1e-5f);
}

TEST_F(TelemetryPlayerPose, ALocalEntrantGetsHeadAndBodyButNoHands) {
  SetRemoteRange(3, 9);
  PlaceEntity(2, 0x80, &entity_);  // 2 < the first remote entrant: the local player or a local unit
  SetUpRemote(1);
  TelemetrySnapshot::PlayerTransform pose{};

  const PoseResult result = Read(2, &pose);

  EXPECT_TRUE(result.head && result.body);
  EXPECT_FALSE(result.hands);
  EXPECT_TRUE(g.remoteGets.empty());
  EXPECT_TRUE(g.leftReads.empty() && g.rightReads.empty() && g.velocityReads.empty());
  EXPECT_TRUE(AllZero(pose.leftHandRot, 4) && AllZero(pose.leftHandPos, 3) && AllZero(pose.velocity, 3));
}

TEST_F(TelemetryPlayerPose, AnEntrantAtTheEndOfTheRemoteRangeGetsNoHands) {
  SetRemoteRange(0, 6);
  PlaceEntity(6, 0x90, &entity_);
  SetUpRemote(1);
  TelemetrySnapshot::PlayerTransform pose{};

  EXPECT_FALSE(Read(6, &pose).hands);
  EXPECT_TRUE(g.remoteGets.empty());
}

TEST_F(TelemetryPlayerPose, EachPlayersPoseLandsInItsSlotFromTheEntrantReadPlayersKept) {
  Component otherHead;
  otherHead.SetValues({0.0f, 0.0f, 0.0f, 1.0f, 50.0f, 51.0f, 52.0f, 0.0f});
  Entity other;
  other.head = &otherHead;
  PlaceEntity(0, 0xA0, &entity_);
  PlaceEntity(1, 0xB0, &other);
  TelemetrySnapshot snap;
  snap.Clear();
  snap.playerInfo[5].present = true;
  snap.playerInfo[5].entrantIndex = 1;
  snap.playerInfo[2].present = true;
  snap.playerInfo[2].entrantIndex = 0;
  snap.playerInfo[9].present = true;
  snap.playerInfo[9].entrantIndex = 7;  // no entity behind entrant 7

  const nevr_telemetry_pose::PosesResult result =
      nevr_telemetry_pose::ReadPlayerPoses(netGame_.data(), &snap, Fakes());

  EXPECT_EQ(result.withEntity, 2u);
  EXPECT_EQ(result.noEntity, 1u);
  ExpectFloats(snap.players[5].headPos, {50.0f, 51.0f, 52.0f}, "slot 5 headPos");
  ExpectFloats(snap.players[2].headPos, {1.0f, 2.0f, 3.0f}, "slot 2 headPos");
  EXPECT_TRUE(AllZero(snap.players[9].headPos, 3));
}

TEST(TelemetryPoseRotation, TheIdentityLeavesAVectorAsItIs) {
  const float q[4] = {0, 0, 0, 1};
  const float v[3] = {1.5f, -2.0f, 3.25f};
  float out[3];
  nevr_telemetry_pose::RotateByQuaternion(q, v, out);
  ExpectFloats(out, {1.5f, -2.0f, 3.25f}, "identity");
}

TEST(TelemetryPoseRotation, QuarterTurnsAboutEachAxis) {
  const float h = 0.70710678f;
  const float aboutX[4] = {h, 0, 0, h}, aboutY[4] = {0, h, 0, h}, aboutZ[4] = {0, 0, h, h};
  const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
  float out[3];
  nevr_telemetry_pose::RotateByQuaternion(aboutX, ey, out);  // y -> z
  EXPECT_NEAR(out[0], 0, 1e-6f);
  EXPECT_NEAR(out[1], 0, 1e-6f);
  EXPECT_NEAR(out[2], 1, 1e-6f);
  nevr_telemetry_pose::RotateByQuaternion(aboutY, ez, out);  // z -> x
  EXPECT_NEAR(out[0], 1, 1e-6f);
  EXPECT_NEAR(out[1], 0, 1e-6f);
  EXPECT_NEAR(out[2], 0, 1e-6f);
  nevr_telemetry_pose::RotateByQuaternion(aboutZ, ex, out);  // x -> y
  EXPECT_NEAR(out[0], 0, 1e-6f);
  EXPECT_NEAR(out[1], 1, 1e-6f);
  EXPECT_NEAR(out[2], 0, 1e-6f);
}

TEST(TelemetryPoseRotation, ANonUnitQuaternionIsUsedAsTheGameUsesItWithoutNormalising) {
  // (0, 0, 1, 1) is a quarter turn about Z scaled by sqrt(2). Normalised it would take x to y, (0, 1, 0); the
  // game's formula on the raw quaternion gives t = (0, 2, 0) and x + t + (0, 0, 1) x t = (-1, 2, 0).
  const float q[4] = {0, 0, 1, 1};
  const float v[3] = {1, 0, 0};
  float out[3];
  nevr_telemetry_pose::RotateByQuaternion(q, v, out);
  ExpectFloats(out, {-1.0f, 2.0f, 0.0f}, "non-unit");
}

}  // namespace

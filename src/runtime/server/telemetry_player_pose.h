#pragma once

#include <cstdint>
#include <cstring>

#include "runtime/server/telemetry_snapshot.h"

// A player's pose, read the way the game's own /session player writer reads it (echovr.exe 34.4.631547.1,
// 0x1401b2ea0, the entity and its parts at 0x1401b3eb0-0x1401b5eb9). The game functions it calls come in as
// pointers (PoseFunctions, resolved from nevr_game_func_addr on the game thread), so a test drives it with fakes.
//
// What /session does for entrant i:
//   - entity: the 16 bytes at the entrant record + 0x08 go to the handle resolver *(netGame + 0x08), which
//     rewrites the first 8 (0x1401b3eb8, 0x1401b3ec9 `call 0x140363610`; its result is ignored), and the first
//     8 then to the entity lookup on the same object (0x1401b3ed2, 0x1401b3ed6 `call 0x1404f37a0`). No entity,
//     no pose (0x1401b3ee1 `je 0x1401b5eb9`).
//   - each part: a component of a type, found by the game's GetComponent<T>(out, entity, handle[1], -1),
//     which looks through the entity's components of type T (0x1401055c0), takes the first the component's
//     own check (vfunc +0x1c8) accepts and otherwise asks the parent entity (0x140363600, then itself).
//     /session inlines its first level and calls it for the parent; calling it on the entity is the same
//     search. A part is read only when the found component is not null and the check accepts it again.
//   - head: GetComponent<ncaTransform> (0x140168420), read by 0x1403c4280(component, out, index) (0x1401b4181):
//     out = { quaternion x, y, z, w; position x, y, z; one more float }.
//   - body: GetComponent<R14Body> (0x140167620), read by 0x1408fc9b0(component, index) (0x1401b45f0), which
//     returns a pointer to { quaternion; position }.
//   - hands and velocity: for a remote entrant, *(u16*)(netGame + 0xE0) <= i < *(u16*)(netGame + 0xDC)
//     (0x1401b500a, 0x1401b5013), GetComponent<R14RemotePlayer> (0x140168260). Its instance row is
//     rows + table[index] * 0x340 (table *(component + 0xD0), u16 per 4 bytes; rows *(component + 0x100)):
//     left-hand quaternion at row + 0x58, right at row + 0x68 (0x1401b5153, 0x1401b51db); positions from
//     0x140179a40 and 0x1401a4dc0 (component, out, index) (0x1401b515f, 0x1401b51e7); velocity is the vector
//     0x140449570(*(component + 0x138), out, row[+0x0C]) gives (0x1401b526e), turned by the quaternion of
//     0x1403c4910(*(component + 0x140), out, row[+0x04]) (0x1401b52a0, the rotation at 0x1401b52a5-0x1401b5395).
//     The local player (R14PlayerNav, 0x1401b53c2) and local units (R15EchoUnit, 0x1401b55b2) take other
//     readers, which this file does not call: their hands and velocity stay zero.
//
// /session writes forward, up and left for each part from its quaternion (the rotation's Z, Y and X columns,
// 0x1401b41c1-0x1401b42ca for the head), so the quaternion is what the game holds: the stream sends it as it is,
// x, y, z, w in the game's order.
namespace nevr_telemetry_pose {

// The 16 bytes /session hands the resolver, and its local for a component reference (0x1401b407b-0x1401b40a1:
// component 0, index 0xffff, then four zero bytes).
struct ComponentRef {
  void* component = nullptr;
  uint32_t index = 0xFFFF;
  uint32_t tail = 0;
};
static_assert(sizeof(ComponentRef) == 16, "GetComponent<T> fills 16 bytes");

// FUN_140363610(resolver, handle) → nonzero when it rewrote handle[0]; it reads handle[1] too.
using ResolveEntityHandleFn = int(__fastcall*)(void* resolver, int64_t* handle);
// FUN_1404f37a0(entitySystem, id) → entity*, or null.
using EntityLookupFn = void*(__fastcall*)(void* entitySystem, int64_t id);
// GetComponent<T>(out, entity, handle[1], -1): FUN_140168420 (ncaTransform), FUN_140167620 (R14Body),
// FUN_140168260 (R14RemotePlayer). It writes out->component (null when none) and out->index.
using GetComponentFn = void(__fastcall*)(ComponentRef* out, void* entity, int64_t handleExtra, int64_t any);
// FUN_1403c4280(ncaTransform component, out, index) → out: { quaternion, position, one float }.
using ReadHeadTransformFn = float*(__fastcall*)(void* component, float* out, uint32_t index);
// FUN_1408fc9b0(R14Body component, index) → { quaternion, position }.
using ReadBodyTransformFn = const float*(__fastcall*)(void* component, uint32_t index);
// FUN_140179a40 (left) and FUN_1401a4dc0 (right) (R14RemotePlayer component, out, index) → out: position.
using ReadHandPositionFn = float*(__fastcall*)(void* component, float* out, uint32_t index);
// FUN_140449570(system, out, handle) → out: the vector /session turns into the velocity.
using ReadLocalVelocityFn = float*(__fastcall*)(void* system, float* out, uint32_t handle);
// FUN_1403c4910(system, out, handle) → out: { quaternion, position, one float }.
using ReadWorldTransformFn = float*(__fastcall*)(void* system, float* out, uint32_t handle);

struct PoseFunctions {
  ResolveEntityHandleFn resolveHandle = nullptr;
  EntityLookupFn lookupEntity = nullptr;
  GetComponentFn headComponent = nullptr;
  GetComponentFn bodyComponent = nullptr;
  GetComponentFn remotePlayerComponent = nullptr;
  ReadHeadTransformFn readHead = nullptr;
  ReadBodyTransformFn readBody = nullptr;
  ReadHandPositionFn readLeftHand = nullptr;
  ReadHandPositionFn readRightHand = nullptr;
  ReadLocalVelocityFn readLocalVelocity = nullptr;
  ReadWorldTransformFn readWorldTransform = nullptr;

  bool Complete() const {
    return resolveHandle && lookupEntity && headComponent && bodyComponent && remotePlayerComponent && readHead &&
           readBody && readLeftHand && readRightHand && readLocalVelocity && readWorldTransform;
  }
};

// The component's own check, its virtual function at +0x1C8 (0x1401b414c-0x1401b4152 for the head).
constexpr uint64_t kComponentCheckVtableOffset = 0x1C8;

// R14RemotePlayer instance data, as /session reads it (0x1401b511e-0x1401b529b).
constexpr uint64_t kRemoteRowTable = 0xD0;     // u16 row per instance, 4 bytes apart
constexpr uint64_t kRemoteRows = 0x100;
constexpr uint64_t kRemoteRowStride = 0x340;
constexpr uint64_t kRemoteRowTransformHandle = 0x04;
constexpr uint64_t kRemoteRowVelocityHandle = 0x0C;
constexpr uint64_t kRemoteRowLeftHandRotation = 0x58;
constexpr uint64_t kRemoteRowRightHandRotation = 0x68;
constexpr uint64_t kRemoteVelocitySystem = 0x138;
constexpr uint64_t kRemoteTransformSystem = 0x140;

template <typename T>
inline T ReadAt(const uint8_t* base, uint64_t offset) {
  T value;
  std::memcpy(&value, base + offset, sizeof(value));
  return value;
}

inline int ComponentCheck(void* component, uint32_t index) {
  using CheckFn = int(__fastcall*)(void* component, uint32_t index);
  const uint8_t* vtable = ReadAt<const uint8_t*>(static_cast<const uint8_t*>(component), 0);
  return ReadAt<CheckFn>(vtable, kComponentCheckVtableOffset)(component, index);
}

// GetComponent<T> on the entity, then /session's own test of what it found: a component, accepted by its
// check (0x1401b4143-0x1401b415e for the head, 0x1401b45c8-0x1401b45e3 body, 0x1401b50fb-0x1401b5118 hands).
inline ComponentRef FindComponent(GetComponentFn getComponent, void* entity, int64_t handleExtra) {
  ComponentRef ref;
  getComponent(&ref, entity, handleExtra, -1);
  if (ref.component == nullptr || ComponentCheck(ref.component, ref.index) == 0) return ComponentRef{};
  return ref;
}

// v turned by q as /session computes it (0x1401b52a5-0x1401b5395): t = 2 (q.xyz × v), v + q.w t + q.xyz × t.
// q is not normalised first, as the game does not.
inline void RotateByQuaternion(const float q[4], const float v[3], float out[3]) {
  const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
  const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
  const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
  out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
  out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
  out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// The parts read for one entrant. A part not read leaves its fields as they were (zero in a cleared snapshot).
struct PoseResult {
  bool entity = false;
  bool head = false;
  bool body = false;
  bool hands = false;
};

// Reads entrant `entrantIndex`'s pose into *out, on the game thread. The checks /session makes are kept at
// the addresses above; the ones it does not make are this reader's own: a null entity system (0x140363610
// checks it, 0x1404f37a0 does not), a null body transform, and null R14RemotePlayer tables and systems.
inline PoseResult ReadPlayerPose(const uint8_t* netGame, uint16_t entrantIndex, const PoseFunctions& fn,
                                 TelemetrySnapshot::PlayerTransform* out) {
  namespace off = nevr_game_offsets;
  PoseResult result;
  if (!fn.Complete()) return result;
  void* entitySystem = ReadAt<void*>(netGame, off::ENTITY_SYSTEM_OFFSET);
  if (entitySystem == nullptr) return result;

  int64_t handle[2];
  std::memcpy(handle,
              netGame + off::PLAYER_ARRAY_OFFSET + entrantIndex * off::PLAYER_STRIDE + off::ENTRANT_ENTITY_HANDLE_OFFSET,
              sizeof(handle));
  (void)fn.resolveHandle(entitySystem, handle);  // /session reads handle[0] whatever it returns (0x1401b3ece)
  void* entity = fn.lookupEntity(entitySystem, handle[0]);
  if (entity == nullptr) return result;
  result.entity = true;
  const int64_t handleExtra = handle[1];

  const ComponentRef head = FindComponent(fn.headComponent, entity, handleExtra);
  if (head.component) {
    float t[8] = {};
    fn.readHead(head.component, t, head.index);
    std::memcpy(out->headRot, t, sizeof(out->headRot));
    std::memcpy(out->headPos, t + 4, sizeof(out->headPos));
    result.head = true;
  }

  const ComponentRef body = FindComponent(fn.bodyComponent, entity, handleExtra);
  if (body.component) {
    const float* t = fn.readBody(body.component, body.index);
    if (t) {
      std::memcpy(out->bodyRot, t, sizeof(out->bodyRot));
      std::memcpy(out->bodyPos, t + 4, sizeof(out->bodyPos));
      result.body = true;
    }
  }

  const uint16_t firstRemote = ReadAt<uint16_t>(netGame, off::REMOTE_ENTRANT_FIRST_OFFSET);
  const uint16_t remoteEnd = ReadAt<uint16_t>(netGame, off::REMOTE_ENTRANT_END_OFFSET);
  if (entrantIndex < firstRemote || entrantIndex >= remoteEnd) return result;

  const ComponentRef remote = FindComponent(fn.remotePlayerComponent, entity, handleExtra);
  if (!remote.component) return result;
  const uint8_t* component = static_cast<const uint8_t*>(remote.component);
  const uint8_t* rowTable = ReadAt<const uint8_t*>(component, kRemoteRowTable);
  const uint8_t* rows = ReadAt<const uint8_t*>(component, kRemoteRows);
  void* velocitySystem = ReadAt<void*>(component, kRemoteVelocitySystem);
  void* transformSystem = ReadAt<void*>(component, kRemoteTransformSystem);
  if (!rowTable || !rows || !velocitySystem || !transformSystem) return result;

  const uint16_t row = ReadAt<uint16_t>(rowTable, static_cast<uint64_t>(remote.index & 0xFFFF) * 4);
  const uint8_t* data = rows + static_cast<uint64_t>(row) * kRemoteRowStride;
  std::memcpy(out->leftHandRot, data + kRemoteRowLeftHandRotation, sizeof(out->leftHandRot));
  std::memcpy(out->rightHandRot, data + kRemoteRowRightHandRotation, sizeof(out->rightHandRot));
  fn.readLeftHand(remote.component, out->leftHandPos, remote.index);
  fn.readRightHand(remote.component, out->rightHandPos, remote.index);

  float local[3] = {};
  float transform[8] = {};
  fn.readLocalVelocity(velocitySystem, local, ReadAt<uint32_t>(data, kRemoteRowVelocityHandle));
  fn.readWorldTransform(transformSystem, transform, ReadAt<uint32_t>(data, kRemoteRowTransformHandle));
  RotateByQuaternion(transform, local, out->velocity);
  result.hands = true;
  return result;
}

// What ReadPlayerPoses found over a snapshot's players.
struct PosesResult {
  uint16_t withEntity = 0;
  uint16_t noEntity = 0;
};

// Reads the pose of every present player into snap->players at its game slot, from the entrant ReadPlayers
// read for that slot (PlayerInfo::entrantIndex).
inline PosesResult ReadPlayerPoses(const uint8_t* netGame, TelemetrySnapshot* snap, const PoseFunctions& fn) {
  PosesResult result;
  for (uint8_t slot = 0; slot < 16; slot++) {
    const auto& info = snap->playerInfo[slot];
    if (!info.present) continue;
    if (ReadPlayerPose(netGame, info.entrantIndex, fn, &snap->players[slot]).entity) {
      result.withEntity++;
    } else {
      result.noEntity++;
    }
  }
  return result;
}

}  // namespace nevr_telemetry_pose

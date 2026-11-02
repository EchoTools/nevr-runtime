// CR15NetFollowPlayerCS::Follow
// Quest libr15.so VA 0x2249764; ReVault binary 576.
// ReVault raw parent 100302998, reconstruction node 100600506; grade M: every branch of the 227-instruction body (908 bytes, ret at the stored end) is translated; callees from Ghidra names and batch-5 siblings; functor layouts checked against the stores; loader-filled slots (0x3815030, 0x380faa0, 0x3826eb0, 0x381e148, 0x382a608) are bound to their names, not byte-checked; the fmadd and fmaxnm sites are pinned to instruction addresses.
// @0x2249764 — CR15NetFollowPlayerCS::Follow
/* @confidence: M */

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Loader-filled pointer slots that Follow reads (ReVault image addresses).
extern const float* const kC3VectorZeroAddr;     // *(u64*)0x03815030: address of C3Vector::zero
extern const void* const kInvokeTransformCQuat;  // *(u64*)0x0380faa0: InvokeMethod for QueueFunctor<CTransformCS, CQuaternion>
extern const void* const kInvokePhysicsC3Vec;    // *(u64*)0x03826eb0: InvokeMethod for QueueFunctor<CPhysicsCS, C3Vector>

namespace q15t::r15::reconstruction {

// CTransfQS as batch-5 defines it (12 floats). Follow reads value[4..6], the position at sret+0x10.
struct CTransfQS {
  float value[12];
};

// Rotation lanes x, y, z, w (the four floats stored at functor +0x1c..+0x28).
struct CQuaternion {
  float x;
  float y;
  float z;
  float w;
};
static_assert(sizeof(CQuaternion) == 0x10);

// Row-major 3x3 matrix, nine floats at sp+0x10..sp+0x34.
struct C33Matrix {
  float value[9];
};
static_assert(sizeof(C33Matrix) == 0x24);

// Three floats (velocities, C3Vector::zero).
struct C3Vector {
  float x;
  float y;
  float z;
};
static_assert(sizeof(C3Vector) == 0x0c);

// Callees: the function each thunk reaches, with the thunk address after the comma.
// CTransformCS::ComputeLocalToWorldTransfQ(SCompactPoolHandle) const @0x17f6ef0, thunk 0x103d910.
CTransfQS CTransformCS_ComputeLocalToWorldTransfQ(const void* self, std::uint32_t handle);
// CComponentHandle<CR15PlayerNavCS>::RefreshCache() const @0x1310848, thunk 0x100c3f0.
void CComponentHandle_RefreshCache(void* handle);
// CR15PlayerNavCS::FinalTransform(SCompactPoolHandle) const @0x1dd4c68, thunk 0xfe8370 (batch-5).
CTransfQS CR15PlayerNavCS_FinalTransform(const void* self, std::uint32_t handle);
// CR15PlayerPhysics::Velocity() const @0x1de2e60, thunk 0x1051b90. The result is three floats in s0..s2.
C3Vector CR15PlayerPhysics_Velocity(const void* self);
// CQuaternion::FromRotationMatrix(C33Matrix const&) @0x140ad54, thunk 0x1005370; writes *self.
void CQuaternion_FromRotationMatrix(CQuaternion* self, const C33Matrix* matrix);
// CTransformCS::SetLocalRotation(SCompactPoolHandle, CQuaternion) @0x1832fb8, thunk 0x104ef10.
void CTransformCS_SetLocalRotation(void* self, std::uint32_t handle, CQuaternion rotation);
// CPhysicsCS::SetWorldVelocity(SCompactPoolHandle, C3Vector) @0x177bec0, thunk 0x1028c90.
void CPhysicsCS_SetWorldVelocity(void* self, std::uint32_t handle, C3Vector velocity);
// CDeferredMethodQueue::QueueFunctor<SFunctor2<void, CTransformCS, SCompactPoolHandle, CQuaternion>>
// @0x150ff8c, thunk 0xff7fe0. Returns the functor storage.
char* CDeferredMethodQueue_QueueFunctor_CTransformCS_CQuaternion(char* queue, const void* invoke_method);
// CDeferredMethodQueue::QueueFunctor<SFunctor2<void, CPhysicsCS, SCompactPoolHandle, C3Vector>>
// @0x15101ec, thunk 0xff29b0. Returns the functor storage.
char* CDeferredMethodQueue_QueueFunctor_CPhysicsCS_C3Vector(char* queue, const void* invoke_method);

namespace {

// The identity quaternion at image 0x02c9fec0, which the code copies to the stack before FromRotationMatrix.
constexpr CQuaternion kIdentityQuat{0.0f, 0.0f, 0.0f, 1.0f};

// Raw field access at a byte offset (memcpy: no alignment or aliasing is assumed).
template <typename T>
T Read(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const char*>(base) + offset, sizeof value);
  return value;
}

template <typename T>
void Write(void* base, std::size_t offset, T value) {
  std::memcpy(static_cast<char*>(base) + offset, &value, sizeof value);
}

// Dense-slot lookup: the u16 at owner+0xc8 + index*4 names a slot, and the record is owner+0xf8 + slot*stride
// (stride 0x20 for the component, 0x898 for the nav system, 0x280 for a physics system).
char* PoolRecord(void* owner, std::uint16_t index, std::size_t stride) {
  const auto table = Read<char*>(owner, 0xc8);
  const auto pool = Read<char*>(owner, 0xf8);
  const auto slot = Read<std::uint16_t>(table, static_cast<std::size_t>(index) * 4);
  return pool + static_cast<std::size_t>(slot) * stride;
}

// The physics pointer of the player the nav system follows: its record (stride 0x898) holds the object at +0x28
// and the object's index at +0x30; that object's record (stride 0x280) holds the physics pointer at +0xd8.
void* NavPhysics(void* nav, std::uint16_t player) {
  char* const record = PoolRecord(nav, player, 0x898);
  void* const object = Read<void*>(record, 0x28);
  char* const physics_record = PoolRecord(object, Read<std::uint16_t>(record, 0x30), 0x280);
  return Read<void*>(physics_record, 0xd8);
}

// CPhysicsCS::SetWorldVelocity, direct when the scene (physics+0x80) has its deferred flag (+0x368) set, else
// queued on the scene's method queue (+0x170) with the functor layout: target +0x00, invoke +0x08, zero +0x10,
// handle +0x18, vector +0x1c..+0x24.
void SetWorldVelocityOrQueue(void* physics, std::uint32_t handle, const C3Vector& velocity) {
  char* const scene = Read<char*>(physics, 0x80);
  if (Read<std::int32_t>(scene, 0x368) != 0) {
    CPhysicsCS_SetWorldVelocity(physics, handle, velocity);
    return;
  }
  char* const functor = CDeferredMethodQueue_QueueFunctor_CPhysicsCS_C3Vector(scene + 0x170, kInvokePhysicsC3Vec);
  Write(functor, 0x00, physics);
  // +0x08 is loaded from loader slot 0x381e148 (zero in the file; adrp at 0x22499ec, 0x2249a7c, 0x2249aac). The C++
  // stores &CPhysicsCS_SetWorldVelocity, the Ghidra name of the function that slot is expected to hold; the binding
  // is not byte-checked.
  Write(functor, 0x08, &CPhysicsCS_SetWorldVelocity);
  Write<std::uint64_t>(functor, 0x10, 0);
  Write(functor, 0x18, handle);
  Write(functor, 0x1c, velocity.x);
  Write(functor, 0x20, velocity.y);
  Write(functor, 0x24, velocity.z);
}

// CTransformCS::SetLocalRotation, direct or queued on the transform's scene (+0x80, flag +0x368, queue +0x170).
// Functor: target +0x00, invoke +0x08, zero +0x10, handle +0x18, quaternion +0x1c..+0x28.
void SetLocalRotationOrQueue(void* transform, std::uint32_t handle, const CQuaternion& rotation) {
  char* const scene = Read<char*>(transform, 0x80);
  if (Read<std::int32_t>(scene, 0x368) != 0) {
    CTransformCS_SetLocalRotation(transform, handle, rotation);
    return;
  }
  char* const functor = CDeferredMethodQueue_QueueFunctor_CTransformCS_CQuaternion(scene + 0x170, kInvokeTransformCQuat);
  Write(functor, 0x00, transform);
  // +0x08 is loaded from loader slot 0x382a608 (zero in the file; adrp at 0x2249a14). The C++ stores
  // &CTransformCS_SetLocalRotation, the Ghidra name of the function that slot is expected to hold; the binding is
  // not byte-checked.
  Write(functor, 0x08, &CTransformCS_SetLocalRotation);
  Write<std::uint64_t>(functor, 0x10, 0);
  Write(functor, 0x18, handle);
  Write(functor, 0x1c, rotation.x);
  Write(functor, 0x20, rotation.y);
  Write(functor, 0x24, rotation.z);
  Write(functor, 0x28, rotation.w);
}

}  // namespace

// NRadEngine::NRadGame::CR15NetFollowPlayerCS::Follow(NRadEngine::SCompactPoolHandle), VA 0x2249764.
// self is the CR15NetFollowPlayerCS. The handle is the 64-bit argument register; the code reads its low 32 bits
// (index in bits 0..15, generation in 16..31).
void CR15NetFollowPlayerCS_Follow(void* self, std::uint64_t handle) {
  char* const cs = static_cast<char*>(self);
  // Component record: 0x20 bytes at the pool slot named by the handle's index. +0x8 transform handle,
  // +0xc physics handle, +0x10 and +0x14 distance thresholds, +0x18 rotate flag, +0x1c state flag (int).
  char* const entry = PoolRecord(cs, static_cast<std::uint16_t>(handle & 0xffffu), 0x20);

  // A: the component's transform (system +0x130, handle entry+0x8). B: the followed player's transform
  // (CR15PlayerNavCS::FinalTransform on the nav system +0x140, handle word +0x190). The handle at +0x148 is
  // refreshed before each use.
  const CTransfQS component =
      CTransformCS_ComputeLocalToWorldTransfQ(Read<void*>(cs, 0x130), Read<std::uint32_t>(entry, 0x8));
  CComponentHandle_RefreshCache(cs + 0x148);
  const CTransfQS player = CR15PlayerNavCS_FinalTransform(Read<void*>(cs, 0x140), Read<std::uint32_t>(cs, 0x190));
  const float dx = player.value[4] - component.value[4];
  const float dy = player.value[5] - component.value[5];
  const float dz = player.value[6] - component.value[6];
  // 0x22497f4 fmul (dx*dx); fmadd at 0x22497fc and 0x2249800 fuse the dy and dz terms (one rounding each).
  float dist_sq = dx * dx;
  dist_sq = std::fma(dy, dy, dist_sq);
  dist_sq = std::fma(dz, dz, dist_sq);
  const float dist = std::sqrt(dist_sq);

  // The player's physics velocity, read after the second refresh.
  CComponentHandle_RefreshCache(cs + 0x148);
  const C3Vector velocity =
      CR15PlayerPhysics_Velocity(NavPhysics(Read<void*>(cs, 0x140), Read<std::uint16_t>(cs, 0x190)));

  bool speed_path = false;
  if (dist < Read<float>(entry, 0x14) || Read<std::int32_t>(entry, 0x1c) != 0) {
    const auto state = Read<std::int32_t>(entry, 0x1c);
    if (dist <= Read<float>(entry, 0x10)) {
      if (state != 0) {
        Write<std::int32_t>(entry, 0x1c, 0);
      }
    } else if (state != 0) {
      speed_path = true;  // the state flag stays set
    }
    if (!speed_path) {
      if (Read<std::int32_t>(entry, 0x18) != 0) {
        // Unit direction u = (dx, dy, dz) / dist, and the rotation FromRotationMatrix builds from the rows below.
        const float inv = 1.0f / dist;
        const float ux = inv * dx;
        const float uy = inv * dy;
        const float uz = inv * dz;
        const float uxx = ux * ux;
        const float uzy = uz * uy;
        // 0x22498c4 fmadd: uz*uz + uxx, one rounding.
        const float uzz = std::fma(uz, uz, uxx);
        const float b = ux * (-uy);
        // 0x22498d0 fmul (b*b); fmadd at 0x22498d4 and 0x22498d8 fuse uzy*uzy and uzz*uzz (one rounding each).
        const float len = std::sqrt(std::fma(uzz, uzz, std::fma(uzy, uzy, b * b)));
        const float r = 1.0f / len;
        const C33Matrix matrix{{-uz, 0.0f, ux, r * b, r * uzz, -(uzy * r), -ux, -uy, -uz}};
        CQuaternion rotation = kIdentityQuat;
        CQuaternion_FromRotationMatrix(&rotation, &matrix);
        SetLocalRotationOrQueue(Read<void*>(cs, 0x130), Read<std::uint32_t>(entry, 0x8), rotation);
        SetWorldVelocityOrQueue(Read<void*>(cs, 0x138), Read<std::uint32_t>(entry, 0xc), C3Vector{-uz, 0.0f, ux});
      } else {
        // C3Vector::zero, three floats read through the slot at 0x03815030. The machine code does not load the
        // addresses Ghidra labels DAT_0386e358 and DAT_0386e35c.
        C3Vector zero{};
        std::memcpy(&zero, kC3VectorZeroAddr, sizeof zero);
        SetWorldVelocityOrQueue(Read<void*>(cs, 0x138), Read<std::uint32_t>(entry, 0xc), zero);
      }
      return;
    }
  } else {
    Write<std::int32_t>(entry, 0x1c, 1);
  }

  // Speed path: the player-relative direction scaled by max(|velocity|, 1) / dist. The velocity is the three
  // floats Velocity returned (s0, s1, s2).
  // 0x224998c fmul (vx*vx); fmadd at 0x2249990 and 0x2249994 fuse the vy and vz terms (one rounding each).
  float speed_sq = velocity.x * velocity.x;
  speed_sq = std::fma(velocity.y, velocity.y, speed_sq);
  speed_sq = std::fma(velocity.z, velocity.z, speed_sq);
  // 0x22499ac fmaxnm (s0, s0, s3 = 1.0f). std::fmax matches it: a quiet NaN operand is ignored.
  const float speed = std::fmax(std::sqrt(speed_sq), 1.0f);
  const float scale = speed / dist;
  SetWorldVelocityOrQueue(Read<void*>(cs, 0x138), Read<std::uint32_t>(entry, 0xc),
                          C3Vector{scale * dx, scale * dy, scale * dz});
}

}  // namespace q15t::r15::reconstruction

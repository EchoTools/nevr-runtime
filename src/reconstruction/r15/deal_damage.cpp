// CR15NetDamageableCS::DealDamage
// Quest libr15.so VA 0x2237aec (RVA 0x2227aec); PC echovr.exe: no direct twin located; closest PC evidence: ApplyDamageAndNotify at 0x140cc10b0.
// ReVault binary 576, raw parent 100302775, reconstruction node 100600181; tier: reconstructed; grade: M, assessed by revault-codex.
// @0x2237aec — NRadEngine::NRadGame::CR15NetDamageableCS::DealDamage
/* @confidence: M */

// Quest libr15.so; raw parent 100302775. The Ghidra signature identifies the
// semantic arguments; the byte offsets below preserve the ARM64 raw-body flow.
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace NRadEngine::NRadGame {
struct C3Vector { float x, y, z; };
struct CSymbol64 { std::uint64_t value; };
struct SCompactPoolHandle { std::uint32_t value; };
enum class ER15NetDamageableHitBody : std::uint32_t {};

struct DamageOps {
    static float CalculateTotalDamage(void *self, std::uint32_t handle,
        std::uint64_t netId, std::uint32_t hitBody, std::uint64_t damageType,
        float *damage);
    static std::uint64_t UserIdFromNetId(std::uint64_t);
    static std::uint64_t PeerForValidatedUser(std::uint16_t);
    static void ProcessDamageServer(float damage, void *self,
        std::uint32_t handle, std::uint64_t damageType, std::uint64_t actorId);
    static int UserId(void *netGame, std::uint64_t gameSpace,
        std::uint64_t compactActor);
    static int IsValidDamageable(std::uint32_t packed);
    static std::uint64_t ResolveDamageSource(void *pool, std::uint32_t packed);
    static std::uint64_t ActorIdForHitBody(std::uint8_t hitBody);
    static void BroadcastLocallyTracked(void *netGame, const void *message,
        std::uint64_t peer);
    static void BroadcastDealDamage(void *netGame, const void *message);
};
template<class T> static T load(const void *p, std::size_t off) {
    return *reinterpret_cast<const T *>(static_cast<const std::byte *>(p) + off);
}
template<class T> static void store(void *p, std::size_t off, T value) {
    *reinterpret_cast<T *>(static_cast<std::byte *>(p) + off) = value;
}

void CR15NetDamageableCS::DealDamage(SCompactPoolHandle handle, float damage,
    std::uint64_t netId, C3Vector position, C3Vector direction,
    ER15NetDamageableHitBody hitBody, CSymbol64 damageType) {
    auto *self = reinterpret_cast<std::byte *>(this);
    const std::uint32_t packedHandle = handle.value;
    const std::uint32_t handleMapOffset = (packedHandle & 0xffffu) * 4u;
    const std::uint16_t slot =
        load<std::uint16_t>(load<void *>(self, 0xd0), handleMapOffset);
    auto *entries = load<std::byte *>(self, 0x100);
    auto *entry = entries + std::uint64_t(slot) * 0xb8;
    auto *actorTable = load<std::byte *>(load<void *>(self, 0xc0), 0);
    auto *actor = actorTable +
        std::uint64_t(load<std::uint16_t>(entry, 0)) * 0x38;

    if ((load<std::uint8_t>(entry, 2) & 4u) != 0 &&
        (load<std::uint8_t>(actor, 0x18) & 0x40u) == 0)
        return;

    float totalDamage = damage;
    const std::uint64_t type = damageType.value;
    const std::uint8_t body = static_cast<std::uint8_t>(hitBody);
    DamageOps::CalculateTotalDamage(this, packedHandle, netId, body, type,
        &totalDamage);
    if (std::fabs(totalDamage) <= 4.7683716e-7f)
        return;

    const std::uint64_t userId =
        DamageOps::UserIdFromNetId(load<std::uint64_t>(self, 0xb8));
    if ((load<std::uint8_t>(actor, 0x18) & 0x10u) != 0) {
        struct LocallyTrackedDamage {
            std::uint64_t gameSpace;
            std::uint64_t damageType;
            float damage;
            std::uint32_t positionX;
            std::uint32_t positionY;
            std::uint32_t positionZ;
            std::uint64_t handleAndLocal;
        } message{};
        message.gameSpace =
            load<std::uint64_t>(load<void *>(self, 0x80), 0x58);
        message.damageType = type;
        message.damage = totalDamage;
        message.positionX = *reinterpret_cast<std::uint32_t *>(&position.x);
        message.positionY = *reinterpret_cast<std::uint32_t *>(&position.y);
        message.positionZ = *reinterpret_cast<std::uint32_t *>(&position.z);
        message.handleAndLocal = std::uint64_t(packedHandle) | 0x100000000ull;

        std::uint64_t peer = 0xffffffffffffffffull;
        const std::uint16_t userSlot = static_cast<std::uint16_t>(userId);
        const std::uint32_t generation =
            static_cast<std::uint32_t>(userId) >> 16;
        auto *netGame = load<std::byte *>(self, 0xb8);
        if ((userId & 0xfff0u) == 0 && generation != 0xffffu &&
            generation == load<std::uint16_t>(netGame,
                std::uint64_t(userSlot) * 4 + 0x13a) &&
            load<std::int16_t>(netGame,
                std::uint64_t(userSlot) * 4 + 0x138) != -1)
            peer = DamageOps::PeerForValidatedUser(userSlot);
        DamageOps::BroadcastLocallyTracked(this, &message, peer);
        return;
    }

    if (load<float>(entry, 0x20) > 0.0f) {
        store<std::uint8_t>(entry, 3, body);
        store<std::uint32_t>(entry, 4, static_cast<std::uint32_t>(userId));
        store(entry, 0x34, direction.x);
        store(entry, 0x38, direction.y);
        store(entry, 0x3c, direction.z);
        store(entry, 0x40, position.x);
        store(entry, 0x44, position.y);
        store(entry, 0x48, position.z);
        store<std::uint64_t>(entry, 0xb0, type);
    }

    std::uint64_t actorId = load<std::uint64_t>(actor, 0x30);
    if (body != 6 && actorId == 0xffffffffffffffffull)
        actorId = DamageOps::ActorIdForHitBody(body);
    DamageOps::ProcessDamageServer(totalDamage, this, packedHandle, type, actorId);

    const std::uint16_t refreshedSlot =
        load<std::uint16_t>(load<void *>(self, 0xd0), handleMapOffset);
    entries = load<std::byte *>(self, 0x100);
    entry = entries + std::uint64_t(refreshedSlot) * 0xb8;
    const int resolvedUser = DamageOps::UserId(
        load<void *>(self, 0xb8),
        load<std::uint64_t>(load<void *>(self, 0x80), 0x58),
        load<std::uint64_t>(load<void *>(self, 0x108),
                            std::uint64_t(refreshedSlot) * 0x10));
    if (load<std::int16_t>(entry, 0x14) == -1 && resolvedUser != -1) {
        store<std::uint32_t>(self, 0x15c, 1);
        return;
    }

    struct DealDamageMessage {
        std::uint64_t gameSpace;
        std::uint64_t source;
        std::uint64_t hitVector;
    } message{};
    message.gameSpace =
        load<std::uint64_t>(load<void *>(self, 0x80), 0x58);
    auto *pool = load<void *>(self, 0x148);
    const std::uint32_t packedDamageable = load<std::uint32_t>(entry, 0x10);
    if (pool != nullptr && DamageOps::IsValidDamageable(packedDamageable) != 0)
        message.source = DamageOps::ResolveDamageSource(pool, packedDamageable);
    else
        message.source = std::uint64_t(packedHandle) | 0x100000000ull;

    const std::uint64_t vectorWords = load<std::uint64_t>(entry, 0x1c);
    message.hitVector = (vectorWords << 32) | (vectorWords >> 32);
    DamageOps::BroadcastDealDamage(load<void *>(self, 0xb8), &message);
}
} // namespace NRadEngine::NRadGame

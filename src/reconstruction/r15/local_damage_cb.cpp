// CR15NetDamageableCS::LocalDamageCB
// Quest libr15.so VA 0x22390f0 (RVA 0x22290f0); PC echovr.exe: no direct twin located; closest PC evidence: SetCurrentShield at 0x140ca6b90.
// ReVault binary 576, raw parent 100302786, reconstruction node 100600182; tier: reconstructed; grade: M, assessed by revault-codex.
// @0x22390f0 — NRadEngine::NRadGame::CR15NetDamageableCS::LocalDamageCB
/* @confidence: M */

// Quest libr15.so; raw parent 100302786. The locked callback ABI exposes the
// payload pointer through callbackContext+0x10. Unknown engine operations stay
// named as Ops calls rather than being assigned guessed behavior.
#include <cstddef>
#include <cstdint>
#include <algorithm>

namespace NRadEngine::NRadGame {
class CR15NetDamageableCS {
public:
    void LocalDamageCB(const void *callbackContext);
};

struct DamageOps {
    static void WriteLog(int level, const char *text);
    static std::uint64_t GlobalClockMicroseconds();
    static void Move(void *dst, const void *src, std::size_t bytes);
    static std::uint32_t CompactHandleWord(void *component, std::uint32_t packedHandle);
    static void SendComponentEventGlobal(std::uint64_t gameSpace,
        std::uint64_t eventType, std::uint32_t handleWord0,
        std::uint32_t handleWord1, int flags);
};
template<class T> static T load(const void *p, std::size_t off) {
    return *reinterpret_cast<const T *>(static_cast<const std::byte *>(p) + off);
}
template<class T> static void store(void *p, std::size_t off, T value) {
    *reinterpret_cast<T *>(static_cast<std::byte *>(p) + off) = value;
}

void CR15NetDamageableCS::LocalDamageCB(const void *callbackContext) {
    auto *self = reinterpret_cast<std::byte *>(this);
    const auto *message = load<const std::byte *>(callbackContext, 0x10);
    auto *gameSpace = load<std::byte *>(self, 0x80);
    if (load<std::uint64_t>(gameSpace, 0x58) !=
        load<std::uint64_t>(message, 0))
        return;

    if (load<std::int32_t>(message, 0x24) != 1) {
        DamageOps::WriteLog(8,
            "Unknown Damageable in NetDamageable::LocalDamageCallback");
        return;
    }
    const std::uint16_t compactIndex = load<std::uint16_t>(message, 0x20);
    if (compactIndex == 0xffffu) {
        DamageOps::WriteLog(8,
            "Invalid Damageable Component Reference from a NetDamageableFinalDamage msg");
        return;
    }
    const std::uint16_t generation = load<std::uint16_t>(message, 0x22);
    const std::uint32_t packedHandle =
        std::uint32_t(compactIndex) | (std::uint32_t(generation) << 16);

    auto *entries = load<std::byte *>(self, 0x100);
    auto *indexMap = load<std::byte *>(self, 0xd0);
    const std::uint16_t slot =
        load<std::uint16_t>(indexMap, std::uint64_t(compactIndex) * 4);
    auto *entry = entries + std::uint64_t(slot) * 0xb8;
    const std::uint8_t entryFlags = load<std::uint8_t>(entry, 2);
    auto *actorTable = load<std::byte *>(load<void *>(self, 0xc0), 0);
    auto *actor = actorTable +
        std::uint64_t(load<std::uint16_t>(entry, 0)) * 0x38;
    const std::uint8_t actorFlags = load<std::uint8_t>(actor, 0x18);
    if ((entryFlags & 4u) != 0 && (actorFlags & 0x40u) == 0)
        return;

    const float oldHealth = load<float>(entry, 0x1c);
    const float oldShield = load<float>(entry, 0x20);
    const float incoming = load<float>(message, 0x10);
    const float totalBefore = oldHealth + oldShield;
    float newHealth = oldHealth;
    float newShield = oldShield;
    float appliedDamage;
    float spillAmount = 0.0f;

    if (totalBefore <= 0.0f) {
        appliedDamage = (actorFlags & 0x40u) != 0 ? incoming : 0.0f;
    } else {
        bool spillToShield = (entryFlags & 2u) != 0;
        if (oldHealth > 0.0f) {
            const float afterHealth = oldHealth - incoming;
            store(entry, 0x1c, afterHealth);
            float excessCandidate = incoming;
            if (afterHealth <= 0.0f) {
                excessCandidate = -afterHealth;
                store<std::uint8_t>(entry, 2, entryFlags | 2u);
            }
            newHealth = std::max(afterHealth, 0.0f);
            const bool doubledHealthReached = oldHealth + oldHealth <= incoming;
            if (doubledHealthReached)
                spillToShield = true;
            spillAmount = doubledHealthReached ? excessCandidate : 0.0f;
            newHealth = std::min(newHealth, load<float>(actor, 0x20));
            store(entry, 0x1c, newHealth);
        }
        if (oldShield > 0.0f && spillToShield) {
            newShield = std::max(oldShield - spillAmount, 0.0f);
            newShield = std::min(newShield, load<float>(actor, 0x24));
            store(entry, 0x20, newShield);
        }
        appliedDamage = (totalBefore - newHealth) - newShield;
    }

    store(entry, 0x24,
        static_cast<float>(DamageOps::GlobalClockMicroseconds()) * 1.0e-6f);
    store<std::uint8_t>(entry, 2, load<std::uint8_t>(entry, 2) & 0xfeu);
    store<std::uint64_t>(entry, 0xb0, load<std::uint64_t>(message, 8));

    float secondaryDamage = 0.0f;
    if (totalBefore > 0.0f && newHealth + newShield <= 0.0f)
        secondaryDamage = 1.4013e-45f;
    if ((actorFlags & 0x20u) == 0)
        return;

    auto *historyCount = reinterpret_cast<std::int32_t *>(entry + 0x4c);
    auto *historyLength = reinterpret_cast<std::uint64_t *>(entry + 0x60);
    const std::uint64_t historyCapacity = load<std::uint64_t>(entry, 0x68);
    std::uint64_t count = *historyLength;
    if (*historyCount >= 1) {
        if (count != 0) {
            const std::uint64_t trim = std::min(
                count, static_cast<std::uint64_t>(*historyCount));
            const std::uint64_t remain = count - trim;
            if (remain != 0) {
                auto *history = load<std::byte *>(entry, 0x58);
                DamageOps::Move(history, history + trim * 0x18,
                                remain * 0x18);
                count = *historyLength - trim;
            } else {
                count = 0;
            }
            *historyLength = count;
        }
        *historyCount = 0;
    }
    if (count == historyCapacity)
        return;

    auto *record = load<std::byte *>(entry, 0x58) + count * 0x18;
    *historyLength = count + 1;
    store(record, 0, appliedDamage);
    store<std::uint64_t>(record, 4, load<std::uint64_t>(message, 0x14));
    store(record, 12, load<float>(message, 0x1c));
    store(record, 16, secondaryDamage);
    store<float>(record, 20, 0.0f);

    const std::uint32_t handleWord0 =
        DamageOps::CompactHandleWord(this, packedHandle);
    const std::uint32_t handleWord1 =
        DamageOps::CompactHandleWord(this, packedHandle);
    DamageOps::SendComponentEventGlobal(
        load<std::uint64_t>(gameSpace, 0),
        0xdf1d5300405f3642ull, handleWord0, handleWord1, 1);
}
} // namespace NRadEngine::NRadGame

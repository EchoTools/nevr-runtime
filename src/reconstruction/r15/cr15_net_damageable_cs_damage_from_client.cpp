// @0x223e80c — CR15NetDamageableCS::DamageFromClient
/* @confidence: M */
// Quest libr15.so VA 0x223e80c (RVA 0x213e80c).
// ReVault binary 576, raw parent 100302826, reconstruction node 100600283; tier: reconstructed; grade: M, assessed by revault-codex.
#include <cstddef>
#include <cstdint>

namespace NRadEngine::NRadGame {
using SCompactPoolHandle = std::uint64_t;
using CSymbol64 = std::uint64_t;

struct SR15NetDamageableDamageDealt {
  std::uint64_t sourceEntity;
  std::uint64_t damageableHandle;
  float damage;
  std::uint8_t hitBody;
  std::uint8_t reserved[3];
  CSymbol64 damageType;
};
static_assert(sizeof(SR15NetDamageableDamageDealt) == 0x20);
static_assert(offsetof(SR15NetDamageableDamageDealt, damageableHandle) == 0x08);
static_assert(offsetof(SR15NetDamageableDamageDealt, damage) == 0x10);
static_assert(offsetof(SR15NetDamageableDamageDealt, hitBody) == 0x14);
static_assert(offsetof(SR15NetDamageableDamageDealt, damageType) == 0x18);

std::uint64_t CR15NetGame_Server(void* game);
void CR15NetGame_BroadcastDamageDealt(
    void* game, const SR15NetDamageableDamageDealt* event,
    const void* userData, std::uint64_t flags, std::uint64_t peer,
    std::uint64_t excludedPeer);

void CR15NetDamageableCS_DamageFromClient(
    void* self, SCompactPoolHandle poolHandle, float damage,
    std::uint32_t hitBody, CSymbol64 damageType) {
  auto* bytes = static_cast<std::byte*>(self);
  SR15NetDamageableDamageDealt event{};
  event.sourceEntity =
      *reinterpret_cast<const std::uint64_t*>(
          *reinterpret_cast<std::uintptr_t*>(bytes + 0x80) + 0x58);

  std::uint64_t handleWord;
  std::uint64_t midAndUpperWord;
  std::uint64_t upperWord;
  auto* component = *reinterpret_cast<void**>(bytes + 0x148);
  if (component != nullptr) {
    const auto poolSlot = *reinterpret_cast<const std::uint16_t*>(
        *reinterpret_cast<std::uintptr_t*>(bytes + 0xd0) +
        (poolHandle & 0xffffu) * sizeof(std::uint32_t));
    auto* damageableEntry = reinterpret_cast<std::byte*>(
        *reinterpret_cast<std::uintptr_t*>(bytes + 0x100) +
        static_cast<std::uintptr_t>(poolSlot) * 0xb8 + 0x10);

    auto** componentVtable = *reinterpret_cast<void***>(component);
    using ValidateEntity = int (*)(void*, std::uint32_t);
    const int valid = reinterpret_cast<ValidateEntity>(
        *reinterpret_cast<void**>(
            reinterpret_cast<std::byte*>(componentVtable) + 0x1d0))(
        component, *reinterpret_cast<const std::uint32_t*>(damageableEntry));

    if (valid != 0) {
      const std::uint32_t entityIndex =
          *reinterpret_cast<const std::uint32_t*>(damageableEntry);
      std::uint64_t mappedEntity = UINT64_MAX;
      if ((~entityIndex & 0xffffu) != 0) {
        const auto componentBytes = static_cast<const std::byte*>(component);
        const auto mapSlot = *reinterpret_cast<const std::uint16_t*>(
            *reinterpret_cast<const std::uintptr_t*>(componentBytes + 0xc8) +
            (entityIndex & 0xffffu) * sizeof(std::uint32_t));
        mappedEntity =
            *reinterpret_cast<const std::uint64_t*>(
                *reinterpret_cast<const std::uintptr_t*>(componentBytes + 0xf8) +
                static_cast<std::uintptr_t>(mapSlot) * 0x18 + 8);
      }
      // Preserve the raw partition/recombine masks at 0x223e8d4-0x223e8e8.
      handleWord = mappedEntity;
      midAndUpperWord = mappedEntity & UINT64_C(0xffffffffffff0000);
      upperWord = mappedEntity & UINT64_C(0xffffffff00000000);
    } else {
      // The invalid-validation path falls through to the same encoding used
      // when the component pointer is null.
      handleWord = poolHandle & UINT64_C(0xffffffff);
      midAndUpperWord = poolHandle & UINT64_C(0xffff0000);
      upperWord = UINT64_C(0x100000000);
    }
  } else {
    handleWord = poolHandle & UINT64_C(0xffffffff);
    midAndUpperWord = poolHandle & UINT64_C(0xffff0000);
    upperWord = UINT64_C(0x100000000);
  }
  event.damageableHandle =
      (midAndUpperWord & UINT64_C(0xffff0000)) | upperWord |
      (handleWord & UINT64_C(0xffff));

  event.damage = damage;
  event.hitBody = static_cast<std::uint8_t>(hitBody);
  event.damageType = damageType;

  void* game = *reinterpret_cast<void**>(bytes + 0xb8);
  const std::uint64_t serverPeer = CR15NetGame_Server(game);
  CR15NetGame_BroadcastDamageDealt(
      game, &event, nullptr, 0, serverPeer, UINT64_MAX);
}
} // namespace NRadEngine::NRadGame

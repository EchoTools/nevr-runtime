// @0x2258954 — CR15NetHealBubbleCS::SetTargetInsideBubble
/* @confidence: M */
// Quest libr15.so VA 0x2258954 (RVA 0x2158954).
// ReVault binary 576, raw parent 100303181, reconstruction node 100600282; tier: reconstructed; grade: M, assessed by revault-codex.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine::NRadGame {
using SCompactPoolHandle = std::uint64_t;

std::uint64_t FindOrCreateTargetLink(void* self, std::uint32_t poolHandle,
                                     const std::byte* targetHandle);
void CComponentHandle_RefreshCache(void* handle);
void CR15NetHealBubbleTargetCS_SetInsideBubble(
    void* targetComponent, std::uint64_t poolHandle,
    const std::uint64_t* componentGuid, std::uint32_t inside);

struct ComponentGuid24 {
  std::uint64_t words[3];
};
static_assert(sizeof(ComponentGuid24) == 0x18);

void CR15NetHealBubbleCS_SetTargetInsideBubble(
    void* self, SCompactPoolHandle poolHandle,
    void* targetHandle, const void* position, std::uint32_t value) {
  // Quest copies exactly 0x4c bytes from the large component-handle argument
  // to a 16-byte-aligned temporary and clears the final dword.
  alignas(16) std::array<std::byte, 0x50> targetHandleCopy{};
  std::memcpy(targetHandleCopy.data(), targetHandle, 0x4c);

  const std::uint64_t linkIndex =
      FindOrCreateTargetLink(self, static_cast<std::uint32_t>(poolHandle),
                             targetHandleCopy.data());
  if (linkIndex == UINT64_MAX) {
    return;
  }

  auto* bytes = static_cast<std::byte*>(self);
  const auto poolSlot = *reinterpret_cast<const std::uint16_t*>(
      *reinterpret_cast<std::uintptr_t*>(bytes + 0xc8) +
      (poolHandle & 0xffffu) * sizeof(std::uint32_t));
  auto* targetLink = reinterpret_cast<std::byte*>(
      *reinterpret_cast<std::uintptr_t*>(bytes + 0xf8) +
      static_cast<std::uintptr_t>(poolSlot) * 0x6c0 +
      static_cast<std::uintptr_t>(linkIndex) * 0x60);

  // A 24-byte aggregate return uses AArch64's x8 indirect-result register.
  // The vtable method receives this in x0 and the pool handle in x1.
  auto** vtable = *reinterpret_cast<void***>(self);
  using GetComponentGuid = ComponentGuid24 (*)(void*, std::uint32_t);
  const auto getGuid = reinterpret_cast<GetComponentGuid>(
      *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(vtable) + 0x1d8));
  const ComponentGuid24 componentGuid =
      getGuid(self, static_cast<std::uint32_t>(poolHandle));

  // Both cache refreshes act on the original by-reference component handle.
  CComponentHandle_RefreshCache(targetHandle);
  const auto targetComponent =
      *reinterpret_cast<void* const*>(
          static_cast<const std::byte*>(targetHandle) + 0x40);
  CComponentHandle_RefreshCache(targetHandle);

  const auto positionWords = static_cast<const std::byte*>(position);
  const std::uint64_t positionLow =
      *reinterpret_cast<const std::uint64_t*>(positionWords);
  const std::uint32_t positionHigh =
      *reinterpret_cast<const std::uint32_t*>(positionWords + 8);
  *reinterpret_cast<std::uint64_t*>(targetLink + 0xa8) = positionLow;
  *reinterpret_cast<std::uint32_t*>(targetLink + 0xb0) = positionHigh;
  *reinterpret_cast<std::uint32_t*>(targetLink + 0xb4) = value;

  const auto targetPoolHandle =
      *reinterpret_cast<const std::uint64_t*>(
          static_cast<const std::byte*>(targetHandle) + 0x48);
  CR15NetHealBubbleTargetCS_SetInsideBubble(
      targetComponent, targetPoolHandle, componentGuid.words, 1);
}
} // namespace NRadEngine::NRadGame

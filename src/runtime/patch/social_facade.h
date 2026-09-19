#pragma once

#include <cstddef>
#include <cstdint>

namespace SocialFacade {

constexpr std::size_t kRealVtableSlotCount = 75;
constexpr std::size_t kMaxObservedGameVtableSlot = 76;
constexpr std::size_t kVtableGuardSlotCount = 8;
constexpr std::size_t kVtableSlotCount = kMaxObservedGameVtableSlot + 1 + kVtableGuardSlotCount;
constexpr std::size_t kObjectSize = 0xBA0;

/// Install the observation detour. Substitution is evaluated from
/// `social.facade` when the accessor is called, after config discovery.
void Install(std::uintptr_t gameBase);

/// Process-lifetime empty social object used only when the opt-in gate is true
/// and the platform provider returned null.
void* Object();

#ifdef NEVR_TEST_HOOKS
std::uint32_t TestInitializeCallCount();
std::uint32_t TestShutdownCallCount();
std::uint32_t TestMaxUsers();
const void* TestCallbacksSource();
#endif

}  // namespace SocialFacade

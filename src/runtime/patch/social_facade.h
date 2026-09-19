#pragma once

#include <cstddef>
#include <cstdint>

namespace SocialFacade {

enum class InstallScope {
  kAccessorOnly,
  kAccessorAndJson,
};

enum class JsonTraceKind : std::uint32_t {
  kSocialJson,
  kSet,
  kNavigateForWrite,
};

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

/// Select the façade only for the opt-in/null-provider case.
void* Select(bool enabled, void* original);

/// Pure install decision used by the accessor and unit tests.
constexpr InstallScope RequiredInstallScope(bool enabled, bool facadeSelected) {
  return enabled && facadeSelected ? InstallScope::kAccessorAndJson : InstallScope::kAccessorOnly;
}

/// Best-effort lock-free producer handoff from hooks running inside game JSON
/// code to Facade::Update. A busy/overwritten slot may drop a diagnostic record.
void QueueJsonTrace(JsonTraceKind kind, std::uint32_t callCount, const char* path,
                    std::uint32_t argument, std::uint64_t result, std::uintptr_t root,
                    std::uintptr_t cache);
void FlushJsonTraces();

#ifdef NEVR_TEST_HOOKS
std::uint32_t TestInitializeCallCount();
std::uint32_t TestShutdownCallCount();
std::uint32_t TestMaxUsers();
const void* TestCallbacksSource();
#endif

}  // namespace SocialFacade

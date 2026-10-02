#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/patch/social_facade_install.h"

namespace SocialFacade {

enum class JsonTraceKind : std::uint32_t {
  kSocialJson,
  kSet,
  kNavigateForWrite,
};

struct JsonTraceRecord {
  JsonTraceKind kind;
  std::uint32_t callCount;
  std::uint32_t argument;
  std::uint64_t result;
  std::uintptr_t root;
  std::uintptr_t cache;
  char path[48];
};

using JsonTraceSink = void (*)(const JsonTraceRecord&, void* context);

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

/// Best-effort lock-free producer handoff from hooks running inside game JSON
/// code to Facade::Update. A busy/overwritten slot may drop a diagnostic record.
void QueueJsonTrace(JsonTraceKind kind, std::uint32_t callCount, const char* path,
                    std::uint32_t argument, std::uint64_t result, std::uintptr_t root,
                    std::uintptr_t cache);
void DrainJsonTraces(JsonTraceSink sink, void* context);
void FlushJsonTraces();

#ifdef NEVR_SCENARIO_CONTROL
/// Scenario-control builds only: FriendIsInvitable's result for this friend (-1 if not in the roster).
std::int32_t FriendInvitableForTest(std::uint64_t friendId);

struct PartyStateForTest {
  std::uint64_t partyId = 0;
  std::uint64_t roomId = 0;  // the Id slot: the party, or the one a join is in flight to
  bool joining = false;
  bool joinable = false;  // the Joinable slot's rule
  bool locked = false;    // the party's server-side lock (slot 4 JoinableInternal is its inverse)
  std::uint32_t joinPolicy = 0;  // slot 21 JoinPolicy
  bool shareDirty = false;  // flags bit 0: the game wrote party data that pnsovr would share (slot 7)
  std::vector<std::uint64_t> memberIds;
};
/// Scenario-control builds only: the party as the facade's slots report it.
PartyStateForTest PartyForTest();

struct InviteForTest {
  std::uint64_t partyId = 0;
  std::uint64_t senderId = 0;
};
/// Scenario-control builds only: the invites in the order the game's invite slots index them.
std::vector<InviteForTest> InvitesForTest();
#endif

#ifdef NEVR_TEST_HOOKS
std::uint32_t TestInitializeCallCount();
std::uint32_t TestShutdownCallCount();
std::uint32_t TestMaxUsers();
const void* TestCallbacksSource();
#endif

}  // namespace SocialFacade

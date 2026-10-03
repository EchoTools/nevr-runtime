#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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

/// The game's CJson functions the party data sync calls (game thread only). Install resolves and
/// prologue-checks them (echovr.exe: load 0x1405f0bd0, clear 0x1405ece60, serialise 0x1405f1dc0 into a
/// CMemBlock released by 0x1400d4e50/0x1400d2760); while any is null, party data is neither loaded nor
/// shared. Tests set fakes.
struct JsonOps {
  /// CJson_LoadFromBuffer: parses, then replaces the document (the old one is released); 0 = loaded.
  std::uint32_t (*load)(void* json, const char* text, std::int64_t length) = nullptr;
  /// Clears a CJson to the empty document, releasing what it held.
  void (*clear)(void* json) = nullptr;
  /// Writes the document's text into `block` (0x40 bytes): text at [+0], length at [+0x30].
  void* (*serialize)(void* json, void* block, std::int32_t sortKeys, const char* path) = nullptr;
  void (*blockReset)(void* block) = nullptr;    // when [block+0x1c] & 6
  void (*blockDestroy)(void* block) = nullptr;
};
void SetJsonOps(const JsonOps& ops);

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
  bool shareDirty = false;  // flags bit 0: the game wrote party data not yet shared
  std::vector<std::uint64_t> memberIds;
  std::vector<std::string> memberData;  // each member's data from the server as loaded ("" none)
  std::string partyData;                // the party's data from the server as loaded ("" none)
  std::uint32_t partyDataShared = 0;    // ShareData sends of the party's data
  std::uint32_t memberDataShared = 0;   // ShareData sends of the local member's data
  std::string lastShared;               // the JSON of the last ShareData send
};
/// Scenario-control builds only: the party as the facade's slots report it.
PartyStateForTest PartyForTest();

struct InviteForTest {
  std::uint64_t partyId = 0;
  std::uint64_t senderId = 0;
};
/// Scenario-control builds only: the invites in the order the game's invite slots index them.
std::vector<InviteForTest> InvitesForTest();

struct RecentlyMetForTest {
  std::uint64_t id = 0;
  std::string name;
  std::uint32_t status = 0;  // slot 63: 2 online, 0 offline
  std::string text;          // slot 64
  bool invitable = false;    // slot 65
  bool joinable = false;     // slot 66
  std::uint64_t partyId = 0;  // slot 67
};
/// Scenario-control builds only: the recently-met list as slots 58-67 report it, in their order.
std::vector<RecentlyMetForTest> RecentlyMetUsersForTest();
/// Scenario-control builds only: slot 56.
bool RecentlyMetRefreshingForTest();
/// Scenario-control builds only, game thread: the game's PartyJoinFailed callback (index 2) with this
/// code as given, past the bridge's mapping of the game service's codes (GameJoinFailureCode).
void FireJoinFailedForTest(std::uint32_t code);
#endif

#ifdef NEVR_TEST_HOOKS
std::uint32_t TestInitializeCallCount();
std::uint32_t TestShutdownCallCount();
std::uint32_t TestMaxUsers();
const void* TestCallbacksSource();
#endif

}  // namespace SocialFacade

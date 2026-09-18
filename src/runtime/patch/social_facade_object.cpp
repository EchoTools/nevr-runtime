#include <array>
#include <cstdint>
#include <cstring>

#include "abi/echovr.h"
#include "core/logging.h"
#include "runtime/patch/social_facade.h"

namespace SocialFacade {
namespace {

using Slot = std::uintptr_t;

struct alignas(16) FacadeObject {
  const Slot* vtable;
  std::array<std::uint8_t, 0x1E0> callbacks;
  std::array<std::uint8_t, kObjectSize - 0x1E8> state;
};

static_assert(sizeof(FacadeObject) == kObjectSize, "CNSISocial façade size drift");
static_assert(offsetof(FacadeObject, callbacks) == 0x08, "callback table offset drift");
static_assert(offsetof(FacadeObject, state) == 0x1E8, "CNSISocial state offset drift");

FacadeObject g_object{};
std::uint32_t g_initializeCalls = 0;
std::uint32_t g_shutdownCalls = 0;
const void* g_callbacksSource = nullptr;

std::uint8_t* Bytes(void* self) { return static_cast<std::uint8_t*>(self); }

void Void0(void*) {}
void VoidU32(void*, std::uint32_t) {}
void VoidU64(void*, std::uint64_t) {}
void VoidU32U32(void*, std::uint32_t, std::uint32_t) {}
std::uint64_t Zero0(void*) { return 0; }
std::uint64_t ZeroU32(void*, std::uint32_t) { return 0; }
const char* EmptyU32(void*, std::uint32_t) { return ""; }

std::uint64_t* ZeroId(void*, std::uint64_t* out, std::uint32_t) {
  if (out != nullptr) *out = 0;
  return out;
}

std::uint64_t Initialize(void* self, std::uint32_t maxUsers, const void* callbacks) {
  auto* object = static_cast<FacadeObject*>(self);
  if (callbacks != nullptr) {
    std::memcpy(object->callbacks.data(), callbacks, object->callbacks.size());
  } else {
    object->callbacks.fill(0);
  }
  std::memcpy(Bytes(self) + 0x250, &maxUsers, sizeof(maxUsers));
  g_callbacksSource = callbacks;
  ++g_initializeCalls;
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade initialize max_users=%u callbacks=%p call_count=%u", maxUsers,
      callbacks, g_initializeCalls);
  return 0;
}

void Shutdown(void* self) {
  auto* object = static_cast<FacadeObject*>(self);
  object->callbacks.fill(0);
  ++g_shutdownCalls;
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade shutdown object=%p call_count=%u lifetime=process", self,
      g_shutdownCalls);
}

void Reset(void* self) {
  std::memset(Bytes(self) + 0x1E8, 0, kObjectSize - 0x1E8);
  const std::uint64_t invalidMatchType = UINT64_MAX;
  const std::uint16_t invalidTeam = UINT16_MAX;
  const std::uint8_t privateLobby = 2;
  const std::uint32_t flags = 2;
  std::memcpy(Bytes(self) + 0x270, &invalidMatchType, sizeof(invalidMatchType));
  std::memcpy(Bytes(self) + 0x278, &invalidTeam, sizeof(invalidTeam));
  std::memcpy(Bytes(self) + 0x27A, &privateLobby, sizeof(privateLobby));
  std::memcpy(Bytes(self) + 0x27C, &flags, sizeof(flags));
}

void EnterLobby(void* self, const void* uuid, std::uint64_t matchType, std::uint16_t team, std::uint8_t lobbyType,
                int offline) {
  if (uuid != nullptr) std::memcpy(Bytes(self) + 0x260, uuid, 16);
  std::memcpy(Bytes(self) + 0x270, &matchType, sizeof(matchType));
  std::memcpy(Bytes(self) + 0x278, &team, sizeof(team));
  std::memcpy(Bytes(self) + 0x27A, &lobbyType, sizeof(lobbyType));
  std::uint32_t flags = 2 | (offline != 0 ? 0x10U : 0U);
  std::memcpy(Bytes(self) + 0x27C, &flags, sizeof(flags));
}

// Slot order is the 75-entry CNSISocial table at pnsovr.dll 0x1801FBFC0.
// Each entry has a signature-compatible empty implementation for its return
// shape; ID wrappers use the Win64 hidden-result-pointer convention.
const std::array<Slot, kVtableSlotCount> kVtable = {
    reinterpret_cast<Slot>(&VoidU32U32),  // 00 SwapMembers
    reinterpret_cast<Slot>(&VoidU64),     // 01 RemoveMember
    reinterpret_cast<Slot>(&Void0),       // 02 CreateInternal
    reinterpret_cast<Slot>(&Void0),       // 03 LeaveInternal
    reinterpret_cast<Slot>(&Zero0),       // 04 JoinableInternal
    reinterpret_cast<Slot>(&VoidU32),     // 05 SetJoinableInternal
    reinterpret_cast<Slot>(&Void0),       // 06 ShareData
    reinterpret_cast<Slot>(&VoidU32),     // 07 ShareData(member)
    reinterpret_cast<Slot>(&VoidU64),     // 08 SendInviteInternal
    reinterpret_cast<Slot>(&Initialize),  // 09 Initialize
    reinterpret_cast<Slot>(&Shutdown),    // 10 Shutdown
    reinterpret_cast<Slot>(&Void0),       // 11 destructor/release
    reinterpret_cast<Slot>(&Reset),       // 12 Reset
    reinterpret_cast<Slot>(&Void0),       // 13 Update
    reinterpret_cast<Slot>(&Void0),       // 14 CacheData
    reinterpret_cast<Slot>(&Void0),       // 15 ReceiveData
    reinterpret_cast<Slot>(&VoidU32),     // 16 RemoveRemoteMember
    reinterpret_cast<Slot>(&Void0),       // 17 Create
    reinterpret_cast<Slot>(&VoidU32),     // 18 SetJoinPolicy
    reinterpret_cast<Slot>(&VoidU32),     // 19 PassOwnership
    reinterpret_cast<Slot>(&VoidU32),     // 20 Kick
    reinterpret_cast<Slot>(&Zero0),       // 21 Ready
    reinterpret_cast<Slot>(&Zero0),       // 22 Host
    reinterpret_cast<Slot>(&Zero0),       // 23 IsHost
    reinterpret_cast<Slot>(&Zero0),       // 24 Id
    reinterpret_cast<Slot>(&Zero0),       // 25 JoinPolicy
    reinterpret_cast<Slot>(&Zero0),       // 26 MemberCount
    reinterpret_cast<Slot>(&ZeroId),      // 27 MemberId
    reinterpret_cast<Slot>(&EmptyU32),    // 28 MemberName
    reinterpret_cast<Slot>(&ZeroU32),     // 29 MemberVisible
    reinterpret_cast<Slot>(&ZeroU32),     // 30 MemberDataWritable
    reinterpret_cast<Slot>(&EnterLobby),  // 31 EnterLobby
    reinterpret_cast<Slot>(&VoidU64),     // 32 SendInvite
    reinterpret_cast<Slot>(&VoidU64),     // 33 Join
    reinterpret_cast<Slot>(&Reset),       // 34 ExitLobby
    reinterpret_cast<Slot>(&Void0),       // 35 EnterGame
    reinterpret_cast<Slot>(&Void0),       // 36 ExitGame
    reinterpret_cast<Slot>(&Void0),       // 37 OpenFriendRequestUI
    reinterpret_cast<Slot>(&Void0),       // 38 OpenSendInviteUI
    reinterpret_cast<Slot>(&VoidU32),     // 39 OpenNewSendInviteUI
    reinterpret_cast<Slot>(&VoidU32),     // 40 OpenNewSendInviteUI(target)
    reinterpret_cast<Slot>(&Void0),       // 41 OpenRecvInviteUI
    reinterpret_cast<Slot>(&VoidU32),     // 42 OpenPartyUI
    reinterpret_cast<Slot>(&VoidU32),     // 43 OpenPartyUI(target)
    reinterpret_cast<Slot>(&Zero0),       // 44 RefreshingFriends
    reinterpret_cast<Slot>(&Void0),       // 45 RefreshFriends
    reinterpret_cast<Slot>(&Zero0),       // 46 FriendCount
    reinterpret_cast<Slot>(&Zero0),       // 47 OnlineFriendCount
    reinterpret_cast<Slot>(&Zero0),       // 48 OfflineFriendCount
    reinterpret_cast<Slot>(&ZeroId),      // 49 FriendId
    reinterpret_cast<Slot>(&EmptyU32),    // 50 FriendName
    reinterpret_cast<Slot>(&ZeroU32),     // 51 FriendStatus
    reinterpret_cast<Slot>(&EmptyU32),    // 52 FriendStatusString
    reinterpret_cast<Slot>(&ZeroU32),     // 53 FriendIsInvitable
    reinterpret_cast<Slot>(&ZeroU32),     // 54 FriendIsJoinable
    reinterpret_cast<Slot>(&ZeroU32),     // 55 FriendPartyId
    reinterpret_cast<Slot>(&Zero0),       // 56 RefreshingRecentlyMetUsers
    reinterpret_cast<Slot>(&Void0),       // 57 RefreshRecentlyMetUsers
    reinterpret_cast<Slot>(&Zero0),       // 58 RecentlyMetUserCount
    reinterpret_cast<Slot>(&Zero0),       // 59 OnlineRecentlyMetUserCount
    reinterpret_cast<Slot>(&Zero0),       // 60 OfflineRecentlyMetUserCount
    reinterpret_cast<Slot>(&ZeroId),      // 61 RecentlyMetUserId
    reinterpret_cast<Slot>(&EmptyU32),    // 62 RecentlyMetUserName
    reinterpret_cast<Slot>(&ZeroU32),     // 63 RecentlyMetUserStatus
    reinterpret_cast<Slot>(&EmptyU32),    // 64 RecentlyMetUserStatusString
    reinterpret_cast<Slot>(&ZeroU32),     // 65 RecentlyMetUserIsInvitable
    reinterpret_cast<Slot>(&ZeroU32),     // 66 RecentlyMetUserIsJoinable
    reinterpret_cast<Slot>(&ZeroU32),     // 67 RecentlyMetUserPartyId
    reinterpret_cast<Slot>(&Zero0),       // 68 RefreshingInvites
    reinterpret_cast<Slot>(&Void0),       // 69 RefreshInvites
    reinterpret_cast<Slot>(&Zero0),       // 70 InviteCount
    reinterpret_cast<Slot>(&EmptyU32),    // 71 InviteSender
    reinterpret_cast<Slot>(&ZeroU32),     // 72 InviteSentTime
    reinterpret_cast<Slot>(&VoidU32),     // 73 AcceptInvite
    reinterpret_cast<Slot>(&VoidU32),     // 74 DismissInvite
};

}  // namespace

void* Object() {
  if (g_object.vtable == nullptr) {
    g_object.vtable = kVtable.data();
    Reset(&g_object);
  }
  return &g_object;
}

#ifdef NEVR_TEST_HOOKS
std::uint32_t TestInitializeCallCount() { return g_initializeCalls; }
std::uint32_t TestShutdownCallCount() { return g_shutdownCalls; }
std::uint32_t TestMaxUsers() {
  std::uint32_t result = 0;
  std::memcpy(&result, Bytes(&g_object) + 0x250, sizeof(result));
  return result;
}
const void* TestCallbacksSource() { return g_callbacksSource; }
#endif

}  // namespace SocialFacade

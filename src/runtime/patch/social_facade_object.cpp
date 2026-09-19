#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

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
std::once_flag g_objectOnce;
std::atomic<std::uint32_t> g_initializeCalls{0};
std::atomic<std::uint32_t> g_shutdownCalls{0};
std::uint32_t g_maxUsers = 0;
const void* g_callbacksSource = nullptr;
std::array<std::atomic<bool>, kVtableSlotCount - kRealVtableSlotCount> g_paddedSlotLogged{};

struct FacadeCallCounts {
  std::atomic<std::uint32_t> update{0};
  std::atomic<std::uint32_t> setLocalUser{0};
  std::atomic<std::uint32_t> ready{0};
  std::atomic<std::uint32_t> joinPolicy{0};
  std::atomic<std::uint32_t> joinable{0};
  std::atomic<std::uint32_t> host{0};
  std::atomic<std::uint32_t> isHost{0};
  std::atomic<std::uint32_t> id{0};
};

FacadeCallCounts g_calls;

constexpr std::size_t kJsonTraceCapacity = 32;
constexpr std::size_t kJsonTracePathBytes = 48;
constexpr std::size_t kJsonTracePathWords = kJsonTracePathBytes / sizeof(std::uint64_t);
static_assert(sizeof(JsonTraceRecord::path) == kJsonTracePathBytes);

struct JsonTraceSlot {
  std::atomic<std::uint64_t> stamp{0};
  std::atomic<std::uint32_t> kind{0};
  std::atomic<std::uint32_t> callCount{0};
  std::atomic<std::uint32_t> argument{0};
  std::atomic<std::uint64_t> result{0};
  std::atomic<std::uintptr_t> root{0};
  std::atomic<std::uintptr_t> cache{0};
  std::array<std::atomic<std::uint64_t>, kJsonTracePathWords> path{};
};

static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);

std::array<JsonTraceSlot, kJsonTraceCapacity> g_jsonTraceRing{};
std::array<std::atomic<std::uint64_t>, kJsonTraceCapacity> g_jsonTraceFlushed{};
std::atomic<std::uint64_t> g_jsonTraceSequence{0};

constexpr std::uint32_t kInitialQueryLogCalls = 3;
constexpr std::uint32_t kUpdateSummaryInterval = 300;

std::uint8_t* Bytes(void* self) { return static_cast<std::uint8_t*>(self); }

std::uint32_t CountCall(std::atomic<std::uint32_t>& counter) {
  return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::uint64_t RoomId(const void* self) {
  std::uint64_t result = 0;
  std::memcpy(&result, static_cast<const std::uint8_t*>(self) + 0x2A8, sizeof(result));
  return result;
}

void LogQuery(const char* name, std::uintptr_t slot, std::uint32_t callCount, std::uint64_t result) {
  if (callCount > kInitialQueryLogCalls) return;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.SOCIAL] facade query slot=0x%llx name=%s call_count=%u result=%llu",
      static_cast<unsigned long long>(slot), name, callCount, static_cast<unsigned long long>(result));
}

void Void0(void*) {}
void VoidU32(void*, std::uint32_t) {}
void VoidU64(void*, std::uint64_t) {}
void VoidU32U32(void*, std::uint32_t, std::uint32_t) {}
std::uint64_t Zero0(void*) { return 0; }
std::uint64_t ZeroU32(void*, std::uint32_t) { return 0; }
const char* EmptyU32(void*, std::uint32_t) { return ""; }

template <std::size_t SlotIndex>
std::uint64_t PaddedSlot(void*) {
  static_assert(SlotIndex >= kRealVtableSlotCount && SlotIndex < kVtableSlotCount);
  bool expected = false;
  if (g_paddedSlotLogged[SlotIndex - kRealVtableSlotCount].compare_exchange_strong(
          expected, true, std::memory_order_relaxed)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade padded_vtable_slot slot=%llu offset=0x%llx call_count=1",
        static_cast<unsigned long long>(SlotIndex), static_cast<unsigned long long>(SlotIndex * sizeof(Slot)));
  }
  return 0;
}

std::uint64_t* ZeroId(void*, std::uint64_t* out, std::uint32_t) {
  if (out != nullptr) *out = 0;
  return out;
}

void Update(void*, const void*) {
  FlushJsonTraces();
  const std::uint32_t callCount = CountCall(g_calls.update);
  if (callCount == 1) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade update call_count=1");
  }
  if (callCount % kUpdateSummaryInterval == 0) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.SOCIAL] facade calls update=%u set_local_user=%u ready=%u join_policy=%u "
        "joinable=%u host=%u is_host=%u id=%u period=%u",
        callCount, g_calls.setLocalUser.load(std::memory_order_relaxed),
        g_calls.ready.load(std::memory_order_relaxed), g_calls.joinPolicy.load(std::memory_order_relaxed),
        g_calls.joinable.load(std::memory_order_relaxed), g_calls.host.load(std::memory_order_relaxed),
        g_calls.isHost.load(std::memory_order_relaxed), g_calls.id.load(std::memory_order_relaxed),
        kUpdateSummaryInterval);
  }
}

void SetLocalUser(void*, std::uint32_t userIndex) {
  const std::uint32_t callCount = CountCall(g_calls.setLocalUser);
  if (callCount == 1) {
    Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade set_local_user user_index=%u call_count=1", userIndex);
  }
}

std::uint32_t Ready(void*) {
  constexpr std::uint32_t result = 0;
  LogQuery("Ready", 0xA0, CountCall(g_calls.ready), result);
  return result;
}

std::uint32_t JoinPolicy(void* self) {
  std::uint32_t result = 0;
  std::memcpy(&result, Bytes(self) + 0x2B4, sizeof(result));
  LogQuery("JoinPolicy", 0xA8, CountCall(g_calls.joinPolicy), result);
  return result;
}

std::uint32_t Joinable(void*) {
  constexpr std::uint32_t result = 0;
  LogQuery("Joinable", 0xB0, CountCall(g_calls.joinable), result);
  return result;
}

std::uint64_t* Host(void*, std::uint64_t* out) {
  constexpr std::uint64_t result = 0;
  if (out != nullptr) *out = result;
  LogQuery("Host", 0xB8, CountCall(g_calls.host), result);
  return out;
}

std::uint32_t IsHost(void* self) {
  const std::uint32_t result = RoomId(self) == 0 ? 1U : 0U;
  CountCall(g_calls.isHost);
  return result;
}

std::uint64_t Id(void* self) {
  const std::uint64_t result = RoomId(self);
  LogQuery("Id", 0xC8, CountCall(g_calls.id), result);
  return result;
}

std::uint64_t Initialize(void* self, std::uint32_t maxUsers, const void* callbacks) {
  auto* object = static_cast<FacadeObject*>(self);
  if (callbacks != nullptr) {
    std::memcpy(object->callbacks.data(), callbacks, object->callbacks.size());
  } else {
    object->callbacks.fill(0);
  }
  g_maxUsers = maxUsers;
  g_callbacksSource = callbacks;
  const std::uint32_t callCount = g_initializeCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade initialize max_users=%u callbacks=%p call_count=%u", maxUsers,
      callbacks, callCount);
  return 0;
}

void Shutdown(void* self) {
  auto* object = static_cast<FacadeObject*>(self);
  object->callbacks.fill(0);
  const std::uint32_t callCount = g_shutdownCalls.fetch_add(1, std::memory_order_relaxed) + 1;
  Log(EchoVR::LogLevel::Info, "[NEVR.SOCIAL] facade shutdown object=%p call_count=%u lifetime=process", self,
      callCount);
}

void Reset(void* self) {
  std::memset(Bytes(self) + 0x1E8, 0, kObjectSize - 0x1E8);
  const std::uint64_t invalidMatchType = UINT64_MAX;
  const std::uint16_t invalidTeam = UINT16_MAX;
  const std::uint8_t privateLobby = 2;
  const std::uint32_t flags = 2;
  const std::uint32_t joinPolicy = 3;
  std::memcpy(Bytes(self) + 0x270, &invalidMatchType, sizeof(invalidMatchType));
  std::memcpy(Bytes(self) + 0x278, &invalidTeam, sizeof(invalidTeam));
  std::memcpy(Bytes(self) + 0x27A, &privateLobby, sizeof(privateLobby));
  std::memcpy(Bytes(self) + 0x27C, &flags, sizeof(flags));
  std::memcpy(Bytes(self) + 0x2B4, &joinPolicy, sizeof(joinPolicy));
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

// Slot order is the 75-entry CNSOVRSocial table at pnsovr.dll 0x1801FC2E0.
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
    reinterpret_cast<Slot>(&Update),      // 13 Update
    reinterpret_cast<Slot>(&SetLocalUser),  // 14 SetLocalUser
    reinterpret_cast<Slot>(&VoidU32),     // 15 RemoveLocalMember
    reinterpret_cast<Slot>(&VoidU32),     // 16 SetJoinPolicy
    reinterpret_cast<Slot>(&Void0),       // 17 Create
    reinterpret_cast<Slot>(&VoidU32),     // 18 PassOwnership
    reinterpret_cast<Slot>(&VoidU32),     // 19 Kick
    reinterpret_cast<Slot>(&Ready),       // 20 Ready
    reinterpret_cast<Slot>(&JoinPolicy),  // 21 JoinPolicy
    reinterpret_cast<Slot>(&Joinable),    // 22 Joinable
    reinterpret_cast<Slot>(&Host),        // 23 Host
    reinterpret_cast<Slot>(&IsHost),      // 24 IsHost
    reinterpret_cast<Slot>(&Id),          // 25 Id
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
    reinterpret_cast<Slot>(&PaddedSlot<75>),  // 75 guard
    reinterpret_cast<Slot>(&PaddedSlot<76>),  // 76 observed DMO-only dispatch at +0x260
    reinterpret_cast<Slot>(&PaddedSlot<77>),  // 77 guard
    reinterpret_cast<Slot>(&PaddedSlot<78>),  // 78 guard
    reinterpret_cast<Slot>(&PaddedSlot<79>),  // 79 guard
    reinterpret_cast<Slot>(&PaddedSlot<80>),  // 80 guard
    reinterpret_cast<Slot>(&PaddedSlot<81>),  // 81 guard
    reinterpret_cast<Slot>(&PaddedSlot<82>),  // 82 guard
    reinterpret_cast<Slot>(&PaddedSlot<83>),  // 83 guard
    reinterpret_cast<Slot>(&PaddedSlot<84>),  // 84 guard
};

static_assert(kVtable.size() >= kRealVtableSlotCount);
static_assert(kVtable.size() >= kMaxObservedGameVtableSlot + 1 + kVtableGuardSlotCount);

}  // namespace

void* Select(bool enabled, void* original) {
  return enabled && original == nullptr ? Object() : original;
}

void QueueJsonTrace(JsonTraceKind kind, std::uint32_t callCount, const char* path,
                    std::uint32_t argument, std::uint64_t result, std::uintptr_t root,
                    std::uintptr_t cache) {
  // The game logger reaches mutexes, heap-backed formatting, and synchronous I/O.
  // JSON hooks publish only atomic fixed-size fields; Update flushes them later.
  const std::uint64_t sequence = g_jsonTraceSequence.fetch_add(1, std::memory_order_relaxed) + 1;
  JsonTraceSlot& slot = g_jsonTraceRing[(sequence - 1) % g_jsonTraceRing.size()];
  std::uint64_t previous = slot.stamp.load(std::memory_order_relaxed);
  if ((previous & 1U) != 0 ||
      !slot.stamp.compare_exchange_strong(previous, sequence * 2 - 1, std::memory_order_acq_rel)) {
    return;
  }

  char pathCopy[kJsonTracePathBytes] = {};
  if (path != nullptr) {
    std::size_t i = 0;
    for (; i + 1 < sizeof(pathCopy) && path[i] != '\0'; ++i) pathCopy[i] = path[i];
    pathCopy[i] = '\0';
  }
  slot.kind.store(static_cast<std::uint32_t>(kind), std::memory_order_relaxed);
  slot.callCount.store(callCount, std::memory_order_relaxed);
  slot.argument.store(argument, std::memory_order_relaxed);
  slot.result.store(result, std::memory_order_relaxed);
  slot.root.store(root, std::memory_order_relaxed);
  slot.cache.store(cache, std::memory_order_relaxed);
  for (std::size_t i = 0; i < slot.path.size(); ++i) {
    std::uint64_t word = 0;
    std::memcpy(&word, pathCopy + i * sizeof(word), sizeof(word));
    slot.path[i].store(word, std::memory_order_relaxed);
  }
  slot.stamp.store(sequence * 2, std::memory_order_release);
}

void DrainJsonTraces(JsonTraceSink sink, void* context) {
  if (sink == nullptr) return;
  for (std::size_t slotIndex = 0; slotIndex < g_jsonTraceRing.size(); ++slotIndex) {
    JsonTraceSlot& slot = g_jsonTraceRing[slotIndex];
    const std::uint64_t stampBefore = slot.stamp.load(std::memory_order_acquire);
    if (stampBefore == 0 || (stampBefore & 1U) != 0) continue;
    const std::uint64_t sequence = stampBefore / 2;
    std::uint64_t flushed = g_jsonTraceFlushed[slotIndex].load(std::memory_order_relaxed);
    if (sequence <= flushed) continue;

    JsonTraceRecord record{};
    record.kind = static_cast<JsonTraceKind>(slot.kind.load(std::memory_order_relaxed));
    record.callCount = slot.callCount.load(std::memory_order_relaxed);
    record.argument = slot.argument.load(std::memory_order_relaxed);
    record.result = slot.result.load(std::memory_order_relaxed);
    record.root = slot.root.load(std::memory_order_relaxed);
    record.cache = slot.cache.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < slot.path.size(); ++i) {
      const std::uint64_t word = slot.path[i].load(std::memory_order_relaxed);
      std::memcpy(record.path + i * sizeof(word), &word, sizeof(word));
    }
    if (slot.stamp.load(std::memory_order_acquire) != stampBefore ||
        !g_jsonTraceFlushed[slotIndex].compare_exchange_strong(flushed, sequence, std::memory_order_relaxed)) {
      continue;
    }

    sink(record, context);
  }
}

void FlushJsonTraces() {
  DrainJsonTraces([](const JsonTraceRecord& record, void*) {
    const char* traceName = "social_json";
    if (record.kind == JsonTraceKind::kSet) traceName = "json_set";
    if (record.kind == JsonTraceKind::kNavigateForWrite) traceName = "json_navigate_write";
    Log(EchoVR::LogLevel::Info,
        "[NEVR.SOCIAL] %s call_count=%u path=%s argument=%u result=0x%llx root=%p cache=%p",
        traceName, record.callCount, record.path[0] != '\0' ? record.path : "<none>", record.argument,
        static_cast<unsigned long long>(record.result), reinterpret_cast<void*>(record.root),
        reinterpret_cast<void*>(record.cache));
  }, nullptr);
}

void* Object() {
  std::call_once(g_objectOnce, [] {
    g_object.vtable = kVtable.data();
    Reset(&g_object);
  });
  return &g_object;
}

#ifdef NEVR_TEST_HOOKS
std::uint32_t TestInitializeCallCount() { return g_initializeCalls.load(std::memory_order_relaxed); }
std::uint32_t TestShutdownCallCount() { return g_shutdownCalls.load(std::memory_order_relaxed); }
std::uint32_t TestMaxUsers() { return g_maxUsers; }
const void* TestCallbacksSource() { return g_callbacksSource; }
#endif

}  // namespace SocialFacade

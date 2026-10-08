// The Quest social provider ABI: what the pinned store build's libr15.so and libpnsovr.so
// say about the object the game talks to as its social interface (CNSISocial), pinned so a
// different binary is refused instead of patched on a guess.
//
// Artifact: store APK v4987566 (libr15 build id b243509c..., libpnsovr build id ca47bb8d...).
// Every number below was read from those ELF files with readelf / llvm-objdump; the evidence is
// recorded in docs/adr/0003-quest-networking-port.md ("Social provider"). Link-time addresses
// are ELF vaddrs. ReVault's decompilation shows pointer immediates into libpnsovr data (and
// FUN_ names) 0x100000 higher than the ELF; this file uses the ELF values.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace quest_social {

// ---- binaries -------------------------------------------------------------------------------

inline constexpr const char* kLibPnsovr = "libpnsovr.so";
inline constexpr const char* kLibPnsovrBuildId = "ca47bb8d03e6f43c1825133bbb9c15f174705c51";

// libr15.so imports CNSProvider::Social(unsigned long) through its own PLT: JUMP_SLOT at
// 0x36ef528 (readelf -rW), the library is linked BIND_NOW, and there is exactly one call site,
// CR15NetGame::Initialize at 0x12866a4. The function is defined at 0x192fe20 and looks up the
// exported symbol "Social" in the provider module with dlsym.
inline constexpr const char* kSocialSymbol = "_ZN10NRadEngine11CNSProvider6SocialEm";
inline constexpr std::uint64_t kSocialSlotVaddr = 0x36ef528ULL;

// The object pnsovr's exported Social() returns is a CNSOVRSocial. Its constructor
// (libpnsovr 0x203238) stores the vtable address point of _ZTVN10NRadEngine12CNSOVRSocialE
// (symbol 0x6a1468, size 0x270): 0x6a1468 + 0x10 = 0x6a1478. The game's Social() result is only
// replaced when its first word equals libpnsovr's load bias plus this value.
inline constexpr std::uint64_t kOvrSocialVptrVaddr = 0x6a1478ULL;

// ---- the CNSISocial vtable ------------------------------------------------------------------

// 76 slots. Quest numbering is the PCVR facade's numbering plus one from slot 12 on: the Itanium
// ABI has a complete and a deleting destructor (slots 11 and 12) where MSVC has one.
inline constexpr std::size_t kSlotCount = 76;

enum Slot : std::size_t {
  kSwapMembers = 0,
  kRemoveRemoteMember = 1,
  kJoinInternal = 2,
  kLeaveInternal = 3,
  kJoinableInternal = 4,
  kSetJoinableInternal = 5,
  kShareDataParty = 6,
  kShareDataMember = 7,
  kSendInviteInternal = 8,
  kInitialize = 9,
  kShutdown = 10,
  kDestructorComplete = 11,
  kDestructorDeleting = 12,
  kReset = 13,
  kUpdate = 14,
  kAddMember = 15,
  kRemoveMember = 16,
  kSetJoinPolicy = 17,
  kLeave = 18,
  kPassOwnership = 19,
  kKick = 20,
  kReady = 21,
  kJoinPolicy = 22,
  kJoinable = 23,
  kHost = 24,
  kIsHost = 25,
  kId = 26,
  kMemberCount = 27,
  kMemberId = 28,
  kMemberName = 29,
  kLocalId = 30,
  kMemberDataWritable = 31,
  kEnterLobby = 32,
  kEnterOnlineLobby = 33,
  kEnterOfflineLobby = 34,
  kExitLobby = 35,
  kEnterGame = 36,
  kExitGame = 37,
  kOpenFriendRequestUI = 38,
  kOpenSendInviteUI = 39,
  kOpenNewSendInviteUI = 40,
  kOpenNewSendInviteUITarget = 41,
  kOpenRecvInviteUI = 42,
  kOpenPartyUI = 43,
  kOpenPartyUITarget = 44,
  kRefreshingFriends = 45,
  kRefreshFriends = 46,
  kFriendCount = 47,
  kOnlineFriendCount = 48,
  kOfflineFriendCount = 49,
  kFriendId = 50,
  kFriendName = 51,
  kFriendStatus = 52,
  kFriendStatusString = 53,
  kFriendIsInvitable = 54,
  kFriendIsJoinable = 55,
  kFriendPartyId = 56,
  kRefreshingRecentlyMetUsers = 57,
  kRefreshRecentlyMetUsers = 58,
  kRecentlyMetUserCount = 59,
  kOnlineRecentlyMetUserCount = 60,
  kOfflineRecentlyMetUserCount = 61,
  kRecentlyMetUserId = 62,
  kRecentlyMetUserName = 63,
  kRecentlyMetUserStatus = 64,
  kRecentlyMetUserStatusString = 65,
  kRecentlyMetUserIsInvitable = 66,
  kRecentlyMetUserIsJoinable = 67,
  kRecentlyMetUserPartyId = 68,
  kRefreshingInvites = 69,
  kRefreshInvites = 70,
  kInviteCount = 71,
  kInviteSender = 72,
  kInviteSentTime = 73,
  kAcceptInvite = 74,
  kDismissInvite = 75,
};

static_assert(kDismissInvite + 1 == kSlotCount, "slot enum must end at the last slot");

// The Itanium-mangled method names of CNSOVRSocial's vtable, in slot order, as read from the
// R_AARCH64_ABS64 relocations of the vtable (readelf -rW libpnsovr.so, offsets 0x6a1478 + 8 * n),
// with the "_ZN[K]10NRadEngine" prefix and the class length stripped. tools/quest_social_vtable.sh
// regenerates the list; src/quest/tests/fixtures/cnsovrsocial_vtable.txt holds the extraction
// and social_abi_test compares the two.
inline constexpr std::array<const char*, kSlotCount> kSlotNames = {
    "CNSOVRSocial11SwapMembersEjj",
    "CNSOVRSocial18RemoveRemoteMemberEj",
    "CNSOVRSocial12JoinInternalEy",
    "CNSOVRSocial13LeaveInternalEPKc",
    "CNSOVRSocial16JoinableInternalEv",
    "CNSOVRSocial19SetJoinableInternalEj",
    "CNSOVRSocial9ShareDataEv",
    "CNSOVRSocial9ShareDataEj",
    "CNSOVRSocial18SendInviteInternalENS_13UserAccountIDE",
    "CNSOVRSocial10InitializeEjRKNS_10CNSISocial10SCallbacksE",
    "CNSOVRSocial8ShutdownEv",
    "CNSOVRSocialD2Ev",
    "CNSOVRSocialD0Ev",
    "CNSOVRSocial5ResetEv",
    "CNSOVRSocial6UpdateERKNS_10CNSISocial17SUpdateParametersE",
    "CNSOVRSocial9AddMemberENS_11LocalUserIDE",
    "CNSOVRSocial12RemoveMemberENS_11LocalUserIDE",
    "CNSOVRSocial13SetJoinPolicyENS_17ESocialJoinPolicyE",
    "CNSISocial5LeaveEv",
    "CNSOVRSocial13PassOwnershipEj",
    "CNSOVRSocial4KickEj",
    "CNSOVRSocial5ReadyEv",
    "CNSOVRSocial10JoinPolicyEv",
    "CNSOVRSocial8JoinableEv",
    "CNSOVRSocial4HostEv",
    "CNSOVRSocial6IsHostEv",
    "CNSOVRSocial2IdEv",
    "CNSOVRSocial11MemberCountEv",
    "CNSOVRSocial8MemberIdEj",
    "CNSOVRSocial10MemberNameEj",
    "CNSOVRSocial7LocalIdEj",
    "CNSOVRSocial18MemberDataWritableENS_11LocalUserIDE",
    "CNSISocial10EnterLobbyERKNS_5SUuidENS_9CSymbol64EtNS_12ENSLobbyTypeEj",
    "CNSISocial16EnterOnlineLobbyERKNS_5SUuidENS_9CSymbol64EtNS_12ENSLobbyTypeE",
    "CNSISocial17EnterOfflineLobbyERKNS_5SUuidENS_9CSymbol64ENS_12ENSLobbyTypeE",
    "CNSISocial9ExitLobbyEv",
    "CNSISocial9EnterGameERKNS_5SUuidE",
    "CNSISocial8ExitGameEv",
    "CNSOVRSocial19OpenFriendRequestUIENS_11LocalUserIDENS_13UserAccountIDE",
    "CNSOVRSocial16OpenSendInviteUIENS_11LocalUserIDE",
    "CNSOVRSocial19OpenNewSendInviteUIENS_11LocalUserIDE",
    "CNSOVRSocial19OpenNewSendInviteUIENS_11LocalUserIDENS_13UserAccountIDE",
    "CNSOVRSocial16OpenRecvInviteUIENS_11LocalUserIDE",
    "CNSOVRSocial11OpenPartyUIENS_11LocalUserIDE",
    "CNSOVRSocial11OpenPartyUIENS_11LocalUserIDENS_13UserAccountIDE",
    "CNSOVRSocial17RefreshingFriendsEv",
    "CNSOVRSocial14RefreshFriendsEv",
    "CNSOVRSocial11FriendCountEv",
    "CNSOVRSocial17OnlineFriendCountEv",
    "CNSOVRSocial18OfflineFriendCountEv",
    "CNSOVRSocial8FriendIdEj",
    "CNSOVRSocial10FriendNameEj",
    "CNSOVRSocial12FriendStatusEj",
    "CNSOVRSocial18FriendStatusStringEj",
    "CNSOVRSocial17FriendIsInvitableEj",
    "CNSISocial16FriendIsJoinableEj",
    "CNSOVRSocial13FriendPartyIdEj",
    "CNSOVRSocial26RefreshingRecentlyMetUsersEv",
    "CNSOVRSocial23RefreshRecentlyMetUsersEv",
    "CNSOVRSocial20RecentlyMetUserCountEv",
    "CNSOVRSocial26OnlineRecentlyMetUserCountEv",
    "CNSOVRSocial27OfflineRecentlyMetUserCountEv",
    "CNSOVRSocial17RecentlyMetUserIdEj",
    "CNSOVRSocial19RecentlyMetUserNameEj",
    "CNSOVRSocial21RecentlyMetUserStatusEj",
    "CNSOVRSocial27RecentlyMetUserStatusStringEj",
    "CNSOVRSocial26RecentlyMetUserIsInvitableEj",
    "CNSISocial25RecentlyMetUserIsJoinableEj",
    "CNSOVRSocial22RecentlyMetUserPartyIdEj",
    "CNSOVRSocial17RefreshingInvitesEv",
    "CNSOVRSocial14RefreshInvitesEv",
    "CNSOVRSocial11InviteCountEv",
    "CNSOVRSocial12InviteSenderEj",
    "CNSOVRSocial14InviteSentTimeEj",
    "CNSOVRSocial12AcceptInviteEj",
    "CNSOVRSocial13DismissInviteEj",
};

// ---- the object -----------------------------------------------------------------------------

// pnsovr's InitGlobals allocates 0xbb0 bytes for the CNSOVRSocial (libpnsovr 0x207384).
inline constexpr std::size_t kObjectSize = 0xBB0;

// CNSISocial::SCallbacks: 15 delegates of 0x20 bytes (context, 16 inline bytes, proxy function),
// stored at +0x08 by Initialize. The order is the order CR15NetGame::Initialize stores them into
// the array it passes (libr15 0x12866ac..0x1286a60, stores at sp + 32 * n).
inline constexpr std::size_t kCallbackBytes = 0x1E0;
inline constexpr std::size_t kCallbackStride = 0x20;
enum Callback : std::size_t {
  kCbCreated = 0,          // PartyCreatedCB()
  kCbJoined = 1,           // PartyJoinedCB()
  kCbJoinFailed = 2,       // PartyJoinFailedCB(ESocialJoinError)
  kCbUpdated = 3,          // PartyUpdatedCB()
  kCbHostChanged = 4,      // PartyHostChangedCB()
  kCbLeft = 5,             // PartyLeftCB()
  kCbKicked = 6,           // PartyKickedCB()
  kCbInviteAccepted = 7,   // PartyInvitationCB(LocalUserID, u32) -> u32, the accept gate
  kCbDeepLink = 8,         // DeepLinkCB(CJson&&) -> u32; no Quest deep link source here
  kCbMemberJoined = 9,     // PartyMemberJoinedCB(u32)
  kCbMemberUpdated = 10,   // PartyMemberUpdatedCB(u32)
  kCbMemberLeft = 11,      // PartyMemberLeftCB(UserAccountID, const char*)
  kCbFriendsRefreshed = 12,// FriendsRefreshedCB()
  kCbInviteFailed = 13,    // PartyInviteFailedCB(UserAccountID, const char*, ESocialInviteError)
  kCbInviteReceived = 14,  // PartyInviteReceivedCB(u32)
};

// Fields of the object the game reads without a slot call. 0x200/0x204/0x27C/0x270/0x278 are
// read by CNSISocial base code compiled into libr15 (MemberCount 0x1935a1c, JoinableInternal
// 0x1919274, EnterLobby at pnsovr 0x208478) and by CR15NetGame; 0x2a8/0x2b0 are CNSOVRSocial's
// room id and owner index (Id 0x205258, Host 0x2051fc); 0x248 is the member CJson array
// (CNSISocial::MemberName 0x1935a4c indexes it with stride 16).
inline constexpr std::size_t kOffLocalCount = 0x200;
inline constexpr std::size_t kOffMemberCount = 0x204;
inline constexpr std::size_t kOffPartyJson = 0x1F0;
inline constexpr std::size_t kOffMemberJson = 0x248;
inline constexpr std::size_t kOffMaxMembers = 0x250;
inline constexpr std::size_t kOffLobbyUuid = 0x260;
inline constexpr std::size_t kOffLobbyMatchType = 0x270;
inline constexpr std::size_t kOffLobbyTeam = 0x278;
inline constexpr std::size_t kOffLobbyType = 0x27A;
inline constexpr std::size_t kOffFlags = 0x27C;
inline constexpr std::size_t kOffRoomId = 0x2A8;
inline constexpr std::size_t kOffOwnerIndex = 0x2B0;
inline constexpr std::size_t kOffJoinPolicy = 0x2B4;

// Flags word (+0x27c): bit 0 party data written, bit 1 host wants the party joinable, bit 2
// creating, bit 3 joining, bit 4 offline lobby.
inline constexpr std::uint32_t kFlagDataWritten = 1U;
inline constexpr std::uint32_t kFlagJoinable = 2U;
inline constexpr std::uint32_t kFlagCreating = 4U;
inline constexpr std::uint32_t kFlagJoining = 8U;
inline constexpr std::uint32_t kFlagOfflineLobby = 0x10U;

// The facade keeps a pointer back to its owner in the last word of the object, a place no game
// or pnsovr code reads (the highest field read anywhere above is 0x2b4).
inline constexpr std::size_t kOffOwner = kObjectSize - sizeof(void*);

// The service's own party size; kR14NetMaxPartyUsers (libr15 0x2b9cd28) is 10, the array size the
// game allocates, and is not the party size.
inline constexpr std::uint32_t kPartyMaxMembers = 4;
inline constexpr std::uint32_t kMemberJsonSlots = 10;

}  // namespace quest_social

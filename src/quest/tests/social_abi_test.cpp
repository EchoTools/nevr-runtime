// The social ABI constants against the recorded ground truth (no APK needed).
//
// fixtures/cnsovrsocial_vtable.txt is the vtable of CNSOVRSocial as read from the pinned
// libpnsovr.so by social_pinned_test --dump (index, then the mangled method name without the
// "_ZN[K]10NRadEngine<len>" prefix). This test fails when social_abi.h's slot table or enum drifts
// from it, and when the SNS symbol hashes no longer equal CSymbol64 of their names.
//
// Run: social_abi_test <cnsovrsocial_vtable.txt>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "abi/symbol_hash.h"
#include "quest/social/social_abi.h"
#include "quest/social/social_frames.h"
#include "quest/tests/test_check.h"

namespace {

using namespace quest_social;

struct Pair {
  std::size_t slot;
  const char* method;  // a distinctive part of the method name, in the fixture
};

// Each slot constant the facade installs is tied to the method name it must be at.
constexpr Pair kExpected[] = {
    {kJoinInternal, "JoinInternalEy"},
    {kJoinableInternal, "JoinableInternalEv"},
    {kSetJoinableInternal, "SetJoinableInternalEj"},
    {kSendInviteInternal, "SendInviteInternal"},
    {kInitialize, "InitializeEjRKNS_10CNSISocial10SCallbacksE"},
    {kShutdown, "ShutdownEv"},
    {kDestructorComplete, "D2Ev"},
    {kDestructorDeleting, "D0Ev"},
    {kReset, "ResetEv"},
    {kUpdate, "UpdateERKNS_10CNSISocial17SUpdateParametersE"},
    {kAddMember, "AddMemberENS_11LocalUserIDE"},
    {kSetJoinPolicy, "SetJoinPolicy"},
    {kLeave, "5LeaveEv"},
    {kPassOwnership, "PassOwnershipEj"},
    {kKick, "KickEj"},
    {kReady, "ReadyEv"},
    {kJoinPolicy, "JoinPolicyEv"},
    {kJoinable, "8JoinableEv"},
    {kHost, "HostEv"},
    {kIsHost, "IsHostEv"},
    {kId, "2IdEv"},
    {kMemberCount, "MemberCountEv"},
    {kMemberId, "MemberIdEj"},
    {kMemberName, "MemberNameEj"},
    {kLocalId, "LocalIdEj"},
    {kMemberDataWritable, "MemberDataWritable"},
    {kEnterLobby, "EnterLobbyERKNS_5SUuid"},
    {kEnterOnlineLobby, "EnterOnlineLobby"},
    {kEnterOfflineLobby, "EnterOfflineLobby"},
    {kExitLobby, "ExitLobbyEv"},
    {kEnterGame, "EnterGame"},
    {kExitGame, "ExitGameEv"},
    {kOpenFriendRequestUI, "OpenFriendRequestUI"},
    {kRefreshingFriends, "RefreshingFriendsEv"},
    {kRefreshFriends, "RefreshFriendsEv"},
    {kFriendCount, "11FriendCountEv"},
    {kOnlineFriendCount, "OnlineFriendCountEv"},
    {kOfflineFriendCount, "OfflineFriendCountEv"},
    {kFriendId, "FriendIdEj"},
    {kFriendName, "FriendNameEj"},
    {kFriendStatus, "FriendStatusEj"},
    {kFriendStatusString, "FriendStatusStringEj"},
    {kFriendIsInvitable, "FriendIsInvitableEj"},
    {kFriendIsJoinable, "FriendIsJoinableEj"},
    {kFriendPartyId, "FriendPartyIdEj"},
    {kRefreshingRecentlyMetUsers, "RefreshingRecentlyMetUsersEv"},
    {kRefreshRecentlyMetUsers, "RefreshRecentlyMetUsersEv"},
    {kRecentlyMetUserCount, "RecentlyMetUserCountEv"},
    {kOnlineRecentlyMetUserCount, "OnlineRecentlyMetUserCountEv"},
    {kOfflineRecentlyMetUserCount, "OfflineRecentlyMetUserCountEv"},
    {kRecentlyMetUserId, "RecentlyMetUserIdEj"},
    {kRecentlyMetUserName, "RecentlyMetUserNameEj"},
    {kRecentlyMetUserStatus, "RecentlyMetUserStatusEj"},
    {kRecentlyMetUserStatusString, "RecentlyMetUserStatusStringEj"},
    {kRecentlyMetUserIsInvitable, "RecentlyMetUserIsInvitableEj"},
    {kRecentlyMetUserIsJoinable, "RecentlyMetUserIsJoinableEj"},
    {kRecentlyMetUserPartyId, "RecentlyMetUserPartyIdEj"},
    {kRefreshingInvites, "RefreshingInvitesEv"},
    {kRefreshInvites, "RefreshInvitesEv"},
    {kInviteCount, "InviteCountEv"},
    {kInviteSender, "InviteSenderEj"},
    {kInviteSentTime, "InviteSentTimeEj"},
    {kAcceptInvite, "AcceptInviteEj"},
    {kDismissInvite, "DismissInviteEj"},
};

void CheckFixture(const char* path) {
  std::ifstream in(path);
  QCHECK(in.good());
  std::vector<std::string> names(kSlotCount);
  std::size_t seen = 0;
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    const std::size_t space = line.find(' ');
    QCHECK(space != std::string::npos);
    if (space == std::string::npos) continue;
    const std::size_t index = static_cast<std::size_t>(std::stoul(line.substr(0, space)));
    QCHECK(index < kSlotCount);
    if (index >= kSlotCount) continue;
    names[index] = line.substr(space + 1);
    ++seen;
  }
  QCHECK(seen == kSlotCount);
  for (std::size_t i = 0; i < kSlotCount; ++i) {
    if (names[i] != kSlotNames[i]) {
      std::fprintf(stderr, "slot %zu: header \"%s\" fixture \"%s\"\n", i, kSlotNames[i], names[i].c_str());
    }
    QCHECK(names[i] == kSlotNames[i]);
  }
  for (const Pair& p : kExpected) {
    const bool found = names[p.slot].find(p.method) != std::string::npos;
    if (!found) std::fprintf(stderr, "enum slot %zu is \"%s\", expected a name containing \"%s\"\n", p.slot, names[p.slot].c_str(), p.method);
    QCHECK(found);
  }
}

void CheckSymbols() {
  QCHECK(kSymFriendStatusNotify == EchoVR::CSymbol64Hash("SNSFriendStatusNotify"));
  QCHECK(kSymFriendListResponse == EchoVR::CSymbol64Hash("SNSFriendListResponse"));
}

void CheckLayout() {
  static_assert(kOffOwner + sizeof(void*) == kObjectSize, "the owner word is the last word of the object");
  static_assert(kOffJoinPolicy + sizeof(std::uint32_t) <= kOffOwner, "game-visible fields end before the owner word");
  static_assert(kCallbackBytes == 15 * kCallbackStride, "15 delegates");
  static_assert(8 + kCallbackBytes <= 0x1E8, "callbacks end where the state word begins");
  QCHECK(kCbInviteReceived * kCallbackStride + kCallbackStride == kCallbackBytes);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: social_abi_test <cnsovrsocial_vtable.txt>\n");
    return 2;
  }
  CheckFixture(argv[1]);
  CheckSymbols();
  CheckLayout();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "social_abi_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("social_abi_test: slot table, enum and symbol hashes match the recorded ground truth\n");
  return 0;
}

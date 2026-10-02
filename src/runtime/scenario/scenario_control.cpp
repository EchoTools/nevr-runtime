#include "runtime/scenario/scenario_control.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr_common.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/social_roster.h"
#include "runtime/compat/ws_bridge.h"
#include "runtime/lifecycle/config.h"
#include "runtime/patch/party_invite_gate.h"
#include "runtime/patch/social_facade.h"
#include "runtime/scenario/scenario_protocol.h"

namespace ScenarioControl {
namespace {

// The friend row's invite button, measured in ReVault (echovr.exe):
//   script node 0x140dddf60 turns the row's user id string into a 16-byte xpid with SNSUserID
//   (0x1400f6c10) and posts FUN_14018aa90(netGame, &xpid) on the NetGame's deferred method queue
//   (0x140f4b690 with queue = netGame + 0x2b20). 0x14018aa90 runs the invite pre-checks and calls
//   the social object's SendInvite slot. "fire friend_invite" does exactly what the node does.
constexpr std::uint64_t kSnsUserIdVA = 0x1400F6C10;
constexpr std::uint64_t kInviteHandlerVA = 0x14018AA90;
constexpr std::uint64_t kDeferredCallVA = 0x140F4B690;
// R15NetAddFriendNode (run 0x140dd90f0) is the same node shape and posts 0x1401870f0 instead (provider
// checks, then social slot 37 OpenFriendRequestUI(0, account)).
constexpr std::uint64_t kAddFriendHandlerVA = 0x1401870F0;
// R15NetPartyRespondToInviteNode (run 0x140dddd30) posts, through the int-argument deferred call
// 0x140198650(queue, netGame, handler, invite index), 0x140188bf0 to accept (bounds-checks the
// index with social slot 70 InviteCount, then slot 73 AcceptInvite) or 0x140188f40 to dismiss
// (slot 74 DismissInvite). Measured from raw disassembly.
constexpr std::uint64_t kDeferredCallU32VA = 0x140198650;
constexpr std::uint64_t kAcceptInviteHandlerVA = 0x140188BF0;
constexpr std::uint64_t kDismissInviteHandlerVA = 0x140188F40;
constexpr std::uintptr_t kDeferredQueueOffset = 0x2B20;
constexpr std::uintptr_t kNetGameOffset = 0x8518;  // g_pGame -> CR15NetGame* (social_facade.cpp)

// First 16 bytes of each, read from echovr.exe .text (b6d08277e5846900).
constexpr std::array<std::uint8_t, 16> kSnsUserIdPrologue = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74,
                                                             0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48};
constexpr std::array<std::uint8_t, 16> kInviteHandlerPrologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
                                                                 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0xFA};
constexpr std::array<std::uint8_t, 16> kDeferredCallPrologue = {0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x41, 0x56,
                                                                0x41, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x83, 0xB9};

constexpr std::array<std::uint8_t, 16> kAddFriendHandlerPrologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
                                                                    0xEC, 0x20, 0x48, 0x83, 0xB9, 0xC8, 0x47, 0x06};

constexpr std::array<std::uint8_t, 16> kDeferredCallU32Prologue = {0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x57, 0x41,
                                                                   0x56, 0x48, 0x83, 0xEC, 0x20, 0x83, 0xB9, 0xF8};
constexpr std::array<std::uint8_t, 16> kRespondInviteHandlerPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                                                        0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x8B, 0xFA, 0x48};
using DeferredCallU32Fn = void (*)(void* queue, void* target, void* method, std::uint32_t arg);

using SnsUserIdFn = std::uint64_t* (*)(std::uint64_t* out, const char* user);
using DeferredCallFn = void (*)(void* queue, void* target, void* method, std::uint64_t* args);

// A fire runs on the game thread (OnFrame), where the script nodes run.
struct FireRequest {
  std::function<std::string()> job;  // returns "" when done, else why not
  std::promise<std::string> result;
};

std::atomic<bool> g_stop{false};
std::atomic<SOCKET> g_listen{INVALID_SOCKET};
std::atomic<SOCKET> g_client{INVALID_SOCKET};
// Never destroyed: a joinable std::thread destructor at process exit calls std::terminate, and a
// join at DLL detach runs under the loader lock. Stop() is the real teardown.
std::thread* g_thread = nullptr;
std::mutex g_fireMutex;
std::string g_lastFireUser;  // the user id a fire resolved ("self", "friend"), under g_fireMutex
std::deque<std::shared_ptr<FireRequest>> g_fireQueue;

void* NetGame() {
  if (g_pGame == nullptr) return nullptr;
  void* netGame = nullptr;
  std::memcpy(&netGame, static_cast<const std::uint8_t*>(g_pGame) + kNetGameOffset, sizeof(netGame));
  return netGame;
}

template <std::size_t N>
void* Checked(std::uint64_t va, const std::array<std::uint8_t, N>& prologue, const char* name, std::string* error) {
  void* target = nevr::ResolveVA_Checked(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress), va);
  if (target == nullptr || !nevr::ValidatePrologue(target, prologue.data(), prologue.size())) {
    *error = std::string(name) + " prologue mismatch: echovr.exe is not the build these addresses were read from";
    return nullptr;
  }
  return target;
}

// Game thread. Does what the node's run function does: SNSUserID on the user string, then post the
// node's handler on the NetGame deferred queue. Returns "" when posted, else why not.
std::string FireNode(bool addFriend, const std::string& user) {
  std::string error;
  void* netGame = NetGame();
  if (netGame == nullptr) return "no NetGame yet";
  auto* snsUserId = reinterpret_cast<SnsUserIdFn>(Checked(kSnsUserIdVA, kSnsUserIdPrologue, "SNSUserID", &error));
  // PartyInviteGate detours this handler for tracing after validating these same bytes; then the
  // first bytes are its jump, and calling the address goes through the trace, as the script node's
  // call does.
  void* handler = addFriend ? Checked(kAddFriendHandlerVA, kAddFriendHandlerPrologue, "add friend handler", &error)
                  : PartyInviteGate::InviteHandlerTraced()
                      ? nevr::ResolveVA_Checked(reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress), kInviteHandlerVA)
                      : Checked(kInviteHandlerVA, kInviteHandlerPrologue, "friend invite handler", &error);
  auto* defer = reinterpret_cast<DeferredCallFn>(Checked(kDeferredCallVA, kDeferredCallPrologue, "deferred call", &error));
  if (snsUserId == nullptr || handler == nullptr || defer == nullptr) return error;
  std::array<std::uint64_t, 2> xpid{};
  snsUserId(xpid.data(), user.c_str());
  if ((xpid[0] & 0xF) == 0 || xpid[1] == 0) return "SNSUserID did not parse \"" + user + "\" into a provider and account";
  Log(EchoVR::LogLevel::Info,
      "[NEVR.SCENARIO] fire %s user=%s provider=%llu account=%llu: posting handler 0x%llx on the NetGame "
      "deferred queue, as script node %s does",
      addFriend ? "add_friend" : "friend_invite", user.c_str(), static_cast<unsigned long long>(xpid[0] & 0xF), static_cast<unsigned long long>(xpid[1]),
      static_cast<unsigned long long>(addFriend ? kAddFriendHandlerVA : kInviteHandlerVA),
      addFriend ? "R15NetAddFriendNode (0x140dd90f0)" : "R15NetPartySendInviteNode (0x140dddf60)");
  defer(static_cast<std::uint8_t*>(netGame) + kDeferredQueueOffset, netGame, handler, xpid.data());
  return std::string();
}

// Game thread. Does what R15NetPartyRespondToInviteNode's run function does for a resolved index.
std::string FireRespondInvite(std::uint32_t index, bool accept) {
  std::string error;
  void* netGame = NetGame();
  if (netGame == nullptr) return "no NetGame yet";
  auto* defer = reinterpret_cast<DeferredCallU32Fn>(
      Checked(kDeferredCallU32VA, kDeferredCallU32Prologue, "int deferred call", &error));
  void* handler = Checked(accept ? kAcceptInviteHandlerVA : kDismissInviteHandlerVA, kRespondInviteHandlerPrologue,
                          accept ? "accept invite handler" : "dismiss invite handler", &error);
  if (defer == nullptr || handler == nullptr) return error;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.SCENARIO] fire respond_to_invite index=%u accept=%d: posting handler 0x%llx on the NetGame deferred "
      "queue, as R15NetPartyRespondToInviteNode (0x140dddd30) does",
      index, accept ? 1 : 0,
      static_cast<unsigned long long>(accept ? kAcceptInviteHandlerVA : kDismissInviteHandlerVA));
  defer(static_cast<std::uint8_t*>(netGame) + kDeferredQueueOffset, netGame, handler, index);
  return std::string();
}

// ---------------------------------------------------------------------------------------------
// Script-node entry points beyond the three above, each measured in ReVault (echovr.exe) and done the
// way the node's run function does it: post its handler on the NetGame deferred queue, or call it
// where the node calls it directly (those nodes call directly whenever they run on the game thread
// with the queue's direct flag set, so a call here from the game thread is the same path).
// docs/design/2026-10-01-social-features-test-plan.md has the node catalog.
// ---------------------------------------------------------------------------------------------
constexpr std::uint64_t kDeferredCallNoArgVA = 0x140198460;
constexpr std::array<std::uint8_t, 16> kDeferredCallNoArgPrologue = {0x40, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20,
                                                                     0x83, 0xB9, 0xF8, 0x01, 0x00, 0x00, 0x00, 0x49};
using DeferredCallNoArgFn = void (*)(void* queue, void* target, void* method);

// R15NetInviteUsersNode (0x140ddc700): mode 0 -> 0x140187170 (slot 38); mode 1 -> 0x140187330 (slot 40) or,
// with a user, 0x140187230 (slot 39); mode 2 -> 0x1401874f0 (slot 43) or, with a user, 0x1401873f0 (slot 42).
constexpr std::array<std::uint8_t, 16> kInviteUsersNoArgPrologue = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
                                                                    0xD9, 0x48, 0x8B, 0x89, 0xD0, 0x28, 0x00, 0x00};
constexpr std::array<std::uint8_t, 16> kInviteUsersUserPrologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
                                                                   0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0xFA};
// R15NetRequestProfileNode (0x140de01f0) posts 0x1401a1930 with the 16-byte user id.
constexpr std::uint64_t kRequestProfileHandlerVA = 0x1401A1930;
constexpr std::array<std::uint8_t, 16> kRequestProfilePrologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                                  0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x55};
// R15NetPartyJoinNode (0x140ddd3a0) calls 0x140189570(netGame, party id) -> slot 2 JoinInternal.
constexpr std::uint64_t kPartyJoinHandlerVA = 0x140189570;
constexpr std::array<std::uint8_t, 16> kPartyJoinPrologue = {0x48, 0x8B, 0x89, 0xC8, 0x47, 0x06, 0x00, 0x48,
                                                             0x85, 0xC9, 0x0F, 0x85, 0x90, 0x0D, 0x48, 0x00};
using PartyJoinFn = void (*)(void* netGame, std::uint64_t partyId);
// R15NetPartyLockNode (0x140ddd870) calls 0x140189f20(netGame, lock, mask): it sets or clears the mask
// in the byte at netGame+0x647ea and, when that byte turns zero/nonzero, sets/clears bit 1 of the
// social object's flags word (the host's joinable bit).
constexpr std::uint64_t kPartyLockHandlerVA = 0x140189F20;
constexpr std::array<std::uint8_t, 16> kPartyLockPrologue = {0x45, 0x33, 0xC9, 0x41, 0x0F, 0xB6, 0xC0, 0x44,
                                                             0x38, 0x89, 0xEA, 0x47, 0x06, 0x00, 0x45, 0x8B};
using PartyLockFn = void (*)(void* netGame, std::uint32_t lock, std::uint8_t mask);
// R15NetPartySetJoinPolicyNode (0x140dde110) calls 0x14018ab90(netGame, policy) -> slot 16.
constexpr std::uint64_t kSetJoinPolicyHandlerVA = 0x14018AB90;
constexpr std::array<std::uint8_t, 16> kSetJoinPolicyPrologue = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
                                                                 0xD9, 0x48, 0x8B, 0x89, 0xC8, 0x47, 0x06, 0x00};
using SetJoinPolicyFn = void (*)(void* netGame, std::uint32_t policy);
// R15NetVoipMuteSelfNode (0x140de5fc0) posts 0x1401b24a0 with the mute flag (u32 deferred call).
constexpr std::uint64_t kMuteSelfHandlerVA = 0x1401B24A0;
constexpr std::array<std::uint8_t, 16> kMuteSelfPrologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x56, 0x48, 0x83,
                                                            0xEC, 0x20, 0x48, 0x83, 0x3D, 0x56, 0x97, 0x51};
// R15NetSocialGroupsSetActiveNode (0x140d8eba0): groups = [netGame+0x2a00] (0x1401b64f0), range-checks
// the index against groups+0x40, then posts 0x1401adf90(groups, index) on the groups' queue.
constexpr std::uint64_t kSocialGroupsVA = 0x1401B64F0;
constexpr std::array<std::uint8_t, 8> kSocialGroupsPrologue = {0x48, 0x8B, 0x81, 0x00, 0x2A, 0x00, 0x00, 0xC3};
constexpr std::uint64_t kSetActiveGroupHandlerVA = 0x1401ADF90;
constexpr std::array<std::uint8_t, 16> kSetActiveGroupPrologue = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83,
                                                                  0xEC, 0x30, 0x8B, 0x41, 0x0C, 0x48, 0x8B, 0xD9};
using GroupsFn = void* (*)(void* netGame);
using SetActiveGroupFn = void (*)(void* groups, std::uint64_t index);
// R15NetEnableSocialFeatureNode (0x140ddb650) maps feature 0..4 to mask 1,2,4,8,0xff and calls
// 0x140cd3850(mask, enable), which sets or clears the mask in the u16 at 0x142025bf4.
constexpr std::uint64_t kEnableFeatureVA = 0x140CD3850;
constexpr std::array<std::uint8_t, 16> kEnableFeaturePrologue = {0x44, 0x0F, 0xB7, 0xC1, 0x85, 0xD2, 0x74, 0x09,
                                                                 0x66, 0x44, 0x09, 0x05, 0x94, 0x23, 0x35, 0x01};
constexpr std::uint64_t kSocialFeaturesVA = 0x142025BF4;
using EnableFeatureFn = void (*)(std::uint32_t mask, std::uint32_t enable);
// R15NetSetPartyMemberStringNode / R15NetSetPartyStringNode post 0x1401b07e0 / 0x1401b0940 with
// (netGame, key, value): the member one writes into the JSON slot 30 MemberDataWritable returns, the
// party one into the party JSON (host only).
constexpr std::uint64_t kSetMemberStringHandlerVA = 0x1401B07E0;
constexpr std::array<std::uint8_t, 16> kSetMemberStringPrologue = {0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74,
                                                                   0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
constexpr std::uint64_t kSetPartyStringHandlerVA = 0x1401B0940;
constexpr std::array<std::uint8_t, 16> kSetPartyStringPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                                  0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48};
using SetStringFn = void (*)(void* netGame, const char* key, const char* value);
// R15NetRefreshFriendsNode (0x140ddf9a0) posts 0x14019ae10 (no argument) -> slot 45 RefreshFriends.
constexpr std::uint64_t kRefreshFriendsHandlerVA = 0x14019AE10;
constexpr std::array<std::uint8_t, 16> kRefreshFriendsPrologue = {0x48, 0x8B, 0x89, 0xC8, 0x47, 0x06, 0x00, 0x48,
                                                                  0x85, 0xC9, 0x74, 0x0A, 0x48, 0x8B, 0x01, 0x48};
// R15NetRefreshRecentlyMetUsersNode (0x140ddfcc0) posts 0x14019b870 (no argument) -> slot 57.
// CR15NetGame::FindIfPartyHost (echovr.exe 0x14016afc0): what R15NetFindMatchNode (0x140ddb740) posts
// for Find Arena. It runs Find (0x140168500) only when there is no social object or IsHost (slot 24)
// is nonzero; a non-host party member's find is dropped. Takes a 0x48-byte request (ReVault decode,
// 2026-10-02): +0x00 u8 type (0 public), +0x04 u32 (the node's default 3), +0x08 gametype symbol,
// +0x10 level symbol (-1: the node's default), +0x18 group (-1), +0x20 u16[16] slots, +0x40 count.
constexpr std::uint64_t kFindIfPartyHostVA = 0x14016AFC0;
constexpr std::array<std::uint8_t, 16> kFindIfPartyHostPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                                                   0xEC, 0x40, 0x48, 0x8B, 0xF9, 0x48, 0x8B, 0xDA};
constexpr std::uint64_t kSymbolEchoArena = 0xCB60A4DE7E1CAF73ULL;  // "echo_arena" (symbol_corpus.cpp)
using FindIfPartyHostFn = void (*)(void* netGame, void* request);

constexpr std::uint64_t kRefreshRecentlyMetHandlerVA = 0x14019B870;
constexpr std::array<std::uint8_t, 16> kRefreshRecentlyMetPrologue = {0x48, 0x8B, 0x89, 0xC8, 0x47, 0x06, 0x00, 0x48,
                                                                      0x85, 0xC9, 0x74, 0x0A, 0x48, 0x8B, 0x01, 0x48};
// R15NetVoipMuteUserNode (0x140de6110 -> 0x140f47cd0): for a user not in a lobby slot it posts
// 0x1401cc930(netGame, &id16, mute) through 0x140d2ca10, which adds or removes the id in the mute list
// (ids at [netGame+0x64780], count at netGame+0x64788) and stores it in the profile ("mute|users").
// The node's binder only supplies the NetGame; this calls the handler with it directly.
constexpr std::uint64_t kMuteUserHandlerVA = 0x1401CC930;
constexpr std::array<std::uint8_t, 15> kMuteUserPrologue = {0x4C, 0x8B, 0xDC, 0x55, 0x41, 0x54, 0x41, 0x55,
                                                            0x48, 0x83, 0xEC, 0x70, 0x0F, 0xB6, 0x02};
using MuteUserFn = void (*)(void* netGame, const std::uint64_t* id, std::uint32_t mute);
constexpr std::uintptr_t kMuteListOffset = 0x64780;
constexpr std::uintptr_t kMuteCountOffset = 0x64788;
constexpr std::uintptr_t kVoipFlagsOffset = 0x2DA0;  // netGame: pointer to the u64 whose bit 38 is "self muted"
constexpr std::uint64_t kSelfMutedBit = 1ULL << 38;

void* Base() { return EchoVR::g_GameBaseAddress; }

std::string PostNoArg(void* netGame, void* handler, std::string* error) {
  auto* defer = reinterpret_cast<DeferredCallNoArgFn>(
      Checked(kDeferredCallNoArgVA, kDeferredCallNoArgPrologue, "no-argument deferred call", error));
  if (defer == nullptr || handler == nullptr) return *error;
  defer(static_cast<std::uint8_t*>(netGame) + kDeferredQueueOffset, netGame, handler);
  return std::string();
}

std::string PostUserId(void* netGame, void* handler, const std::string& user, std::string* error) {
  auto* snsUserId = reinterpret_cast<SnsUserIdFn>(Checked(kSnsUserIdVA, kSnsUserIdPrologue, "SNSUserID", error));
  auto* defer = reinterpret_cast<DeferredCallFn>(Checked(kDeferredCallVA, kDeferredCallPrologue, "deferred call", error));
  if (snsUserId == nullptr || defer == nullptr || handler == nullptr) return *error;
  std::array<std::uint64_t, 2> xpid{};
  snsUserId(xpid.data(), user.c_str());
  if ((xpid[0] & 0xF) == 0 || xpid[1] == 0) return "SNSUserID did not parse \"" + user + "\" into a provider and account";
  defer(static_cast<std::uint8_t*>(netGame) + kDeferredQueueOffset, netGame, handler, xpid.data());
  return std::string();
}

// Game thread.
std::string FireAction(const ScenarioProtocol::Command& cmd) {
  std::string error;
  void* netGame = NetGame();
  if (netGame == nullptr) return "no NetGame yet";
  std::string user = cmd.user;
  if (user == "self") {
    const std::uint64_t self = SocialParty::Global().Snapshot().selfId;
    if (self == 0) return "no local user yet";
    user = "OVR-ORG-" + std::to_string(self);
  } else if (user == "friend") {  // the first friend in the roster: a real player, read-only use
    std::uint64_t friendId = 0;
    if (!SocialRoster::Global().IdAt(0, &friendId) || friendId == 0) return "the roster has no friend yet";
    user = "OVR-ORG-" + std::to_string(friendId);
  }
  {
    std::lock_guard<std::mutex> lock(g_fireMutex);
    g_lastFireUser = user;
  }
  Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] fire %s user=%s number=%llu flag=%d party=%llu key=%s",
      cmd.action.c_str(), user.c_str(), static_cast<unsigned long long>(cmd.number), cmd.flag ? 1 : 0,
      static_cast<unsigned long long>(cmd.partyId), cmd.key.c_str());
  if (cmd.action == "invite_users") {
    static constexpr std::uint64_t kNoArg[3] = {0x140187170, 0x140187330, 0x1401874F0};
    if (cmd.number == 0 || user.empty())
      return PostNoArg(netGame, Checked(kNoArg[cmd.number], kInviteUsersNoArgPrologue, "invite users handler", &error), &error);
    const std::uint64_t va = cmd.number == 1 ? 0x140187230 : 0x1401873F0;
    return PostUserId(netGame, Checked(va, kInviteUsersUserPrologue, "invite users handler", &error), user, &error);
  }
  if (cmd.action == "request_profile")
    return PostUserId(netGame, Checked(kRequestProfileHandlerVA, kRequestProfilePrologue, "request profile handler", &error),
                      user, &error);
  if (cmd.action == "find_arena") {
    auto* find = reinterpret_cast<FindIfPartyHostFn>(
        Checked(kFindIfPartyHostVA, kFindIfPartyHostPrologue, "find if party host", &error));
    if (find == nullptr) return error;
    // FindIfPartyHost writes the request into the NetClientLobby at *(netGame+0x40), which the game only
    // has once it has entered a lobby (measured: at "logged in" it is null, and the call faulted writing
    // +0x698, run 20261002T093432).
    void* lobby = nullptr;
    std::memcpy(&lobby, static_cast<std::uint8_t*>(netGame) + 0x40, sizeof(lobby));
    if (lobby == nullptr) return "no lobby object yet (the game creates it when it enters a lobby)";
    alignas(8) std::array<std::uint8_t, 0x48> request{};
    const std::uint32_t nodeDefault = 3;
    const std::uint64_t gametype = kSymbolEchoArena;
    const std::uint64_t unset = UINT64_MAX;
    std::memcpy(request.data() + 0x04, &nodeDefault, sizeof(nodeDefault));
    std::memcpy(request.data() + 0x08, &gametype, sizeof(gametype));
    std::memcpy(request.data() + 0x10, &unset, sizeof(unset));
    std::memcpy(request.data() + 0x18, &unset, sizeof(unset));
    find(netGame, request.data());
    return std::string();
  }
  if (cmd.action == "party_join") {
    auto* join = reinterpret_cast<PartyJoinFn>(Checked(kPartyJoinHandlerVA, kPartyJoinPrologue, "party join handler", &error));
    if (join == nullptr) return error;
    join(netGame, cmd.partyId);
    return std::string();
  }
  if (cmd.action == "party_lock") {
    auto* lock = reinterpret_cast<PartyLockFn>(Checked(kPartyLockHandlerVA, kPartyLockPrologue, "party lock handler", &error));
    if (lock == nullptr) return error;
    lock(netGame, cmd.flag ? 1U : 0U, static_cast<std::uint8_t>(cmd.number));
    return std::string();
  }
  if (cmd.action == "set_join_policy") {
    auto* policy = reinterpret_cast<SetJoinPolicyFn>(
        Checked(kSetJoinPolicyHandlerVA, kSetJoinPolicyPrologue, "set join policy handler", &error));
    if (policy == nullptr) return error;
    policy(netGame, static_cast<std::uint32_t>(cmd.number));
    return std::string();
  }
  if (cmd.action == "voip_mute_self") {
    auto* defer = reinterpret_cast<DeferredCallU32Fn>(
        Checked(kDeferredCallU32VA, kDeferredCallU32Prologue, "int deferred call", &error));
    void* handler = Checked(kMuteSelfHandlerVA, kMuteSelfPrologue, "mute self handler", &error);
    if (defer == nullptr || handler == nullptr) return error;
    defer(static_cast<std::uint8_t*>(netGame) + kDeferredQueueOffset, netGame, handler, cmd.flag ? 1U : 0U);
    return std::string();
  }
  if (cmd.action == "voip_mute_user") {
    auto* snsUserId = reinterpret_cast<SnsUserIdFn>(Checked(kSnsUserIdVA, kSnsUserIdPrologue, "SNSUserID", &error));
    auto* mute = reinterpret_cast<MuteUserFn>(Checked(kMuteUserHandlerVA, kMuteUserPrologue, "mute user handler", &error));
    if (snsUserId == nullptr || mute == nullptr) return error;
    std::array<std::uint64_t, 2> xpid{};
    snsUserId(xpid.data(), user.c_str());
    if ((xpid[0] & 0xF) == 0 || xpid[1] == 0) return "SNSUserID did not parse \"" + user + "\"";
    mute(netGame, xpid.data(), cmd.flag ? 1U : 0U);
    return std::string();
  }
  if (cmd.action == "social_groups_set_active") {
    auto* groupsOf = reinterpret_cast<GroupsFn>(Checked(kSocialGroupsVA, kSocialGroupsPrologue, "social groups getter", &error));
    auto* setActive = reinterpret_cast<SetActiveGroupFn>(
        Checked(kSetActiveGroupHandlerVA, kSetActiveGroupPrologue, "set active group handler", &error));
    if (groupsOf == nullptr || setActive == nullptr) return error;
    void* groups = groupsOf(netGame);
    if (groups == nullptr) return "no social groups object";
    std::uint64_t count = 0;
    std::memcpy(&count, static_cast<const std::uint8_t*>(groups) + 0x40, sizeof(count));
    std::uint64_t index = cmd.number;
    if (index == ScenarioProtocol::kCurrentGroup) {
      std::uint32_t active = 0;
      std::memcpy(&active, static_cast<const std::uint8_t*>(groups) + 0xC, sizeof(active));
      index = active;
    }
    if (index >= count) return "group index " + std::to_string(index) + " >= " + std::to_string(count) + " groups";
    Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] social group set active index=%llu of %llu",
        static_cast<unsigned long long>(index), static_cast<unsigned long long>(count));
    setActive(groups, index);
    return std::string();
  }
  if (cmd.action == "enable_social_feature") {
    static constexpr std::uint32_t kMasks[5] = {1, 2, 4, 8, 0xFF};
    auto* enable = reinterpret_cast<EnableFeatureFn>(
        Checked(kEnableFeatureVA, kEnableFeaturePrologue, "enable social feature", &error));
    if (enable == nullptr) return error;
    enable(kMasks[cmd.number], cmd.flag ? 1U : 0U);
    return std::string();
  }
  if (cmd.action == "set_party_member_string" || cmd.action == "set_party_string") {
    const bool member = cmd.action == "set_party_member_string";
    auto* set = reinterpret_cast<SetStringFn>(
        member ? Checked(kSetMemberStringHandlerVA, kSetMemberStringPrologue, "set member string handler", &error)
               : Checked(kSetPartyStringHandlerVA, kSetPartyStringPrologue, "set party string handler", &error));
    if (set == nullptr) return error;
    std::array<char, 0x40> key{};
    std::array<char, 0x40> value{};
    std::memcpy(key.data(), cmd.key.data(), cmd.key.size());
    std::memcpy(value.data(), cmd.value.data(), cmd.value.size());
    set(netGame, key.data(), value.data());
    return std::string();
  }
  if (cmd.action == "refresh_friends")
    return PostNoArg(netGame, Checked(kRefreshFriendsHandlerVA, kRefreshFriendsPrologue, "refresh friends handler", &error),
                     &error);
  if (cmd.action == "refresh_recently_met")
    return PostNoArg(netGame, Checked(kRefreshRecentlyMetHandlerVA, kRefreshRecentlyMetPrologue, "refresh recently met handler",
                                      &error), &error);
  return "unknown action " + cmd.action;
}

// What the game itself holds for the actions above, for the runner to check against.
nlohmann::json GameStateJson() {
  nlohmann::json out = nlohmann::json::object();
  void* netGame = NetGame();
  std::uint16_t features = 0;
  std::memcpy(&features, static_cast<const std::uint8_t*>(Base()) + (kSocialFeaturesVA - 0x140000000ULL), sizeof(features));
  out["social_features"] = features;
  if (netGame == nullptr) return out;
  const auto* bytes = static_cast<const std::uint8_t*>(netGame);
  const std::uint64_t* voipFlags = nullptr;
  std::memcpy(&voipFlags, bytes + kVoipFlagsOffset, sizeof(voipFlags));
  if (voipFlags != nullptr) out["self_muted"] = (*voipFlags & kSelfMutedBit) != 0;
  std::uint64_t muteCount = 0;
  const std::uint64_t* muteList = nullptr;
  std::memcpy(&muteCount, bytes + kMuteCountOffset, sizeof(muteCount));
  std::memcpy(&muteList, bytes + kMuteListOffset, sizeof(muteList));
  nlohmann::json muted = nlohmann::json::array();
  for (std::uint64_t i = 0; muteList != nullptr && i < muteCount && i < 256; ++i) muted.push_back(muteList[i * 2 + 1]);
  out["muted_users"] = muted;
  const void* groups = nullptr;
  std::memcpy(&groups, bytes + 0x2A00, sizeof(groups));
  if (groups != nullptr) {
    std::uint32_t active = 0;
    std::uint64_t count = 0;
    std::memcpy(&active, static_cast<const std::uint8_t*>(groups) + 0xC, sizeof(active));
    std::memcpy(&count, static_cast<const std::uint8_t*>(groups) + 0x40, sizeof(count));
    out["social_group_active"] = active;
    out["social_group_count"] = count;
  }
  return out;
}

nlohmann::json StateJson() {
  nlohmann::json out;
  out["ok"] = true;
  out["netgame"] = NetGame() != nullptr;
  const SocialFacade::PartyStateForTest party = SocialFacade::PartyForTest();
  out["party"] = {{"id", party.partyId},           {"room", party.roomId},
                  {"joining", party.joining},      {"joinable", party.joinable},
                  {"locked", party.locked},        {"join_policy", party.joinPolicy},
                  {"share_dirty", party.shareDirty},
                  {"members", party.memberIds},
                  {"member_data", party.memberData},
                  {"data", party.partyData},
                  {"party_data_shared", party.partyDataShared},
                  {"member_data_shared", party.memberDataShared},
                  {"last_shared", party.lastShared}};
  nlohmann::json friends = nlohmann::json::array();
  std::uint64_t id = 0;
  for (std::uint32_t index = 0; SocialRoster::Global().IdAt(index, &id); ++index) {
    friends.push_back({{"id", id},
                       {"online", SocialRoster::Global().OnlineAt(index)},
                       {"invitable", SocialFacade::FriendInvitableForTest(id)},
                       {"text", SocialRoster::Global().StatusTextAt(index)},
                       {"party", SocialRoster::Global().PartyIdAt(index)}});
  }
  out["friends"] = friends;
  nlohmann::json recent = nlohmann::json::array();
  for (const SocialFacade::RecentlyMetForTest& user : SocialFacade::RecentlyMetUsersForTest()) {
    recent.push_back({{"id", user.id}, {"name", user.name}, {"status", user.status}, {"text", user.text},
                      {"invitable", user.invitable}, {"joinable", user.joinable}, {"party", user.partyId}});
  }
  out["recently_met"] = {{"refreshing", SocialFacade::RecentlyMetRefreshingForTest()},
                         {"count", recent.size()},
                         {"users", recent}};
  nlohmann::json invites = nlohmann::json::array();
  for (const SocialFacade::InviteForTest& invite : SocialFacade::InvitesForTest()) {
    invites.push_back({{"party", invite.partyId}, {"sender", invite.senderId}});
  }
  out["invites"] = invites;
  out["game"] = GameStateJson();
  return out;
}

nlohmann::json Fail(const std::string& why) { return {{"ok", false}, {"error", why}}; }

nlohmann::json Handle(const std::string& line) {
  ScenarioProtocol::Command cmd;
  std::string error;
  if (!ScenarioProtocol::ParseCommand(line, &cmd, &error)) return Fail(error);
  switch (cmd.op) {
    case ScenarioProtocol::Op::kState:
      return StateJson();
    case ScenarioProtocol::Op::kInjectFriendStatus: {
      Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] inject FriendStatusNotify id=%llu status=%u",
          static_cast<unsigned long long>(cmd.friendId), static_cast<unsigned>(cmd.status));
      const std::string frame = ScenarioProtocol::BuildFriendStatusNotify(cmd.friendId, cmd.status);
      if (!InjectServerFrameForTest(frame, &error)) return Fail(error);
      return {{"ok", true}};
    }
    case ScenarioProtocol::Op::kInjectFriendNotify: {
      Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] inject %s id=%llu", cmd.notifyName.c_str(),
          static_cast<unsigned long long>(cmd.friendId));
      const ScenarioProtocol::FriendNotify* notify = ScenarioProtocol::FindFriendNotify(cmd.notifyName);
      if (notify == nullptr) return Fail("unknown notify " + cmd.notifyName);
      if (!InjectServerFrameForTest(ScenarioProtocol::BuildFriendNotify(*notify, cmd.friendId), &error)) return Fail(error);
      return {{"ok", true}};
    }
    case ScenarioProtocol::Op::kInjectPartyInvite: {
      Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] inject PartyInviteNotify party=%llu inviter=%llu",
          static_cast<unsigned long long>(cmd.partyId), static_cast<unsigned long long>(cmd.inviterId));
      if (!InjectServerFrameForTest(ScenarioProtocol::BuildPartyInviteNotify(cmd.partyId, cmd.inviterId), &error)) {
        return Fail(error);
      }
      return {{"ok", true}};
    }
    case ScenarioProtocol::Op::kInjectPartyJoinFailure: {
      Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] inject PartyJoinFailure party=%llu code=%u",
          static_cast<unsigned long long>(cmd.partyId), static_cast<unsigned>(cmd.failureCode));
      if (!InjectServerFrameForTest(ScenarioProtocol::BuildPartyJoinFailure(cmd.partyId, cmd.failureCode), &error)) {
        return Fail(error);
      }
      return {{"ok", true}};
    }
    case ScenarioProtocol::Op::kInjectFriendPresence: {
      Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] inject FriendPresenceNotify id=%llu party=%llu joinable=%d text=%s",
          static_cast<unsigned long long>(cmd.friendId), static_cast<unsigned long long>(cmd.partyId), cmd.flag ? 1 : 0,
          cmd.value.c_str());
      if (!InjectServerFrameForTest(ScenarioProtocol::BuildFriendPresenceNotify(cmd.friendId, cmd.partyId, cmd.flag, cmd.value),
                                    &error)) {
        return Fail(error);
      }
      return {{"ok", true}};
    }
    case ScenarioProtocol::Op::kInjectPartyMember: {
      const std::uint64_t party = cmd.partyId != 0 ? cmd.partyId : SocialFacade::PartyForTest().partyId;
      if (party == 0) return Fail("inject " + cmd.notifyName + ": no current party and no \"party\" given");
      Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] inject %s party=%llu member=%llu", cmd.notifyName.c_str(),
          static_cast<unsigned long long>(party), static_cast<unsigned long long>(cmd.memberId));
      if (!InjectServerFrameForTest(ScenarioProtocol::BuildPartyMemberNotify(cmd.notifyName.c_str(), party, cmd.memberId),
                                    &error)) {
        return Fail(error);
      }
      return {{"ok", true}};
    }
    case ScenarioProtocol::Op::kInjectRecentlyMet: {
      Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] inject RecentlyMetListResponse users=%zu", cmd.people.size());
      if (!InjectServerFrameForTest(ScenarioProtocol::BuildRecentlyMetListResponse(cmd.people), &error)) return Fail(error);
      return {{"ok", true}};
    }
    case ScenarioProtocol::Op::kInjectPartyData: {
      const std::uint64_t party = cmd.partyId != 0 ? cmd.partyId : SocialFacade::PartyForTest().partyId;
      if (party == 0) return Fail("inject PartyDataNotify: no current party and no \"party\" given");
      Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] inject PartyDataNotify party=%llu member=%llu bytes=%zu",
          static_cast<unsigned long long>(party), static_cast<unsigned long long>(cmd.memberId), cmd.value.size());
      if (!InjectServerFrameForTest(ScenarioProtocol::BuildPartyDataNotify(party, cmd.memberId, 1, cmd.value), &error))
        return Fail(error);
      return {{"ok", true}};
    }
    case ScenarioProtocol::Op::kFireFriendInvite:
    case ScenarioProtocol::Op::kFireAddFriend:
    case ScenarioProtocol::Op::kFireRespondInvite:
    case ScenarioProtocol::Op::kFireAction: {
      auto request = std::make_shared<FireRequest>();
      if (cmd.op == ScenarioProtocol::Op::kFireAction)
        request->job = [cmd] { return FireAction(cmd); };
      else if (cmd.op == ScenarioProtocol::Op::kFireRespondInvite)
        request->job = [cmd] { return FireRespondInvite(cmd.inviteIndex, cmd.accept); };
      else
        request->job = [cmd] { return FireNode(cmd.op == ScenarioProtocol::Op::kFireAddFriend, cmd.user); };
      std::future<std::string> done = request->result.get_future();
      {
        std::lock_guard<std::mutex> lock(g_fireMutex);
        g_fireQueue.push_back(request);
      }
      if (done.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        return Fail("no game frame tick ran the fire within 5 s");
      }
      const std::string why = done.get();
      if (!why.empty()) return Fail(why);
      nlohmann::json reply = {{"ok", true}, {"posted", true}};
      if (cmd.op == ScenarioProtocol::Op::kFireAction) {
        std::lock_guard<std::mutex> lock(g_fireMutex);
        reply["user"] = g_lastFireUser;
      }
      return reply;
    }
  }
  return Fail("unhandled op");
}

void SendLine(SOCKET client, const std::string& text) {
  const std::string line = text + "\n";
  std::size_t sent = 0;
  while (sent < line.size()) {
    const int n = send(client, line.data() + sent, static_cast<int>(line.size() - sent), 0);
    if (n <= 0) return;
    sent += static_cast<std::size_t>(n);
  }
}

void Serve(SOCKET client) {
  std::string buffer;
  std::array<char, 4096> chunk{};
  while (!g_stop.load()) {
    const int n = recv(client, chunk.data(), static_cast<int>(chunk.size()), 0);
    if (n <= 0) return;
    buffer.append(chunk.data(), static_cast<std::size_t>(n));
    std::size_t newline = 0;
    while ((newline = buffer.find('\n')) != std::string::npos) {
      const std::string line = buffer.substr(0, newline);
      buffer.erase(0, newline + 1);
      if (line.empty()) continue;
      const nlohmann::json reply = Handle(line);
      if (!reply.value("ok", false)) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.SCENARIO] command failed: %s", reply.value("error", "").c_str());
      }
      SendLine(client, reply.dump());
    }
  }
}

void AcceptLoop() {
  while (!g_stop.load()) {
    const SOCKET client = accept(g_listen.load(), nullptr, nullptr);
    if (client == INVALID_SOCKET) return;  // Stop() closed the listener
    g_client.store(client);
    Serve(client);
    // Whoever takes it out of g_client closes it: this loop, or Stop() racing it.
    const SOCKET owned = g_client.exchange(INVALID_SOCKET);
    if (owned != INVALID_SOCKET) closesocket(owned);
  }
}

}  // namespace

void Start() {
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    Log(EchoVR::LogLevel::Error, "[NEVR.SCENARIO] control endpoint not started: WSAStartup failed");
    return;
  }
  const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  int len = static_cast<int>(sizeof(addr));
  if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&addr), len) != 0 ||
      listen(listener, 1) != 0 || getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    Log(EchoVR::LogLevel::Error, "[NEVR.SCENARIO] control endpoint not started: socket setup failed (wsa=%d)",
        WSAGetLastError());
    if (listener != INVALID_SOCKET) closesocket(listener);
    return;
  }
  g_listen.store(listener);
  g_thread = new std::thread(AcceptLoop);
  Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] control listening on 127.0.0.1:%u (test build only)",
      static_cast<unsigned>(ntohs(addr.sin_port)));
}

void OnFrame() {
  std::deque<std::shared_ptr<FireRequest>> pending;
  {
    std::lock_guard<std::mutex> lock(g_fireMutex);
    pending.swap(g_fireQueue);
  }
  for (const auto& request : pending) request->result.set_value(request->job());
}

void Stop() {
  g_stop.store(true);
  const SOCKET listener = g_listen.exchange(INVALID_SOCKET);
  if (listener != INVALID_SOCKET) closesocket(listener);
  const SOCKET client = g_client.exchange(INVALID_SOCKET);
  if (client != INVALID_SOCKET) closesocket(client);
  if (g_thread != nullptr && g_thread->joinable()) g_thread->join();
  Log(EchoVR::LogLevel::Info, "[NEVR.SCENARIO] control endpoint stopped");
}

}  // namespace ScenarioControl

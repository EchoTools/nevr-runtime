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
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr_common.h"
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

struct FireRequest {
  bool addFriend = false;  // false: friend_invite (R15NetPartySendInviteNode); true: add_friend (R15NetAddFriendNode)
  bool respond = false;    // respond_to_invite (R15NetPartyRespondToInviteNode)
  std::uint32_t inviteIndex = 0;
  bool accept = false;
  std::string user;
  std::promise<std::string> result;  // empty string = posted; otherwise the reason it was not
};

std::atomic<bool> g_stop{false};
std::atomic<SOCKET> g_listen{INVALID_SOCKET};
std::atomic<SOCKET> g_client{INVALID_SOCKET};
// Never destroyed: a joinable std::thread destructor at process exit calls std::terminate, and a
// join at DLL detach runs under the loader lock. Stop() is the real teardown.
std::thread* g_thread = nullptr;
std::mutex g_fireMutex;
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

nlohmann::json StateJson() {
  nlohmann::json out;
  out["ok"] = true;
  out["netgame"] = NetGame() != nullptr;
  const SocialFacade::PartyStateForTest party = SocialFacade::PartyForTest();
  out["party"] = {{"id", party.partyId}, {"joinable", party.joinable}, {"members", party.memberIds}};
  nlohmann::json friends = nlohmann::json::array();
  std::uint64_t id = 0;
  for (std::uint32_t index = 0; SocialRoster::Global().IdAt(index, &id); ++index) {
    friends.push_back({{"id", id},
                       {"online", SocialRoster::Global().OnlineAt(index)},
                       {"invitable", SocialFacade::FriendInvitableForTest(id)}});
  }
  out["friends"] = friends;
  nlohmann::json invites = nlohmann::json::array();
  for (const SocialFacade::InviteForTest& invite : SocialFacade::InvitesForTest()) {
    invites.push_back({{"party", invite.partyId}, {"sender", invite.senderId}});
  }
  out["invites"] = invites;
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
    case ScenarioProtocol::Op::kFireFriendInvite:
    case ScenarioProtocol::Op::kFireAddFriend:
    case ScenarioProtocol::Op::kFireRespondInvite: {
      auto request = std::make_shared<FireRequest>();
      request->addFriend = cmd.op == ScenarioProtocol::Op::kFireAddFriend;
      request->respond = cmd.op == ScenarioProtocol::Op::kFireRespondInvite;
      request->inviteIndex = cmd.inviteIndex;
      request->accept = cmd.accept;
      request->user = cmd.user;
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
      return {{"ok", true}, {"posted", true}};
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
  for (const auto& request : pending) request->result.set_value(request->respond ? FireRespondInvite(request->inviteIndex, request->accept)
                                               : FireNode(request->addFriend, request->user));
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

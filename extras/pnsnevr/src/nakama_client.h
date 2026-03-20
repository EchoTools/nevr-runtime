/*
 * pnsnevr - Nakama Client Wrapper
 *
 * Wraps the nakama-cpp SDK for use in NEVR.
 * This is a prototype implementation - production code should use
 * the full nakama-cpp SDK with proper async handling.
 */

#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

// Forward declarations for nakama-cpp types
// In production, include: #include <nakama-cpp/Nakama.h>
namespace Nakama {
class NClientInterface;
class NSessionInterface;
class NRtClientInterface;
using NClientPtr = std::shared_ptr<NClientInterface>;
using NSessionPtr = std::shared_ptr<NSessionInterface>;
using NRtClientPtr = std::shared_ptr<NRtClientInterface>;
}  // namespace Nakama

struct NakamaConfig;

// Friend data structure
struct NakamaFriend {
  std::string userId;
  std::string username;
  std::string displayName;
  std::string avatarUrl;
  int state;  // 0=friend, 1=invite_sent, 2=invite_received, 3=blocked
  bool online;
};

// Party data structure
struct NakamaParty {
  std::string partyId;
  std::string leaderId;
  std::vector<std::string> memberIds;
  bool open;
  int maxSize;
};

// Match data structure
struct NakamaMatch {
  std::string matchId;
  std::string ticketId;
  std::string status;  // "searching", "found", "cancelled"
  std::vector<std::string> playerIds;
};

// Presence data structure
struct NakamaPresence {
  std::string userId;
  std::string username;
  bool online;
  std::string status;
};

// Callback types
using FriendsCallback = std::function<void(const std::vector<NakamaFriend>&)>;
using PartyCallback = std::function<void(const NakamaParty&)>;
using MatchCallback = std::function<void(const NakamaMatch&)>;
using PresenceCallback = std::function<void(const std::vector<NakamaPresence>&)>;
using ErrorCallback = std::function<void(int code, const std::string& message)>;

class NakamaClient {
 public:
  NakamaClient(const NakamaConfig& config);
  ~NakamaClient();

  // Lifecycle
  bool Initialize();
  void Tick();  // Must be called periodically (~50ms)
  void Disconnect();

  // Authentication
  bool AuthenticateDevice(const std::string& deviceId);
  bool AuthenticateCustom(const std::string& customId);
  bool AuthenticateEmail(const std::string& email, const std::string& password);
  bool IsAuthenticated() const;
  std::string GetUserId() const;
  std::string GetUsername() const;

  // Friends API
  void GetFriendsList(FriendsCallback callback, ErrorCallback errorCallback = nullptr);
  void AddFriend(const std::string& userId, ErrorCallback errorCallback = nullptr);
  void RemoveFriend(const std::string& userId, ErrorCallback errorCallback = nullptr);
  void BlockFriend(const std::string& userId, ErrorCallback errorCallback = nullptr);

  // Presence API
  void UpdatePresence(const std::string& status);
  void SubscribePresence(const std::vector<std::string>& userIds, PresenceCallback callback,
                         ErrorCallback errorCallback = nullptr);

  // Party API
  void CreateParty(bool open, int maxSize, PartyCallback callback, ErrorCallback errorCallback = nullptr);
  void JoinParty(const std::string& partyId, PartyCallback callback, ErrorCallback errorCallback = nullptr);
  void LeaveParty(const std::string& partyId, ErrorCallback errorCallback = nullptr);
  void SendPartyData(const std::string& partyId, const std::vector<uint8_t>& data);

  // Matchmaking API
  void FindMatch(const std::string& query, int minPlayers, int maxPlayers, MatchCallback callback,
                 ErrorCallback errorCallback = nullptr);
  void CancelMatchmaking(const std::string& ticketId, ErrorCallback errorCallback = nullptr);

  // RPC for custom server logic
  void CallRpc(const std::string& rpcId, const std::string& payload, std::function<void(const std::string&)> callback,
               ErrorCallback errorCallback = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;

  const NakamaConfig& m_config;
  bool m_authenticated;
  std::string m_userId;
  std::string m_username;

  // Thread-safe callback queue
  std::mutex m_callbackMutex;
  std::queue<std::function<void()>> m_callbackQueue;

  void QueueCallback(std::function<void()> callback);
  void ProcessCallbacks();
};

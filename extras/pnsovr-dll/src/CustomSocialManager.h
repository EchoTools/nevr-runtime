#pragma once

#include <memory>

#include "PNSOVRSocialInterceptor.h"

namespace pnsovr::social {

// ============================================================================
// Custom Social Features Implementation
// ============================================================================

/**
 * @class CustomSocialManager
 * @brief Replacement implementation for Oculus social features
 *
 * This class provides custom implementations of social features that can
 * completely replace or augment the Oculus Platform SDK social API.
 *
 * Features:
 * - Custom room management
 * - Custom user profiles
 * - Custom presence system
 * - Custom friend lists
 * - Custom notifications
 */
class CustomSocialManager {
 public:
  struct CustomRoom {
    ovrID id;
    std::string name;
    std::string description;
    uint32_t max_users = 0;
    std::vector<ovrID> user_ids;
    ovrID owner_id = 0;
    bool is_private = false;
    bool is_joinable = true;
  };

  struct CustomUser {
    ovrID id;
    std::string username;
    std::string display_name;
    std::string avatar_url;
    std::string status;
    ovrID current_room = 0;
    bool is_online = true;
    std::vector<ovrID> friends;
  };

  struct CustomPresence {
    ovrID user_id;
    std::string status;
    std::string rich_presence_str;
    std::string lobby_session_id;
    std::string match_session_id;
    bool is_playing = false;
  };

  // Singleton access
  static CustomSocialManager& Get();

  // Initialization
  bool Initialize();
  void Shutdown();

  // Room management
  ovrID CreateRoom(const std::string& name, uint32_t max_users, bool is_private);
  bool JoinRoom(ovrID room_id, ovrID user_id);
  bool LeaveRoom(ovrID room_id, ovrID user_id);
  CustomRoom* GetRoom(ovrID room_id);
  std::vector<ovrID> GetRoomUsers(ovrID room_id);
  bool SetRoomDescription(ovrID room_id, const std::string& description);

  // User management
  bool CreateUser(ovrID user_id, const std::string& username);
  CustomUser* GetUser(ovrID user_id);
  bool SetUserStatus(ovrID user_id, const std::string& status);
  bool SetUserPresence(ovrID user_id, const CustomPresence& presence);
  CustomPresence GetUserPresence(ovrID user_id);

  // Friend management
  bool AddFriend(ovrID user_id, ovrID friend_id);
  bool RemoveFriend(ovrID user_id, ovrID friend_id);
  std::vector<ovrID> GetFriends(ovrID user_id);
  bool AreFriends(ovrID user_id_1, ovrID user_id_2);

  // Network packet processing
  void ProcessIncomingPacket(ovrID sender_id, const void* data, int size);
  std::vector<uint8_t> BuildOutgoingPacket(ovrID recipient_id, const std::string& message);

  // Notifications
  void SendNotification(ovrID recipient_id, const std::string& notification);
  std::vector<std::string> GetPendingNotifications(ovrID user_id);

  // Statistics
  uint32_t GetRoomCount() const;
  uint32_t GetUserCount() const;
  uint32_t GetActiveConnections() const;

 private:
  CustomSocialManager();
  ~CustomSocialManager();

  // Helper methods for placeholder data
  void InitializePlaceholderData();
  std::vector<ovrID> GetPlaceholderFriends(ovrID user_id);

  std::map<ovrID, CustomRoom> rooms_;
  std::map<ovrID, CustomUser> users_;
  std::map<ovrID, CustomPresence> presences_;
  std::map<ovrID, std::vector<std::string>> notifications_;

  ovrID next_room_id_ = 1000;
  ovrID next_request_id_ = 1;
};

// ============================================================================
// Integration Helper - Easy Setup
// ============================================================================

/**
 * @brief Initialize social features interception with custom implementation
 *
 * This function sets up:
 * 1. MinHook framework
 * 2. Function hooking
 * 3. Custom social manager
 * 4. Callback connections
 *
 * @return true if successful, false otherwise
 */
bool InitializeSocialFeatures();

/**
 * @brief Shutdown social features
 */
void ShutdownSocialFeatures();

/**
 * @brief Enable/disable specific interception features
 */
void EnableRoomInterception(bool enable);
void EnableUserInterception(bool enable);
void EnableNetworkInterception(bool enable);
void EnableVoipInterception(bool enable);

/**
 * @brief Register callbacks for social events
 */
void OnRoomCreated(RoomCreatedCallback callback);
void OnUserJoined(UserJoinedCallback callback);
void OnUserLeft(UserLeftCallback callback);
void OnPacketReceived(PacketReceivedCallback callback);
void OnVoipData(VoipDataCallback callback);

}  // namespace pnsovr::social

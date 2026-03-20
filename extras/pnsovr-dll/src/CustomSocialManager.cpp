#pragma warning(disable : 4711)  // Suppress function not inlined warnings (happens with LTCG)

#include "CustomSocialManager.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>

namespace pnsovr::social {

// ============================================================================
// Singleton Implementation
// ============================================================================

CustomSocialManager& CustomSocialManager::Get() {
  static CustomSocialManager instance;
  return instance;
}

CustomSocialManager::CustomSocialManager() {}

CustomSocialManager::~CustomSocialManager() {}

// ============================================================================
// Initialization
// ============================================================================

bool CustomSocialManager::Initialize() {
  std::cout << "[CustomSocial] Initializing CustomSocialManager (STANDALONE - no libOVRPlatform)" << std::endl;

  // Initialize interceptor (standalone mode, no MinHook)
  if (!SocialInterceptor::Initialize()) {
    std::cerr << "[CustomSocial::ERROR] Failed to initialize SocialInterceptor" << std::endl;
    return false;
  }

  SocialInterceptor& interceptor = SocialInterceptor::Get();

  // Install hooks (no-op in standalone mode)
  if (!interceptor.InstallHooks()) {
    std::cerr << "[CustomSocial::ERROR] Failed to install hooks" << std::endl;
    return false;
  }

  // Create placeholder data
  InitializePlaceholderData();

  std::cout << "[CustomSocial] CustomSocialManager initialized successfully with placeholder data" << std::endl;
  return true;
}

void CustomSocialManager::Shutdown() {
  std::cout << "[CustomSocial] Shutting down CustomSocialManager" << std::endl;
  SocialInterceptor::Shutdown();
  rooms_.clear();
  users_.clear();
  presences_.clear();
  notifications_.clear();
}

// ============================================================================
// Room Management
// ============================================================================

ovrID CustomSocialManager::CreateRoom(const std::string& name, uint32_t max_users, bool is_private) {
  ovrID room_id = next_room_id_++;

  CustomRoom room;
  room.id = room_id;
  room.name = name;
  room.max_users = max_users;
  room.is_private = is_private;
  room.is_joinable = true;

  rooms_[room_id] = room;

  std::cout << "[CustomSocial::Room] Created room " << std::hex << room_id << std::dec << " (name=" << name
            << ", max_users=" << max_users << ")" << std::endl;

  return room_id;
}

bool CustomSocialManager::JoinRoom(ovrID room_id, ovrID user_id) {
  auto room_it = rooms_.find(room_id);
  if (room_it == rooms_.end()) {
    std::cerr << "[CustomSocial::ERROR] Room " << std::hex << room_id << std::dec << " not found" << std::endl;
    return false;
  }

  CustomRoom& room = room_it->second;

  // Check if room is full
  if (room.user_ids.size() >= room.max_users) {
    std::cerr << "[CustomSocial::ERROR] Room is full" << std::endl;
    return false;
  }

  // Check if user is already in room
  auto user_it = std::find(room.user_ids.begin(), room.user_ids.end(), user_id);
  if (user_it != room.user_ids.end()) {
    std::cerr << "[CustomSocial::ERROR] User already in room" << std::endl;
    return false;
  }

  room.user_ids.push_back(user_id);

  auto user_it_map = users_.find(user_id);
  if (user_it_map != users_.end()) {
    user_it_map->second.current_room = room_id;
  }

  std::cout << "[CustomSocial::Room] User " << std::hex << user_id << std::dec << " joined room " << std::hex << room_id
            << std::dec << " (" << room.user_ids.size() << "/" << room.max_users << ")" << std::endl;

  return true;
}

bool CustomSocialManager::LeaveRoom(ovrID room_id, ovrID user_id) {
  auto room_it = rooms_.find(room_id);
  if (room_it == rooms_.end()) {
    return false;
  }

  CustomRoom& room = room_it->second;

  auto user_it = std::find(room.user_ids.begin(), room.user_ids.end(), user_id);
  if (user_it == room.user_ids.end()) {
    return false;
  }

  room.user_ids.erase(user_it);

  auto user_it_map = users_.find(user_id);
  if (user_it_map != users_.end()) {
    user_it_map->second.current_room = 0;
  }

  std::cout << "[CustomSocial::Room] User " << std::hex << user_id << std::dec << " left room " << std::hex << room_id
            << std::dec << std::endl;

  return true;
}

CustomSocialManager::CustomRoom* CustomSocialManager::GetRoom(ovrID room_id) {
  auto it = rooms_.find(room_id);
  if (it != rooms_.end()) {
    return &it->second;
  }
  return nullptr;
}

std::vector<ovrID> CustomSocialManager::GetRoomUsers(ovrID room_id) {
  auto it = rooms_.find(room_id);
  if (it != rooms_.end()) {
    return it->second.user_ids;
  }
  return {};
}

bool CustomSocialManager::SetRoomDescription(ovrID room_id, const std::string& description) {
  auto it = rooms_.find(room_id);
  if (it != rooms_.end()) {
    it->second.description = description;
    return true;
  }
  return false;
}

// ============================================================================
// User Management
// ============================================================================

bool CustomSocialManager::CreateUser(ovrID user_id, const std::string& username) {
  CustomUser user;
  user.id = user_id;
  user.username = username;
  user.display_name = username;
  user.is_online = true;

  users_[user_id] = user;

  std::cout << "[CustomSocial::User] Created user " << std::hex << user_id << std::dec << " (username=" << username
            << ")" << std::endl;

  return true;
}

CustomSocialManager::CustomUser* CustomSocialManager::GetUser(ovrID user_id) {
  auto it = users_.find(user_id);
  if (it != users_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool CustomSocialManager::SetUserStatus(ovrID user_id, const std::string& status) {
  auto it = users_.find(user_id);
  if (it != users_.end()) {
    it->second.status = status;

    // Update presence as well
    presences_[user_id].status = status;

    std::cout << "[CustomSocial::User] Updated status for user " << std::hex << user_id << std::dec << ": " << status
              << std::endl;
    return true;
  }
  return false;
}

bool CustomSocialManager::SetUserPresence(ovrID user_id, const CustomPresence& presence) {
  presences_[user_id] = presence;

  std::cout << "[CustomSocial::User] Updated presence for user " << std::hex << user_id << std::dec << ": "
            << presence.status << std::endl;

  return true;
}

CustomSocialManager::CustomPresence CustomSocialManager::GetUserPresence(ovrID user_id) {
  auto it = presences_.find(user_id);
  if (it != presences_.end()) {
    return it->second;
  }

  CustomPresence empty;
  empty.user_id = user_id;
  return empty;
}

// ============================================================================
// Friend Management
// ============================================================================

bool CustomSocialManager::AddFriend(ovrID user_id, ovrID friend_id) {
  auto user_it = users_.find(user_id);
  if (user_it == users_.end()) {
    return false;
  }

  CustomUser& user = user_it->second;

  // Check if already friends
  auto friend_it = std::find(user.friends.begin(), user.friends.end(), friend_id);
  if (friend_it != user.friends.end()) {
    return false;  // Already friends
  }

  user.friends.push_back(friend_id);

  std::cout << "[CustomSocial::Friends] User " << std::hex << user_id << std::dec << " added friend " << std::hex
            << friend_id << std::dec << std::endl;

  return true;
}

bool CustomSocialManager::RemoveFriend(ovrID user_id, ovrID friend_id) {
  auto user_it = users_.find(user_id);
  if (user_it == users_.end()) {
    return false;
  }

  CustomUser& user = user_it->second;
  auto friend_it = std::find(user.friends.begin(), user.friends.end(), friend_id);

  if (friend_it == user.friends.end()) {
    return false;
  }

  user.friends.erase(friend_it);
  return true;
}

std::vector<ovrID> CustomSocialManager::GetFriends(ovrID user_id) {
  auto it = users_.find(user_id);
  if (it != users_.end()) {
    // Return actual friends if user exists
    return it->second.friends;
  }

  // Return placeholder friends for unknown users
  // This provides 4 hardcoded friends as requested
  return GetPlaceholderFriends(user_id);
}

bool CustomSocialManager::AreFriends(ovrID user_id_1, ovrID user_id_2) {
  auto it = users_.find(user_id_1);
  if (it != users_.end()) {
    auto friend_it = std::find(it->second.friends.begin(), it->second.friends.end(), user_id_2);
    return friend_it != it->second.friends.end();
  }
  return false;
}

// ============================================================================
// Network Packet Processing
// ============================================================================

void CustomSocialManager::ProcessIncomingPacket(ovrID sender_id, const void* data, int size) {
  std::cout << "[CustomSocial::Network] Processing " << size << " byte packet from " << std::hex << sender_id
            << std::dec << std::endl;

  // Placeholder for custom packet processing
  // This can be extended to handle custom message formats
}

std::vector<uint8_t> CustomSocialManager::BuildOutgoingPacket(ovrID recipient_id, const std::string& message) {
  std::vector<uint8_t> packet;

  // Simple packet format: [message_length:4][message:variable]
  uint32_t msg_len = message.length();
  packet.resize(sizeof(msg_len) + msg_len);

  std::memcpy(packet.data(), &msg_len, sizeof(msg_len));
  std::memcpy(packet.data() + sizeof(msg_len), message.data(), msg_len);

  std::cout << "[CustomSocial::Network] Built " << packet.size() << " byte packet for " << std::hex << recipient_id
            << std::dec << std::endl;

  return packet;
}

// ============================================================================
// Notifications
// ============================================================================

void CustomSocialManager::SendNotification(ovrID recipient_id, const std::string& notification) {
  notifications_[recipient_id].push_back(notification);

  std::cout << "[CustomSocial::Notifications] Sent to " << std::hex << recipient_id << std::dec << ": " << notification
            << std::endl;
}

std::vector<std::string> CustomSocialManager::GetPendingNotifications(ovrID user_id) {
  auto it = notifications_.find(user_id);
  if (it != notifications_.end()) {
    std::vector<std::string> result = it->second;
    it->second.clear();
    return result;
  }
  return {};
}

// ============================================================================
// Statistics
// ============================================================================

uint32_t CustomSocialManager::GetRoomCount() const { return rooms_.size(); }

uint32_t CustomSocialManager::GetUserCount() const { return users_.size(); }

uint32_t CustomSocialManager::GetActiveConnections() const {
  uint32_t count = 0;
  for (const auto& pair : rooms_) {
    count += pair.second.user_ids.size();
  }
  return count;
}

// ============================================================================
// Integration Helper Functions
// ============================================================================

bool InitializeSocialFeatures() {
  std::cout << "[PNSOVRSocial] Initializing social features" << std::endl;

  CustomSocialManager& mgr = CustomSocialManager::Get();
  if (!mgr.Initialize()) {
    std::cerr << "[PNSOVRSocial::ERROR] Failed to initialize CustomSocialManager" << std::endl;
    return false;
  }

  std::cout << "[PNSOVRSocial] Social features initialized successfully" << std::endl;
  return true;
}

void ShutdownSocialFeatures() { CustomSocialManager::Get().Shutdown(); }

void EnableRoomInterception(bool enable) { SocialInterceptor::Get().EnableRoomInterception(enable); }

void EnableUserInterception(bool enable) { SocialInterceptor::Get().EnableUserInterception(enable); }

void EnableNetworkInterception(bool enable) { SocialInterceptor::Get().EnableNetworkInterception(enable); }

void EnableVoipInterception(bool enable) { SocialInterceptor::Get().EnableVoipInterception(enable); }

void OnRoomCreated(RoomCreatedCallback callback) { SocialInterceptor::Get().SetRoomCreatedCallback(callback); }

void OnUserJoined(UserJoinedCallback callback) { SocialInterceptor::Get().SetUserJoinedCallback(callback); }

void OnUserLeft(UserLeftCallback callback) { SocialInterceptor::Get().SetUserLeftCallback(callback); }

void OnPacketReceived(PacketReceivedCallback callback) { SocialInterceptor::Get().SetPacketReceivedCallback(callback); }

void OnVoipData(VoipDataCallback callback) { SocialInterceptor::Get().SetVoipDataCallback(callback); }

// ============================================================================
// Placeholder Data Implementation
// ============================================================================

void CustomSocialManager::InitializePlaceholderData() {
  std::cout << "[CustomSocial] Initializing placeholder data (4 hardcoded friends per user)" << std::endl;

  // Placeholder users
  for (int i = 1; i <= 4; i++) {
    ovrID user_id = 1000 + i;
    std::string username = "FriendPlayer" + std::to_string(i);
    CreateUser(user_id, username);
  }

  std::cout << "[CustomSocial] Created 4 placeholder friends" << std::endl;
}

std::vector<ovrID> CustomSocialManager::GetPlaceholderFriends(ovrID user_id) {
  // Always return 4 hardcoded friend IDs
  // These are realistic Oculus user IDs (64-bit)
  static const std::vector<ovrID> placeholder_friends = {
      0x1000000000000001ULL,  // Friend 1
      0x1000000000000002ULL,  // Friend 2
      0x1000000000000003ULL,  // Friend 3
      0x1000000000000004ULL   // Friend 4
  };

  std::cout << "[CustomSocial::Friends] GetFriends(" << std::hex << user_id << std::dec
            << ") returning 4 placeholder friends" << std::endl;

  return placeholder_friends;
}

}  // namespace pnsovr::social

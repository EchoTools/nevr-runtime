#pragma once

/**
 * @file SOCIAL_INTEGRATION_EXAMPLE.md
 * @brief Integration guide for custom social features replacement
 *
 * This document shows how to integrate and use the custom social features
 * system in your application to replace Oculus Platform SDK social APIs.
 */

#include <iostream>

#include "CustomSocialManager.h"

namespace pnsovr::social::examples {

// ============================================================================
// EXAMPLE 1: Basic Initialization
// ============================================================================

void Example_BasicInitialization() {
  std::cout << "=== Example 1: Basic Initialization ===" << std::endl;

  // Initialize social features (hooks all Oculus Platform social APIs)
  if (!InitializeSocialFeatures()) {
    std::cerr << "Failed to initialize social features!" << std::endl;
    return;
  }

  // Now all calls to Oculus Platform social APIs will be intercepted
  // and routed through our custom implementations

  std::cout << "Social features initialized successfully!" << std::endl;
}

// ============================================================================
// EXAMPLE 2: Room Management
// ============================================================================

void Example_RoomManagement() {
  std::cout << "\n=== Example 2: Room Management ===" << std::endl;

  CustomSocialManager& mgr = CustomSocialManager::Get();

  // Create a room
  ovrID room_id = mgr.CreateRoom("Main Lobby", 32, false);
  std::cout << "Created room: 0x" << std::hex << room_id << std::dec << std::endl;

  // Create some users
  ovrID user1 = mgr.CreateUser(100, "Player1") ? 100 : 0;
  ovrID user2 = mgr.CreateUser(200, "Player2") ? 200 : 0;

  // Users join the room
  mgr.JoinRoom(room_id, user1);
  mgr.JoinRoom(room_id, user2);

  // Get room info
  CustomSocialManager::CustomRoom* room = mgr.GetRoom(room_id);
  if (room) {
    std::cout << "Room: " << room->name << std::endl;
    std::cout << "Users in room: " << room->user_ids.size() << " / " << room->max_users << std::endl;
  }

  // User leaves
  mgr.LeaveRoom(room_id, user1);

  // Update room description
  mgr.SetRoomDescription(room_id, "A lobby for competitive players");
}

// ============================================================================
// EXAMPLE 3: User Management
// ============================================================================

void Example_UserManagement() {
  std::cout << "\n=== Example 3: User Management ===" << std::endl;

  CustomSocialManager& mgr = CustomSocialManager::Get();

  // Create a user
  ovrID user_id = 123;
  mgr.CreateUser(user_id, "CoolPlayer");

  // Get user info
  CustomSocialManager::CustomUser* user = mgr.GetUser(user_id);
  if (user) {
    std::cout << "User: " << user->username << " (ID: 0x" << std::hex << user->id << std::dec << ")" << std::endl;
  }

  // Set user status
  mgr.SetUserStatus(user_id, "In a match");

  // Set presence
  CustomSocialManager::CustomPresence presence;
  presence.user_id = user_id;
  presence.status = "Playing Deathmatch";
  presence.is_playing = true;
  mgr.SetUserPresence(user_id, presence);

  // Get presence
  auto current_presence = mgr.GetUserPresence(user_id);
  std::cout << "User presence: " << current_presence.status << std::endl;
}

// ============================================================================
// EXAMPLE 4: Friend Management
// ============================================================================

void Example_FriendManagement() {
  std::cout << "\n=== Example 4: Friend Management ===" << std::endl;

  CustomSocialManager& mgr = CustomSocialManager::Get();

  // Create some users
  ovrID user1 = 100;
  ovrID user2 = 200;

  mgr.CreateUser(user1, "Alice");
  mgr.CreateUser(user2, "Bob");

  // Add friend relationship
  mgr.AddFriend(user1, user2);
  mgr.AddFriend(user2, user1);  // Make it mutual

  // Check if friends
  bool are_friends = mgr.AreFriends(user1, user2);
  std::cout << "Are Alice and Bob friends? " << (are_friends ? "Yes" : "No") << std::endl;

  // Get friends list
  auto friends = mgr.GetFriends(user1);
  std::cout << "Alice's friends: " << friends.size() << std::endl;

  // Remove friendship
  mgr.RemoveFriend(user1, user2);
}

// ============================================================================
// EXAMPLE 5: Packet Interception with Callbacks
// ============================================================================

void Example_PacketInterception() {
  std::cout << "\n=== Example 5: Packet Interception ===" << std::endl;

  // Register a callback to see all incoming packets
  OnPacketReceived([](ovrID sender, const void* data, int size) {
    std::cout << "Received " << size << " bytes from 0x" << std::hex << sender << std::dec << std::endl;

    // Parse custom packet format
    if (size >= 4) {
      uint32_t msg_len = *static_cast<const uint32_t*>(data);
      if (msg_len <= size - 4) {
        std::string message(static_cast<const char*>(data) + 4, msg_len);
        std::cout << "Message: " << message << std::endl;
      }
    }
  });

  // Now whenever packets are received through the original
  // Oculus Platform API, our callback will be invoked

  std::cout << "Packet callback registered" << std::endl;
}

// ============================================================================
// EXAMPLE 6: Notifications
// ============================================================================

void Example_Notifications() {
  std::cout << "\n=== Example 6: Notifications ===" << std::endl;

  CustomSocialManager& mgr = CustomSocialManager::Get();

  ovrID user_id = 100;
  mgr.CreateUser(user_id, "TestUser");

  // Send a notification
  mgr.SendNotification(user_id, "A friend logged in");
  mgr.SendNotification(user_id, "Match found!");

  // Get pending notifications
  auto notifications = mgr.GetPendingNotifications(user_id);
  std::cout << "Notifications: " << notifications.size() << std::endl;
  for (const auto& notif : notifications) {
    std::cout << "  - " << notif << std::endl;
  }
}

// ============================================================================
// EXAMPLE 7: Feature Toggle
// ============================================================================

void Example_FeatureToggle() {
  std::cout << "\n=== Example 7: Feature Toggle ===" << std::endl;

  // Selectively enable/disable interception by feature

  // Intercept rooms but not users
  EnableRoomInterception(true);
  EnableUserInterception(false);
  EnableNetworkInterception(true);
  EnableVoipInterception(false);

  std::cout << "Configured partial interception:" << std::endl;
  std::cout << "  Room interception: ON" << std::endl;
  std::cout << "  User interception: OFF" << std::endl;
  std::cout << "  Network interception: ON" << std::endl;
  std::cout << "  VOIP interception: OFF" << std::endl;

  // This allows fine-grained control over which APIs are hooked
}

// ============================================================================
// EXAMPLE 8: Statistics and Monitoring
// ============================================================================

void Example_StatisticsAndMonitoring() {
  std::cout << "\n=== Example 8: Statistics and Monitoring ===" << std::endl;

  CustomSocialManager& mgr = CustomSocialManager::Get();

  // Create some data to track
  mgr.CreateRoom("Arena", 12, false);
  mgr.CreateRoom("Lobby", 32, true);
  mgr.CreateUser(100, "Player1");
  mgr.CreateUser(200, "Player2");

  // Monitor stats
  std::cout << "Room count: " << mgr.GetRoomCount() << std::endl;
  std::cout << "User count: " << mgr.GetUserCount() << std::endl;
  std::cout << "Active connections: " << mgr.GetActiveConnections() << std::endl;
}

// ============================================================================
// EXAMPLE 9: Complete Application Flow
// ============================================================================

void Example_CompleteFlow() {
  std::cout << "\n=== Example 9: Complete Application Flow ===" << std::endl;

  // 1. Initialize
  if (!InitializeSocialFeatures()) {
    std::cerr << "Initialization failed!" << std::endl;
    return;
  }

  CustomSocialManager& mgr = CustomSocialManager::Get();

  // 2. Create lobby room
  ovrID lobby_id = mgr.CreateRoom("Main Lobby", 64, false);
  std::cout << "Lobby created: 0x" << std::hex << lobby_id << std::dec << std::endl;

  // 3. Register packet callback
  OnPacketReceived([&mgr](ovrID sender, const void* data, int size) { mgr.ProcessIncomingPacket(sender, data, size); });

  // 4. Create players
  for (int i = 0; i < 4; i++) {
    ovrID user_id = 1000 + i;
    std::string username = "Player" + std::to_string(i + 1);
    mgr.CreateUser(user_id, username);

    // Players join lobby
    mgr.JoinRoom(lobby_id, user_id);

    // Set presence
    CustomSocialManager::CustomPresence presence;
    presence.user_id = user_id;
    presence.status = "In Lobby";
    presence.is_playing = true;
    mgr.SetUserPresence(user_id, presence);
  }

  // 5. Create match room
  ovrID match_id = mgr.CreateRoom("Deathmatch", 8, true);
  std::cout << "Match room created: 0x" << std::hex << match_id << std::dec << std::endl;

  // 6. Move some players to match
  mgr.LeaveRoom(lobby_id, 1000);
  mgr.JoinRoom(match_id, 1000);

  mgr.LeaveRoom(lobby_id, 1001);
  mgr.JoinRoom(match_id, 1001);

  // 7. Report stats
  std::cout << "\nFinal stats:" << std::endl;
  std::cout << "  Rooms: " << mgr.GetRoomCount() << std::endl;
  std::cout << "  Users: " << mgr.GetUserCount() << std::endl;
  std::cout << "  Active: " << mgr.GetActiveConnections() << std::endl;

  // 8. Shutdown
  ShutdownSocialFeatures();
  std::cout << "\nApplication shutdown complete" << std::endl;
}

// ============================================================================
// Main Entry Point (for testing)
// ============================================================================

inline void RunAllExamples() {
  Example_BasicInitialization();
  Example_RoomManagement();
  Example_UserManagement();
  Example_FriendManagement();
  Example_PacketInterception();
  Example_Notifications();
  Example_FeatureToggle();
  Example_StatisticsAndMonitoring();
  Example_CompleteFlow();
}

}  // namespace pnsovr::social::examples

/**
 * Usage:
 *
 * In your main initialization code:
 *   pnsovr::social::InitializeSocialFeatures();
 *
 * After that, any calls to Oculus Platform social APIs will be intercepted
 * and routed through your custom implementations.
 *
 * At shutdown:
 *   pnsovr::social::ShutdownSocialFeatures();
 */

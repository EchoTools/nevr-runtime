/**
 * @file pnsovr_users.h
 * @brief NEVR PNSOvr Compatibility - User Management
 *
 * Implements user identification, authentication, and account operations.
 *
 * Binary Reference: pnsovr.dll v34.4 (Echo VR)
 * - ovr_User_GetID: 0x1801cb000
 * - ovr_User_GetOculusID: 0x1801cb100
 * - ovr_User_GetPresence: 0x1801cb200
 * - ovr_User_GetInviteToken: 0x1801cb300
 * - ovr_UserArray_*: 0x1801cb400
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

/**
 * @brief User account information.
 *
 * Binary reference: User structure at 0x1801cb000
 * Size: 256 bytes
 *
 * Field layout:
 * +0x00: id (uint64_t)
 * +0x08: oculus_id (string, max 64 chars)
 * +0x48: display_name (string, max 64 chars)
 * +0x88: image_url (string, max 256 chars)
 * +0x188: verified (uint8_t)
 * +0x189: privacy_level (uint8_t) - 0=Public, 1=Friends, 2=Private
 */
#pragma warning(push)
#pragma warning(disable : 4820)  // Suppress struct padding warnings
struct UserInfo {
  uint64_t id;
  std::string oculus_id;
  std::string display_name;
  std::string image_url;
  bool verified;
  uint8_t privacy_level;  // 0=Public, 1=Friends, 2=Private
};
#pragma warning(pop)

/**
 * @brief User presence and activity status.
 *
 * Binary reference: Presence data at 0x1801cb200
 * Size: 128 bytes
 *
 * Field layout:
 * +0x00: user_id (uint64_t)
 * +0x08: activity (uint32_t) - 0=Offline, 1=Online, 2=Away, 3=Busy, 4=InGame
 * +0x0c: updated_at (int64_t)
 * +0x14: current_room_id (uint64_t)
 * +0x1c: custom_data (string, max 256 chars)
 */
#pragma warning(push)
#pragma warning(disable : 4820)  // Suppress struct padding warnings
struct UserPresence {
  uint64_t user_id;
  uint32_t activity;  // 0=Offline, 1=Online, 2=Away, 3=Busy, 4=InGame
  int64_t updated_at;
  uint64_t current_room_id;
  std::string custom_data;
};
#pragma warning(pop)

/**
 * @brief Invite token for cross-game invitations.
 *
 * Binary reference: Token structure at 0x1801cb300
 * Size: 512 bytes
 *
 * Field layout:
 * +0x00: token (string, max 256 chars)
 * +0x100: source_user_id (uint64_t)
 * +0x108: target_app_id (uint64_t)
 * +0x110: expiry_time (int64_t)
 * +0x118: custom_data (string, max 256 chars)
 */
#pragma pack(push, 1)
struct InviteToken {
  std::string token;
  uint64_t source_user_id;
  uint64_t target_app_id;
  int64_t expiry_time;
  std::string custom_data;
};
#pragma pack(pop)

/**
 * @brief User management subsystem.
 *
 * Manages user accounts, authentication, presence broadcasting, and invites.
 *
 * Binary reference: Implementation at 0x1801cb000-0x1801cc000
 */
class UserSubsystem {
 public:
  UserSubsystem();
  ~UserSubsystem();

  /**
   * @brief Initialize user subsystem.
   * @return true if initialization succeeded.
   *
   * Binary reference: Initialization at 0x1801cb000
   */
  bool Initialize();

  /**
   * @brief Shutdown user subsystem.
   *
   * Binary reference: Cleanup at 0x1801cbf00
   */
  void Shutdown();

  /**
   * @brief Get current authenticated user.
   * @return User info, or empty if not authenticated.
   *
   * Binary reference: Current user at 0x1801cb000 (ovr_User_GetID)
   * Returns:
   * - User ID (uint64_t)
   * - Display name
   * - Oculus ID (if available)
   * - Verification status
   */
  UserInfo GetCurrentUser() const;

  /**
   * @brief Set current authenticated user (login).
   * @param user User info to authenticate.
   * @return true if authentication succeeded.
   *
   * Binary reference: Auth at 0x1801cbf00
   * Process:
   * 1. Validate user credentials
   * 2. Store as current user
   * 3. Notify observers
   */
  bool SetCurrentUser(const UserInfo& user);

  /**
   * @brief Get user by ID.
   * @param user_id User ID to look up.
   * @return User info, or empty if not found.
   *
   * Binary reference: Lookup at 0x1801cb000 (ovr_User_GetID)
   */
  UserInfo GetUser(uint64_t user_id);

  /**
   * @brief Get user by Oculus ID.
   * @param oculus_id Oculus account ID.
   * @return User info, or empty if not found.
   *
   * Binary reference: ID lookup at 0x1801cb100 (ovr_User_GetOculusID)
   */
  UserInfo GetUserByOculusID(const std::string& oculus_id);

  /**
   * @brief Get presence status for user.
   * @param user_id User to query.
   * @return Presence info, or default if not found.
   *
   * Binary reference: Presence query at 0x1801cb200 (ovr_User_GetPresence)
   * Returns:
   * - Current activity status
   * - Last update timestamp
   * - Current room if in-game
   * - Custom activity data
   */
  UserPresence GetUserPresence(uint64_t user_id);

  /**
   * @brief Update presence for current user.
   * @param presence New presence status.
   * @return true if update succeeded.
   *
   * Binary reference: Update at 0x1801cb200
   * Broadcasts to connected friends.
   */
  bool UpdatePresence(const UserPresence& presence);

  /**
   * @brief Generate invite token for user.
   * @param target_user_id User to invite.
   * @param target_app_id Target application ID.
   * @param custom_data Optional invite message.
   * @return Invite token, or empty if generation failed.
   *
   * Binary reference: Token generation at 0x1801cb300 (ovr_User_GetInviteToken)
   * Token properties:
   * - Valid for 24 hours
   * - Single-use (consumed on accept)
   * - Scope-limited (target app only)
   * - Includes source user ID for reply
   */
  InviteToken GenerateInviteToken(uint64_t target_user_id, uint64_t target_app_id, const std::string& custom_data = "");

  /**
   * @brief Validate and decode invite token.
   * @param token Invite token to validate.
   * @return Decoded token, or invalid token if validation failed.
   *
   * Binary reference: Validation at 0x1801cb300
   * Checks:
   * - Token format validity
   * - Expiry time (must not be expired)
   * - Source user exists
   * - Not already consumed
   */
  InviteToken ValidateInviteToken(const std::string& token);

  /**
   * @brief Register a user with the system.
   * @param user User to register.
   * @return true if registration succeeded.
   *
   * Binary reference: Registration at 0x1801cbf00
   * Process:
   * 1. Assign unique user ID
   * 2. Store user info
   * 3. Initialize default presence (Offline)
   */
  bool RegisterUser(UserInfo& user);

  /**
   * @brief Get list of all users.
   * @return Vector of user IDs.
   *
   * Binary reference: Enumeration at 0x1801cb400 (ovr_UserArray_*)
   * Supports pagination.
   */
  std::vector<uint64_t> GetAllUsers();

 private:
  struct Impl;
  Impl* impl_;
};

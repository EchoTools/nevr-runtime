/**
 * @file pnsovr_notifications.h
 * @brief NEVR PNSOvr Compatibility - Notifications & Invitations
 *
 * Implements room invitations and social notifications.
 *
 * Binary Reference: pnsovr.dll v34.4 (Echo VR)
 * - ovr_Notification_GetRoomInvites: 0x1801d2000
 * - ovr_Notification_MarkAsRead: 0x1801d2100
 * - ovr_RoomInviteNotification_*: 0x1801d2200
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief Room invitation notification.
 *
 * Binary reference: Structure at 0x1801d2200
 * Size: 256 bytes
 *
 * Field layout:
 * +0x00: id (uint64_t)
 * +0x08: from_user_id (uint64_t)
 * +0x10: from_user_name (string, max 64 chars)
 * +0x50: room_id (uint64_t)
 * +0x58: room_name (string, max 64 chars)
 * +0x98: message (string, max 256 chars)
 * +0x198: sent_at (int64_t)
 * +0x1a0: expires_at (int64_t)
 * +0x1a8: read (uint8_t)
 */
#pragma pack(push, 1)
struct RoomInviteNotification {
  uint64_t id;                 // Notification ID
  uint64_t from_user_id;       // User who sent invite
  std::string from_user_name;  // Display name of sender
  uint64_t room_id;            // Room being invited to
  std::string room_name;       // Room display name
  std::string message;         // Custom invite message
  int64_t sent_at;             // When invitation was sent
  int64_t expires_at;          // When invitation expires
  bool read;                   // Whether notification has been read
};
#pragma pack(pop)

/**
 * @brief Notification management subsystem.
 *
 * Handles social notifications:
 * - Room invitations
 * - Friend requests
 * - Party updates
 *
 * Binary reference: Implementation at 0x1801d2000-0x1801d3000
 */
class NotificationSubsystem {
 public:
  NotificationSubsystem();
  ~NotificationSubsystem();

  /**
   * @brief Initialize notification subsystem.
   * @return true if initialization succeeded.
   *
   * Binary reference: Initialization at 0x1801d2000
   */
  bool Initialize();

  /**
   * @brief Shutdown notification subsystem.
   *
   * Binary reference: Cleanup at 0x1801d2f00
   */
  void Shutdown();

  /**
   * @brief Get room invite notifications.
   * @param limit Maximum notifications to return.
   * @return Vector of room invite notifications.
   *
   * Binary reference: Retrieval at 0x1801d2000 (ovr_Notification_GetRoomInvites)
   * Returns:
   * - Pending (unaccepted) room invitations
   * - Sorted by sent time (newest first)
   * - Includes both read and unread
   *
   * Invitations expire after 24 hours.
   */
  std::vector<RoomInviteNotification> GetRoomInvites(uint32_t limit = 50);

  /**
   * @brief Mark notification as read.
   * @param notification_id Notification to mark as read.
   * @return true if marking succeeded.
   *
   * Binary reference: Update at 0x1801d2100 (ovr_Notification_MarkAsRead)
   * Purpose: Track which notifications user has seen.
   */
  bool MarkInviteAsRead(uint64_t notification_id);

  /**
   * @brief Accept room invitation.
   * @param notification_id Notification to accept.
   * @return Room ID if acceptance succeeded, 0 otherwise.
   *
   * Binary reference: Acceptance at 0x1801d2200
   * Process:
   * 1. Validate notification exists
   * 2. Check if still valid (not expired)
   * 3. Check room still exists
   * 4. Add user to room
   * 5. Remove notification
   * 6. Return room ID
   */
  uint64_t AcceptInvite(uint64_t notification_id);

  /**
   * @brief Decline room invitation.
   * @param notification_id Notification to decline.
   * @return true if declining succeeded.
   *
   * Binary reference: Declining at 0x1801d2300
   * Process:
   * 1. Validate notification exists
   * 2. Mark as declined (or remove)
   * 3. Optionally notify sender
   */
  bool DeclineInvite(uint64_t notification_id);

  /**
   * @brief Send room invitation to users.
   * @param user_ids Target users to invite.
   * @param room_id Room being invited to.
   * @param message Custom message (optional).
   * @return true if invitations were sent.
   *
   * Binary reference: Sending at 0x1801d2400
   * Process:
   * 1. Create notification for each user
   * 2. Set expiry time (24 hours)
   * 3. Queue for delivery
   * 4. Return success
   *
   * In real implementation, would broadcast to:
   * - Target users' connected sessions
   * - Persistent notification store (in case offline)
   */
  bool SendRoomInvites(const std::vector<uint64_t>& user_ids, uint64_t room_id, const std::string& message = "");

  /**
   * @brief Get unread notification count.
   * @return Number of unread notifications.
   */
  uint32_t GetUnreadCount();

  /**
   * @brief Mark all notifications as read.
   * @return true if operation succeeded.
   */
  bool MarkAllAsRead();

 private:
  struct Impl;
  Impl* impl_;
};

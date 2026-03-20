/**
 * @file pnsovr_notifications.cpp
 * @brief NEVR PNSOvr Compatibility - Notifications Implementation
 *
 * Binary reference: pnsovr.dll v34.4 (Echo VR)
 */

#include "pnsovr_notifications.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>

/**
 * @brief Internal implementation for NotificationSubsystem.
 *
 * Reference: Notification storage at 0x1801d2000+
 */
#pragma warning(push)
#pragma warning(disable : 4625 5026 4626 5027)  // Suppress copy constructor warnings
struct NotificationSubsystem::Impl {
  // Room invitation notifications
  // Reference: Notification storage at 0x1801d2200
  std::map<uint64_t, RoomInviteNotification> notifications;  // Indexed by ID
  uint64_t next_notification_id;

  // Thread safety
  mutable std::mutex notifications_mutex;

  Impl() : next_notification_id(1) {}
};
#pragma warning(pop)

NotificationSubsystem::NotificationSubsystem() : impl_(new Impl()) {}

NotificationSubsystem::~NotificationSubsystem() {
  if (impl_) {
    Shutdown();
    delete impl_;
    impl_ = nullptr;
  }
}

bool NotificationSubsystem::Initialize() {
  // Reference: Initialization at 0x1801d2000
  return true;
}

void NotificationSubsystem::Shutdown() {
  // Reference: Cleanup at 0x1801d2f00
  std::lock_guard<std::mutex> lock(impl_->notifications_mutex);
  impl_->notifications.clear();
}

std::vector<RoomInviteNotification> NotificationSubsystem::GetRoomInvites(uint32_t limit) {
  // Reference: Retrieval at 0x1801d2000 (ovr_Notification_GetRoomInvites)
  std::lock_guard<std::mutex> lock(impl_->notifications_mutex);

  // Collect non-expired invitations
  std::vector<RoomInviteNotification> result;
  int64_t now =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  for (const auto& pair : impl_->notifications) {
    if (pair.second.expires_at > now) {
      result.push_back(pair.second);
    }
  }

  // Sort by sent time (newest first)
  std::sort(result.begin(), result.end(),
            [](const RoomInviteNotification& a, const RoomInviteNotification& b) { return a.sent_at > b.sent_at; });

  // Apply limit
  if (result.size() > limit) {
    result.resize(limit);
  }

  return result;
}

bool NotificationSubsystem::MarkInviteAsRead(uint64_t notification_id) {
  // Reference: Update at 0x1801d2100 (ovr_Notification_MarkAsRead)
  std::lock_guard<std::mutex> lock(impl_->notifications_mutex);

  auto it = impl_->notifications.find(notification_id);
  if (it == impl_->notifications.end()) {
    return false;
  }

  it->second.read = true;
  return true;
}

uint64_t NotificationSubsystem::AcceptInvite(uint64_t notification_id) {
  // Reference: Acceptance at 0x1801d2200
  std::lock_guard<std::mutex> lock(impl_->notifications_mutex);

  auto it = impl_->notifications.find(notification_id);
  if (it == impl_->notifications.end()) {
    return 0;  // Notification not found
  }

  // Check if still valid (not expired)
  int64_t now =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  if (it->second.expires_at < now) {
    impl_->notifications.erase(it);
    return 0;  // Invitation expired
  }

  uint64_t room_id = it->second.room_id;

  // Remove notification (consumed)
  impl_->notifications.erase(it);

  // In real implementation:
  // 1. Validate room still exists
  // 2. Add user to room
  // 3. Notify room members

  return room_id;
}

bool NotificationSubsystem::DeclineInvite(uint64_t notification_id) {
  // Reference: Declining at 0x1801d2300
  std::lock_guard<std::mutex> lock(impl_->notifications_mutex);

  auto it = impl_->notifications.find(notification_id);
  if (it == impl_->notifications.end()) {
    return false;
  }

  impl_->notifications.erase(it);

  // In real implementation, could notify sender of decline
  return true;
}

bool NotificationSubsystem::SendRoomInvites(const std::vector<uint64_t>& user_ids, uint64_t room_id,
                                            const std::string& message) {
  // Reference: Sending at 0x1801d2400
  std::lock_guard<std::mutex> lock(impl_->notifications_mutex);

  int64_t now =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  int64_t expiry = now + (24 * 60 * 60);  // 24 hour expiry

  // Create notification for each user
  for (uint64_t user_id : user_ids) {
    (void)user_id;  // Suppress unused variable warning
    RoomInviteNotification notification;
    notification.id = impl_->next_notification_id++;
    notification.from_user_id = 0;     // Set by caller
    notification.from_user_name = "";  // Set by caller
    notification.room_id = room_id;
    notification.room_name = "";  // Set by caller
    notification.message = message;
    notification.sent_at = now;
    notification.expires_at = expiry;
    notification.read = false;

    impl_->notifications[notification.id] = notification;

    // In real implementation:
    // 1. Send to user's connected sessions (if online)
    // 2. Store in persistent queue (for offline delivery)
    // 3. Broadcast via notification server
  }

  return true;
}

uint32_t NotificationSubsystem::GetUnreadCount() {
  std::lock_guard<std::mutex> lock(impl_->notifications_mutex);

  uint32_t count = 0;
  for (const auto& pair : impl_->notifications) {
    if (!pair.second.read) {
      count++;
    }
  }

  return count;
}

bool NotificationSubsystem::MarkAllAsRead() {
  std::lock_guard<std::mutex> lock(impl_->notifications_mutex);

  for (auto& pair : impl_->notifications) {
    pair.second.read = true;
  }

  return true;
}

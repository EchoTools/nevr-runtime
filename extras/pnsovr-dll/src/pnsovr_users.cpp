/**
 * @file pnsovr_users.cpp
 * @brief NEVR PNSOvr Compatibility - User Management Implementation
 *
 * Binary reference: pnsovr.dll v34.4 (Echo VR)
 */

#include "pnsovr_users.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <random>
#include <sstream>

/**
 * @brief Internal implementation for UserSubsystem.
 *
 * Reference: User storage at 0x1801cb000+
 */
#pragma warning(push)
#pragma warning(disable : 4625 5026 4626 5027)  // Suppress copy constructor warnings
struct UserSubsystem::Impl {
  // User registry
  // Reference: User storage at 0x1801cb000
  std::map<uint64_t, UserInfo> users;
  std::map<std::string, uint64_t> oculus_id_to_user_id;  // Quick lookup by Oculus ID

  // User presence
  // Reference: Presence data at 0x1801cb200
  std::map<uint64_t, UserPresence> presence;

  // Invite tokens
  // Reference: Token storage at 0x1801cb300
  std::map<std::string, InviteToken> tokens;

  // Current authenticated user
  uint64_t current_user_id;

  // Thread safety
  mutable std::mutex users_mutex;
  mutable std::mutex presence_mutex;
  mutable std::mutex tokens_mutex;

  // ID generation
  uint64_t next_user_id;

  Impl() : current_user_id(0), next_user_id(1) {}
};
#pragma warning(pop)

UserSubsystem::UserSubsystem() : impl_(new Impl()) {}

UserSubsystem::~UserSubsystem() {
  if (impl_) {
    Shutdown();
    delete impl_;
    impl_ = nullptr;
  }
}

bool UserSubsystem::Initialize() {
  // Reference: Initialization at 0x1801cb000
  return true;
}

void UserSubsystem::Shutdown() {
  // Reference: Cleanup at 0x1801cbf00
  {
    std::lock_guard<std::mutex> lock(impl_->users_mutex);
    impl_->users.clear();
    impl_->oculus_id_to_user_id.clear();
  }

  {
    std::lock_guard<std::mutex> lock(impl_->presence_mutex);
    impl_->presence.clear();
  }

  {
    std::lock_guard<std::mutex> lock(impl_->tokens_mutex);
    impl_->tokens.clear();
  }

  impl_->current_user_id = 0;
}

UserInfo UserSubsystem::GetCurrentUser() const {
  // Reference: Current user at 0x1801cb000 (ovr_User_GetID)
  std::lock_guard<std::mutex> lock(impl_->users_mutex);

  if (impl_->current_user_id == 0) {
    return UserInfo();
  }

  auto it = impl_->users.find(impl_->current_user_id);
  if (it == impl_->users.end()) {
    return UserInfo();
  }

  return it->second;
}

bool UserSubsystem::SetCurrentUser(const UserInfo& user) {
  // Reference: Auth at 0x1801cbf00
  std::lock_guard<std::mutex> lock(impl_->users_mutex);

  // Validate user exists
  auto it = impl_->users.find(user.id);
  if (it == impl_->users.end()) {
    return false;
  }

  impl_->current_user_id = user.id;
  return true;
}

UserInfo UserSubsystem::GetUser(uint64_t user_id) {
  // Reference: Lookup at 0x1801cb000 (ovr_User_GetID)
  std::lock_guard<std::mutex> lock(impl_->users_mutex);

  auto it = impl_->users.find(user_id);
  if (it == impl_->users.end()) {
    return UserInfo();
  }

  return it->second;
}

UserInfo UserSubsystem::GetUserByOculusID(const std::string& oculus_id) {
  // Reference: ID lookup at 0x1801cb100 (ovr_User_GetOculusID)
  std::lock_guard<std::mutex> lock(impl_->users_mutex);

  auto id_it = impl_->oculus_id_to_user_id.find(oculus_id);
  if (id_it == impl_->oculus_id_to_user_id.end()) {
    return UserInfo();
  }

  auto it = impl_->users.find(id_it->second);
  if (it == impl_->users.end()) {
    return UserInfo();
  }

  return it->second;
}

UserPresence UserSubsystem::GetUserPresence(uint64_t user_id) {
  // Reference: Presence query at 0x1801cb200 (ovr_User_GetPresence)
  std::lock_guard<std::mutex> lock(impl_->presence_mutex);

  auto it = impl_->presence.find(user_id);
  if (it == impl_->presence.end()) {
    // Return default presence (Offline)
    UserPresence p;
    p.user_id = user_id;
    p.activity = 0;  // Offline
    p.updated_at = 0;
    p.current_room_id = 0;
    return p;
  }

  return it->second;
}

bool UserSubsystem::UpdatePresence(const UserPresence& presence) {
  // Reference: Update at 0x1801cb200
  std::lock_guard<std::mutex> lock(impl_->presence_mutex);

  UserPresence p = presence;
  p.updated_at =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  impl_->presence[presence.user_id] = p;
  return true;
}

InviteToken UserSubsystem::GenerateInviteToken(uint64_t target_user_id, uint64_t target_app_id,
                                               const std::string& custom_data) {
  // Reference: Token generation at 0x1801cb300 (ovr_User_GetInviteToken)
  (void)target_user_id;  // Suppress unused parameter warning
  std::lock_guard<std::mutex> lock(impl_->tokens_mutex);

  // Generate token string (format: hex of random 128-bit value)
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dis;

  std::stringstream ss;
  ss << std::hex << dis(gen) << dis(gen);
  std::string token = ss.str();

  InviteToken t;
  t.token = token;
  t.source_user_id = impl_->current_user_id;
  t.target_app_id = target_app_id;
  t.expiry_time =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() +
      (24 * 60 * 60);  // 24 hours
  t.custom_data = custom_data;

  impl_->tokens[token] = t;
  return t;
}

InviteToken UserSubsystem::ValidateInviteToken(const std::string& token) {
  // Reference: Validation at 0x1801cb300
  std::lock_guard<std::mutex> lock(impl_->tokens_mutex);

  auto it = impl_->tokens.find(token);
  if (it == impl_->tokens.end()) {
    return InviteToken();  // Invalid token
  }

  InviteToken t = it->second;

  // Check expiry
  int64_t now =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  if (t.expiry_time < now) {
    impl_->tokens.erase(it);  // Expired token
    return InviteToken();
  }

  return t;
}

bool UserSubsystem::RegisterUser(UserInfo& user) {
  // Reference: Registration at 0x1801cbf00
  std::lock_guard<std::mutex> lock(impl_->users_mutex);

  // Assign user ID if not already assigned
  if (user.id == 0) {
    user.id = impl_->next_user_id++;
  }

  // Check for duplicate Oculus ID
  if (!user.oculus_id.empty()) {
    auto it = impl_->oculus_id_to_user_id.find(user.oculus_id);
    if (it != impl_->oculus_id_to_user_id.end()) {
      return false;  // Duplicate Oculus ID
    }
  }

  impl_->users[user.id] = user;

  // Index by Oculus ID for quick lookup
  if (!user.oculus_id.empty()) {
    impl_->oculus_id_to_user_id[user.oculus_id] = user.id;
  }

  // Initialize default presence (Offline)
  {
    std::lock_guard<std::mutex> presence_lock(impl_->presence_mutex);
    UserPresence p;
    p.user_id = user.id;
    p.activity = 0;  // Offline
    p.updated_at =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    impl_->presence[user.id] = p;
  }

  return true;
}

std::vector<uint64_t> UserSubsystem::GetAllUsers() {
  // Reference: Enumeration at 0x1801cb400 (ovr_UserArray_*)
  std::lock_guard<std::mutex> lock(impl_->users_mutex);

  std::vector<uint64_t> result;
  for (const auto& pair : impl_->users) {
    result.push_back(pair.first);
  }

  return result;
}

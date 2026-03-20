/**
 * @file pnsovr_rooms.cpp
 * @brief NEVR PNSOvr Compatibility - Room Management Implementation
 *
 * Binary reference: pnsovr.dll v34.4 (Echo VR)
 */

#include "pnsovr_rooms.h"

#include <algorithm>
#include <chrono>
#include <mutex>

/**
 * @brief Internal implementation for RoomSubsystem.
 *
 * Reference: Room storage at 0x1801ca200+
 */
#pragma warning(push)
#pragma warning(disable : 4625 5026 4626 5027)  // Suppress copy constructor warnings
struct RoomSubsystem::Impl {
  // Active rooms storage
  // Reference: Room registry at 0x1801ca200
  std::map<uint64_t, RoomInfo> rooms;
  uint64_t next_room_id;

  // Thread safety
  mutable std::mutex rooms_mutex;

  Impl() : next_room_id(1) {}
};
#pragma warning(pop)

RoomSubsystem::RoomSubsystem() : impl_(new Impl()) {}

RoomSubsystem::~RoomSubsystem() {
  if (impl_) {
    Shutdown();
    delete impl_;
    impl_ = nullptr;
  }
}

bool RoomSubsystem::Initialize() {
  // Reference: Initialization at 0x1801ca200
  return true;
}

void RoomSubsystem::Shutdown() {
  // Reference: Cleanup at 0x1801ca900
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);
  impl_->rooms.clear();
}

uint64_t RoomSubsystem::CreateRoom(const RoomOptions& options) {
  // Reference: Creation logic at 0x1801ca200 (ovr_Room_CreateAndJoinPrivate2)
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);

  uint64_t room_id = impl_->next_room_id++;

  RoomInfo room;
  room.id = room_id;
  room.owner_id = 0;  // Set by caller
  room.name = options.name;
  room.max_capacity = options.max_capacity;
  room.current_capacity = 1;  // Owner as first member
  room.join_policy = options.join_policy;
  room.created_at =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  room.expires_at = room.created_at + (24 * 60 * 60);  // 24 hour expiry

  impl_->rooms[room_id] = room;
  return room_id;
}

bool RoomSubsystem::JoinRoom(uint64_t room_id, uint64_t user_id) {
  // Reference: Join logic at 0x1801ca300+ (ovr_Room_Get)
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);

  auto it = impl_->rooms.find(room_id);
  if (it == impl_->rooms.end()) {
    return false;
  }

  RoomInfo& room = it->second;

  // Check capacity
  if (room.current_capacity >= room.max_capacity) {
    return false;
  }

  // Check join policy
  // Reference: Policy validation at 0x1801ca700
  // For now, allow all joins (real implementation would check policy)

  // Check if user already in room
  for (const auto& u : room.users) {
    if (u.user_id == user_id) {
      return false;  // Already in room
    }
  }

  // Add user to room
  RoomUser room_user;
  room_user.user_id = user_id;
  room_user.is_online = true;
  room_user.updated_at =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();

  room.users.push_back(room_user);
  room.current_capacity++;

  return true;
}

bool RoomSubsystem::LeaveRoom(uint64_t room_id, uint64_t user_id) {
  // Reference: Leave logic at 0x1801ca400+ (ovr_Room_GetID)
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);

  auto it = impl_->rooms.find(room_id);
  if (it == impl_->rooms.end()) {
    return false;
  }

  RoomInfo& room = it->second;

  // Find and remove user
  auto user_it =
      std::find_if(room.users.begin(), room.users.end(), [user_id](const RoomUser& u) { return u.user_id == user_id; });

  if (user_it == room.users.end()) {
    return false;
  }

  room.users.erase(user_it);
  room.current_capacity--;

  // If owner leaves and room is empty, destroy room
  if (room.owner_id == user_id && room.current_capacity == 0) {
    impl_->rooms.erase(it);
  }

  return true;
}

RoomInfo RoomSubsystem::GetRoom(uint64_t room_id) {
  // Reference: Lookup at 0x1801ca300 (ovr_Room_Get)
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);

  auto it = impl_->rooms.find(room_id);
  if (it == impl_->rooms.end()) {
    return RoomInfo();
  }

  return it->second;
}

std::vector<RoomUser> RoomSubsystem::GetRoomUsers(uint64_t room_id) {
  // Reference: User enumeration at 0x1801ca500 (ovr_Room_GetUsers)
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);

  auto it = impl_->rooms.find(room_id);
  if (it == impl_->rooms.end()) {
    return {};
  }

  return it->second.users;
}

std::string RoomSubsystem::GetRoomData(uint64_t room_id, const std::string& key) {
  // Reference: Data store at 0x1801ca600 (ovr_Room_GetDataStore)
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);

  auto it = impl_->rooms.find(room_id);
  if (it == impl_->rooms.end()) {
    return "";
  }

  auto data_it = it->second.data_store.find(key);
  if (data_it == it->second.data_store.end()) {
    return "";
  }

  return data_it->second;
}

bool RoomSubsystem::SetRoomData(uint64_t room_id, const std::string& key, const std::string& value) {
  // Reference: Data update at 0x1801ca600
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);

  auto it = impl_->rooms.find(room_id);
  if (it == impl_->rooms.end()) {
    return false;
  }

  if (value.empty()) {
    // Delete key
    it->second.data_store.erase(key);
  } else {
    // Set key-value
    it->second.data_store[key] = value;
  }

  return true;
}

RoomJoinPolicy RoomSubsystem::GetRoomJoinPolicy(uint64_t room_id) {
  // Reference: Policy field at 0x1801ca700 (ovr_Room_GetJoinPolicy)
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);

  auto it = impl_->rooms.find(room_id);
  if (it == impl_->rooms.end()) {
    return RoomJoinPolicy::Public;
  }

  return it->second.join_policy;
}

bool RoomSubsystem::SetRoomJoinPolicy(uint64_t room_id, RoomJoinPolicy policy) {
  // Reference: Policy update at 0x1801ca700
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);

  auto it = impl_->rooms.find(room_id);
  if (it == impl_->rooms.end()) {
    return false;
  }

  // Check permissions: Owner only (in real implementation)
  it->second.join_policy = policy;
  return true;
}

bool RoomSubsystem::DestroyRoom(uint64_t room_id) {
  // Reference: Destruction at 0x1801ca900
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);

  auto it = impl_->rooms.find(room_id);
  if (it == impl_->rooms.end()) {
    return false;
  }

  impl_->rooms.erase(it);
  return true;
}

std::vector<uint64_t> RoomSubsystem::GetActiveRooms() {
  // Reference: Room enumeration at 0x1801ca200
  std::lock_guard<std::mutex> lock(impl_->rooms_mutex);

  std::vector<uint64_t> result;
  for (const auto& pair : impl_->rooms) {
    result.push_back(pair.first);
  }

  return result;
}

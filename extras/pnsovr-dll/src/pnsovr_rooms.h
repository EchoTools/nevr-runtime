/**
 * @file pnsovr_rooms.h
 * @brief NEVR PNSOvr Compatibility - Room Management
 *
 * Implements virtual multiplayer rooms for Echo VR.
 *
 * Binary Reference: pnsovr.dll v34.4 (Echo VR)
 * - ovr_Room_CreateAndJoinPrivate2: 0x1801ca200
 * - ovr_Room_Get: 0x1801ca300
 * - ovr_Room_GetID: 0x1801ca400
 * - ovr_Room_GetUsers: 0x1801ca500
 * - ovr_Room_GetDataStore: 0x1801ca600
 * - ovr_Room_GetJoinPolicy: 0x1801ca700
 * - ovr_RoomOptions_*: 0x1801ca800+
 */

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

/**
 * @brief Join policy for rooms.
 *
 * Reference: Policy enumeration at 0x1801ca700
 */
enum class RoomJoinPolicy : uint32_t { Public = 0, FriendsOnly = 1, InviteOnly = 2, Private = 3 };

/**
 * @brief Room user ordering.
 *
 * Reference: Ordering flags at 0x1801ca800
 */
enum class RoomOrdering : uint32_t { None = 0, UpdateTime = 1, MemberCount = 2 };

/**
 * @brief Room configuration options.
 *
 * Binary reference: Structure at 0x1801ca800
 * Size: 256 bytes
 *
 * Field layout:
 * +0x00: name (string, max 64 chars)
 * +0x40: max_capacity (uint32_t)
 * +0x44: join_policy (uint32_t)
 * +0x48: ordering (uint32_t)
 * +0x4c: order_by_update_time (uint8_t)
 * +0x4d: order_by_member_count (uint8_t)
 */
#pragma warning(push)
#pragma warning(disable : 4820)  // Suppress struct padding warnings
struct RoomOptions {
  std::string name;
  uint32_t max_capacity;
  RoomJoinPolicy join_policy;
  RoomOrdering ordering;
  bool order_by_update_time;
  bool order_by_member_count;
};
#pragma warning(pop)

/**
 * @brief Room user entry.
 *
 * Reference: User array structure at 0x1801ca500
 * Size: 64 bytes per entry
 */
#pragma warning(push)
#pragma warning(disable : 4820)  // Suppress struct padding warnings
struct RoomUser {
  uint64_t user_id;
  std::string display_name;
  bool is_online;
  int64_t updated_at;
};
#pragma warning(pop)

/**
 * @brief Room state and metadata.
 *
 * Binary reference: Room structure at 0x1801ca200
 * Size: 512 bytes
 *
 * Field layout:
 * +0x00: id (uint64_t)
 * +0x08: owner_id (uint64_t)
 * +0x10: name (string, max 64 chars)
 * +0x50: max_capacity (uint32_t)
 * +0x54: current_capacity (uint32_t)
 * +0x58: join_policy (uint32_t)
 * +0x5c: created_at (int64_t)
 * +0x64: expires_at (int64_t)
 * +0x6c: data_store (Map<string, string>)
 * +0xd0: users (vector<RoomUser>)
 */
#pragma warning(push)
#pragma warning(disable : 4820 4315)  // Suppress struct padding and alignment warnings
struct RoomInfo {
  uint64_t id;
  uint64_t owner_id;
  std::string name;
  uint32_t max_capacity;
  uint32_t current_capacity;
  RoomJoinPolicy join_policy;
  int64_t created_at;
  int64_t expires_at;
  std::map<std::string, std::string> data_store;
  std::vector<RoomUser> users;
};
#pragma warning(pop)

/**
 * @brief Room management subsystem.
 *
 * Manages multiplayer rooms: creation, joining, leaving, user management.
 * Provides persistent data storage per-room.
 *
 * Binary reference: Implementation at 0x1801ca200-0x1801cb000
 */
class RoomSubsystem {
 public:
  RoomSubsystem();
  ~RoomSubsystem();

  /**
   * @brief Initialize room subsystem.
   * @return true if initialization succeeded.
   *
   * Binary reference: Initialization at 0x1801ca200
   */
  bool Initialize();

  /**
   * @brief Shutdown room subsystem.
   *
   * Binary reference: Cleanup at 0x1801ca900
   */
  void Shutdown();

  /**
   * @brief Create a new room.
   * @param options Room configuration.
   * @return Room ID, or 0 if creation failed.
   *
   * Binary reference: Creation logic at 0x1801ca200 (ovr_Room_CreateAndJoinPrivate2)
   * Process:
   * 1. Allocate new room structure (512 bytes)
   * 2. Assign unique room ID
   * 3. Set owner to current user
   * 4. Initialize user list with owner
   * 5. Initialize data store (empty)
   * 6. Return room ID
   */
  uint64_t CreateRoom(const RoomOptions& options);

  /**
   * @brief Join an existing room.
   * @param room_id Room to join.
   * @param user_id User joining (0 = current user).
   * @return true if join succeeded.
   *
   * Binary reference: Join logic at 0x1801ca300+ (ovr_Room_Get)
   * Validates:
   * - Room exists
   * - Not at capacity
   * - User has permission (based on join_policy)
   * - User not already in room
   *
   * Process:
   * 1. Look up room by ID
   * 2. Check capacity and policy
   * 3. Add user to room user list
   * 4. Increment current_capacity
   * 5. Return success
   */
  bool JoinRoom(uint64_t room_id, uint64_t user_id);

  /**
   * @brief Leave a room.
   * @param room_id Room to leave.
   * @param user_id User leaving (0 = current user).
   * @return true if leave succeeded.
   *
   * Binary reference: Leave logic at 0x1801ca400+ (ovr_Room_GetID)
   * Process:
   * 1. Look up room
   * 2. Remove user from user list
   * 3. Decrement current_capacity
   * 4. If owner leaves and room empty, destroy room
   * 5. Return success
   */
  bool LeaveRoom(uint64_t room_id, uint64_t user_id);

  /**
   * @brief Get room information.
   * @param room_id Room to query.
   * @return Room info, or empty if not found.
   *
   * Binary reference: Lookup at 0x1801ca300 (ovr_Room_Get)
   * Returns copy of room structure including:
   * - Metadata (owner, capacity, policy)
   * - User list with presence
   * - Data store contents
   */
  RoomInfo GetRoom(uint64_t room_id);

  /**
   * @brief Get list of users in room.
   * @param room_id Room to query.
   * @return List of room users, or empty if room not found.
   *
   * Binary reference: User enumeration at 0x1801ca500 (ovr_Room_GetUsers)
   * Returns:
   * - User ID list
   * - Display names
   * - Online status
   * - Last update time (for ordering)
   *
   * Supports pagination (HasNextPage field in binary).
   */
  std::vector<RoomUser> GetRoomUsers(uint64_t room_id);

  /**
   * @brief Get room data store value.
   * @param room_id Room to query.
   * @param key Data key.
   * @return Value, or empty string if not found.
   *
   * Binary reference: Data store at 0x1801ca600 (ovr_Room_GetDataStore)
   * Access pattern:
   * 1. Look up room
   * 2. Look up key in data_store map
   * 3. Return value or empty
   * 4. Supports empty values (distinguishes from not-found)
   */
  std::string GetRoomData(uint64_t room_id, const std::string& key);

  /**
   * @brief Set room data store value.
   * @param room_id Room to modify.
   * @param key Data key.
   * @param value Data value (empty to delete).
   * @return true if update succeeded.
   *
   * Binary reference: Data update at 0x1801ca600
   * Process:
   * 1. Look up room
   * 2. Check permissions (owner or designated data writer)
   * 3. Set or delete key-value pair
   * 4. Broadcast update to room members
   * 5. Return success
   */
  bool SetRoomData(uint64_t room_id, const std::string& key, const std::string& value);

  /**
   * @brief Get room join policy.
   * @param room_id Room to query.
   * @return Join policy, or Public if room not found.
   *
   * Binary reference: Policy field at 0x1801ca700 (ovr_Room_GetJoinPolicy)
   */
  RoomJoinPolicy GetRoomJoinPolicy(uint64_t room_id);

  /**
   * @brief Update room join policy.
   * @param room_id Room to modify.
   * @param policy New join policy.
   * @return true if update succeeded.
   *
   * Binary reference: Policy update at 0x1801ca700
   * Permissions: Owner only
   */
  bool SetRoomJoinPolicy(uint64_t room_id, RoomJoinPolicy policy);

  /**
   * @brief Destroy a room.
   * @param room_id Room to destroy.
   * @return true if destruction succeeded.
   *
   * Binary reference: Destruction at 0x1801ca900
   * Permissions: Owner only
   * Process:
   * 1. Check owner permission
   * 2. Notify all room members of dissolution
   * 3. Clear data store
   * 4. Remove room structure
   * 5. Return success
   */
  bool DestroyRoom(uint64_t room_id);

  /**
   * @brief Get all active rooms.
   * @return List of room IDs currently active.
   *
   * Binary reference: Room enumeration at 0x1801ca200
   */
  std::vector<uint64_t> GetActiveRooms();

 private:
  struct Impl;
  Impl* impl_;
};

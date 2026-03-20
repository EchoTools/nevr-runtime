#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pnsovr::social {

/**
 * @note This module is completely self-contained and does NOT depend on
 * libOVRPlatform or any Oculus SDK. It provides static/placeholder implementations
 * of all social features.
 */

// ============================================================================
// Oculus Platform SDK Type Definitions (from analysis)
// ============================================================================

using ovrID = uint64_t;
using ovrRequest = uint64_t;

// Room-related types
struct ovrRoomOptions {
  ovrID exclude_recent_arrived_user;
  int max_user_results;
  int sorting_option;
};

struct ovrRoomInviteAccept_Params {
  const char* room_id;
  const char* inviter_id;
};

struct ovrUser {
  ovrID id;
  char display_name[256];
  char image_url[2048];
  char small_image_url[2048];
  uint32_t presence_status;
  char presence_string[512];
};

struct ovrRoom {
  ovrID id;
  char name[256];
  uint32_t max_users;
  uint32_t current_users;
  ovrUser* users;
  uint32_t user_count;
  char description[1024];
};

// ============================================================================
// Hook Function Signatures (matching original library)
// ============================================================================

// Room Functions
using ovr_Room_CreateAndJoinPrivate2_FuncPtr = ovrRequest (*)(ovrRequest request_id, unsigned int max_users,
                                                              bool join_policy, bool visibility);

using ovr_Room_Join2_FuncPtr = ovrRequest (*)(ovrID room_id);

using ovr_Room_Leave_FuncPtr = ovrRequest (*)(ovrID room_id);

using ovr_Room_Get_FuncPtr = ovrRequest (*)(ovrID room_id);

using ovr_Room_GetCurrentForUser_FuncPtr = ovrRequest (*)(void);

using ovr_Room_InviteUser_FuncPtr = ovrRequest (*)(ovrID room_id, ovrID user_id);

using ovr_Room_KickUser_FuncPtr = ovrRequest (*)(ovrID room_id, ovrID user_id, int kick_reason);

using ovr_Room_UpdateDescription_FuncPtr = ovrRequest (*)(ovrID room_id, const char* description);

using ovr_Room_UpdateMembershipLocked_FuncPtr = ovrRequest (*)(ovrID room_id, bool locked);

using ovr_Room_UpdateOwner_FuncPtr = ovrRequest (*)(ovrID room_id, ovrID user_id);

// User Functions
using ovr_User_Get_FuncPtr = ovrRequest (*)(ovrID user_id);

using ovr_User_GetLoggedInUser_FuncPtr = ovrRequest (*)(void);

using ovr_User_GetUserProof_FuncPtr = ovrRequest (*)(void);

using ovr_Users_GetLoggedInUser_FuncPtr = ovrRequest (*)(void);

// Network Functions
using ovr_Net_SendPacket_FuncPtr = bool (*)(ovrID peer_id, int packet_size, const void* packet, bool reliable);

using ovr_Net_ReadPacket_FuncPtr = bool (*)(ovrID* out_peer_id, unsigned int* out_packet_size, void* out_packet);

using ovr_Net_IsConnected_FuncPtr = bool (*)(ovrID peer_id);

// VOIP Functions
using ovr_Voip_Start_FuncPtr = ovrRequest (*)(ovrID user_id);

using ovr_Voip_Stop_FuncPtr = ovrRequest (*)(ovrID user_id);

using ovr_Voip_SetLocalMicrophoneActive_FuncPtr = ovrRequest (*)(bool active);

// ============================================================================
// Callback Types for Custom Implementations
// ============================================================================

using RoomCreatedCallback = std::function<void(ovrRoom* room)>;
using UserJoinedCallback = std::function<void(ovrUser* user)>;
using UserLeftCallback = std::function<void(ovrID user_id)>;
using PacketReceivedCallback = std::function<void(ovrID sender, const void* data, int size)>;
using VoipDataCallback = std::function<void(ovrID speaker, const void* data, int size)>;

// ============================================================================
// Social Interceptor Manager
// ============================================================================

class SocialInterceptor {
 public:
  // Initialization and teardown
  static bool Initialize();
  static void Shutdown();

  // Hook installation/removal
  bool InstallHooks();
  bool RemoveHooks();

  // Callback registration
  void SetRoomCreatedCallback(RoomCreatedCallback callback);
  void SetUserJoinedCallback(UserJoinedCallback callback);
  void SetUserLeftCallback(UserLeftCallback callback);
  void SetPacketReceivedCallback(PacketReceivedCallback callback);
  void SetVoipDataCallback(VoipDataCallback callback);

  // Configuration
  void EnableRoomInterception(bool enable) { room_interception_enabled_ = enable; }
  void EnableUserInterception(bool enable) { user_interception_enabled_ = enable; }
  void EnableNetworkInterception(bool enable) { network_interception_enabled_ = enable; }
  void EnableVoipInterception(bool enable) { voip_interception_enabled_ = enable; }

  // Custom implementations (can be overridden)
  virtual ovrRequest OnRoomCreate(unsigned int max_users, bool private_room, bool visibility);
  virtual ovrRequest OnRoomJoin(ovrID room_id);
  virtual ovrRequest OnRoomLeave(ovrID room_id);
  virtual ovrRequest OnUserGet(ovrID user_id);
  virtual ovrRequest OnUserGetLoggedIn();
  virtual bool OnPacketSend(ovrID peer_id, int packet_size, const void* packet, bool reliable);
  virtual bool OnPacketReceive(ovrID* out_peer_id, unsigned int* out_packet_size, void* out_packet);

  // Singleton access
  static SocialInterceptor& Get();

 private:
  SocialInterceptor();
  ~SocialInterceptor();

  // Original function pointers (from pnsovr.dll)
  ovr_Room_CreateAndJoinPrivate2_FuncPtr original_room_create_ = nullptr;
  ovr_Room_Join2_FuncPtr original_room_join_ = nullptr;
  ovr_Room_Leave_FuncPtr original_room_leave_ = nullptr;
  ovr_Room_Get_FuncPtr original_room_get_ = nullptr;
  ovr_User_Get_FuncPtr original_user_get_ = nullptr;
  ovr_User_GetLoggedInUser_FuncPtr original_user_get_logged_in_ = nullptr;
  ovr_Net_SendPacket_FuncPtr original_net_send_ = nullptr;
  ovr_Net_ReadPacket_FuncPtr original_net_read_ = nullptr;
  ovr_Voip_Start_FuncPtr original_voip_start_ = nullptr;
  ovr_Voip_Stop_FuncPtr original_voip_stop_ = nullptr;

  // Interception flags
  bool room_interception_enabled_ = true;
  bool user_interception_enabled_ = true;
  bool network_interception_enabled_ = true;
  bool voip_interception_enabled_ = true;
  bool hooks_installed_ = false;

  // Callbacks
  RoomCreatedCallback room_created_callback_;
  UserJoinedCallback user_joined_callback_;
  UserLeftCallback user_left_callback_;
  PacketReceivedCallback packet_received_callback_;
  VoipDataCallback voip_data_callback_;

  // State tracking
  std::map<ovrID, ovrRoom> cached_rooms_;
  std::map<ovrID, ovrUser> cached_users_;
  ovrID current_room_id_ = 0;
  ovrID logged_in_user_id_ = 0;
};

// ============================================================================
// Hook Detour Functions (called by MinHook redirects)
// ============================================================================

namespace hooks {
ovrRequest DetourRoomCreateAndJoinPrivate2(ovrRequest request_id, unsigned int max_users, bool join_policy,
                                           bool visibility);

ovrRequest DetourRoomJoin2(ovrID room_id);
ovrRequest DetourRoomLeave(ovrID room_id);
ovrRequest DetourRoomGet(ovrID room_id);
ovrRequest DetourUserGet(ovrID user_id);
ovrRequest DetourUserGetLoggedInUser(void);

bool DetourNetSendPacket(ovrID peer_id, int packet_size, const void* packet, bool reliable);

bool DetourNetReadPacket(ovrID* out_peer_id, unsigned int* out_packet_size, void* out_packet);

ovrRequest DetourVoipStart(ovrID user_id);
ovrRequest DetourVoipStop(ovrID user_id);
}  // namespace hooks

// ============================================================================
// Helper Functions
// ============================================================================

// Convert string to Oculus ID
ovrID StringToOvrID(const std::string& str);

// Convert Oculus ID to string
std::string OvrIDToString(ovrID id);

// Get function address in pnsovr.dll by name
void* GetPnsOvrFunctionAddress(const std::string& function_name);

// Logging helpers
void LogRoomEvent(const std::string& event, ovrID room_id, const std::string& details = "");
void LogUserEvent(const std::string& event, ovrID user_id, const std::string& details = "");
void LogNetworkEvent(const std::string& event, ovrID peer_id, int size, bool reliable = false);

}  // namespace pnsovr::social

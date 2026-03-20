#pragma warning(disable : 4711)  // Suppress function not inlined warnings (happens with LTCG)

#include "PNSOVRSocialInterceptor.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <sstream>

/**
 * @note This implementation is completely standalone - no MinHook or libOVRPlatform dependency.
 * It provides static/placeholder implementations of all Oculus Platform APIs.
 */

namespace pnsovr::social {

// ============================================================================
// Static Variables
// ============================================================================

static std::atomic<bool> g_interceptor_initialized = false;

// ============================================================================
// Helper Logging Functions
// ============================================================================

void LogRoomEvent(const std::string& event, ovrID room_id, const std::string& details) {
  std::stringstream ss;
  ss << "[PNSOVRSocial::Room] " << event << " | ID: 0x" << std::hex << room_id << std::dec;
  if (!details.empty()) ss << " | " << details;
  std::cout << ss.str() << std::endl;
}

void LogUserEvent(const std::string& event, ovrID user_id, const std::string& details) {
  std::stringstream ss;
  ss << "[PNSOVRSocial::User] " << event << " | ID: 0x" << std::hex << user_id << std::dec;
  if (!details.empty()) ss << " | " << details;
  std::cout << ss.str() << std::endl;
}

void LogNetworkEvent(const std::string& event, ovrID peer_id, int size, bool reliable) {
  std::stringstream ss;
  ss << "[PNSOVRSocial::Network] " << event << " | Peer: 0x" << std::hex << peer_id << std::dec << " | Size: " << size
     << " | Reliable: " << (reliable ? "yes" : "no");
  std::cout << ss.str() << std::endl;
}

ovrID StringToOvrID(const std::string& str) { return std::stoull(str, nullptr, 0); }

std::string OvrIDToString(ovrID id) {
  std::stringstream ss;
  ss << "0x" << std::hex << id;
  return ss.str();
}

// ============================================================================
// SocialInterceptor Implementation (Standalone)
// ============================================================================

SocialInterceptor::SocialInterceptor() {}

SocialInterceptor::~SocialInterceptor() {
  if (hooks_installed_) {
    RemoveHooks();
  }
}

SocialInterceptor& SocialInterceptor::Get() {
  static SocialInterceptor instance;
  return instance;
}

bool SocialInterceptor::Initialize() {
  if (g_interceptor_initialized.exchange(true)) {
    std::cout << "[PNSOVRSocial] SocialInterceptor already initialized" << std::endl;
    return true;
  }

  std::cout << "[PNSOVRSocial] SocialInterceptor initialized (STANDALONE - no libOVRPlatform dependency)" << std::endl;
  return true;
}

void SocialInterceptor::Shutdown() {
  SocialInterceptor& interceptor = Get();
  if (interceptor.hooks_installed_) {
    interceptor.RemoveHooks();
  }

  if (g_interceptor_initialized.exchange(false)) {
    std::cout << "[PNSOVRSocial] SocialInterceptor uninitialized" << std::endl;
  }
}

bool SocialInterceptor::InstallHooks() {
  if (hooks_installed_) {
    std::cout << "[PNSOVRSocial] Hooks already installed" << std::endl;
    return true;
  }

  // In standalone mode, we don't hook any functions.
  // All API calls are handled through CustomSocialManager with static data.

  hooks_installed_ = true;
  std::cout << "[PNSOVRSocial] Standalone mode active - using static placeholder data" << std::endl;
  return true;
}

bool SocialInterceptor::RemoveHooks() {
  if (!hooks_installed_) {
    return true;
  }

  hooks_installed_ = false;
  std::cout << "[PNSOVRSocial] Standalone mode disabled" << std::endl;
  return true;
}

void SocialInterceptor::SetRoomCreatedCallback(RoomCreatedCallback callback) { room_created_callback_ = callback; }

void SocialInterceptor::SetUserJoinedCallback(UserJoinedCallback callback) { user_joined_callback_ = callback; }

void SocialInterceptor::SetUserLeftCallback(UserLeftCallback callback) { user_left_callback_ = callback; }

void SocialInterceptor::SetPacketReceivedCallback(PacketReceivedCallback callback) {
  packet_received_callback_ = callback;
}

void SocialInterceptor::SetVoipDataCallback(VoipDataCallback callback) { voip_data_callback_ = callback; }

// ============================================================================
// Stub Implementation Methods (Return Placeholder Data)
// ============================================================================

ovrRequest SocialInterceptor::OnRoomCreate(unsigned int max_users, bool private_room, bool visibility) {
  LogRoomEvent("OnRoomCreate", 0,
               "MaxUsers=" + std::to_string(max_users) + ", Private=" + (private_room ? "true" : "false"));
  return 1001;  // Return placeholder request ID
}

ovrRequest SocialInterceptor::OnRoomJoin(ovrID room_id) {
  LogRoomEvent("OnRoomJoin", room_id);
  current_room_id_ = room_id;
  return 1002;
}

ovrRequest SocialInterceptor::OnRoomLeave(ovrID room_id) {
  LogRoomEvent("OnRoomLeave", room_id);
  if (current_room_id_ == room_id) {
    current_room_id_ = 0;
  }
  return 1003;
}

ovrRequest SocialInterceptor::OnUserGet(ovrID user_id) {
  LogUserEvent("OnUserGet", user_id);
  return 1004;
}

ovrRequest SocialInterceptor::OnUserGetLoggedIn() {
  std::cout << "[PNSOVRSocial::User] OnUserGetLoggedIn" << std::endl;
  return 1005;
}

bool SocialInterceptor::OnPacketSend(ovrID peer_id, int packet_size, const void* packet, bool reliable) {
  LogNetworkEvent("OnPacketSend", peer_id, packet_size, reliable);

  if (packet_received_callback_) {
    packet_received_callback_(peer_id, packet, packet_size);
  }

  return true;  // Return success
}

bool SocialInterceptor::OnPacketReceive(ovrID* out_peer_id, unsigned int* out_packet_size, void* out_packet) {
  // Return false (no packets available) for placeholder implementation
  if (out_peer_id) *out_peer_id = 0;
  if (out_packet_size) *out_packet_size = 0;
  return false;
}

// ============================================================================
// Hook Detour Implementations (Now Standalone)
// ============================================================================

namespace hooks {

ovrRequest DetourRoomCreateAndJoinPrivate2(ovrRequest request_id, unsigned int max_users, bool join_policy,
                                           bool visibility) {
  return SocialInterceptor::Get().OnRoomCreate(max_users, join_policy, visibility);
}

ovrRequest DetourRoomJoin2(ovrID room_id) { return SocialInterceptor::Get().OnRoomJoin(room_id); }

ovrRequest DetourRoomLeave(ovrID room_id) { return SocialInterceptor::Get().OnRoomLeave(room_id); }

ovrRequest DetourRoomGet(ovrID room_id) {
  LogRoomEvent("DetourRoomGet", room_id);
  return 0;
}

ovrRequest DetourUserGet(ovrID user_id) { return SocialInterceptor::Get().OnUserGet(user_id); }

ovrRequest DetourUserGetLoggedInUser(void) { return SocialInterceptor::Get().OnUserGetLoggedIn(); }

bool DetourNetSendPacket(ovrID peer_id, int packet_size, const void* packet, bool reliable) {
  return SocialInterceptor::Get().OnPacketSend(peer_id, packet_size, packet, reliable);
}

bool DetourNetReadPacket(ovrID* out_peer_id, unsigned int* out_packet_size, void* out_packet) {
  return SocialInterceptor::Get().OnPacketReceive(out_peer_id, out_packet_size, out_packet);
}

ovrRequest DetourVoipStart(ovrID user_id) {
  LogUserEvent("VoipStart", user_id);
  return 1006;
}

ovrRequest DetourVoipStop(ovrID user_id) {
  LogUserEvent("VoipStop", user_id);
  return 1007;
}

}  // namespace hooks

}  // namespace pnsovr::social

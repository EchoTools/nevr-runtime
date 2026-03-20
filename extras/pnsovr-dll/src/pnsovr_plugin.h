/**
 * @file pnsovr_plugin.h
 * @brief NEVR PNSOvr Compatibility Layer - Main Plugin Interface
 *
 * Complete replacement for pnsovr.dll v34.4 (Echo VR platform native services).
 * Implements all subsystems: voice, users, rooms, presence, IAP, notifications.
 *
 * Binary Reference: pnsovr.dll v34.4 (Echo VR)
 * Main entry point (DLL export):
 * - RadPluginInit: 0x180090000
 * - RadPluginMain: 0x180090100
 * - RadPluginShutdown: 0x180090200
 */

#pragma once

#include <cstdint>

#include "pnsovr_iap.h"
#include "pnsovr_notifications.h"
#include "pnsovr_presence.h"
#include "pnsovr_rooms.h"
#include "pnsovr_users.h"
#include "pnsovr_voip.h"

/**
 * @brief PNSOvr plugin configuration.
 *
 * Binary reference: Config structure passed to RadPluginInit
 * Size: 256 bytes
 */
#pragma warning(push)
#pragma warning(disable : 4820)  // Suppress struct padding warnings
struct PNSOvrConfig {
  // Voip configuration
  // Reference: Voice settings at 0x1801ba500
  uint32_t voip_sample_rate;     // 16000, 24000, 48000 Hz
  uint32_t voip_bit_rate;        // 12000-64000 bps
  uint32_t voip_frame_duration;  // 20-120 ms
  bool voip_enable_dtx;          // Discontinuous transmission
  bool voip_enable_fec;          // Forward error correction

  // Network configuration
  // Reference: Network settings at 0x1801c0000
  const char* server_address;  // NEVR server hostname
  uint16_t server_port;        // NEVR server port
  bool use_encrypted_voip;     // Enable DTLS for voice

  // Platform configuration
  // Reference: Platform integration at 0x1801a0000
  uint64_t app_id;         // Oculus application ID
  uint64_t user_id;        // Current user ID
  const char* user_token;  // Session token
};
#pragma warning(pop)

/**
 * @brief PNSOvr compatibility plugin.
 *
 * Complete reimplementation of pnsovr.dll platform native services.
 * Provides all voice, social, and commerce features from original Echo VR.
 *
 * Binary reference: Implementation at 0x180090000-0x180200000 (original DLL span)
 *
 * Integration Points:
 * - Game calls RadPluginInit at startup (0x180090000)
 * - Game calls RadPluginMain each frame (0x180090100)
 * - Game calls subsystem functions via DLL exports
 * - Game processes callbacks for async operations
 */
class PNSOvrPlugin {
 public:
  PNSOvrPlugin();
  ~PNSOvrPlugin();

  /**
   * @brief Initialize plugin with configuration.
   * @param config Plugin configuration.
   * @return true if initialization succeeded.
   *
   * Binary reference: RadPluginInit at 0x180090000
   * Process:
   * 1. Initialize all subsystems (Voip, Users, Rooms, etc.)
   * 2. Load product catalog
   * 3. Connect to NEVR backend server
   * 4. Authenticate current user
   * 5. Load user's friends list
   * 6. Load pending notifications/invites
   * 7. Start network event loop
   */
  bool Initialize(const PNSOvrConfig& config);

  /**
   * @brief Shutdown plugin and cleanup.
   *
   * Binary reference: RadPluginShutdown at 0x180090200
   * Process:
   * 1. Close all active voice calls
   * 2. Save user state
   * 3. Disconnect from backend
   * 4. Shutdown all subsystems
   * 5. Release memory
   */
  void Shutdown();

  /**
   * @brief Process pending operations (call each frame).
   * @return true if plugin is still running.
   *
   * Binary reference: RadPluginMain at 0x180090100
   * Operations:
   * 1. Process network events
   * 2. Deliver voice frames (encode/decode)
   * 3. Handle async operation completion
   * 4. Process user callbacks
   * 5. Update presence
   * 6. Handle connection state changes
   *
   * Called from game main loop at ~90 FPS (Echo VR frame rate).
   */
  bool Tick();

  /**
   * @brief Get voice communication subsystem.
   * @return Pointer to VoipSubsystem, or nullptr if not initialized.
   *
   * Reference: VoipSubsystem at 0x1801b8c30+
   */
  VoipSubsystem* GetVoipSubsystem();

  /**
   * @brief Get user management subsystem.
   * @return Pointer to UserSubsystem, or nullptr if not initialized.
   *
   * Reference: UserSubsystem at 0x1801cb000+
   */
  UserSubsystem* GetUserSubsystem();

  /**
   * @brief Get room management subsystem.
   * @return Pointer to RoomSubsystem, or nullptr if not initialized.
   *
   * Reference: RoomSubsystem at 0x1801ca200+
   */
  RoomSubsystem* GetRoomSubsystem();

  /**
   * @brief Get rich presence subsystem.
   * @return Pointer to PresenceSubsystem, or nullptr if not initialized.
   *
   * Reference: PresenceSubsystem at 0x1801cc200+
   */
  PresenceSubsystem* GetPresenceSubsystem();

  /**
   * @brief Get in-app purchase subsystem.
   * @return Pointer to IAPSubsystem, or nullptr if not initialized.
   *
   * Reference: IAPSubsystem at 0x1801d0000+
   */
  IAPSubsystem* GetIAPSubsystem();

  /**
   * @brief Get notification subsystem.
   * @return Pointer to NotificationSubsystem, or nullptr if not initialized.
   *
   * Reference: NotificationSubsystem at 0x1801d2000+
   */
  NotificationSubsystem* GetNotificationSubsystem();

  /**
   * @brief Check if plugin is initialized and running.
   */
  bool IsInitialized() const;

  /**
   * @brief Get current error message (if any).
   */
  const char* GetLastError() const;

 private:
  struct Impl;
  Impl* impl_;
};

/**
 * @brief Global plugin instance (singleton).
 *
 * Reference: Global instance at 0x180350000 (data section)
 * Created by RadPluginInit, destroyed by RadPluginShutdown.
 */
extern PNSOvrPlugin* g_pnsovr_plugin;

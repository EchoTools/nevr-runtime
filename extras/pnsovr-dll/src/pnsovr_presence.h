/**
 * @file pnsovr_presence.h
 * @brief NEVR PNSOvr Compatibility - Rich Presence Broadcasting
 *
 * Implements rich presence to inform Oculus platform and friends about current activity.
 *
 * Binary Reference: pnsovr.dll v34.4 (Echo VR)
 * - ovr_RichPresence_Set: 0x1801cc200
 * - ovr_RichPresence_Clear: 0x1801cc300
 * - ovr_RichPresenceOptions_*: 0x1801cc400
 */

#pragma once

#include <cstdint>
#include <string>

/**
 * @brief Rich presence configuration.
 *
 * Binary reference: Structure at 0x1801cc400
 * Size: 512 bytes
 *
 * Field layout:
 * +0x00: api_name (string, max 64 chars)
 * +0x40: current_capacity (uint32_t)
 * +0x44: max_capacity (uint32_t)
 * +0x48: start_time (int64_t)
 * +0x50: end_time (int64_t)
 * +0x58: instance_id (string, max 128 chars)
 * +0xd8: is_joinable (uint8_t)
 * +0xd9: deeplink_message_override (string, max 256 chars)
 * +0x1d9: extra_context (string, max 256 chars)
 */
#pragma warning(push)
#pragma warning(disable : 4820 4315)  // Suppress struct padding and alignment warnings
struct RichPresenceConfig {
  std::string api_name;                   // Platform identifier (e.g., "echovr")
  uint32_t current_capacity;              // Current players in session
  uint32_t max_capacity;                  // Maximum players
  int64_t start_time;                     // Session start timestamp
  int64_t end_time;                       // Session end timestamp
  std::string instance_id;                // Game instance UUID
  bool is_joinable;                       // Friends can join via social features
  std::string deeplink_message_override;  // Custom invite text
  std::string extra_context;              // JSON game data (e.g., map name, mode)
};
#pragma warning(pop)

/**
 * @brief Rich presence management.
 *
 * Broadcasts activity to Oculus platform, enabling:
 * - Activity display in Oculus home
 * - Friend visibility of current game
 * - One-click social join via deeplinks
 * - Custom activity data
 *
 * Binary reference: Implementation at 0x1801cc200-0x1801cd000
 */
class PresenceSubsystem {
 public:
  PresenceSubsystem();
  ~PresenceSubsystem();

  /**
   * @brief Initialize presence subsystem.
   * @return true if initialization succeeded.
   *
   * Binary reference: Initialization at 0x1801cc200
   */
  bool Initialize();

  /**
   * @brief Shutdown presence subsystem.
   *
   * Binary reference: Cleanup at 0x1801ccf00
   */
  void Shutdown();

  /**
   * @brief Set rich presence configuration.
   * @param config New presence configuration.
   * @return true if update succeeded.
   *
   * Binary reference: Set operation at 0x1801cc200 (ovr_RichPresence_Set)
   * Process:
   * 1. Validate configuration
   * 2. Store current presence state
   * 3. Broadcast to Oculus platform via network
   * 4. Update friend visibility
   *
   * This should be called whenever player activity changes:
   * - Entering/leaving game
   * - Switching maps/modes
   * - Changing lobby size
   */
  bool SetPresence(const RichPresenceConfig& config);

  /**
   * @brief Clear rich presence.
   * @return true if clear succeeded.
   *
   * Binary reference: Clear operation at 0x1801cc300 (ovr_RichPresence_Clear)
   * Notifies Oculus platform that player is no longer in activity.
   * Should be called when:
   * - Exiting game
   * - Logout
   * - Session ended
   */
  bool ClearPresence();

  /**
   * @brief Get current rich presence configuration.
   * @return Current config, or empty if not set.
   *
   * Binary reference: Accessor at 0x1801cc400
   */
  RichPresenceConfig GetCurrentPresence() const;

  /**
   * @brief Update presence with custom data.
   * @param instance_id Game instance identifier.
   * @param custom_data JSON string with game-specific data.
   * @return true if update succeeded.
   *
   * Binary reference: Update at 0x1801cc200
   * Partial update (preserves other fields).
   *
   * Custom data format (JSON):
   * {
   *   "map": "map_name",
   *   "mode": "game_mode",
   *   "level": "difficulty",
   *   "custom_field": "any_value"
   * }
   */
  bool UpdatePresenceData(const std::string& instance_id, const std::string& custom_data);

  /**
   * @brief Check if presence is currently active.
   * @return true if presence has been set and not cleared.
   */
  bool IsPresenceActive() const;

 private:
  struct Impl;
  Impl* impl_;
};

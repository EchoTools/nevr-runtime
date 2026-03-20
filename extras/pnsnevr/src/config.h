/*
 * pnsnevr - Configuration
 *
 * Parses Nakama configuration from the game's config system.
 */

#pragma once

#include <string>

struct NakamaFeatures {
  bool friends = true;
  bool parties = true;
  bool matchmaking = true;
  bool leaderboards = false;
  bool storage = false;
};

struct NakamaConfig {
  std::string api_endpoint = "http://127.0.0.1";
  int http_port = 7350;
  int grpc_port = 7349;
  std::string server_key = "defaultkey";

  NakamaFeatures features;

  // Authentication settings
  bool auto_create_user = true;
  std::string auth_method = "device";  // "device", "custom", "email"

  // Retry settings
  int max_retries = 3;
  int retry_delay_ms = 1000;

  // Polling intervals (ms)
  int presence_poll_interval = 5000;
  int matchmaking_poll_interval = 1000;
};

/*
 * Load Nakama configuration from game config.
 *
 * Expected config.json structure:
 * {
 *   "social_platform": "nakama",
 *   "nakama": {
 *     "api_endpoint": "http://nakama.example.com",
 *     "http_port": 7350,
 *     "grpc_port": 7349,
 *     "server_key": "your-key",
 *     "features": {
 *       "friends": true,
 *       "parties": true,
 *       "matchmaking": true
 *     }
 *   }
 * }
 *
 * @param game_config Pointer to game's config structure
 * @param out_config Output configuration structure
 * @return true if config was loaded successfully
 */
bool LoadNakamaConfig(void* game_config, NakamaConfig* out_config);

/*
 * Load Nakama configuration from a JSON file.
 * Used for standalone testing.
 *
 * @param filepath Path to nakama_config.json
 * @param out_config Output configuration structure
 * @return true if config was loaded successfully
 */
bool LoadNakamaConfigFromFile(const char* filepath, NakamaConfig* out_config);

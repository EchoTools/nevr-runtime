/*
 * pnsnevr - Configuration Implementation
 */

#include "config.h"

#include <Windows.h>

#include <fstream>
#include <sstream>
#include <string>

// Simple JSON parsing helpers (minimal implementation for prototype)
// In production, use a proper JSON library like nlohmann/json

static std::string ReadEntireFile(const char* filepath) {
  std::ifstream file(filepath);
  if (!file.is_open()) {
    return "";
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

static std::string ExtractJsonString(const std::string& json, const std::string& key) {
  // Find "key": "value"
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) return "";

  pos = json.find(':', pos);
  if (pos == std::string::npos) return "";

  pos = json.find('"', pos);
  if (pos == std::string::npos) return "";

  size_t start = pos + 1;
  size_t end = json.find('"', start);
  if (end == std::string::npos) return "";

  return json.substr(start, end - start);
}

static int ExtractJsonInt(const std::string& json, const std::string& key, int default_val) {
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) return default_val;

  pos = json.find(':', pos);
  if (pos == std::string::npos) return default_val;

  // Skip whitespace
  while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t')) {
    pos++;
  }

  // Parse number
  std::string num_str;
  while (pos < json.size() && (json[pos] >= '0' && json[pos] <= '9')) {
    num_str += json[pos++];
  }

  if (num_str.empty()) return default_val;
  return std::stoi(num_str);
}

static bool ExtractJsonBool(const std::string& json, const std::string& key, bool default_val) {
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) return default_val;

  pos = json.find(':', pos);
  if (pos == std::string::npos) return default_val;

  // Skip whitespace
  while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t')) {
    pos++;
  }

  if (json.compare(pos, 4, "true") == 0) return true;
  if (json.compare(pos, 5, "false") == 0) return false;

  return default_val;
}

static std::string ExtractJsonObject(const std::string& json, const std::string& key) {
  std::string search = "\"" + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) return "";

  pos = json.find('{', pos);
  if (pos == std::string::npos) return "";

  // Find matching closing brace
  int depth = 1;
  size_t start = pos;
  pos++;

  while (pos < json.size() && depth > 0) {
    if (json[pos] == '{')
      depth++;
    else if (json[pos] == '}')
      depth--;
    pos++;
  }

  return json.substr(start, pos - start);
}

bool LoadNakamaConfig(void* game_config, NakamaConfig* out_config) {
  if (!out_config) return false;

  // Set defaults first
  *out_config = NakamaConfig{};

  if (!game_config) {
    OutputDebugStringA("[pnsnevr] No game config provided, using defaults\n");
    return true;
  }

  /*
   * TODO: Parse game config structure
   *
   * Based on Ghidra analysis, the game config is accessed via:
   *   config_get_string(config, "key", "default")
   *   config_get_int(config, "key", default)
   *   config_get_bool(config, "key", default)
   *
   * We need to find these functions and call them to extract our config.
   *
   * For now, fall back to file-based config.
   */

  // Try to load from nakama_config.json in game directory
  char exe_path[MAX_PATH];
  GetModuleFileNameA(NULL, exe_path, MAX_PATH);

  // Get directory
  char* last_slash = strrchr(exe_path, '\\');
  if (last_slash) *last_slash = '\0';

  std::string config_path = std::string(exe_path) + "\\nakama_config.json";

  if (LoadNakamaConfigFromFile(config_path.c_str(), out_config)) {
    char msg[512];
    sprintf_s(msg, "[pnsnevr] Loaded config from %s\n", config_path.c_str());
    OutputDebugStringA(msg);
    return true;
  }

  OutputDebugStringA("[pnsnevr] Using default configuration\n");
  return true;
}

bool LoadNakamaConfigFromFile(const char* filepath, NakamaConfig* out_config) {
  if (!out_config || !filepath) return false;

  std::string json = ReadEntireFile(filepath);
  if (json.empty()) {
    return false;
  }

  // Parse top-level values
  std::string endpoint = ExtractJsonString(json, "api_endpoint");
  if (!endpoint.empty()) {
    out_config->api_endpoint = endpoint;
  }

  out_config->http_port = ExtractJsonInt(json, "http_port", 7350);
  out_config->grpc_port = ExtractJsonInt(json, "grpc_port", 7349);

  std::string server_key = ExtractJsonString(json, "server_key");
  if (!server_key.empty()) {
    out_config->server_key = server_key;
  }

  std::string auth_method = ExtractJsonString(json, "auth_method");
  if (!auth_method.empty()) {
    out_config->auth_method = auth_method;
  }

  out_config->auto_create_user = ExtractJsonBool(json, "auto_create_user", true);
  out_config->max_retries = ExtractJsonInt(json, "max_retries", 3);
  out_config->retry_delay_ms = ExtractJsonInt(json, "retry_delay_ms", 1000);
  out_config->presence_poll_interval = ExtractJsonInt(json, "presence_poll_interval", 5000);
  out_config->matchmaking_poll_interval = ExtractJsonInt(json, "matchmaking_poll_interval", 1000);

  // Parse features object
  std::string features = ExtractJsonObject(json, "features");
  if (!features.empty()) {
    out_config->features.friends = ExtractJsonBool(features, "friends", true);
    out_config->features.parties = ExtractJsonBool(features, "parties", true);
    out_config->features.matchmaking = ExtractJsonBool(features, "matchmaking", true);
    out_config->features.leaderboards = ExtractJsonBool(features, "leaderboards", false);
    out_config->features.storage = ExtractJsonBool(features, "storage", false);
  }

  return true;
}

#include "json_override_hook.h"
#include <Windows.h>
#include <MinHook.h>
#include <nlohmann/json.hpp> // Assuming this is in the include path
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include <iostream>
#include <vector>

// Using nlohmann::json
using json = nlohmann::json;

// Pointers to the original functions
CJson_LoadFromPath_t o_CJson_LoadFromPath = nullptr;
CJson_Parse_t o_CJson_Parse = nullptr;

// Master override config
static json master_override_config;
static bool config_loaded = false;

// Helper to load the master config
void LoadMasterConfig() {
    if (config_loaded) return;

    const std::string config_path = "dbghooks/config/json_overrides.json";
    std::ifstream config_file(config_path);
    if (config_file.is_open()) {
        try {
            master_override_config = json::parse(config_file);
            config_loaded = true;
            std::cout << "[JSON Override] Master override config loaded from " << config_path << std::endl;
        } catch (const json::parse_error& e) {
            std::cerr << "[JSON Override] Error parsing master config: " << e.what() << std::endl;
        }
    } else {
        std::cerr << "[JSON Override] Could not open master config file: " << config_path << std::endl;
    }
}

// The detour function for CJson_LoadFromPath
int __fastcall Detour_CJson_LoadFromPath(void* this_ptr, const char* path, bool flag) {
    // Ensure master config is loaded
    LoadMasterConfig();

    if (!config_loaded || path == nullptr) {
        return o_CJson_LoadFromPath(this_ptr, path, flag);
    }

    std::string filename = std::filesystem::path(path).filename().string();

    // Check if an override exists for this filename
    if (master_override_config.find(filename) == master_override_config.end()) {
        // No override, call the original function
        return o_CJson_LoadFromPath(this_ptr, path, flag);
    }

    std::cout << "[JSON Override] Overriding file: " << filename << std::endl;

    // Override exists, we need to handle it manually.
    // 1. Read the original file content
    std::ifstream original_file(path);
    if (!original_file.is_open()) {
        std::cerr << "[JSON Override] Failed to open original file for override: " << path << std::endl;
        // Fallback to original function, which will likely also fail, but it's the game's behavior.
        return o_CJson_LoadFromPath(this_ptr, path, flag);
    }

    json original_json;
    try {
        original_json = json::parse(original_file);
    } catch (const json::parse_error& e) {
        std::cerr << "[JSON Override] Failed to parse original JSON file: " << path << " | " << e.what() << std::endl;
        return o_CJson_LoadFromPath(this_ptr, path, flag); // Fallback
    }

    // 2. Get the override data
    const json& override_data = master_override_config[filename];

    // 3. Merge the override into the original JSON
    original_json.merge_patch(override_data);

    // 4. Serialize the merged JSON to a string buffer
    std::string merged_json_str = original_json.dump();
    
    // The game's parser might need a mutable buffer.
    std::vector<char> buffer(merged_json_str.begin(), merged_json_str.end());
    buffer.push_back('\0'); // Null-terminate

    // 5. Call the game's internal CJson_Parse function
    return o_CJson_Parse(this_ptr, buffer.data(), buffer.size());
}

void InitializeJsonOverrideHook() {
    // Initialize MinHook
    if (MH_Initialize() != MH_OK) {
        std::cerr << "[JSON Override] MinHook initialization failed." << std::endl;
        return;
    }

    // Define target addresses
    LPVOID p_CJson_LoadFromPath_target = (LPVOID)0x1405f0990;
    LPVOID p_CJson_Parse_target = (LPVOID)0x1405f0bd0;

    // Store the original CJson_Parse function pointer
    o_CJson_Parse = (CJson_Parse_t)p_CJson_Parse_target;

    // Create and enable the hook for CJson_LoadFromPath
    if (MH_CreateHook(p_CJson_LoadFromPath_target, &Detour_CJson_LoadFromPath, reinterpret_cast<LPVOID*>(&o_CJson_LoadFromPath)) != MH_OK) {
        std::cerr << "[JSON Override] Failed to create hook for CJson_LoadFromPath." << std::endl;
        return;
    }

    if (MH_EnableHook(p_CJson_LoadFromPath_target) != MH_OK) {
        std::cerr << "[JSON Override] Failed to enable hook for CJson_LoadFromPath." << std::endl;
        return;
    }

    std::cout << "[JSON Override] Hook for CJson_LoadFromPath initialized and enabled." << std::endl;
}

void ShutdownJsonOverrideHook() {
    LPVOID p_CJson_LoadFromPath_target = (LPVOID)0x1405f0990;

    // Disable and remove the hook
    MH_DisableHook(p_CJson_LoadFromPath_target);
    MH_RemoveHook(p_CJson_LoadFromPath_target);

    // Uninitialize MinHook
    MH_Uninitialize();

    std::cout << "[JSON Override] Hook for CJson_LoadFromPath shut down." << std::endl;
}

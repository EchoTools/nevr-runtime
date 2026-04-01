#pragma once

#include <Windows.h>
#include <string>
#include <map>
#include <vector>
#include <nlohmann/json.hpp>

namespace dbghooks::weapon_system {

    // Hook function typedefs
    typedef void* (*GearTableLookupFn)(const char* weapon_name);

    // =========================================================================
    // Hook Addresses (EchoVR.exe - Build Jan 25 2026)
    // =========================================================================
    
    // Property reader function - reads weapon JSON properties from resources
    constexpr uintptr_t ADDR_PROPERTY_READER_RVA = 0x140784bb0;
    
    // Gear table base - weapon definitions
    constexpr uintptr_t ADDR_GEAR_TABLE_RVA = 0x141ca2d98;
    
    // Known component resource addresses (for reference/substitution)
    constexpr uintptr_t ADDR_GUNCR_COMPONENT_RVA = 0x142073180;   // Working assault rifle
    constexpr uintptr_t ADDR_GUN2CR_COMPONENT_RVA = 0x1420732c0;  // Broken (zero-init)

    // =========================================================================
    // Data Structures
    // =========================================================================

    // Weapon property structure matching binary format
    struct WeaponDamageTable {
        float head = 1.0f;
        float arm = 1.0f;
        float leg = 1.0f;
        float torso = 1.0f;
        float ordnance = 1.0f;
        float barrier = 1.0f;
        float ssi = 1.0f;
    };

    struct WeaponProperties {
        float heat_per_shot = 0.0f;
        float heat_per_second = 0.0f;
        float heat_cooldown_delay = 0.0f;
        float refire_delay = 0.0f;
        float heat_cooling_rate = 0.0f;
        float heat_overheat_venting_rate = 0.0f;
        float heat_venting_rate = 0.0f;
        float projectile_velocity = 0.0f;
        float projectile_damage = 0.0f;
        float fire_delay = 0.0f;
        float min_spread = 0.0f;
        float max_spread = 0.0f;
        float instability_max = 0.0f;
        float instability_per_shot = 0.0f;
        float instability_cooldown_delay = 0.0f;
        float instability_cooldown_rate = 0.0f;
        float hand_ori_prediction_when_firing = 1.0f;
        float hand_pos_prediction_when_firing = 1.0f;
        
        // Optional properties (weapon-specific)
        std::map<std::string, float> additional_properties;
    };

    struct WeaponSoundConfig {
        std::string impact_player;
        std::string impact_barrier;
        std::string impact_default;
        std::string whizzby_trail;
    };

    struct WeaponDefinition {
        std::string name;
        std::string display_name;
        std::vector<std::string> aliases;
        bool enabled = false;
        std::string internal_name;
        uint64_t component_hash = 0;
        std::string component_resource;
        
        WeaponDamageTable damage_table;
        WeaponProperties weapon_properties;
        WeaponSoundConfig sound_settings;
        
        // Additional variant damage tables (for AOE variants)
        std::map<std::string, WeaponDamageTable> variant_damage_tables;
        std::map<std::string, WeaponProperties> variant_properties;
    };

    // Global weapon configuration store
    class WeaponConfigManager {
    public:
        static WeaponConfigManager& Instance();
        
        // Load configuration from JSON file
        bool LoadConfigFromFile(const char* config_path);
        
        // Get weapon definition by name
        const WeaponDefinition* GetWeaponByName(const std::string& name) const;
        
        // Get all weapons
        const std::map<std::string, WeaponDefinition>& GetAllWeapons() const;
        
        // Check if weapon is enabled
        bool IsWeaponEnabled(const std::string& name) const;
        
        // Get damage value for a weapon and hit location
        float GetDamageMultiplier(const std::string& weapon_name, const std::string& location) const;
        
        // Get property value
        float GetWeaponProperty(const std::string& weapon_name, const std::string& property_name) const;
        
        // Get sound config
        const WeaponSoundConfig* GetSoundConfig(const std::string& weapon_name) const;
        
        // Get weapon count
        size_t GetWeaponCount() const { return weapons_.size(); }
        size_t GetEnabledWeaponCount() const;
        
    private:
        WeaponConfigManager() = default;
        
        std::map<std::string, WeaponDefinition> weapons_;
        bool is_loaded_ = false;
        
        // Helper methods for JSON parsing
        void ParseWeaponFromJson(const std::string& name, const nlohmann::json& weapon_json);
        WeaponDamageTable ParseDamageTable(const nlohmann::json& damage_json);
        WeaponProperties ParseWeaponProperties(const nlohmann::json& props_json);
        WeaponSoundConfig ParseSoundConfig(const nlohmann::json& sound_json);
    };

    // Hook interface for weapon system interception
    class WeaponSystemHook {
    public:
        // Initialize hook system
        static bool Initialize();
        
        // Shut down hooks
        static bool Shutdown();
        
        // Hook targets (these are called by the main code via detours)
        
        // Intercept gear table lookup
        static const WeaponDefinition* OnWeaponLookup(const char* weapon_name);
        
        // Intercept damage table lookup
        static float OnDamageLookup(const char* weapon_name, const char* location);
        
        // Intercept property lookup
        static float OnPropertyLookup(const char* weapon_name, const char* property_name);
        
        // Intercept sound lookup
        static const char* OnSoundLookup(const char* weapon_name, const char* sound_type);
        
        // Make detour functions friends so they can access private members
        friend void* HOOK_GearTableLookup(const char* weapon_name);
        
     private:
         static bool is_initialized_;
         static WeaponConfigManager* config_manager_;
         
         // Original function pointers (trampolines)
         static GearTableLookupFn original_gear_table_lookup_;
     };

    // Forward declaration for detour function
    void* HOOK_GearTableLookup(const char* weapon_name);

 }  // namespace dbghooks::weapon_system

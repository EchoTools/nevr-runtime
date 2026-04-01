#include "weapon_enabler.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include "../../common/hooking.h"

namespace dbghooks::weapon_system {

    // Static member initialization
    bool WeaponSystemHook::is_initialized_ = false;
    WeaponConfigManager* WeaponSystemHook::config_manager_ = nullptr;
    GearTableLookupFn WeaponSystemHook::original_gear_table_lookup_ = nullptr;

    // WeaponConfigManager singleton implementation
    WeaponConfigManager& WeaponConfigManager::Instance() {
        static WeaponConfigManager instance;
        return instance;
    }

    bool WeaponConfigManager::LoadConfigFromFile(const char* config_path) {
        if (!config_path) {
            printf("[WeaponEnabler] ERROR: config_path is NULL\n");
            return false;
        }

        std::ifstream file(config_path);
        if (!file.is_open()) {
            printf("[WeaponEnabler] ERROR: Failed to open config file: %s\n", config_path);
            return false;
        }

        nlohmann::json root;
        try {
            root = nlohmann::json::parse(file);
        } catch (const nlohmann::json::parse_error& e) {
            printf("[WeaponEnabler] ERROR: JSON parse error: %s\n", e.what());
            return false;
        }

        // Parse weapons section
        if (!root.contains("weapons") || !root["weapons"].is_object()) {
            printf("[WeaponEnabler] ERROR: No 'weapons' object in config\n");
            return false;
        }

        const auto& weapons_obj = root["weapons"];
        for (const auto& [name, value] : weapons_obj.items()) {
            ParseWeaponFromJson(name, value);
        }

        is_loaded_ = true;
        printf("[WeaponEnabler] Config loaded: %zu weapons\n", weapons_.size());
        return true;
    }

    void WeaponConfigManager::ParseWeaponFromJson(const std::string& name, const nlohmann::json& weapon_json) {
        WeaponDefinition weapon;
        weapon.name = name;

        // Parse basic properties
        if (weapon_json.contains("display_name")) {
            weapon.display_name = weapon_json["display_name"].get<std::string>();
        }

        if (weapon_json.contains("enabled")) {
            weapon.enabled = weapon_json["enabled"].get<bool>();
        }

        if (weapon_json.contains("internal_name")) {
            weapon.internal_name = weapon_json["internal_name"].get<std::string>();
        }

        // Parse aliases
        if (weapon_json.contains("aliases") && weapon_json["aliases"].is_array()) {
            for (const auto& alias : weapon_json["aliases"]) {
                weapon.aliases.push_back(alias.get<std::string>());
            }
        }

        // Parse damage table
        if (weapon_json.contains("damage_table")) {
            weapon.damage_table = ParseDamageTable(weapon_json["damage_table"]);
        }

        // Parse weapon properties
        if (weapon_json.contains("weapon_properties")) {
            weapon.weapon_properties = ParseWeaponProperties(weapon_json["weapon_properties"]);
        }

        // Parse sound settings
        if (weapon_json.contains("sound_settings")) {
            weapon.sound_settings = ParseSoundConfig(weapon_json["sound_settings"]);
        }

        // Parse variant damage tables
        if (weapon_json.contains("scout_aoe_damage")) {
            weapon.variant_damage_tables["aoe"] = ParseDamageTable(weapon_json["scout_aoe_damage"]);
        }
        if (weapon_json.contains("blaster_cone_damage")) {
            weapon.variant_damage_tables["cone"] = ParseDamageTable(weapon_json["blaster_cone_damage"]);
        }
        if (weapon_json.contains("rocket_aoe_damage")) {
            weapon.variant_damage_tables["aoe"] = ParseDamageTable(weapon_json["rocket_aoe_damage"]);
        }
        if (weapon_json.contains("magnum_aoe_damage")) {
            weapon.variant_damage_tables["aoe"] = ParseDamageTable(weapon_json["magnum_aoe_damage"]);
        }

        weapons_[name] = weapon;

        if (weapon.enabled) {
            printf("[WeaponEnabler] ✓ Loaded weapon: %s (ENABLED)\n", name.c_str());
        } else {
            printf("[WeaponEnabler] ✗ Loaded weapon: %s (disabled)\n", name.c_str());
        }
    }

    WeaponDamageTable WeaponConfigManager::ParseDamageTable(const nlohmann::json& damage_json) {
        WeaponDamageTable table;

        if (damage_json.contains("head")) table.head = damage_json["head"].get<float>();
        if (damage_json.contains("arm")) table.arm = damage_json["arm"].get<float>();
        if (damage_json.contains("leg")) table.leg = damage_json["leg"].get<float>();
        if (damage_json.contains("torso")) table.torso = damage_json["torso"].get<float>();
        if (damage_json.contains("ordnance")) table.ordnance = damage_json["ordnance"].get<float>();
        if (damage_json.contains("barrier")) table.barrier = damage_json["barrier"].get<float>();
        if (damage_json.contains("ssi")) table.ssi = damage_json["ssi"].get<float>();

        return table;
    }

    WeaponProperties WeaponConfigManager::ParseWeaponProperties(const nlohmann::json& props_json) {
        WeaponProperties props;

        #define PARSE_PROPERTY(field) \
            if (props_json.contains(#field)) { \
                props.field = props_json[#field].get<float>(); \
            }

        PARSE_PROPERTY(heat_per_shot)
        PARSE_PROPERTY(heat_per_second)
        PARSE_PROPERTY(heat_cooldown_delay)
        PARSE_PROPERTY(refire_delay)
        PARSE_PROPERTY(heat_cooling_rate)
        PARSE_PROPERTY(heat_overheat_venting_rate)
        PARSE_PROPERTY(heat_venting_rate)
        PARSE_PROPERTY(projectile_velocity)
        PARSE_PROPERTY(projectile_damage)
        PARSE_PROPERTY(fire_delay)
        PARSE_PROPERTY(min_spread)
        PARSE_PROPERTY(max_spread)
        PARSE_PROPERTY(instability_max)
        PARSE_PROPERTY(instability_per_shot)
        PARSE_PROPERTY(instability_cooldown_delay)
        PARSE_PROPERTY(instability_cooldown_rate)
        PARSE_PROPERTY(hand_ori_prediction_when_firing)
        PARSE_PROPERTY(hand_pos_prediction_when_firing)

        #undef PARSE_PROPERTY

        // Parse any additional properties
        for (const auto& [key, value] : props_json.items()) {
            // Skip if already parsed
            if (key.find("_when_") != std::string::npos) continue;
            if (key.find("heat_") == 0) continue;
            if (key.find("instability_") == 0) continue;
            if (key.find("projectile_") == 0) continue;
            if (key.find("fire_") == 0) continue;
            if (key.find("min_") == 0 || key.find("max_") == 0) continue;
            if (key.find("refire_") == 0) continue;

            props.additional_properties[key] = value.get<float>();
        }

        return props;
    }

    WeaponSoundConfig WeaponConfigManager::ParseSoundConfig(const nlohmann::json& sound_json) {
        WeaponSoundConfig config;

        if (sound_json.contains("bullet_impact")) {
            const auto& impact = sound_json["bullet_impact"];
            if (impact.contains("player")) config.impact_player = impact["player"].get<std::string>();
            if (impact.contains("barrier")) config.impact_barrier = impact["barrier"].get<std::string>();
            if (impact.contains("default")) config.impact_default = impact["default"].get<std::string>();
        }

        if (sound_json.contains("bullet_whizzby")) {
            config.whizzby_trail = sound_json["bullet_whizzby"].get<std::string>();
        }

        return config;
    }

    const WeaponDefinition* WeaponConfigManager::GetWeaponByName(const std::string& name) const {
        auto it = weapons_.find(name);
        if (it != weapons_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    const std::map<std::string, WeaponDefinition>& WeaponConfigManager::GetAllWeapons() const {
        return weapons_;
    }

    bool WeaponConfigManager::IsWeaponEnabled(const std::string& name) const {
        const auto* weapon = GetWeaponByName(name);
        return weapon && weapon->enabled;
    }

    float WeaponConfigManager::GetDamageMultiplier(const std::string& weapon_name, const std::string& location) const {
        const auto* weapon = GetWeaponByName(weapon_name);
        if (!weapon) return 1.0f;

        const std::string location_lower = [&location]() {
            std::string result = location;
            std::transform(result.begin(), result.end(), result.begin(),
                          [](unsigned char c) { return std::tolower(c); });
            return result;
        }();

        if (location_lower == "head") return weapon->damage_table.head;
        if (location_lower == "arm") return weapon->damage_table.arm;
        if (location_lower == "leg") return weapon->damage_table.leg;
        if (location_lower == "torso") return weapon->damage_table.torso;
        if (location_lower == "ordnance") return weapon->damage_table.ordnance;
        if (location_lower == "barrier") return weapon->damage_table.barrier;
        if (location_lower == "ssi") return weapon->damage_table.ssi;

        return 1.0f;
    }

    float WeaponConfigManager::GetWeaponProperty(const std::string& weapon_name, const std::string& property_name) const {
        const auto* weapon = GetWeaponByName(weapon_name);
        if (!weapon) return 0.0f;

        const auto& props = weapon->weapon_properties;

        #define CHECK_PROPERTY(field) \
            if (property_name == #field) return props.field;

        CHECK_PROPERTY(heat_per_shot)
        CHECK_PROPERTY(heat_per_second)
        CHECK_PROPERTY(heat_cooldown_delay)
        CHECK_PROPERTY(refire_delay)
        CHECK_PROPERTY(heat_cooling_rate)
        CHECK_PROPERTY(heat_overheat_venting_rate)
        CHECK_PROPERTY(heat_venting_rate)
        CHECK_PROPERTY(projectile_velocity)
        CHECK_PROPERTY(projectile_damage)
        CHECK_PROPERTY(fire_delay)
        CHECK_PROPERTY(min_spread)
        CHECK_PROPERTY(max_spread)
        CHECK_PROPERTY(instability_max)
        CHECK_PROPERTY(instability_per_shot)
        CHECK_PROPERTY(instability_cooldown_delay)
        CHECK_PROPERTY(instability_cooldown_rate)
        CHECK_PROPERTY(hand_ori_prediction_when_firing)
        CHECK_PROPERTY(hand_pos_prediction_when_firing)

        #undef CHECK_PROPERTY

        // Check additional properties
        auto it = props.additional_properties.find(property_name);
        if (it != props.additional_properties.end()) {
            return it->second;
        }

        return 0.0f;
    }

    const WeaponSoundConfig* WeaponConfigManager::GetSoundConfig(const std::string& weapon_name) const {
        const auto* weapon = GetWeaponByName(weapon_name);
        if (weapon) {
            return &weapon->sound_settings;
        }
        return nullptr;
    }

    size_t WeaponConfigManager::GetEnabledWeaponCount() const {
        size_t count = 0;
        for (const auto& [name, weapon] : weapons_) {
            if (weapon.enabled) count++;
        }
        return count;
    }

    // Hook system implementation
    bool WeaponSystemHook::Initialize() {
        if (is_initialized_) {
            printf("[WeaponEnabler] Hook system already initialized\n");
            return true;
        }

        config_manager_ = &WeaponConfigManager::Instance();

        // Try to load config from dbghooks directory
        const char* config_paths[] = {
            "dbghooks/gun2cr_fix/weapon_config.json",
            "../dbghooks/gun2cr_fix/weapon_config.json",
            "gun2cr_fix/weapon_config.json",
            "weapon_config.json",
        };

        bool loaded = false;
        for (const char* path : config_paths) {
            if (config_manager_->LoadConfigFromFile(path)) {
                printf("[WeaponEnabler] ✓ Config loaded from: %s\n", path);
                loaded = true;
                break;
            }
        }

        if (!loaded) {
            printf("[WeaponEnabler] WARNING: Could not load weapon_config.json\n");
            printf("[WeaponEnabler] Weapon system operating without config\n");
        }

        // Initialize hooking library
        Hooking::Initialize();
        
        // Install gear table hook (STUB - address TBD)
        // For now, just log that we're ready
        printf("[WeaponEnabler] ✓ Hook system ready (hooks TBD)\n");
        printf("[WeaponEnabler] ✓ Chain weapon config loaded: %s\n", 
               config_manager_->IsWeaponEnabled("chain") ? "YES" : "NO");
        
        is_initialized_ = true;
        printf("[WeaponEnabler] Hook system initialized (enabled weapons: %zu)\n",
               config_manager_->GetEnabledWeaponCount());
        return true;
    }

    bool WeaponSystemHook::Shutdown() {
        is_initialized_ = false;
        config_manager_ = nullptr;
        printf("[WeaponEnabler] Hook system shut down\n");
        return true;
    }

    const WeaponDefinition* WeaponSystemHook::OnWeaponLookup(const char* weapon_name) {
        if (!is_initialized_ || !config_manager_) {
            return nullptr;
        }
        
        if (!weapon_name) {
            return nullptr;
        }

        const auto* weapon = config_manager_->GetWeaponByName(weapon_name);
        if (weapon && weapon->enabled) {
            printf("[WeaponEnabler] Weapon lookup: %s -> FOUND (enabled)\n", weapon_name);
            return weapon;
        }

        return nullptr;
    }

    float WeaponSystemHook::OnDamageLookup(const char* weapon_name, const char* location) {
        if (!is_initialized_ || !config_manager_ || !weapon_name || !location) {
            return 1.0f;  // Default multiplier
        }

        return config_manager_->GetDamageMultiplier(weapon_name, location);
    }

    float WeaponSystemHook::OnPropertyLookup(const char* weapon_name, const char* property_name) {
        if (!is_initialized_ || !config_manager_ || !weapon_name || !property_name) {
            return 0.0f;
        }

        return config_manager_->GetWeaponProperty(weapon_name, property_name);
    }

    const char* WeaponSystemHook::OnSoundLookup(const char* weapon_name, const char* sound_type) {
        if (!is_initialized_ || !config_manager_ || !weapon_name || !sound_type) {
            return nullptr;
        }

        const auto* config = config_manager_->GetSoundConfig(weapon_name);
        if (!config) {
            return nullptr;
        }

        const std::string type_lower = [&sound_type]() {
            std::string result(sound_type);
            std::transform(result.begin(), result.end(), result.begin(),
                          [](unsigned char c) { return std::tolower(c); });
            return result;
        }();

        if (type_lower.find("impact_player") != std::string::npos) {
            return config->impact_player.c_str();
        }
        if (type_lower.find("impact_barrier") != std::string::npos) {
            return config->impact_barrier.c_str();
        }
        if (type_lower.find("impact") != std::string::npos) {
            return config->impact_default.c_str();
        }
        if (type_lower.find("whizzby") != std::string::npos) {
            return config->whizzby_trail.c_str();
        }

        return nullptr;
    }

    // Detour for gear table lookup - makes Chain weapon available
    void* HOOK_GearTableLookup(const char* weapon_name) {
        // Call original
        void* result = WeaponSystemHook::original_gear_table_lookup_(weapon_name);
        
        // If looking up "chain" and it's enabled in config, return assault as substitute
        if (weapon_name && strcmp(weapon_name, "chain") == 0) {
            if (WeaponSystemHook::is_initialized_ && 
                WeaponSystemHook::config_manager_ &&
                WeaponSystemHook::config_manager_->IsWeaponEnabled("chain")) {
                
                // Return assault rifle component as substitute
                printf("[WeaponEnabler] ✓ Substituting assault for chain weapon\n");
                return WeaponSystemHook::original_gear_table_lookup_("assault");
            }
        }
        
        return result;
    }

}  // namespace dbghooks::weapon_system

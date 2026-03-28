/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "address_registry.h"

/* Re-export address constants from the central registry for convenience */
using nevr::addresses::VA_BALANCE_CONFIG;
using nevr::addresses::VA_RESOLVE_CONFIG_HASH;
using nevr::addresses::VA_CAN_PLAYER_TAKE_DAMAGE;

/* Image base for the PC binary (matches nevr::ResolveVA) */
constexpr uint64_t IMAGE_BASE = 0x140000000;

/*
 * Balance config struct layout (from docs/arena-combat-plugin-reference.md).
 *
 * The struct is 0x168 bytes, heap-allocated. The global at VA_BALANCE_CONFIG
 * is a POINTER that must be dereferenced to get the actual struct.
 */
#pragma pack(push, 1)
struct SBalanceConfig {
    /* Frisbee/Arena balance fields (from Inspect @ 0x14013f9c0) */
    uint8_t use_grab_bubble;           // +0x00: grab bubble mechanic toggle
    uint8_t _pad01[3];                 // +0x01: alignment padding
    float   grab_range;                // +0x04: disc grab distance
    float   possession_time;           // +0x08: possession timer
    float   clear_throw_distance;      // +0x0C: clear throw range
    float   clear_distance_from_goal;  // +0x10: clear distance from home goal
    float   interception_min_distance; // +0x14: min interception range
    float   aim_assist_min_angle;      // +0x18: aim assist min angle
    float   aim_assist_max_angle;      // +0x1C: aim assist max angle
    float   aim_assist_target_half;    // +0x20: target selection cone half-angle
    float   aim_assist_min_strength;   // +0x24: aim strength at min angle
    float   aim_assist_max_strength;   // +0x28: aim strength at max angle

    uint8_t _pad2C[0x10];              // +0x2C..+0x3B: unknown fields

    /* Combat-critical fields */
    float   max_stun_duration;         // +0x3C: max stun duration (GetStunFraction)
    float   max_health;                // +0x40: max health (CanPlayerTakeDamage)
                                       //   CRITICAL: 0.0 in Arena = no damage
                                       //             >0 enables combat damage

    uint8_t _remainder[0x168 - 0x44];  // +0x44..+0x167: remaining fields
};
#pragma pack(pop)

static_assert(sizeof(SBalanceConfig) == 0x168);
static_assert(offsetof(SBalanceConfig, use_grab_bubble) == 0x00);
static_assert(offsetof(SBalanceConfig, grab_range) == 0x04);
static_assert(offsetof(SBalanceConfig, possession_time) == 0x08);
static_assert(offsetof(SBalanceConfig, clear_throw_distance) == 0x0C);
static_assert(offsetof(SBalanceConfig, clear_distance_from_goal) == 0x10);
static_assert(offsetof(SBalanceConfig, interception_min_distance) == 0x14);
static_assert(offsetof(SBalanceConfig, aim_assist_min_angle) == 0x18);
static_assert(offsetof(SBalanceConfig, aim_assist_max_angle) == 0x1C);
static_assert(offsetof(SBalanceConfig, aim_assist_target_half) == 0x20);
static_assert(offsetof(SBalanceConfig, aim_assist_min_strength) == 0x24);
static_assert(offsetof(SBalanceConfig, aim_assist_max_strength) == 0x28);
static_assert(offsetof(SBalanceConfig, max_stun_duration) == 0x3C);
static_assert(offsetof(SBalanceConfig, max_health) == 0x40);

/* Validation limits */
constexpr float MAX_ALLOWED_STUN_DURATION = 60.0f;

/*
 * Parsed override config. Fields use negative sentinels to indicate
 * "not set" (balances are always non-negative).
 */
struct GameRulesOverrideConfig {
    float max_health         = -1.0f;
    float max_stun_duration  = -1.0f;

    /* Frisbee balance overrides */
    float grab_range                = -1.0f;
    float possession_time           = -1.0f;
    float clear_throw_distance      = -1.0f;
    float clear_distance_from_goal  = -1.0f;
    float interception_min_distance = -1.0f;
    float aim_assist_min_angle      = -1.0f;
    float aim_assist_max_angle      = -1.0f;
    float aim_assist_target_half    = -1.0f;
    float aim_assist_min_strength   = -1.0f;
    float aim_assist_max_strength   = -1.0f;

    /* Arena timing overrides (applied via CJsonGetFloat hook, not balance struct) */
    float round_time                       = -1.0f;
    float point_score_celebration_time     = -1.0f;
    float mercy_win_point_spread           = -1.0f;

    bool  valid = false;
};

/*
 * LoadConfig - Parse a JSONC config file into a GameRulesOverrideConfig.
 * Returns a config with valid=false on error.
 */
GameRulesOverrideConfig LoadConfig(const char* path);

/*
 * ParseConfigString - Parse a JSON/JSONC string into a config.
 * Exposed for testing.
 */
GameRulesOverrideConfig ParseConfigString(const std::string& json_str);

/*
 * ApplyOverrides - Resolve g_balance_config pointer and write override values.
 * Returns true on success.
 */
bool ApplyOverrides(uintptr_t base_addr, const GameRulesOverrideConfig& config);

/*
 * VerifyAddresses - Check first bytes at each target VA to confirm
 * we are patching the right binary. Returns true if all checks pass.
 */
bool VerifyAddresses(uintptr_t base_addr);

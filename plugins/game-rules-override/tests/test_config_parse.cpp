/* SYNTHESIS -- custom tool code, not from binary */

#include <gtest/gtest.h>
#include "game_rules_override.h"

/* ----------------------------------------------------------------
 * VA constant sanity checks
 * ---------------------------------------------------------------- */

TEST(AddressConstants, AllVAsAreValid) {
    /* Each VA must be above IMAGE_BASE (i.e. the RVA is positive) */
    EXPECT_GT(VA_BALANCE_CONFIG - IMAGE_BASE, static_cast<uint64_t>(0));
    EXPECT_GT(VA_RESOLVE_CONFIG_HASH - IMAGE_BASE, static_cast<uint64_t>(0));
    EXPECT_GT(VA_CAN_PLAYER_TAKE_DAMAGE - IMAGE_BASE, static_cast<uint64_t>(0));
}

/* ----------------------------------------------------------------
 * Balance config struct offset checks
 * ---------------------------------------------------------------- */

TEST(BalanceConfigStruct, SizeIs0x168) {
    EXPECT_EQ(sizeof(SBalanceConfig), 0x168u);
}

TEST(BalanceConfigStruct, FieldOffsets) {
    EXPECT_EQ(offsetof(SBalanceConfig, use_grab_bubble), 0x00u);
    EXPECT_EQ(offsetof(SBalanceConfig, grab_range), 0x04u);
    EXPECT_EQ(offsetof(SBalanceConfig, possession_time), 0x08u);
    EXPECT_EQ(offsetof(SBalanceConfig, clear_throw_distance), 0x0Cu);
    EXPECT_EQ(offsetof(SBalanceConfig, clear_distance_from_goal), 0x10u);
    EXPECT_EQ(offsetof(SBalanceConfig, interception_min_distance), 0x14u);
    EXPECT_EQ(offsetof(SBalanceConfig, aim_assist_min_angle), 0x18u);
    EXPECT_EQ(offsetof(SBalanceConfig, aim_assist_max_angle), 0x1Cu);
    EXPECT_EQ(offsetof(SBalanceConfig, aim_assist_target_half), 0x20u);
    EXPECT_EQ(offsetof(SBalanceConfig, aim_assist_min_strength), 0x24u);
    EXPECT_EQ(offsetof(SBalanceConfig, aim_assist_max_strength), 0x28u);
    EXPECT_EQ(offsetof(SBalanceConfig, max_stun_duration), 0x3Cu);
    EXPECT_EQ(offsetof(SBalanceConfig, max_health), 0x40u);
}

/* ----------------------------------------------------------------
 * Config parsing: valid config
 * ---------------------------------------------------------------- */

TEST(ConfigParse, ValidConfigParsesCorrectly) {
    const char* json = R"({
        "max_health": 100.0,
        "max_stun_duration": 3.0,
        "grab_range": 2.5
    })";
    auto config = ParseConfigString(json);
    EXPECT_TRUE(config.valid);
    EXPECT_FLOAT_EQ(config.max_health, 100.0f);
    EXPECT_FLOAT_EQ(config.max_stun_duration, 3.0f);
    EXPECT_FLOAT_EQ(config.grab_range, 2.5f);
}

/* ----------------------------------------------------------------
 * Config parsing: missing optional fields get defaults
 * ---------------------------------------------------------------- */

TEST(ConfigParse, MissingOptionalFieldsGetDefaults) {
    const char* json = R"({
        "max_health": 50.0
    })";
    auto config = ParseConfigString(json);
    EXPECT_TRUE(config.valid);
    EXPECT_FLOAT_EQ(config.max_health, 50.0f);
    /* All unset fields should remain at sentinel -1.0 */
    EXPECT_FLOAT_EQ(config.max_stun_duration, -1.0f);
    EXPECT_FLOAT_EQ(config.grab_range, -1.0f);
    EXPECT_FLOAT_EQ(config.possession_time, -1.0f);
    EXPECT_FLOAT_EQ(config.clear_throw_distance, -1.0f);
    EXPECT_FLOAT_EQ(config.clear_distance_from_goal, -1.0f);
    EXPECT_FLOAT_EQ(config.interception_min_distance, -1.0f);
    EXPECT_FLOAT_EQ(config.aim_assist_min_angle, -1.0f);
    EXPECT_FLOAT_EQ(config.aim_assist_max_angle, -1.0f);
    EXPECT_FLOAT_EQ(config.aim_assist_target_half, -1.0f);
    EXPECT_FLOAT_EQ(config.aim_assist_min_strength, -1.0f);
    EXPECT_FLOAT_EQ(config.aim_assist_max_strength, -1.0f);
}

/* ----------------------------------------------------------------
 * Config parsing: invalid JSON returns error
 * ---------------------------------------------------------------- */

TEST(ConfigParse, InvalidJsonReturnsError) {
    /* Not JSON at all */
    auto config1 = ParseConfigString("this is not json");
    EXPECT_FALSE(config1.valid);

    /* Empty string */
    auto config2 = ParseConfigString("");
    EXPECT_FALSE(config2.valid);
}

/* ----------------------------------------------------------------
 * Config parsing: JSONC comments are stripped
 * ---------------------------------------------------------------- */

TEST(ConfigParse, JsoncCommentsAreStripped) {
    const char* jsonc = R"({
        // This is a comment
        "max_health": 75.0,
        /* Block comment */
        "max_stun_duration": 2.0
    })";
    auto config = ParseConfigString(jsonc);
    EXPECT_TRUE(config.valid);
    EXPECT_FLOAT_EQ(config.max_health, 75.0f);
    EXPECT_FLOAT_EQ(config.max_stun_duration, 2.0f);
}

/* ----------------------------------------------------------------
 * Config validation: reject stun > 60s
 * ---------------------------------------------------------------- */

TEST(ConfigParse, RejectExcessiveStunDuration) {
    const char* json = R"({
        "max_stun_duration": 120.0
    })";
    auto config = ParseConfigString(json);
    EXPECT_FALSE(config.valid);
}

/* ----------------------------------------------------------------
 * Config parsing: empty object is valid (no overrides)
 * ---------------------------------------------------------------- */

TEST(ConfigParse, EmptyObjectIsValid) {
    auto config = ParseConfigString("{}");
    EXPECT_TRUE(config.valid);
}

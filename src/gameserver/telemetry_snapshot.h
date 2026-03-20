#pragma once

#include <cstdint>
#include <cstring>

// Game state snapshot captured on game thread, read by telemetry thread.
// Two instances form a double-buffer for lock-free producer/consumer.
struct TelemetrySnapshot {
  // --- Game function results (game thread only) ---
  float gameClock;
  uint64_t gameStatusHash;  // raw symbol hash from property lookup
  int32_t bluePoints;
  int32_t orangePoints;

  // Per-player transforms (from entity system, game thread)
  struct PlayerTransform {
    float bodyPos[3], bodyRot[4];
    float headPos[3], headRot[4];
    float leftHandPos[3], leftHandRot[4];
    float rightHandPos[3], rightHandRot[4];
    float velocity[3];
  } players[16];

  // Per-player bone data (from entity/animation system, game thread)
  struct PlayerBoneData {
    float bones[23][7];  // 23 bones x (4 orient + 3 translate)
    bool valid;
  } playerBones[16];

  // --- Direct memory reads ---

  struct PlayerInfo {
    uint16_t flags;
    uint64_t accountId;
    char displayName[36];
    uint16_t ping;
    uint16_t teamIndex;
  } playerInfo[16];
  uint16_t playerCount;

  // Disc state (direct read from GameStateData)
  float discPos[3], discVel[3], discOrient[4];
  uint64_t discBounceCount;

  // Last score data (direct read from GameStateData)
  struct LastScore {
    int16_t team;
    uint16_t pointAmount;
    float discSpeed;
    float distanceThrown;
    uint32_t assistPacked;
    uint32_t scorerPacked;
  } lastScore;

  // Per-player stats (direct read)
  struct PlayerStats {
    uint32_t points, goals, assists, stuns, blocks, saves;
    uint32_t interceptions, steals, catches, passes, shotsTaken;
    uint32_t possessionTime;
  } stats[16];

  void Clear() { std::memset(this, 0, sizeof(*this)); }
};

// Memory layout constants (verified via RE)
namespace GameOffsets {

constexpr uint64_t GAME_CONTEXT_OFFSET = 0x20a0478;
constexpr uint64_t NETGAME_OFFSET = 0x8518;
constexpr uint64_t GAME_STATE_DATA_OFFSET = 0x2AA8;

// Player array: CR15NetGame + 0x178, stride 0x250
constexpr uint64_t PLAYER_ARRAY_OFFSET = 0x178;
constexpr uint64_t PLAYER_STRIDE = 0x250;
constexpr uint64_t PLAYER_FLAGS_OFFSET = 0x00;
constexpr uint64_t PLAYER_ACCOUNT_ID_OFFSET = 0x08;
constexpr uint64_t PLAYER_DISPLAY_NAME_OFFSET = 0x3C;
constexpr uint64_t PLAYER_PING_OFFSET = 0x8A;
constexpr uint64_t PLAYER_TEAM_INDEX_OFFSET = 0x8E;
constexpr uint64_t PLAYER_COUNT_OFFSET = 0xE2;

// Player stats: CR15NetGame + 0x72AC + teamRole * 0x4D08
constexpr uint64_t STATS_BASE_OFFSET = 0x72AC;
constexpr uint64_t STATS_STRIDE = 0x4D08;
// Each stat entry: { uint32_t type, uint32_t count, double value } = 16 bytes
constexpr uint64_t STAT_POINTS = 0x000;
constexpr uint64_t STAT_GOALS = 0x010;
constexpr uint64_t STAT_ASSISTS = 0x020;
constexpr uint64_t STAT_STUNS = 0x030;
constexpr uint64_t STAT_BLOCKS = 0x040;
constexpr uint64_t STAT_SAVES = 0x050;
constexpr uint64_t STAT_INTERCEPTIONS = 0x060;
constexpr uint64_t STAT_STEALS = 0x070;
constexpr uint64_t STAT_CATCHES = 0x080;
constexpr uint64_t STAT_PASSES = 0x090;
constexpr uint64_t STAT_SHOTS_TAKEN = 0x0A0;
constexpr uint64_t STAT_POSSESSION_TIME = 0x300;

// Disc: GameStateData offsets
constexpr uint64_t DISC_ORIENT_OFFSET = 0xE4;   // float[4] quaternion
constexpr uint64_t DISC_POS_OFFSET = 0xF4;      // float[3]
constexpr uint64_t DISC_VEL_OFFSET = 0x104;     // float[3]
constexpr uint64_t DISC_BOUNCE_OFFSET = 0x158;  // uint64_t

// Last score: GameStateData offsets
constexpr uint64_t LAST_SCORE_TEAM_OFFSET = 0x10;
constexpr uint64_t LAST_SCORE_POINT_AMOUNT_OFFSET = 0x12;
constexpr uint64_t LAST_SCORE_DISC_SPEED_OFFSET = 0x48;
constexpr uint64_t LAST_SCORE_DISTANCE_THROWN_OFFSET = 0x50;
constexpr uint64_t LAST_SCORE_ASSIST_PACKED_OFFSET = 0x68;
constexpr uint64_t LAST_SCORE_SCORER_PACKED_OFFSET = 0x6C;

}  // namespace GameOffsets

// Game function pointer typedefs (cast known VAs to these)
// FUN_140d5ced0(CR15NetGame, hash) → uint64_t (game_status symbol hash)
using GetSymbolHashFn = uint64_t(__fastcall*)(void* netGame, uint64_t propertyHash);

// FUN_140d36750(CR15NetGame, hash) → float (game_clock)
using GetFloatPropertyFn = float(__fastcall*)(void* netGame, uint64_t propertyHash);

// FUN_140d05730(GameStateData, hash) → int32_t (blue/orange points)
using GetIntPropertyFn = int32_t(__fastcall*)(void* gameStateData, uint64_t propertyHash);

// FUN_1404f37a0(transform_ptr) → entity*
using GetEntityFromTransformFn = void*(__fastcall*)(void* transformPtr);

// FUN_1408f8090(entity, boneSlot) → bone data ptr (7 floats: 4 orient + 3 translate)
using GetBoneDataFn = float*(__fastcall*)(void* entity, uint32_t boneSlot);

// FUN_1408f8080(entity) → uint32_t bone count
using GetBoneCountFn = uint32_t(__fastcall*)(void* entity);

// Property hashes for game function calls
namespace PropertyHash {
constexpr uint64_t GAME_STATUS = 0xB29936AD399FC0FF;
constexpr uint64_t GAME_CLOCK = 0xB29936D484F1D1E8;
constexpr uint64_t BLUE_POINTS = 0x1C97052A21E09F11;
constexpr uint64_t ORANGE_POINTS = 0x2ED07B1C8C27548D;
}  // namespace PropertyHash

// Game function virtual addresses (offsets from base)
namespace GameFuncAddr {
constexpr uint64_t GET_SYMBOL_HASH = 0xd5ced0;    // FUN_140d5ced0
constexpr uint64_t GET_FLOAT_PROPERTY = 0xd36750;  // FUN_140d36750
constexpr uint64_t GET_INT_PROPERTY = 0xd05730;    // FUN_140d05730
constexpr uint64_t GET_ENTITY_FROM_TRANSFORM = 0x4f37a0;  // FUN_1404f37a0
constexpr uint64_t GET_BONE_DATA = 0x8f8090;       // FUN_1408f8090
constexpr uint64_t GET_BONE_COUNT = 0x8f8080;      // FUN_1408f8080
}  // namespace GameFuncAddr

// Game status symbol hash → proto enum mapping
namespace GameStatusHash {
// PRE_MATCH
constexpr uint64_t PRE_MATCH_1 = 0x6C19A96227539FAC;
constexpr uint64_t PRE_MATCH_2 = 0x3979BA686488BA64;
constexpr uint64_t PRE_MATCH_3 = 0xD69CC3ED9F1E31BF;
// ROUND_START
constexpr uint64_t ROUND_START_1 = 0xDAD765F3754F6262;
constexpr uint64_t ROUND_START_2 = 0x37756D8877ED5768;
constexpr uint64_t ROUND_START_3 = 0x7B54C48FED3ECD93;
// PLAYING
constexpr uint64_t PLAYING_1 = 0x3B529C356A118CC6;
constexpr uint64_t PLAYING_2 = 0xAB08D245B707B56B;
// SCORE
constexpr uint64_t SCORE_1 = 0x56EDCA9065F29296;
constexpr uint64_t SCORE_2 = 0x75C7A4739D7A1739;
// ROUND_OVER
constexpr uint64_t ROUND_OVER_1 = 0xCCEA96E48128F05E;
constexpr uint64_t ROUND_OVER_2 = 0x67E1E232F03BE11D;
// POST_MATCH
constexpr uint64_t POST_MATCH_1 = 0x9567032E22CDC734;
constexpr uint64_t POST_MATCH_2 = 0xF83FCDBB25BA6F07;
// PRE_SUDDEN_DEATH
constexpr uint64_t PRE_SUDDEN_DEATH_1 = 0xE1C684082C60875C;
constexpr uint64_t PRE_SUDDEN_DEATH_2 = 0xFC23497054CBCBCF;
// SUDDEN_DEATH
constexpr uint64_t SUDDEN_DEATH_1 = 0xB0E1B68558DBA498;
constexpr uint64_t SUDDEN_DEATH_2 = 0x939313B41BE27085;
// POST_SUDDEN_DEATH
constexpr uint64_t POST_SUDDEN_DEATH_1 = 0x9D3569956C5E79A0;
constexpr uint64_t POST_SUDDEN_DEATH_2 = 0xF09C446F72B86057;
}  // namespace GameStatusHash

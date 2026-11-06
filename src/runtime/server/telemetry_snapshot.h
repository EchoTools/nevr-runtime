#pragma once

#include <cstdint>
#include <cstring>

// Game state snapshot captured on game thread and serialized by telemetry
// thread. TelemetrySnapshotStore owns three instances and leases writer,
// published, and reader roles under its metadata mutex.
struct TelemetrySnapshot {
  // SnapshotGameState found the net game and its game state. A snapshot it could not read is published
  // cleared, with this false, and event detection skips it (telemetry_events.h).
  bool gameRead;

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

  // Indexed by game slot (the entrant word's low 5 bits), not by the net game's entrant array order.
  // flags and teamIndex come from the net-game entrant; displayName from the game's GetUserName for that
  // entrant; accountId and ping from the lobby entrant at the same slot (telemetry_player_read.h). present: a
  // player (team 0 to 2) whose slot is below 16, whose slot's handle-table entry names it, and whose slot has
  // an entry in the lobby's entrant table.
  struct PlayerInfo {
    bool present;
    uint16_t flags;
    uint64_t accountId;
    char displayName[36];
    uint16_t ping;
    uint16_t teamIndex;  // 0 blue, 1 orange, 2 spectator
  } playerInfo[16];
  uint16_t playerCount;  // players read (present slots)

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

  // Per-player stats (direct read), indexed by game slot like playerInfo
  struct PlayerStats {
    uint32_t points, goals, assists, stuns, blocks, saves;
    uint32_t interceptions, steals, catches, passes, shotsTaken;
    float possessionTime;  // seconds
  } stats[16];

  void Clear() { std::memset(this, 0, sizeof(*this)); }
};

// Memory layout constants
// Validated against echovr-reconstruction:
//   CR15NetGame.h: GetNetGameFromContext = *(context + 0x8518), global at DAT_1420a0478
//   CR15NetGameLayout.h: loadout table, game flags, broadcaster offsets
//   CServerConfig.h: EntrantData field offsets (player array matches)
namespace nevr_game_offsets {

constexpr uint64_t GAME_CONTEXT_OFFSET = 0x20a0478;  // DAT_1420a0478 (global game context)
constexpr uint64_t NETGAME_OFFSET = 0x8518;           // GetNetGameFromContext: *(context + 0x8518)
constexpr uint64_t GAME_STATE_DATA_OFFSET = 0x2AA8;

// Entity system access (from CR15NetGame)
constexpr uint64_t ENTITY_HANDLE_RESOLVER_OFFSET = 0x08;  // *(CR15NetGame + 8) → handle resolver
constexpr uint64_t WORLD_OBJECT_OFFSET = 0x2B08;          // *(CR15NetGame + 0x2B08) → world/scene
constexpr uint64_t ENTITY_MANAGER_OFFSET = 0x08;          // *(world + 8) → entity manager

// Net-game entrants: CR15NetGame + 0x178, stride 0x250, count u16 at + 0xE2 (echovr.exe 0x1401563c7
// `movzx ecx, word ptr [rax + 0xe2]`, 0x14015642a `imul r12, rax, 0x250`, 0x140156435
// `movzx eax, word ptr [rcx + r12 + 0x178]`). The game reads a slot's stats only for a slot below 16
// (0x14015652b `and ax, 0x1f`, 0x140156548 `cmp bx, 0x10; jae`), so the snapshot holds 16 slots. Its loop
// runs to the count (0x140156403 `cmp bx, cx`); PLAYER_ARRAY_CAPACITY is this reader's own cap on the count.
constexpr uint64_t PLAYER_ARRAY_OFFSET = 0x178;
constexpr uint64_t PLAYER_STRIDE = 0x250;
constexpr uint64_t PLAYER_FLAGS_OFFSET = 0x00;  // the entrant word
constexpr uint64_t PLAYER_COUNT_OFFSET = 0xE2;
constexpr uint16_t PLAYER_ARRAY_CAPACITY = 16;
// The entrant word: game slot in bits 0-4 (0x14015649b `and edx, 0x1f`), team in bits 10-12
// (0x140156441 `shr ax, 0xa`, 0x140156445 `and ax, 7`). /session skips a team above 2 (0x140156452
// `cmp r14w, 2; ja`): 0 blue, 1 orange, 2 spectator; the server's own entrant is team 4.
constexpr uint16_t ENTRANT_SLOT_MASK = 0x1F;
constexpr uint16_t ENTRANT_TEAM_SHIFT = 10;
constexpr uint16_t ENTRANT_TEAM_MASK = 0x7;
constexpr uint16_t ENTRANT_TEAM_LAST_PLAYER = 2;

// The net game's handle table: 16 entries { u16 entrant index, u16 generation } at CR15NetGame + 0x138,
// indexed by game slot. The /session writer turns an entrant's slot into a handle and reads the entrant the
// table names (0x140156522 `and ax, 0x1f`, 0x140156537 generation `[ng + slot*4 + 0x13a]`, 0x140156548
// `cmp bx, 0x10; jae`, 0x140156557 generation 0xffff skips, 0x140156564 generation compare, 0x140156572
// entrant `[ng + slot*4 + 0x138]`, 0xffff skips, 0x140156584 that entrant's record is read). GetUserName by
// handle (0x1401c9a20) makes the same checks before it names a player, else "[INVALID]".
constexpr uint64_t SLOT_HANDLE_TABLE_OFFSET = 0x138;
constexpr uint64_t SLOT_HANDLE_ENTRANT = 0x0;
constexpr uint64_t SLOT_HANDLE_GENERATION = 0x2;
constexpr uint64_t SLOT_HANDLE_STRIDE = 4;
constexpr uint16_t SLOT_HANDLE_NONE = 0xFFFF;

// The net game's session-flags pointer. GetUserName (0x1401c98c0) loads it and reads the word it points to
// with no null check (0x1401c9948 `mov rax, [rcx + 0x2da0]`, 0x1401c994f `mov ecx, [rax]`); the crash path's
// table of session-flags null dereferences names GetUserName (crash_recovery_sites.h, N71). The reader calls
// GetUserName only while this pointer is set.
constexpr uint64_t SESSION_FLAGS_POINTER_OFFSET = 0x2DA0;

// The lobby: the CNSLobby (EchoVR::Lobby) at *(CR15NetGame + 0x40) + 0x18 (echovr.exe 0x1401b3a94
// `mov rcx, [rax + 0x40]`, 0x1401b3aa8 `add rcx, 0x18`; CR15NetGame hands the same object to LoadServerSupport,
// 0x14015995b, which passes it to IServerLib::Initialize, 0x14060bcfa). Its entrant table is
// EchoVR::Lobby::entrantData (0xD8 per slot, indexed by game slot): items at +0x360, length at +0x368
// (0x1406093c9 `cmp [rcx + 0x368], r8`, 0x1406093d2 `mov rax, [rcx + 0x360]`). The game reads the user id
// (0x1406093c0), the name (0x140608310 with flag 1: +0x60, sfwDisplayName) and the ping (0x1406084e7
// `movzx eax, word ptr [rax + rdx + 0x8a]`) there.
constexpr uint64_t LOBBY_POINTER_OFFSET = 0x40;
constexpr uint64_t LOBBY_SUBOBJECT_OFFSET = 0x18;

// Player stats: one block per game slot at CR15NetGame + slot * 0x4D08 (0x1401565ab `imul rcx, rax,
// 0x4d08`); each stat is an entry { u32 type, value, u32 count } at the offset below (the `lea rax,
// [rdx + off]` blocks at 0x1401565b2-0x140156785). The value of an entry whose type is 5 is value / count,
// 0 when count is 0 (0x140156ad0 `cmp dword ptr [rax], 5`, 0x140156af2 `div ecx`, 0x140156af6); possession
// time's value is a float (0x140156a09 `movss`, 0x140156a27 `divss`).
constexpr uint64_t SLOT_STATS_STRIDE = 0x4D08;
constexpr uint64_t STAT_ENTRY_TYPE = 0x0;
constexpr uint64_t STAT_ENTRY_VALUE = 0x4;
constexpr uint64_t STAT_ENTRY_COUNT = 0x8;
constexpr uint32_t STAT_TYPE_AVERAGE = 5;
constexpr uint64_t SLOT_STAT_STUNS = 0x7184;
constexpr uint64_t SLOT_STAT_BLOCKS = 0x71B4;
constexpr uint64_t SLOT_STAT_POINTS = 0x72A4;
constexpr uint64_t SLOT_STAT_GOALS = 0x72D4;
constexpr uint64_t SLOT_STAT_STEALS = 0x7394;
constexpr uint64_t SLOT_STAT_ASSISTS = 0x73C4;
constexpr uint64_t SLOT_STAT_SAVES = 0x73F4;
constexpr uint64_t SLOT_STAT_SHOTS_TAKEN = 0x7454;
constexpr uint64_t SLOT_STAT_PASSES = 0x74B4;
constexpr uint64_t SLOT_STAT_CATCHES = 0x74E4;
constexpr uint64_t SLOT_STAT_INTERCEPTIONS = 0x7544;
constexpr uint64_t SLOT_STAT_POSSESSION_TIME = 0x75A4;

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

}  // namespace nevr_game_offsets

// Game function pointer typedefs (cast known VAs to these)
// FUN_140d5ced0(out, CR15NetGame, key) → out: the replicated symbol variable `key` of the net game.
// It writes 8 bytes to *out, the symbol or -1 when the variable is absent (FUN_140d5cdb0 writes
// both), and returns out; GameState_SerializeToJson (0x140155c80) calls it this way.
using GetSymbolHashFn = uint64_t*(__fastcall*)(uint64_t* out, void* netGame, uint64_t propertyHash);

// FUN_140d36750(CR15NetGame, key) → float (game_timer, seconds)
using GetFloatPropertyFn = float(__fastcall*)(void* netGame, uint64_t propertyHash);

// FUN_140d05730(CR15NetGame, key) → int32_t (blue_score / red_score). The owner is the net game, not
// the object at net game + GAME_STATE_DATA_OFFSET.
using GetIntPropertyFn = int32_t(__fastcall*)(void* netGame, uint64_t propertyHash);

// FUN_1401c98c0(CR15NetGame, entrant index) → the player's name, as /session writes it (0x1401b3b5b `call
// 0x1401c98c0` with the loop's entrant index, copied at 0x1401b3b6d): the "displayname" of the entrant's state
// data (*(ng + e*0x250 + 0x3c0), read by json path through 0x1405fe290) unless bit 27 of **(ng + 0x2da0) is
// set; when that is empty the lobby's sfw name of the entrant's slot (0x140608310 flag 1); then "Unknown".
// An index from the entrant count *(ng + 0xE2) up to *(ng + 0xDC) is named "Replayer %u". Never null.
using GetUserNameFn = const char*(__fastcall*)(void* netGame, uint16_t entrantIndex);

// FUN_1404f37a0(entityManager, handle) → entity*
using EntityLookupFn = void*(__fastcall*)(void* entityManager, int64_t handle);

// FUN_140363610(handleResolver, accountIdPair) → bool (modifies pair[0] to transform handle)
using ResolveEntityHandleFn = int(__fastcall*)(void* handleResolver, int64_t* accountIdPair);

// FUN_1408f8090(entity, boneSlot) → bone data ptr
using GetBoneDataFn = void*(__fastcall*)(void* entity, uint16_t boneSlot);

// FUN_1408f8080(entity) → uint32_t bone count (always 0x17=23)
using GetBoneCountFn = uint32_t(__fastcall*)(void* entity);

// FUN_14042d690(entity, slot) → transform component ptr (returns ptr+0xa0 into component data)
using GetTransformComponentFn = void*(__fastcall*)(void* entity, uint16_t slot);

// Property keys for game function calls: each is the CSymbol64 of the replicated variable's name
// (test_telemetry_game_properties pins each to its name).
namespace nevr_property_hash {
constexpr uint64_t GAME_STATUS = 0xB2996AD399FDC0FF;    // "game_state"
constexpr uint64_t GAME_CLOCK = 0xB2996AD484F1D1E8;     // "game_timer"
constexpr uint64_t BLUE_POINTS = 0x1C97052A21E09F11;    // "blue_score"
constexpr uint64_t ORANGE_POINTS = 0x2ED07B1C8C27548D;  // "red_score"
}  // namespace nevr_property_hash

// The game's three property getters, resolved from nevr_game_func_addr (a test passes fakes).
struct GamePropertyGetters {
  GetSymbolHashFn symbol;
  GetFloatPropertyFn f32;
  GetIntPropertyFn i32;
};

// What the getters return for one snapshot. statusHash is -1 when the symbol getter is absent or the
// variable is; the others stay 0 when their getter is absent.
struct GameProperties {
  uint64_t statusHash = ~0ULL;
  float clock = 0.0f;
  int32_t bluePoints = 0;
  int32_t orangePoints = 0;
};

// Reads the game status, clock and scores of `netGame`, every getter with the net game as its owner.
// Game thread only.
inline GameProperties ReadGameProperties(const GamePropertyGetters& getters, void* netGame) {
  GameProperties props;
  if (getters.symbol) {
    uint64_t out = ~0ULL;
    getters.symbol(&out, netGame, nevr_property_hash::GAME_STATUS);
    props.statusHash = out;
  }
  if (getters.f32) {
    props.clock = getters.f32(netGame, nevr_property_hash::GAME_CLOCK);
  }
  if (getters.i32) {
    props.bluePoints = getters.i32(netGame, nevr_property_hash::BLUE_POINTS);
    props.orangePoints = getters.i32(netGame, nevr_property_hash::ORANGE_POINTS);
  }
  return props;
}

// Game function virtual addresses (offsets from base)
namespace nevr_game_func_addr {
constexpr uint64_t GET_SYMBOL_HASH = 0xd5ced0;       // FUN_140d5ced0
constexpr uint64_t GET_FLOAT_PROPERTY = 0xd36750;    // FUN_140d36750
constexpr uint64_t GET_INT_PROPERTY = 0xd05730;      // FUN_140d05730
constexpr uint64_t GET_USER_NAME = 0x1c98c0;         // FUN_1401c98c0(netGame, entrant index)
constexpr uint64_t ENTITY_LOOKUP = 0x4f37a0;         // FUN_1404f37a0(entityMgr, handle)
constexpr uint64_t RESOLVE_ENTITY_HANDLE = 0x363610;  // FUN_140363610(resolver, pair)
constexpr uint64_t GET_BONE_DATA = 0x8f8090;          // FUN_1408f8090(entity, slot)
constexpr uint64_t GET_BONE_COUNT = 0x8f8080;         // FUN_1408f8080() → 0x17
constexpr uint64_t GET_TRANSFORM_COMPONENT = 0x42d690; // FUN_14042d690(entity, slot)
}  // namespace nevr_game_func_addr

// Game status symbol hash → proto enum mapping
namespace nevr_game_status_hash {
// PRE_MATCH
constexpr uint64_t PRE_MATCH_1 = 0x6C19A96227539FAC;
constexpr uint64_t PRE_MATCH_2 = 0x3979BA686488BA64;
constexpr uint64_t PRE_MATCH_3 = 0xD69CC3ED9F1E31BF;
// ROUND_START
constexpr uint64_t ROUND_START_1 = 0xDAD765F3754F6262;
constexpr uint64_t ROUND_START_2 = 0x37756D8877ED5768;
constexpr uint64_t ROUND_START_3 = 0x7B54C48FED3ECD93;
constexpr uint64_t ROUND_START_4 = 0xE1C16321DEF4CC6C;  // team_room_wait_enter
constexpr uint64_t ROUND_START_5 = 0xFBC46082C080FBCF;  // team_room_wait_update
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
constexpr uint64_t POST_MATCH_3 = 0x344D080EA700B7EA;  // post_match_celebration_personal_stats_enter
constexpr uint64_t POST_MATCH_4 = 0x3EDA28AAE966FA2D;  // post_match_celebration_personal_stats_update
constexpr uint64_t POST_MATCH_5 = 0xFC6140CC02F9C63D;  // waiting_to_return_to_lobby
// PRE_SUDDEN_DEATH
constexpr uint64_t PRE_SUDDEN_DEATH_1 = 0xE1C684082C60875C;
constexpr uint64_t PRE_SUDDEN_DEATH_2 = 0xFC23497054CBCBCF;
// SUDDEN_DEATH
constexpr uint64_t SUDDEN_DEATH_1 = 0xB0E1B68558DBA498;
constexpr uint64_t SUDDEN_DEATH_2 = 0x939313B41BE27085;
// POST_SUDDEN_DEATH
constexpr uint64_t POST_SUDDEN_DEATH_1 = 0x9D3569956C5E79A0;
constexpr uint64_t POST_SUDDEN_DEATH_2 = 0xF09C446F72B86057;
}  // namespace nevr_game_status_hash

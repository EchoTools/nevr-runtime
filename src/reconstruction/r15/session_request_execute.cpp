// NRadEngine::NRadGame::NR15RestAPI::SSessionRequest::Execute
// Quest libr15.so VA 0x13b76d4; ReVault binary 576.
// ReVault raw parent 100224893, reconstruction node 100600754; grade M: whole function (2141 instructions) compared block by block with the listing and syntax-checked; 11 globals stay DAT_ placeholders (0x386d340, 0x386d348, 0x386d350, 0x386d358, 0x386d360, 0x386d368, 0x386d370, 0x386d378, 0x386d380, 0x386d388, 0x386d390); WriteSessionJson is the merged tree's name for ReVault FUN_012b9848; signature follows the listing (int, references), not the tree's void/pointer declaration.
// @0x13b76d4 — NRadEngine::NRadGame::NR15RestAPI::SSessionRequest::Execute
/* @confidence: M */
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// 11 globals stay placeholders (no ReVault name): 64-bit symbol hashes written once by _INIT_162 (0x105b780).
extern std::uint64_t DAT_0386d340;  // 0xefead03a7d21b627: type key of the payload and physics component lookups
extern std::uint64_t DAT_0386d348;  // 0xba11136c4dd2e646: ClaimState value that means "contested"
extern std::uint64_t DAT_0386d350;  // 0x34f3a75dbb96c0b3: name of the symbol var read for "contested" in the capture block
extern std::uint64_t DAT_0386d358;  // 0xba4a488efd29847b: real var scaled by 100 into "orange_points"
extern std::uint64_t DAT_0386d360;  // 0x8bd541a7f8565a03: real var scaled by 100 into "blue_points"
extern std::uint64_t DAT_0386d368;  // 0xb546f3aceb9c95ab: int var of "orange_round_score"
extern std::uint64_t DAT_0386d370;  // 0x72c158616250640b: int var of "blue_round_score"
extern std::uint64_t DAT_0386d378;  // 0xb2996ad484f1d1e8: real var of "timers|game_timer" (same hash as rodata 0x2cadf00, "game_timer")
extern std::uint64_t DAT_0386d380;  // 0x5faf3c373f5a7771: flag var that enables the overtime timer
extern std::uint64_t DAT_0386d388;  // 0x9d89a48a426ba48f: real var of the overtime timer
extern std::uint64_t DAT_0386d390;  // 0x810366b6b900baad: balance-settings key multiplied into the overtime timer

namespace NRadEngine {

class CJson {
 public:
  void SetInt(const char* key, std::int64_t value);    // 0x1028210 (CJson::SetInt, 0x10a5edc)
  void SetString(const char* key, const char* value);  // 0x103a7c0 (CJson::SetString, 0x10a3444)
  void SetReal(const char* key, float value);          // 0xfe52d0 (CJson::SetReal, 0x10a62e8)
  void SetBoolean(const char* key, bool value);        // 0xff2f00 (CJson::SetBoolean, 0x10a2f94)
};

// Fixed text returned by value in memory (x8); the stack slot is 0x200 bytes.
template <unsigned long long N>
struct TStr {
  char text[N];
};
template <unsigned long long N>
TStr<N> MakeTStrPrintF(const char* format, ...);  // NRadEngine::MakeTStrPrintF<512ull>, 0x102b370 (0x1086b9c)

template <unsigned long long N>
class CFixedStringT {
 public:
  void SPrintF(const char* format, ...);  // CFixedStringT<9ull>::SPrintF, 0x101a740 (0x13cdacc); this is the buffer
  char text[N];
};

class CMemory {
 public:
  static void Fill(void* destination, int value, std::size_t count);  // 0x1047300 (0x12cafcc)
};

class CSysString {
 public:
  static int Compare(const char* first, const char* second, unsigned flags, unsigned long long length);  // 0xff0d10
};

class CSymbol64 {
 public:
  static std::uint64_t Append(std::uint64_t symbol, const char* suffix, int first, int second);  // 0x1031ea0 (0x12ccb34)
};

// Json tree walker. The seven slots are the ReVault vtable names (vtable 0x36fb028); their parameters are not recovered.
class CJsonTraversal {
 public:
  virtual void ProcessBoolean();  // 0x10a9964
  virtual void ProcessInt();      // 0x10a9a9c
  virtual void ProcessReal();     // 0x10a9bd8
  virtual void ProcessString();   // 0x10a9cb0
  virtual void ProcessNull();     // 0x10a9df0
  virtual void ProcessArray();    // 0x10a9df4
  virtual void ProcessObject();   // 0x10aa0d0
  void Traverse(CJson& json, const char* path, const char* otherPath);  // 0x10aa3dc via thunk 0x104b140
};

// vtable 0x36fb0e0. The listing stores the vptr and zeroes the three state words (object size 0x20).
class CJsonAccumulator : public CJsonTraversal {
 public:
  CJsonAccumulator() : state{} {}
  void ProcessBoolean() override;  // 0x10ab294
  void ProcessInt() override;      // 0x10aa980
  void ProcessReal() override;     // 0x10aaed4
  void ProcessString() override;   // 0x10ab470
  alignas(8) std::byte state[0x18];  // +0x08..+0x20, contents not recovered
};
static_assert(sizeof(CJsonAccumulator) == 0x20);

// 16-byte actor id; ResolveActorAlias rewrites it in place.
struct SActorGUID {
  alignas(8) std::byte raw[16];
};
static_assert(sizeof(SActorGUID) == 0x10);

class CncaGame {
 public:
  void* GameSpaceMT(std::uint64_t first);  // 0xffef20 (0x18f8f24): this = context+0x10, returns the game space or null
};
void ResolveActorAlias(CncaGame* game, SActorGUID* guid);  // 0xff1570 (0x14d6da8)

class CNSLobby {
 public:
  const char* EntrantString(std::uint64_t entrant, const char* key, const char* fallback);  // 0x1018180 (0x1a2dbb8)
};

// Component system base. The listing calls slot 58 (+0x1d0); the slot names are the ReVault vtable names of
// CR15NetUISettingsCS (vtable 0x379b628), whose parameters are not recovered.
class CComponentSystem {
 public:
  virtual ~CComponentSystem();               // slots 0 and 1
  virtual void IsA();                        // 2
  virtual void ParentInfo();                 // 3
  virtual void Condemn();                    // 4
  virtual void Event();                      // 5
  virtual void Notify();                     // 6
  virtual void ResetActorGenerations();      // 7
  virtual void SaveLoadEnabledComponents();  // 8
  virtual void SaveLoadInternal();           // 9
  virtual void SaveLoadComponentInternal();  // 10
  virtual void OnComponentEnabled();         // 11
  virtual void OnComponentDisabled();        // 12
  virtual void OnComponentRecycled();        // 13
  virtual void OnComponentRecycleInit();     // 14
  virtual void OnEnabledBeforeComponents();  // 15
  virtual void OnEnabledAfterComponents();   // 16
  virtual void OnDisabledBeforeComponents(); // 17
  virtual void OnDisabledAfterComponents();  // 18
  virtual void DataOpaque();                 // 19
  virtual void SetDataOpaque();              // 20
  virtual void PrimaryData();                // 21
  virtual void CreateData();                 // 22
  virtual void DestroyData();                // 23
  virtual void GetDestroyDataFunction();     // 24
  virtual void CloneData();                  // 25
  virtual void GetCloneDataFunction();       // 26
  virtual void SaveLoadData();               // 27
  virtual void CopyComponentData();          // 28
  virtual void FinalizeData();               // 29
  virtual void RemoveActorData();            // 30
  virtual void GetRemoveActorDataFunction(); // 31
  virtual void ComponentDataCount();         // 32
  virtual void GetComponentDataCountFunction();  // 33
  virtual void CopyData();                   // 34
  virtual void ModifyData();                 // 35
  virtual void GetModifyDataFunction();      // 36
  virtual void AppendDataEntry();            // 37
  virtual void RemoveDataEntry();            // 38
  virtual void ImportDataFromJSON();         // 39
  virtual void ExportDataToJSON();           // 40
  virtual void GetLookupData();              // 41
  virtual void Finalize();                   // 42
  virtual void RemoveReferences();           // 43
  virtual void Shutdown();                   // 44
  virtual void OnArchiveUnload();            // 45
  virtual void OnGameSpaceLoaded();          // 46
  virtual void OnGameSpaceUnloaded();        // 47
  virtual void OnGameSpaceAborted();         // 48
  virtual void OnGameSpaceEnabled();         // 49
  virtual void OnGameSpaceDisabled();        // 50
  virtual void SaveLoadComponent();          // 51
  virtual void PrepareForLoad();             // 52
  virtual void RestoreCheckpoint();          // 53
  virtual void Component();                  // 54
  virtual void Component(int);               // 55
  virtual void AnyComponent();               // 56
  virtual void AnyEnabledComponent();        // 57
  virtual int IsComponentValid(std::uint32_t id);  // 58, +0x1d0
};

// Handle stored on the stack: 0x50 bytes, the component pointer is cached at +0x40 and its id at +0x48.
template <typename TComponent>
class CComponentHandle {
 public:
  // The UI settings handle (listing 0x13b7cb8..0x13b7cec, base sp+0x2a0) stores +0x00 (zero), +0x08, +0x10, +0x16..+0x1d,
  // +0x28, +0x30 and +0x38 (all ones), +0x1e..+0x1f (zero), +0x20 and +0x48 (0xffff, 8 bytes each). It never stores
  // +0x40..+0x47 (the cached component pointer, read at sp+0x2e0 after RefreshCache); the C++ raw{} zero-initialises
  // the whole object first, so that slot and the zero stores are covered there.
  CComponentHandle() : raw{} {
    const std::uint64_t none = ~std::uint64_t{0};
    const std::uint64_t wide = 0xffff;
    std::memcpy(raw + 0x08, &none, sizeof none);
    std::memcpy(raw + 0x10, &none, sizeof none);
    std::memcpy(raw + 0x16, &none, sizeof none);  // bytes +0x16..+0x1d; +0x1e/+0x1f are then cleared
    std::memset(raw + 0x1e, 0, 2);
    std::memcpy(raw + 0x20, &wide, sizeof wide);
    std::memcpy(raw + 0x28, &none, sizeof none);
    std::memcpy(raw + 0x30, &none, sizeof none);
    std::memcpy(raw + 0x38, &none, sizeof none);
    std::memcpy(raw + 0x48, &wide, sizeof wide);
  }
  void Find(void* components, std::uint64_t typeKey, std::uint64_t instance);  // payload 0x1007c30, physics 0xff12c0, UI settings 0xfdbc80
  void RefreshCache();  // payload 0x1021e50, physics 0x1034720, UI settings 0x10182d0
  alignas(8) std::byte raw[0x50];
};

// Handle stored on the stack: 0x10 bytes, the component pointer at +0 and its id at +8.
template <typename TComponent>
class CLocalComponentHandle {
 public:
  CLocalComponentHandle() : raw{} {
    const std::uint16_t wide = 0xffff;
    std::memcpy(raw + 0x08, &wide, sizeof wide);  // strh w26, [sp+0x68]; +0xa (u32) and +0xe (u16) stay zero
  }
  void Find(void* gameSpace, std::uint64_t instance, std::uint64_t flags);  // punchable 0xfed390, equipment 0x1014fb0
  alignas(8) std::byte raw[0x10];
};

namespace NRadGame {

class CR15NetGame {
 public:
  const char* UserName(std::uint16_t index);  // 0x1026fe0 (0x134f73c)
};

// vtable 0x3731438; slots 0 and 1 are pure virtual.
class CR15NetLobby {
 public:
  virtual ~CR15NetLobby() = 0;
  virtual void Update();                // 0x1345abc
  virtual void End();                   // 0x13476e0
  virtual void ProcessUI();             // 0x13af8a4
  virtual std::uint64_t GameType();     // 0x1347894, slot 5 (+0x28)
  virtual void MatchmakingGameType();   // 0x13c074c
  virtual void JoinQueuedGameType();    // 0x13c0754
  virtual void FailedGameType();        // 0x13c075c
  virtual std::uint64_t LevelName();    // 0x13478c4, slot 9 (+0x48)
};

struct SR15NetUserId {
  static const std::uint32_t kInvalid;  // 0xffffffff, written by _INIT_162; read through the GOT slot 0x381a350
};

// Net var lookups: the first argument is the object at context+0x18 (or the object the score block starts with).
class CR15NetVarCS {
 public:
  static std::uint64_t Symbol(CR15NetGame* game, std::uint64_t name);  // 0xff6dc0 (0x22bdd9c)
  static float Real(CR15NetGame* game, std::uint64_t name);            // 0x1031840 (0x22bdadc)
  static std::uint32_t Flag(CR15NetGame* game, std::uint64_t name);    // 0xffdba0 (0x223498c)
  static std::int32_t Int(const void* game, std::uint64_t name);       // 0x1014840 (0x22bdc40)
};

class CR15NetBalanceSettingsCS {
 public:
  static float Real(std::uint64_t settings, std::uint64_t key, float fallback);  // 0x1045c80 (0x220f348)
};

class CR15NetProfileSymbolLookup {
 public:
  static const char* WeaponName(std::uint64_t slot);    // 0x101dd30 (0x21fe5d8)
  static const char* TacModName(std::uint64_t slot);    // 0x1027e20 (0x21fe800)
  static const char* OrdnanceName(std::uint64_t slot);  // 0xfe99d0 (0x21fe6ec)
};

struct Float3 {
  float x;
  float y;
  float z;
};

class CR15NetUISettingsCS : public CComponentSystem {
 public:
  std::uint64_t LastUserToModifySettings(std::uint32_t id);  // 0xfdae30 (0x22baee8)
  std::uint64_t LastTimeSettingsModified(std::uint32_t id);  // 0x1044ce0 (0x22baf14)
};
static_assert(sizeof(CComponentHandle<CR15NetUISettingsCS>) == 0x50);

class CR15NetPunchableCS : public CComponentSystem {};
static_assert(sizeof(CLocalComponentHandle<CR15NetPunchableCS>) == 0x10);

class CR15NetPlayerEquipmentCS : public CComponentSystem {
 public:
  std::uint64_t GetWeaponSlot(std::uint32_t id);   // 0xfd2df0 (0x227d0d8)
  std::uint64_t GetWeaponArm(std::uint32_t id);    // 0xff9250 (0x227d100)
  std::uint64_t GetAbilitySlot(std::uint32_t id);  // 0xfeb0a0 (0x227d128)
  std::uint64_t GetGrenadeSlot(std::uint32_t id);  // 0xfe9780 (0x227d150)
};

class CR15NetPayloadCS : public CComponentSystem {
 public:
  std::uint8_t Checkpoint(std::uint32_t id);                // 0xffd360 (0x226f088)
  std::uint64_t ClaimState(std::uint32_t id);               // 0x1002da0 (0x226ed6c)
  float DistAlongTrack(std::uint32_t id);                   // 0xfd0ed0
  std::uint32_t NumAttackersClaiming(std::uint32_t id);     // 0xff2330 (0x226eddc)
  std::uint32_t NumDefendersClaiming(std::uint32_t id);     // 0xfff0f0 (0x226ee04)
  std::int8_t CachedDirectionMovementState(std::uint32_t id);  // 0xfd8b00 (0x226f0b0)
};

class CR15NetPhysicsCS : public CComponentSystem {
 public:
  float Speed(std::uint32_t id);                // 0x101f3c0 (0x22785e8)
  Float3 SmoothedWorldPosition(std::uint32_t id);  // 0xfe1290 (0x2278674)
};

namespace NR15RestAPI {

struct SR15APIContext;  // the request context; only the words read below are known

// Writes the session header fields. ReVault names the row FUN_012b9848; the merged tree
// (src/reconstruction/r15/session_player_writer.cpp) names it WriteSessionJson.
void WriteSessionJson(const SR15APIContext& context, CJson& out);  // 0x13b9848

class SSessionRequest {
 public:
  static int Execute(const SR15APIContext& context, const CJson& body, CJson& response, long long& extra);  // 0x13b76d4 via thunk 0x1026720
};

namespace {

// Offsets of the SR15APIContext words (listing registers: x0 context).
constexpr std::size_t kContextCncaGame = 0x10;  // CncaGame*
constexpr std::size_t kContextNet = 0x18;       // CR15NetGame*, called pSVar19
constexpr std::size_t kContextScore = 0x20;     // pointer to the last-score and disc block

// Offsets inside the CR15NetGame object.
constexpr std::size_t kNetGame = 0x08;           // CncaGame*
constexpr std::size_t kNetLobby = 0x40;          // CR15NetLobby*, CNSLobby subobject at +0x18
constexpr std::size_t kNetUserCount = 0xdc;      // u16
constexpr std::size_t kNetEntrantCount = 0xe2;   // u16
constexpr std::size_t kNetSlotEntrant = 0x138;   // u16 per slot, stride 4: entrant index or 0xffff
constexpr std::size_t kNetSlotGeneration = 0x13a;  // u16 per slot, stride 4
constexpr std::size_t kNetEntrants = 0x178;      // entrant records, stride 0x250; word 0 bits 0-4 slot, bits 10-12 team
constexpr std::size_t kNetEntrantStride = 0x250;
constexpr std::size_t kNetComponents = 0x2b10;   // component manager pointer
constexpr std::size_t kNetStats = 0x7098;        // per-slot stats, stride 0x4d08
constexpr std::size_t kNetStatsStride = 0x4d08;

// Offsets inside a component handle.
constexpr std::size_t kHandleCache = 0x40;
constexpr std::size_t kHandleId = 0x48;

constexpr std::uint64_t kNoInstance = ~std::uint64_t{0};
constexpr std::uint16_t kInvalidIndex = 0xffff;

// Game types (CR15NetLobby::GameType, rodata symbol words; the name after each word is the text next to it).
constexpr std::uint64_t kArenaGameTypes[] = {
    0xcb60a4de7e1caf73ULL,  // echo_arena            0x2ca0950
    0x09990965f4db8c03ULL,  // echo_arena_private    0x2ca4af0
    0xd53a9c73b8b2c760ULL,  // echo_arena_tournament 0x2cacec0
    0x4dc1ddbd89b28531ULL,  // echo_demo             0x2ca4b18
    0x339c5dc66c03b81fULL,  // echo_demo_public      0x2cacee8
    0xd54d62c40852c2f9ULL,  // echo_arena_public_ai  0x2cacf08
    0x888ac0cfd102345fULL,  // echo_arena_practice_ai 0x2cace30
    0xdf8258f63e878e9dULL,  // echo_arena_private_ai 0x2cacf30
    0xea8d9b0bbdaa1c6aULL,  // echo_arena_first_match 0x2cac900
    0xd894afea8246bc6bULL,  // echo_arena_npe        0x2cacf58
};
constexpr std::uint64_t kCombatGameTypes[] = {
    0x3d5c3976578a321aULL,  // echo_combat             0x2ca0970
    0x33bbf6842df97a3fULL,  // echo_combat_private     0x2cacf70
    0x6b44ed1ad5234fbcULL,  // echo_combat_tournament  0x2cacf98
    0x4311cb2c132e9299ULL,  // echo_combat_public_ai   0x2cacfc0
    0x25c1c8218705f693ULL,  // echo_combat_practice_ai 0x2cace58
    0x61fc299f53160641ULL,  // echo_combat_private_ai  0x2cacfe8
    0x47c693e5ebaddea6ULL,  // echo_combat_first_match 0x2cac920
};
// Levels (CR15NetLobby::LevelName).
constexpr std::uint64_t kPayloadLevels[] = {
    0xdf5ca7b7dfa383d4ULL,  // mpl_combat_fission 0x2caa1a8
    0x43e2da7a0c623a19ULL,  // mpl_combat_gauss   0x2caa1d0
    0xdf4aaba6cea689c9ULL,  // mpl_combat_pebbles 0x2caa1f0
    0xea49cde332aa536fULL,  // 0x2caa210, the text next to it reads "pty_mpl_combat_pebbles"
};
constexpr std::uint64_t kCaptureLevels[] = {
    0x42670f2bed45703cULL,  // mpl_combat_combustion 0x2caa238
    0x43e2da7914642604ULL,  // mpl_combat_dyson      0x2caa260
};

// Net var names (rodata symbol words).
constexpr std::uint64_t kHashGameState = 0xb2996ad399fdc0ffULL;                  // "game_state"           0x2ca7418
constexpr std::uint64_t kHashGameTimer = 0xb2996ad484f1d1e8ULL;                  // "game_timer"           0x2cadf00
constexpr std::uint64_t kHashGameFrisbeeOwnerId = 0x1b10bdc962b47fccULL;         // "game_frisbee_ownerid" 0x2cac118
constexpr std::uint64_t kHashBlueTeamRestartRequest = 0xef31e81d7ccf9310ULL;     // 0x2caf998
constexpr std::uint64_t kHashOrangeTeamRestartRequest = 0xd5fc00b1ec025beeULL;   // 0x2caf9c0
constexpr std::uint64_t kHashBlueScore = 0x1c97052a21e09f11ULL;                  // "blue_score" 0x2cac420
constexpr std::uint64_t kHashRedScore = 0x2ed07b1c8c27548dULL;                   // "red_score"  0x2cac438
constexpr std::uint64_t kHashMpArenaRulesNetsettings = 0xe3b8354d79b3d383ULL;    // "mp_arena_rules_netsettings" 0x2cac450

// Game states (CR15NetVarCS::Symbol of "game_state"), grouped by the text game_status gets.
constexpr std::uint64_t kStatesPreMatch[] = {
    0x6c19a96227539facULL,  // pre_match_enter                 0x2cafe00
    0x3979ba686488ba64ULL,  // pre_match_wait_for_ready_enter  0x2cafe28
    0xd69cc3ed9f1e31bfULL,  // pre_match_wait_for_ready_update 0x2cafe58
};
constexpr std::uint64_t kStatesRoundStart[] = {
    0xdad765f3754f6262ULL,  // round_start           0x2cafe88
    0x37756d8877ed5768ULL,  // pre_launch_enter      0x2cab7f8
    0x7b54c48fed3ecd93ULL,  // pre_launch_update     0x2cafea0
    0xe1c16321def4cc6cULL,  // team_room_wait_enter  0x2caff58
    0xfbc46082c080fbcfULL,  // team_room_wait_update 0x2caff78
};
constexpr std::uint64_t kStatesPlaying[] = {
    0x3b529c356a118cc6ULL,  // main_loop_enter  0x2ca86e8
    0xab08d245b707b56bULL,  // main_loop_update 0x2cafec0
};
constexpr std::uint64_t kStatesScore[] = {
    0x56edca9065f29296ULL,  // point_score_enter  0x2cafee8
    0x75c7a4739d7a1739ULL,  // point_score_update 0x2caff08
};
constexpr std::uint64_t kStatesRoundOver[] = {
    0xccea96e48128f05eULL,  // round_over_enter  0x2ca8708
    0x67e1e232f03be11dULL,  // round_over_update 0x2caff38
};
constexpr std::uint64_t kStatesPostMatch[] = {
    0x9567032e22cdc734ULL,  // post_match_celebration_enter                  0x2caffa0
    0xf83fcdbb25ba6f07ULL,  // post_match_celebration_update                 0x2caffd8
    0x344d080ea700b7eaULL,  // post_match_celebration_personal_stats_enter   0x2cb0008
    0x3eda28aae966fa2dULL,  // post_match_celebration_personal_stats_update  0x2cb0040
    0xfc6140cc02f9c63dULL,  // waiting_to_return_to_lobby                    0x2cb0080
};
constexpr std::uint64_t kStatesPreSuddenDeath[] = {
    0xe1c684082c60875cULL,  // pre_sudden_death_enter  0x2cb00a8
    0xfc23497054cbcbcfULL,  // pre_sudden_death_update 0x2cb00e0
};
constexpr std::uint64_t kStatesSuddenDeath[] = {
    0xb0e1b68558dba498ULL,  // sudden_death_new_round_enter  0x2cb0108
    0x939313b41be27085ULL,  // sudden_death_new_round_update 0x2cb0138
};
constexpr std::uint64_t kStatePostSuddenDeathEnter = 0x9d3569956c5e79a0ULL;   // post_sudden_death_enter  0x2cb0168
constexpr std::uint64_t kStatePostSuddenDeathUpdate = 0xf09c446f72b86057ULL;  // post_sudden_death_update 0x2cb01a0

// Text tables copied from rodata: team defaults (0x3731db0) and the goal types (0x3731d58).
constexpr const char* kDefaultTeamNames[3] = {"BLUE TEAM", "ORANGE TEAM", "SPECTATOR"};
constexpr const char* kGoalTypeNames[11] = {
    "[NO GOAL]", "SLAM DUNK", "INSIDE SHOT", "LONG SHOT", "BOUNCE SHOT", "LONG BOUNCE SHOT",
    "HEADBUTT",  "LONG HEADBUTT", "BUMPER SHOT", "LONG BUMPER SHOT", "SELF GOAL",
};

template <typename T>
T Load(const void* base, std::size_t offset) {
  T value{};
  std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
  return value;
}

template <std::size_t N>
bool IsOneOf(std::uint64_t value, const std::uint64_t (&set)[N]) {
  for (const std::uint64_t candidate : set) {
    if (candidate == value) {
      return true;
    }
  }
  return false;
}

}  // namespace

int SSessionRequest::Execute(const SR15APIContext& context, const CJson& body, CJson& response, long long& extra) {
  const std::byte* const ctx = reinterpret_cast<const std::byte*>(&context);
  WriteSessionJson(context, response);  // bl 0x13b770c: FUN_012b9848(x0 context, x1 response)

  // The listing reloads context+0x18 / context+0x20 after calls; these re-read them the same way.
  const auto net = [ctx]() { return Load<CR15NetGame*>(ctx, kContextNet); };
  const auto scoreBlock = [ctx]() { return Load<const std::byte*>(ctx, kContextScore); };

  CR15NetLobby* lobby = Load<CR15NetLobby*>(net(), kNetLobby);
  if (lobby == nullptr) {
    return 0;  // 0x13b771c cbz x0 -> 0x13b8cd8
  }
  const std::uint64_t gameType = lobby->GameType();  // blr 0x13b772c, vtable +0x28

  if (IsOneOf(gameType, kArenaGameTypes)) {  // 10 compares 0x13b7734..0x13b782c, any hit -> 0x13b7830
    // ---- arena: game status, clock, last score, disc state, entrants ----
    const std::uint64_t statusSymbol = CR15NetVarCS::Symbol(net(), kHashGameState);   // bl 0x13b7844
    const float gameTimer = CR15NetVarCS::Real(net(), kHashGameTimer);                // bl 0x13b7864
    const std::uint64_t ownerSymbol = CR15NetVarCS::Symbol(net(), kHashGameFrisbeeOwnerId);  // bl 0x13b7880
    const std::uint32_t blueRestart = CR15NetVarCS::Flag(net(), kHashBlueTeamRestartRequest);      // bl 0x13b78a0
    const std::uint32_t orangeRestart = CR15NetVarCS::Flag(net(), kHashOrangeTeamRestartRequest);  // bl 0x13b78c0
    response.SetInt("blue_team_restart_request", blueRestart);      // bl 0x13b78d8 (key 0x2caf9a6)
    response.SetInt("orange_team_restart_request", orangeRestart);  // bl 0x13b78ec (key 0x2caf9d0)

    const char* statusText = "";  // 0x2c419ad
    if (IsOneOf(statusSymbol, kStatesPreMatch)) {
      statusText = "pre_match";  // 0x2cafe19
    } else if (IsOneOf(statusSymbol, kStatesRoundStart)) {
      statusText = "round_start";  // 0x2cac267
    } else if (IsOneOf(statusSymbol, kStatesPlaying)) {
      statusText = "playing";  // 0x2cac296
    } else if (IsOneOf(statusSymbol, kStatesScore)) {
      statusText = "score";  // 0x2cac44a
    } else if (IsOneOf(statusSymbol, kStatesRoundOver)) {
      statusText = "round_over";  // 0x2caff26
    } else if (IsOneOf(statusSymbol, kStatesPostMatch)) {
      statusText = "post_match";  // 0x2caffcb
    } else if (IsOneOf(statusSymbol, kStatesPreSuddenDeath)) {
      statusText = "pre_sudden_death";  // 0x2cb00cf
    } else if (IsOneOf(statusSymbol, kStatesSuddenDeath)) {
      statusText = "sudden_death";  // 0x2cb00d3
    } else if (statusSymbol == kStatePostSuddenDeathEnter) {
      statusText = "post_sudden_death";  // 0x2cb018b
    } else if (statusSymbol == kStatePostSuddenDeathUpdate) {
      statusText = "post_sudden_death";
    }
    response.SetString("game_status", statusText);  // bl 0x13b79b4 (key 0x2caf9ec)
    response.SetReal("game_clock", gameTimer);      // bl 0x13b79c8 (key 0x2caf9f8)

    // Clock text mm:ss.tt from the timer clamped at zero (fmaxnm with 0.0).
    const float clamped = std::fmax(gameTimer, 0.0f);
    const float minutesScaled = clamped * 0.016666668f;
    const std::uint32_t minutes = static_cast<std::uint32_t>(minutesScaled);
    const float seconds = std::fmaf(std::trunc(minutesScaled), -60.0f, clamped);
    const float tenthsScaled = (seconds - std::trunc(seconds)) * 10.0f;
    const float hundredthsScaled = (tenthsScaled - std::trunc(tenthsScaled)) * 10.0f;
    CFixedStringT<9> clockText;
    CMemory::Fill(clockText.text, 0, 9);  // bl 0x13b7a20
    clockText.SPrintF("%02u:%02u.%1u%1u", minutes, static_cast<std::uint32_t>(seconds),
                      static_cast<std::uint32_t>(tenthsScaled),
                      static_cast<std::uint32_t>(hundredthsScaled));  // bl 0x13b7a40 (0x2cb01c6); the 9 bytes are then copied onto themselves
    response.SetString("game_clock_display", clockText.text);  // bl 0x13b7a64 (key 0x2cafa03)

    response.SetInt("possession[0]", -1);  // bl 0x13b7a78 (key 0x2cafa16)
    response.SetInt("possession[1]", -1);  // bl 0x13b7a8c (key 0x2cafa24)
    response.SetInt("blue_points",
                    static_cast<std::uint32_t>(CR15NetVarCS::Int(Load<const void*>(scoreBlock(), 0), kHashBlueScore)));  // bl 0x13b7aac, 0x13b7ac0
    response.SetInt("orange_points",
                    static_cast<std::uint32_t>(CR15NetVarCS::Int(Load<const void*>(scoreBlock(), 0), kHashRedScore)));   // bl 0x13b7adc, 0x13b7af0

    // Last score: team, amount, disc speed and throw distance.
    response.SetString("last_score|team",
                       Load<std::uint16_t>(scoreBlock(), 0x10) == 0 ? "blue" : "orange");  // bl 0x13b7b20 (strings 0x2cae0bf / 0x2cae0e4)
    response.SetInt("last_score|point_amount", Load<std::uint16_t>(scoreBlock(), 0x12));    // bl 0x13b7b38
    response.SetReal("last_score|disc_speed", Load<float>(scoreBlock(), 0x48));             // bl 0x13b7b50
    response.SetReal("last_score|distance_thrown", Load<float>(scoreBlock(), 0x50));        // bl 0x13b7b68

    // User name of a packed user id: low 16 bits slot (bits 4-15 must be clear), high 16 bits generation.
    const auto userName = [&net](std::uint32_t id) -> const char* {
      const std::uint32_t generation = id >> 16;
      if (generation == kInvalidIndex || (id & 0xfff0) != 0) {
        return "[INVALID]";  // 0x2caa2a7
      }
      CR15NetGame* game = net();
      const std::uint32_t slot = id & 0xffff;
      if (generation != Load<std::uint16_t>(game, slot * 4 + kNetSlotGeneration)) {
        return "[INVALID]";
      }
      const std::uint16_t entrant = Load<std::uint16_t>(game, slot * 4 + kNetSlotEntrant);
      if (entrant == kInvalidIndex) {
        return "[INVALID]";
      }
      return game->UserName(entrant);  // bl 0x13b7bc4, 0x13b7c40, 0x13b7dc0
    };
    response.SetString("last_score|person_scored", userName(Load<std::uint32_t>(scoreBlock(), 0x6c)));  // bl 0x13b7be4
    response.SetString("last_score|assist_scored", userName(Load<std::uint32_t>(scoreBlock(), 0x68)));  // bl 0x13b7c60
    const std::int64_t goalType = Load<std::int32_t>(scoreBlock(), 0x44);
    response.SetString("last_score|goal_type",
                       static_cast<std::uint32_t>(goalType) <= 10 ? kGoalTypeNames[goalType] : "[INVALID GOAL TYPE]");  // bl 0x13b7c9c (0x2cac407)

    // Rules settings: who changed them last, and when.
    {
      CComponentHandle<CR15NetUISettingsCS> settings;  // sp+0x2a0
      settings.Find(Load<void*>(net(), kNetComponents), kHashMpArenaRulesNetsettings, kNoInstance);  // bl 0x13b7cf0
      settings.RefreshCache();                                                                        // bl 0x13b7cf8
      CR15NetUISettingsCS* ui = Load<CR15NetUISettingsCS*>(settings.raw, kHandleCache);
      if (ui != nullptr && Load<std::uint64_t>(settings.raw, 0) != 0) {
        settings.RefreshCache();  // bl 0x13b7d10
        if (ui->IsComponentValid(Load<std::uint32_t>(settings.raw, kHandleId)) != 0) {  // blr 0x13b7d24, vtable +0x1d0
          settings.RefreshCache();  // bl 0x13b7d30
          ui = Load<CR15NetUISettingsCS*>(settings.raw, kHandleCache);
          settings.RefreshCache();  // bl 0x13b7d3c
          const std::uint64_t lastUser = ui->LastUserToModifySettings(Load<std::uint32_t>(settings.raw, kHandleId));  // bl 0x13b7d48
          settings.RefreshCache();  // bl 0x13b7d54
          ui = Load<CR15NetUISettingsCS*>(settings.raw, kHandleCache);
          settings.RefreshCache();  // bl 0x13b7d60
          const std::uint64_t lastTime = ui->LastTimeSettingsModified(Load<std::uint32_t>(settings.raw, kHandleId));  // bl 0x13b7d6c
          response.SetString("rules_changed_by", userName(static_cast<std::uint32_t>(lastUser)));  // bl 0x13b7de0
          response.SetInt("rules_changed_at", static_cast<std::int64_t>(lastTime));                // bl 0x13b7df4
        }
      }
    }

    // Disc position, orientation axes and velocity, each component rounded to milli-units with floor(x * 1000 + 0.5) * 0.001.
    const auto roundMilli = [](float value) { return std::floor(std::fmaf(value, 1000.0f, 0.5f)) * 0.001f; };
    const auto roundMilliFromTop = [](float sum) { return std::floor(std::fmaf(-sum, 1000.0f, 1000.5f)) * 0.001f; };
    {
      const std::byte* disc = scoreBlock();
      const float qx = Load<float>(disc, 0xe4);
      const float qy = Load<float>(disc, 0xe8);
      const float qz = Load<float>(disc, 0xec);
      const float qw = Load<float>(disc, 0xf0);
      const float px = Load<float>(disc, 0xf4);
      const float py = Load<float>(disc, 0xf8);
      const float pz = Load<float>(disc, 0xfc);
      const float x2 = qx + qx;
      const float y2 = qy + qy;
      const float z2 = qz + qz;
      const float wx2 = qw * x2;
      const float wy2 = qw * y2;
      const float wz2 = z2 * qw;
      const float xx2 = x2 * qx;
      const float yy2 = y2 * qy;
      const float zz2 = z2 * qz;
      { const auto key = MakeTStrPrintF<512>("%s[0]", "disc|position"); response.SetReal(key.text, roundMilli(px)); }  // bl 0x13b7e34, 0x13b7e60
      { const auto key = MakeTStrPrintF<512>("%s[1]", "disc|position"); response.SetReal(key.text, roundMilli(py)); }  // bl 0x13b7e78, 0x13b7e90
      { const auto key = MakeTStrPrintF<512>("%s[2]", "disc|position"); response.SetReal(key.text, roundMilli(pz)); }  // bl 0x13b7ea8, 0x13b7ec0
      { const auto key = MakeTStrPrintF<512>("%s[0]", "disc|forward"); response.SetReal(key.text, roundMilli(std::fmaf(qz, x2, -wy2))); }  // bl 0x13b7f24, 0x13b7f3c
      { const auto key = MakeTStrPrintF<512>("%s[1]", "disc|forward"); response.SetReal(key.text, roundMilli(std::fmaf(qz, y2, wx2))); }   // bl 0x13b7f4c, 0x13b7f64
      { const auto key = MakeTStrPrintF<512>("%s[2]", "disc|forward"); response.SetReal(key.text, roundMilliFromTop(yy2 + xx2)); }          // bl 0x13b7f74, 0x13b7f9c
      { const auto key = MakeTStrPrintF<512>("%s[0]", "disc|up"); response.SetReal(key.text, roundMilli(std::fmaf(qy, x2, wz2))); }   // bl 0x13b7fe0, 0x13b7ff8
      { const auto key = MakeTStrPrintF<512>("%s[1]", "disc|up"); response.SetReal(key.text, roundMilliFromTop(zz2 + xx2)); }          // bl 0x13b8008, 0x13b8020
      { const auto key = MakeTStrPrintF<512>("%s[2]", "disc|up"); response.SetReal(key.text, roundMilli(std::fmaf(z2, qy, -wx2))); }  // bl 0x13b8030, 0x13b804c
      { const auto key = MakeTStrPrintF<512>("%s[0]", "disc|left"); response.SetReal(key.text, roundMilliFromTop(zz2 + yy2)); }        // bl 0x13b8084, 0x13b809c
      { const auto key = MakeTStrPrintF<512>("%s[1]", "disc|left"); response.SetReal(key.text, roundMilli(std::fmaf(y2, qx, -wz2))); }  // bl 0x13b80ac, 0x13b80c4
      { const auto key = MakeTStrPrintF<512>("%s[2]", "disc|left"); response.SetReal(key.text, roundMilli(std::fmaf(z2, qx, wy2))); }   // bl 0x13b80d4, 0x13b80ec
    }
    {
      const std::byte* disc = scoreBlock();
      const float vx = Load<float>(disc, 0x104);
      const float vy = Load<float>(disc, 0x108);
      const float vz = Load<float>(disc, 0x10c);
      { const auto key = MakeTStrPrintF<512>("%s[0]", "disc|velocity"); response.SetReal(key.text, roundMilli(vx)); }  // bl 0x13b8114, 0x13b812c
      { const auto key = MakeTStrPrintF<512>("%s[1]", "disc|velocity"); response.SetReal(key.text, roundMilli(vy)); }  // bl 0x13b813c, 0x13b8154
      { const auto key = MakeTStrPrintF<512>("%s[2]", "disc|velocity"); response.SetReal(key.text, roundMilli(vz)); }  // bl 0x13b8168, 0x13b8180
    }
    response.SetInt("disc|bounce_count", Load<std::int64_t>(scoreBlock(), 0x158));  // bl 0x13b8198

    // Entrants: team names by team, per-player possession and stats.
    const char* teamNames[3] = {"", "", ""};  // 0x3731a98 copied to sp+0x280
    std::uint16_t teamCounts[3] = {0, 0, 0};  // sp+0x298..0x29d
    const std::uint16_t entrantCount = Load<std::uint16_t>(net(), kNetEntrantCount);
    for (std::uint64_t i = 0; i != entrantCount; ++i) {
      const std::size_t offset = kNetEntrants + i * kNetEntrantStride;
      const std::uint16_t word = Load<std::uint16_t>(net(), offset);
      const std::uint32_t teamBits = (word >> 10) & 7;
      const std::uint16_t team = teamBits < 5 ? static_cast<std::uint16_t>(teamBits) : kInvalidIndex;
      if (team > 2) {
        continue;  // b.hi 0x13b8824
      }
      CNSLobby* lobbyBase = reinterpret_cast<CNSLobby*>(Load<std::byte*>(net(), kNetLobby) + 0x18);
      const char* name = lobbyBase->EntrantString(word & 0x1f, "teamname", kDefaultTeamNames[team]);  // bl 0x13b8218 (key 0x32e1c2a)
      if (*teamNames[team] != '\0') {
        CSysString::Compare(teamNames[team], name, 1, ~0ull);  // bl 0x13b8240
      }
      const std::uint16_t slotInTeam = teamCounts[team];
      teamNames[team] = name;
      teamCounts[team] = static_cast<std::uint16_t>(slotInTeam + 1);

      std::uint32_t userId = SR15NetUserId::kInvalid;
      if (i < Load<std::uint16_t>(net(), kNetUserCount)) {
        const std::uint32_t slot = Load<std::uint16_t>(net(), offset) & 0x1f;
        userId = slot | (static_cast<std::uint32_t>(Load<std::uint16_t>(net(), slot * 4 + kNetSlotGeneration)) << 16);
      }
      // Stats block of this user; the listing reads it without a null check when the id does not resolve.
      const std::byte* stats = nullptr;
      const std::uint32_t userGeneration = userId >> 16;
      if (userGeneration != kInvalidIndex && (userId & 0xfff0) == 0) {
        const std::uint32_t userSlot = userId & 0xffff;
        if (userGeneration == Load<std::uint16_t>(net(), userSlot * 4 + kNetSlotGeneration)) {
          const std::uint64_t entrantIndex = Load<std::uint16_t>(net(), userSlot * 4 + kNetSlotEntrant);
          if (entrantIndex != kInvalidIndex) {
            const std::uint32_t statSlot = Load<std::uint16_t>(net(), entrantIndex * kNetEntrantStride + kNetEntrants) & 0x1f;
            stats = reinterpret_cast<const std::byte*>(net()) + statSlot * kNetStatsStride + kNetStats;
          }
        }
      }
      // A count stat: word 0 == 5 means sum / count (0 when count is 0); otherwise the sum itself.
      const auto statCount = [stats](std::size_t base) -> std::int64_t {
        std::uint32_t value;
        if (Load<std::uint32_t>(stats, base) == 5) {
          const std::uint32_t divisor = Load<std::uint32_t>(stats, base + 8);
          value = divisor == 0 ? 0 : Load<std::uint32_t>(stats, base + 4) / divisor;
        } else {
          value = Load<std::uint32_t>(stats, base + 4);
        }
        return static_cast<std::int32_t>(value);
      };
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|possession", team, slotInTeam);  // bl 0x13b8310
        response.SetBoolean(key.text, userId == static_cast<std::uint32_t>(ownerSymbol)); }            // bl 0x13b8328
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|possession_time", team, slotInTeam);  // bl 0x13b8340
        float possessionTime;
        if (Load<std::uint32_t>(stats, 0x50c) == 5) {
          const std::uint32_t divisor = Load<std::uint32_t>(stats, 0x514);
          possessionTime = divisor == 0 ? 0.0f : Load<float>(stats, 0x510) / static_cast<float>(divisor);
        } else {
          possessionTime = Load<float>(stats, 0x510);
        }
        response.SetReal(key.text, possessionTime); }  // bl 0x13b837c
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|points", team, slotInTeam);  // bl 0x13b8394
        response.SetInt(key.text, statCount(0x20c)); }  // bl 0x13b83c8
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|goals", team, slotInTeam);  // bl 0x13b83e0
        response.SetInt(key.text, statCount(0x23c)); }  // bl 0x13b8414
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|stuns", team, slotInTeam);  // bl 0x13b842c
        response.SetInt(key.text, statCount(0xec)); }   // bl 0x13b8460
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|blocks", team, slotInTeam);  // bl 0x13b8478
        response.SetInt(key.text, statCount(0x11c)); }  // bl 0x13b84ac
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|assists", team, slotInTeam);  // bl 0x13b84c4
        response.SetInt(key.text, statCount(0x32c)); }  // bl 0x13b84f8
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|saves", team, slotInTeam);  // bl 0x13b8510
        response.SetInt(key.text, statCount(0x35c)); }  // bl 0x13b8544
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|interceptions", team, slotInTeam);  // bl 0x13b855c
        response.SetInt(key.text, statCount(0x4ac)); }  // bl 0x13b8590
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|steals", team, slotInTeam);  // bl 0x13b85a8
        response.SetInt(key.text, statCount(0x2fc)); }  // bl 0x13b85dc
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|catches", team, slotInTeam);  // bl 0x13b85f4
        response.SetInt(key.text, statCount(0x44c)); }  // bl 0x13b8628
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|passes", team, slotInTeam);  // bl 0x13b8640
        response.SetInt(key.text, statCount(0x41c)); }  // bl 0x13b8674
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats|shots_taken", team, slotInTeam);  // bl 0x13b868c
        response.SetInt(key.text, statCount(0x3bc)); }  // bl 0x13b86c0

      if (userId == static_cast<std::uint32_t>(ownerSymbol)) {
        response.SetInt("possession[0]", team != kInvalidIndex ? static_cast<std::int64_t>(team) : -1);              // bl 0x13b86e4
        response.SetInt("possession[1]", slotInTeam != kInvalidIndex ? static_cast<std::int64_t>(slotInTeam) : -1);  // bl 0x13b86fc
      }

      // Punch state of the entrant's player actor.
      SActorGUID guid = Load<SActorGUID>(net(), offset + 8);
      ResolveActorAlias(Load<CncaGame*>(net(), kNetGame), &guid);  // bl 0x13b871c
      void* gameSpace = Load<CncaGame*>(ctx, kContextCncaGame)->GameSpaceMT(Load<std::uint64_t>(guid.raw, 0));  // bl 0x13b872c
      if (gameSpace == nullptr) {
        continue;
      }
      CLocalComponentHandle<CR15NetPunchableCS> punch;
      punch.Find(gameSpace, Load<std::uint64_t>(guid.raw, 8), kNoInstance);  // bl 0x13b8754
      CR15NetPunchableCS* punchable = Load<CR15NetPunchableCS*>(punch.raw, 0);
      if (punchable == nullptr) {
        continue;
      }
      if (punchable->IsComponentValid(Load<std::uint32_t>(punch.raw, 8)) == 0) {  // blr 0x13b876c, vtable +0x1d0
        continue;
      }
      // CR15NetPunchableCS has no layout in ReVault: +0xc8 holds u16 row indices (stride 4), +0xf8 the 0x48-byte rows.
      const std::uint16_t row = Load<std::uint16_t>(Load<const std::byte*>(punchable, 0xc8), static_cast<std::size_t>(Load<std::uint16_t>(punch.raw, 8)) * 4);
      const std::uint32_t punchFlags = Load<std::uint32_t>(Load<const std::byte*>(punchable, 0xf8) + row * 0x48, 4);
      const bool stunned = ((punchFlags >> 1) & 1) != 0;
      const bool invulnerable = ((punchFlags >> 2) & 1) != 0;
      const bool blocking = (punchFlags & 0xe0) != 0;
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stunned", team, slotInTeam);  // bl 0x13b87bc
        response.SetBoolean(key.text, stunned); }  // bl 0x13b87cc
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|invulnerable", team, slotInTeam);  // bl 0x13b87e4
        response.SetBoolean(key.text, invulnerable); }  // bl 0x13b87f4
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|blocking", team, slotInTeam);  // bl 0x13b880c
        response.SetBoolean(key.text, blocking); }  // bl 0x13b8820
    }

    // Per-team possession flag and the player stats summed into the team stats.
    const std::uint32_t ownerId = static_cast<std::uint32_t>(ownerSymbol);
    const bool ownerValid = (ownerId & 0xfff0) == 0 && (ownerId >> 16) != kInvalidIndex;  // 0x13b8850..0x13b8868
    const auto ownerOnTeam = [&net, ownerId, ownerValid](std::uint32_t team) -> bool {
      if (!ownerValid) {
        return false;  // 0x13b88ac: SetBoolean(..., 0)
      }
      CR15NetGame* game = net();
      const std::uint32_t ownerSlot = ownerId & 0xffff;
      if ((ownerId >> 16) != Load<std::uint16_t>(game, ownerSlot * 4 + kNetSlotGeneration)) {
        return false;  // 0x13b8a78
      }
      const std::uint64_t entrantIndex = Load<std::uint16_t>(game, ownerSlot * 4 + kNetSlotEntrant);
      if (entrantIndex == kInvalidIndex) {
        return false;
      }
      // team 0: tst byte +0x179, #0x1c; teams 1 and 2: (word & 0x1c00) == 0x400 / 0x800
      return (Load<std::uint16_t>(game, entrantIndex * kNetEntrantStride + kNetEntrants) & 0x1c00) == (team << 10);
    };
    for (std::uint32_t team = 0; team < 3; ++team) {
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|possession", team);  // bl 0x13b8854, 0x13b894c, 0x13b89e8, 0x13b8b18, 0x13b8c00 (the listing holds one copy per owner-valid case)
        response.SetBoolean(key.text, ownerOnTeam(team)); }  // bl 0x13b88b8, 0x13b8a84, 0x13b895c, 0x13b89f8, 0x13b8b6c, 0x13b8c54
      for (std::uint32_t player = 0; player != teamCounts[team]; ++player) {
        CJsonAccumulator accumulator;
        const auto teamStats = MakeTStrPrintF<512>("teams[%hu]|stats", team);  // bl 0x13b8904, 0x13b89a0, 0x13b8a3c, 0x13b8ad0, 0x13b8bb8, 0x13b8ca0
        const auto playerStats = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|stats", team, player);  // bl 0x13b8918, 0x13b89b4, 0x13b8a50, 0x13b8ae4, 0x13b8bcc, 0x13b8cb4
        accumulator.Traverse(response, teamStats.text, playerStats.text);  // bl 0x13b892c, 0x13b89c8, 0x13b8a64, 0x13b8af8, 0x13b8be0, 0x13b8cc8
      }
    }
  } else if (Load<CR15NetLobby*>(net(), kNetLobby) != nullptr &&
             IsOneOf(Load<CR15NetLobby*>(net(), kNetLobby)->GameType(), kCombatGameTypes)) {  // 0x13b8d84..0x13b8e40; blr 0x13b8d98
    // ---- combat: equipment names per player, then the payload or capture block of the level ----
    const char* teamNames[3] = {"", "", ""};  // 0x3731a98 copied to sp+0x280
    std::uint16_t teamCounts[3] = {0, 0, 0};  // sp+0x298..0x29d
    const std::uint16_t entrantCount = Load<std::uint16_t>(net(), kNetEntrantCount);
    for (std::uint64_t i = 0; i != entrantCount; ++i) {
      const std::size_t offset = kNetEntrants + i * kNetEntrantStride;
      const std::uint16_t word = Load<std::uint16_t>(net(), offset);
      const std::uint32_t teamBits = (word >> 10) & 7;
      const std::uint16_t team = teamBits < 5 ? static_cast<std::uint16_t>(teamBits) : kInvalidIndex;
      if (team > 2) {
        continue;  // b.hi 0x13b8e98
      }
      CNSLobby* lobbyBase = reinterpret_cast<CNSLobby*>(Load<std::byte*>(net(), kNetLobby) + 0x18);
      const char* name = lobbyBase->EntrantString(word & 0x1f, "teamname", kDefaultTeamNames[team]);  // bl 0x13b8ee8 (key 0x32e1c2a)
      if (*teamNames[team] != '\0') {
        CSysString::Compare(teamNames[team], name, 1, ~0ull);  // bl 0x13b8f0c
      }
      const std::uint16_t slotInTeam = teamCounts[team];
      teamNames[team] = name;
      teamCounts[team] = static_cast<std::uint16_t>(slotInTeam + 1);

      SActorGUID guid = Load<SActorGUID>(net(), offset + 8);  // ldr q0, [net + i * 0x250 + 0x180]: the actor id, record + 8
      ResolveActorAlias(Load<CncaGame*>(net(), kNetGame), &guid);  // bl 0x13b8f40
      void* gameSpace = Load<CncaGame*>(ctx, kContextCncaGame)->GameSpaceMT(Load<std::uint64_t>(guid.raw, 0));  // bl 0x13b8f50
      if (gameSpace == nullptr) {
        continue;
      }
      CLocalComponentHandle<CR15NetPlayerEquipmentCS> equipment;
      equipment.Find(gameSpace, Load<std::uint64_t>(guid.raw, 8), kNoInstance);  // bl 0x13b8f80
      CR15NetPlayerEquipmentCS* equipmentCs = Load<CR15NetPlayerEquipmentCS*>(equipment.raw, 0);
      if (equipmentCs == nullptr) {
        continue;
      }
      if (equipmentCs->IsComponentValid(Load<std::uint32_t>(equipment.raw, 8)) == 0) {  // blr 0x13b8f98, vtable +0x1d0
        continue;
      }
      const std::uint32_t equipmentId = Load<std::uint32_t>(equipment.raw, 8);
      const std::uint64_t weaponSlot = equipmentCs->GetWeaponSlot(equipmentId);    // bl 0x13b8fbc
      const std::uint64_t weaponArm = equipmentCs->GetWeaponArm(equipmentId);      // bl 0x13b8fdc
      const std::uint64_t abilitySlot = equipmentCs->GetAbilitySlot(equipmentId);  // bl 0x13b8ff8
      const std::uint64_t grenadeSlot = equipmentCs->GetGrenadeSlot(equipmentId);  // bl 0x13b9018
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|Weapon", team, slotInTeam);  // bl 0x13b9034
        response.SetString(key.text, CR15NetProfileSymbolLookup::WeaponName(weaponSlot)); }  // bl 0x13b9044, 0x13b9054
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|Arm", team, slotInTeam);  // bl 0x13b906c
        response.SetString(key.text, weaponArm == 1 ? "Right" : (weaponArm == 0 ? "Left" : "Invalid")); }  // bl 0x13b90a0 (0x331c405, 0x331c3fa, 0x2cb0232)
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|TacMod", team, slotInTeam);  // bl 0x13b90b8
        response.SetString(key.text, CR15NetProfileSymbolLookup::TacModName(abilitySlot)); }  // bl 0x13b90c0, 0x13b90d0
      { const auto key = MakeTStrPrintF<512>("teams[%hu]|players[%hu]|Ordnance", team, slotInTeam);  // bl 0x13b90e8
        response.SetString(key.text, CR15NetProfileSymbolLookup::OrdnanceName(grenadeSlot)); }  // bl 0x13b90f0, 0x13b9100
    }

    // Level name of the lobby (all ones when there is no lobby).
    const auto levelName = [&net]() -> std::uint64_t {
      CR15NetLobby* current = Load<CR15NetLobby*>(net(), kNetLobby);
      return current == nullptr ? kNoInstance : current->LevelName();  // blr 0x13b9154, 0x13b9670
    };
    if (IsOneOf(levelName(), kPayloadLevels)) {  // 0x13b91a0..0x13b95c8
      // Payload: checkpoint, claim state, distance, claimers, speed and position of the payload actor, then the timers.
      void* components = Load<void*>(net(), kNetComponents);
      CComponentHandle<CR15NetPayloadCS> payload;  // sp+0x60
      payload.Find(components, DAT_0386d340, kNoInstance);  // bl 0x13b9228
      CComponentHandle<CR15NetPhysicsCS> physics;  // sp+0x2a0
      physics.Find(components, DAT_0386d340, kNoInstance);  // bl 0x13b9264
      payload.RefreshCache();  // bl 0x13b926c
      CR15NetPayloadCS* payloadCs = Load<CR15NetPayloadCS*>(payload.raw, kHandleCache);
      if (payloadCs != nullptr && Load<std::uint64_t>(payload.raw, 0) != 0) {
        payload.RefreshCache();  // bl 0x13b9284
        if (payloadCs->IsComponentValid(Load<std::uint32_t>(payload.raw, kHandleId)) != 0) {  // blr 0x13b9298, vtable +0x1d0
          payload.RefreshCache();  // bl 0x13b92a4
          payloadCs = Load<CR15NetPayloadCS*>(payload.raw, kHandleCache);
          payload.RefreshCache();  // bl 0x13b92b0
          const std::uint32_t payloadId = Load<std::uint32_t>(payload.raw, kHandleId);
          response.SetInt("payload|checkpoint", payloadCs->Checkpoint(payloadId));  // bl 0x13b92c0, 0x13b92d4
          response.SetBoolean("contested", payloadCs->ClaimState(payloadId) == DAT_0386d348);  // bl 0x13b92e0, 0x13b9300 (key 0x2caded0)
          float distance = payloadCs->DistAlongTrack(payloadId);  // bl 0x13b930c
          if (!(std::fabs(distance) > 0.002f)) {
            distance = 0.0f;
          }
          response.SetReal("payload|distance", distance);  // bl 0x13b9338
          const std::uint32_t attackers = payloadCs->NumAttackersClaiming(payloadId);  // bl 0x13b9344
          const std::uint32_t defenders = payloadCs->NumDefendersClaiming(payloadId);  // bl 0x13b9354
          const std::int8_t direction = payloadCs->CachedDirectionMovementState(payloadId);  // bl 0x13b9364
          const std::int64_t picked = static_cast<std::uint8_t>(direction) == 1 ? static_cast<std::int64_t>(attackers & 0xffff)
                                                                                : static_cast<std::int64_t>(direction);
          response.SetInt("payload|multiplier", static_cast<std::int16_t>(picked));  // bl 0x13b9390
          response.SetInt("payload|attackers", attackers & 0xffff);                  // bl 0x13b93a4
          response.SetInt("payload|defenders", defenders & 0xffff);                  // bl 0x13b93b8
          physics.RefreshCache();  // bl 0x13b93c0
          CR15NetPhysicsCS* physicsCs = Load<CR15NetPhysicsCS*>(physics.raw, kHandleCache);
          if (physicsCs != nullptr && Load<std::uint64_t>(physics.raw, 0) != 0) {
            physics.RefreshCache();  // bl 0x13b93d8
            if (physicsCs->IsComponentValid(Load<std::uint32_t>(physics.raw, kHandleId)) != 0) {  // blr 0x13b93ec, vtable +0x1d0
              physics.RefreshCache();  // bl 0x13b93f8
              physicsCs = Load<CR15NetPhysicsCS*>(physics.raw, kHandleCache);
              physics.RefreshCache();  // bl 0x13b9404
              const std::uint32_t physicsId = Load<std::uint32_t>(physics.raw, kHandleId);
              float speed = physicsCs->Speed(physicsId);  // bl 0x13b9414
              if (speed > 0.0f && direction == -1) {
                speed = -speed;  // fcsel on (speed > 0 && direction == -1)
              }
              response.SetReal("payload|speed", speed);  // bl 0x13b943c
              const Float3 position = physicsCs->SmoothedWorldPosition(physicsId);  // bl 0x13b9448
              { const auto key = MakeTStrPrintF<512>("%s[0]", "payload|pos"); response.SetReal(key.text, std::floor(std::fmaf(position.x, 1000.0f, 0.5f)) * 0.001f); }  // bl 0x13b9474, 0x13b94a0
              { const auto key = MakeTStrPrintF<512>("%s[1]", "payload|pos"); response.SetReal(key.text, std::floor(std::fmaf(position.y, 1000.0f, 0.5f)) * 0.001f); }  // bl 0x13b94b4, 0x13b94cc
              { const auto key = MakeTStrPrintF<512>("%s[2]", "payload|pos"); response.SetReal(key.text, std::floor(std::fmaf(position.z, 1000.0f, 0.5f)) * 0.001f); }  // bl 0x13b94e0, 0x13b94f8
            }
          }
        }
      }
      // Timers: the game timer and the overtime timer (the overtime flag scales a map-settings value).
      const float gameTimer = CR15NetVarCS::Real(net(), DAT_0386d378);  // bl 0x13b950c
      float overtimeTimer = 0.0f;
      if (CR15NetVarCS::Flag(net(), DAT_0386d380) != 0) {  // bl 0x13b951c
        const float overtimeBase = CR15NetVarCS::Real(net(), DAT_0386d388);  // bl 0x13b9530
        const std::uint64_t mapSettings = CSymbol64::Append(levelName(), "_map_settings", 1, 1);  // blr 0x13b954c, bl 0x13b9570 (0x2cb035b)
        overtimeTimer = CR15NetBalanceSettingsCS::Real(mapSettings, DAT_0386d390, 0.0f) * overtimeBase;  // bl 0x13b9580
      }
      response.SetReal("timers|game_timer", gameTimer);        // bl 0x13b9598
      response.SetReal("timers|overtime_timer", overtimeTimer);  // bl 0x13b95ac
    } else if (IsOneOf(levelName(), kCaptureLevels)) {  // blr 0x13b9670; compares 0x13b9680..0x13b96a8
      // Capture: contested flag, scaled points and round scores.
      response.SetBoolean("contested", CR15NetVarCS::Symbol(net(), DAT_0386d350) == DAT_0386d348);  // bl 0x13b96c0, 0x13b96dc
      const float orangeScale = CR15NetVarCS::Real(net(), DAT_0386d358);  // bl 0x13b96e8
      const float blueScale = CR15NetVarCS::Real(net(), DAT_0386d360);    // bl 0x13b96f8
      response.SetReal("orange_points", orangeScale * 100.0f);  // bl 0x13b9718
      response.SetReal("blue_points", blueScale * 100.0f);      // bl 0x13b972c
      const std::int32_t blueRoundScore = CR15NetVarCS::Int(net(), DAT_0386d370);    // bl 0x13b9738
      const std::int32_t orangeRoundScore = CR15NetVarCS::Int(net(), DAT_0386d368);  // bl 0x13b974c
      response.SetInt("blue_round_score", blueRoundScore);      // bl 0x13b9764
      response.SetInt("orange_round_score", orangeRoundScore);  // bl 0x13b9778
    }
  }
  return 0;  // 0x13b8cd8
}

}  // namespace NR15RestAPI
}  // namespace NRadGame
}  // namespace NRadEngine

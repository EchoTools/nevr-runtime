#include "telemetry_streamer.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "echovr.h"
#include "echovr_functions.h"
#include "telemetry/v2/capture.pb.h"
#include "telemetry/v2/echo_arena.pb.h"

extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

// ============================================================================
// Lifecycle
// ============================================================================

TelemetryStreamer::TelemetryStreamer() {
  m_snapshots[0].Clear();
  m_snapshots[1].Clear();
  m_prevSnapshot.Clear();
}

TelemetryStreamer::~TelemetryStreamer() {
  Stop();
  Disconnect();
}

bool TelemetryStreamer::Connect(const std::string& uri) {
  if (uri.empty()) return false;

  m_ws = std::make_unique<ix::WebSocket>();
  m_ws->setUrl(uri);

  m_ws->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open:
        Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Connected to telemetry server");
        m_wsConnected.store(true, std::memory_order_release);
        break;
      case ix::WebSocketMessageType::Close:
        Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Disconnected from telemetry server (code: %d)",
            msg->closeInfo.code);
        m_wsConnected.store(false, std::memory_order_release);
        break;
      case ix::WebSocketMessageType::Error:
        Log(EchoVR::LogLevel::Error, "[NEVR.TELEMETRY] Connection error: %s", msg->errorInfo.reason.c_str());
        m_wsConnected.store(false, std::memory_order_release);
        break;
      case ix::WebSocketMessageType::Message:
        // Telemetry server responses (acks) — currently just log
        Log(EchoVR::LogLevel::Debug, "[NEVR.TELEMETRY] Received response (%zu bytes)", msg->str.size());
        break;
      default:
        break;
    }
  });

  Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Connecting to %s", uri.c_str());
  m_ws->start();
  return true;
}

void TelemetryStreamer::Start(const std::string& sessionId, uint32_t rateHz) {
  if (m_active.load(std::memory_order_relaxed)) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.TELEMETRY] Already streaming, stopping previous session");
    Stop();
  }

  m_sessionId = sessionId;
  m_rateHz = rateHz > 0 ? rateHz : 10;
  m_frameIndex = 0;
  m_sessionStartTime = std::chrono::steady_clock::now();
  m_lastSnapshotTime = m_sessionStartTime;
  m_prevSnapshot.Clear();
  m_snapshotReady.store(false, std::memory_order_relaxed);
  m_stopping.store(false, std::memory_order_relaxed);

  m_active.store(true, std::memory_order_release);
  m_thread = std::thread(&TelemetryStreamer::Run, this);

  Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Started streaming session=%s at %uHz", sessionId.c_str(), m_rateHz);
}

void TelemetryStreamer::Stop() {
  if (!m_active.load(std::memory_order_relaxed)) return;

  Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Stopping telemetry stream");
  m_stopping.store(true, std::memory_order_release);
  m_active.store(false, std::memory_order_release);

  if (m_thread.joinable()) {
    m_thread.join();
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Telemetry stream stopped (sent %u frames)", m_frameIndex);
}

void TelemetryStreamer::Disconnect() {
  if (m_ws) {
    Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Disconnecting from telemetry server");
    m_ws->stop();
    m_ws.reset();
    m_wsConnected.store(false, std::memory_order_relaxed);
  }
}

bool TelemetryStreamer::IsConnected() const { return m_wsConnected.load(std::memory_order_relaxed); }

// ============================================================================
// Game thread: SnapshotIfDue — called every Update() tick
// ============================================================================

void TelemetryStreamer::SnapshotIfDue() {
  if (!m_active.load(std::memory_order_relaxed)) return;

  auto now = std::chrono::steady_clock::now();
  auto interval = std::chrono::microseconds(1000000 / m_rateHz);
  if (now - m_lastSnapshotTime < interval) return;

  m_lastSnapshotTime = now;

  // Write to the current write buffer
  int writeIdx = m_writeIndex.load(std::memory_order_relaxed);
  TelemetrySnapshot* snap = &m_snapshots[writeIdx];
  snap->Clear();

  SnapshotGameState(snap);
  SnapshotPlayerBones(snap);

  // Swap: toggle write index so telemetry thread reads the one we just wrote
  int newWriteIdx = 1 - writeIdx;
  m_writeIndex.store(newWriteIdx, std::memory_order_relaxed);
  m_snapshotReady.store(true, std::memory_order_release);
}

void TelemetryStreamer::PushEvent(const TelemetryEvent& event) { m_eventBuffer.Push(event); }

void TelemetryStreamer::ProcessResponses() {
  // Currently a no-op — telemetry server responses are handled in the WS callback.
  // This method exists so the game thread Update() loop has a consistent call pattern.
}

// ============================================================================
// Game thread: Snapshot capture
// ============================================================================

void TelemetryStreamer::ResolveFunctionPointers() {
  if (m_funcPtrsResolved) return;

  CHAR* base = EchoVR::g_GameBaseAddress;
  if (!base) return;

  m_getSymbolHash = reinterpret_cast<GetSymbolHashFn>(base + GameFuncAddr::GET_SYMBOL_HASH);
  m_getFloatProperty = reinterpret_cast<GetFloatPropertyFn>(base + GameFuncAddr::GET_FLOAT_PROPERTY);
  m_getIntProperty = reinterpret_cast<GetIntPropertyFn>(base + GameFuncAddr::GET_INT_PROPERTY);
  m_entityLookup = reinterpret_cast<EntityLookupFn>(base + GameFuncAddr::ENTITY_LOOKUP);
  m_resolveEntityHandle = reinterpret_cast<ResolveEntityHandleFn>(base + GameFuncAddr::RESOLVE_ENTITY_HANDLE);
  m_getBoneData = reinterpret_cast<GetBoneDataFn>(base + GameFuncAddr::GET_BONE_DATA);
  m_getTransformComponent = reinterpret_cast<GetTransformComponentFn>(base + GameFuncAddr::GET_TRANSFORM_COMPONENT);

  m_funcPtrsResolved = true;
  Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Game function pointers resolved");
}

bool TelemetryStreamer::ResolveGamePointers() {
  CHAR* base = EchoVR::g_GameBaseAddress;
  if (!base) return false;

  void** contextPtr = reinterpret_cast<void**>(base + GameOffsets::GAME_CONTEXT_OFFSET);
  void* gameContext = *contextPtr;
  if (!gameContext) return false;

  CHAR* contextBase = reinterpret_cast<CHAR*>(gameContext);
  m_netGame = *reinterpret_cast<void**>(contextBase + GameOffsets::NETGAME_OFFSET);
  if (!m_netGame) return false;

  CHAR* netGameBase = reinterpret_cast<CHAR*>(m_netGame);
  m_gameStateData = *reinterpret_cast<void**>(netGameBase + GameOffsets::GAME_STATE_DATA_OFFSET);

  // Entity system pointers
  m_handleResolver = *reinterpret_cast<void**>(netGameBase + GameOffsets::ENTITY_HANDLE_RESOLVER_OFFSET);
  void* worldObj = *reinterpret_cast<void**>(netGameBase + GameOffsets::WORLD_OBJECT_OFFSET);
  if (worldObj) {
    m_entityManager = *reinterpret_cast<void**>(reinterpret_cast<CHAR*>(worldObj) + GameOffsets::ENTITY_MANAGER_OFFSET);
  } else {
    m_entityManager = nullptr;
  }

  return m_gameStateData != nullptr;
}

void* TelemetryStreamer::ResolvePlayerEntity(uint64_t accountId) {
  if (!m_resolveEntityHandle || !m_entityLookup || !m_handleResolver || !m_entityManager) return nullptr;

  // FUN_140363610 expects a pair: [accountId, extra_data]
  // It modifies pair[0] to the transform handle on success
  int64_t pair[2] = {static_cast<int64_t>(accountId), 0};
  int result = m_resolveEntityHandle(m_handleResolver, pair);
  if (result == 0 || pair[0] == -1) return nullptr;

  // Look up entity by the resolved handle
  void* entity = m_entityLookup(m_entityManager, pair[0]);
  return entity;
}

void TelemetryStreamer::SnapshotGameState(TelemetrySnapshot* snap) {
  ResolveFunctionPointers();

  if (!ResolveGamePointers()) return;

  CHAR* netGameBase = reinterpret_cast<CHAR*>(m_netGame);
  CHAR* gsdBase = reinterpret_cast<CHAR*>(m_gameStateData);

  // --- Game function calls (game thread only) ---
  if (m_getSymbolHash) {
    snap->gameStatusHash = m_getSymbolHash(m_netGame, PropertyHash::GAME_STATUS);
  }
  if (m_getFloatProperty) {
    snap->gameClock = m_getFloatProperty(m_netGame, PropertyHash::GAME_CLOCK);
  }
  if (m_getIntProperty && m_gameStateData) {
    snap->bluePoints = m_getIntProperty(m_gameStateData, PropertyHash::BLUE_POINTS);
    snap->orangePoints = m_getIntProperty(m_gameStateData, PropertyHash::ORANGE_POINTS);
  }

  // --- Direct memory reads ---

  // Player count
  snap->playerCount = *reinterpret_cast<uint16_t*>(netGameBase + GameOffsets::PLAYER_COUNT_OFFSET);
  if (snap->playerCount > 16) snap->playerCount = 16;

  // Player info array
  for (uint16_t i = 0; i < snap->playerCount; i++) {
    CHAR* playerBase = netGameBase + GameOffsets::PLAYER_ARRAY_OFFSET + i * GameOffsets::PLAYER_STRIDE;
    auto& info = snap->playerInfo[i];

    info.flags = *reinterpret_cast<uint16_t*>(playerBase + GameOffsets::PLAYER_FLAGS_OFFSET);
    info.accountId = *reinterpret_cast<uint64_t*>(playerBase + GameOffsets::PLAYER_ACCOUNT_ID_OFFSET);
    std::memcpy(info.displayName, playerBase + GameOffsets::PLAYER_DISPLAY_NAME_OFFSET, 36);
    info.displayName[35] = '\0';
    info.ping = *reinterpret_cast<uint16_t*>(playerBase + GameOffsets::PLAYER_PING_OFFSET);
    info.teamIndex = *reinterpret_cast<uint16_t*>(playerBase + GameOffsets::PLAYER_TEAM_INDEX_OFFSET);
  }

  // Player stats
  for (uint16_t i = 0; i < snap->playerCount; i++) {
    uint16_t teamRole = snap->playerInfo[i].flags & 0x1F;
    CHAR* statsBase = netGameBase + GameOffsets::STATS_BASE_OFFSET + teamRole * GameOffsets::STATS_STRIDE;
    auto& s = snap->stats[i];

    // Each stat entry: [4B type][4B count][8B value] — we read the count field at offset +4
    s.points = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_POINTS + 4);
    s.goals = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_GOALS + 4);
    s.assists = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_ASSISTS + 4);
    s.stuns = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_STUNS + 4);
    s.blocks = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_BLOCKS + 4);
    s.saves = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_SAVES + 4);
    s.interceptions = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_INTERCEPTIONS + 4);
    s.steals = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_STEALS + 4);
    s.catches = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_CATCHES + 4);
    s.passes = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_PASSES + 4);
    s.shotsTaken = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_SHOTS_TAKEN + 4);
    s.possessionTime = *reinterpret_cast<uint32_t*>(statsBase + GameOffsets::STAT_POSSESSION_TIME + 4);
  }

  // Disc state
  std::memcpy(snap->discOrient, gsdBase + GameOffsets::DISC_ORIENT_OFFSET, sizeof(float) * 4);
  std::memcpy(snap->discPos, gsdBase + GameOffsets::DISC_POS_OFFSET, sizeof(float) * 3);
  std::memcpy(snap->discVel, gsdBase + GameOffsets::DISC_VEL_OFFSET, sizeof(float) * 3);
  snap->discBounceCount = *reinterpret_cast<uint64_t*>(gsdBase + GameOffsets::DISC_BOUNCE_OFFSET);

  // Last score data
  snap->lastScore.team = *reinterpret_cast<int16_t*>(gsdBase + GameOffsets::LAST_SCORE_TEAM_OFFSET);
  snap->lastScore.pointAmount = *reinterpret_cast<uint16_t*>(gsdBase + GameOffsets::LAST_SCORE_POINT_AMOUNT_OFFSET);
  snap->lastScore.discSpeed = *reinterpret_cast<float*>(gsdBase + GameOffsets::LAST_SCORE_DISC_SPEED_OFFSET);
  snap->lastScore.distanceThrown = *reinterpret_cast<float*>(gsdBase + GameOffsets::LAST_SCORE_DISTANCE_THROWN_OFFSET);
  snap->lastScore.assistPacked = *reinterpret_cast<uint32_t*>(gsdBase + GameOffsets::LAST_SCORE_ASSIST_PACKED_OFFSET);
  snap->lastScore.scorerPacked = *reinterpret_cast<uint32_t*>(gsdBase + GameOffsets::LAST_SCORE_SCORER_PACKED_OFFSET);

  // Player transforms via entity system
  // For each player, resolve their entity and read transform component data.
  // The transform component (FUN_14042d690) returns a pointer to transform data
  // containing position and orientation. We read bone 0 (root) for body position.
  for (uint16_t i = 0; i < snap->playerCount; i++) {
    if (snap->playerInfo[i].accountId == 0) continue;

    void* entity = ResolvePlayerEntity(snap->playerInfo[i].accountId);
    if (!entity) continue;

    // Get transform component for body pose
    // FUN_14042d690 returns ptr to transform data (position at +0x00, orientation at +0x0C or similar)
    if (m_getTransformComponent) {
      // Bone slot 0 = root/body transform
      void* transformData = m_getTransformComponent(entity, 0);
      if (transformData) {
        // Transform data layout: position float[3] at +0x00, orientation quat float[4] at +0x0C
        float* fdata = reinterpret_cast<float*>(transformData);
        snap->players[i].bodyPos[0] = fdata[0];
        snap->players[i].bodyPos[1] = fdata[1];
        snap->players[i].bodyPos[2] = fdata[2];
        snap->players[i].bodyRot[0] = fdata[3];
        snap->players[i].bodyRot[1] = fdata[4];
        snap->players[i].bodyRot[2] = fdata[5];
        snap->players[i].bodyRot[3] = fdata[6];
      }
    }

    // Read head/hand positions from bone data (more reliable than transform component)
    // Bone layout per FUN_1408f8090: returns ptr, bone data at various offsets
    if (m_getBoneData) {
      // Head bone (bone 4 in Echo VR skeleton)
      void* headBone = m_getBoneData(entity, 4);
      if (headBone) {
        // Bone data: orientation quat float[4] then position float[3]
        // Offset 0x18 from bone base is where the spectator stream reads from
        float* boneFloats = reinterpret_cast<float*>(reinterpret_cast<CHAR*>(headBone) + 0x18);
        snap->players[i].headRot[0] = boneFloats[0];
        snap->players[i].headRot[1] = boneFloats[1];
        snap->players[i].headRot[2] = boneFloats[2];
        snap->players[i].headRot[3] = boneFloats[3];
        snap->players[i].headPos[0] = boneFloats[4];
        snap->players[i].headPos[1] = boneFloats[5];
        snap->players[i].headPos[2] = boneFloats[6];
      }

      // Left hand bone (bone 10)
      void* lhBone = m_getBoneData(entity, 10);
      if (lhBone) {
        float* boneFloats = reinterpret_cast<float*>(reinterpret_cast<CHAR*>(lhBone) + 0x18);
        snap->players[i].leftHandRot[0] = boneFloats[0];
        snap->players[i].leftHandRot[1] = boneFloats[1];
        snap->players[i].leftHandRot[2] = boneFloats[2];
        snap->players[i].leftHandRot[3] = boneFloats[3];
        snap->players[i].leftHandPos[0] = boneFloats[4];
        snap->players[i].leftHandPos[1] = boneFloats[5];
        snap->players[i].leftHandPos[2] = boneFloats[6];
      }

      // Right hand bone (bone 16)
      void* rhBone = m_getBoneData(entity, 16);
      if (rhBone) {
        float* boneFloats = reinterpret_cast<float*>(reinterpret_cast<CHAR*>(rhBone) + 0x18);
        snap->players[i].rightHandRot[0] = boneFloats[0];
        snap->players[i].rightHandRot[1] = boneFloats[1];
        snap->players[i].rightHandRot[2] = boneFloats[2];
        snap->players[i].rightHandRot[3] = boneFloats[3];
        snap->players[i].rightHandPos[0] = boneFloats[4];
        snap->players[i].rightHandPos[1] = boneFloats[5];
        snap->players[i].rightHandPos[2] = boneFloats[6];
      }
    }
  }
}

void TelemetryStreamer::SnapshotPlayerBones(TelemetrySnapshot* snap) {
  if (!m_getBoneData || !m_entityManager) return;

  for (uint16_t i = 0; i < snap->playerCount; i++) {
    if (snap->playerInfo[i].accountId == 0) continue;

    void* entity = ResolvePlayerEntity(snap->playerInfo[i].accountId);
    if (!entity) continue;

    auto& boneData = snap->playerBones[i];

    // Read all 23 bones
    bool allValid = true;
    for (uint32_t b = 0; b < 23; b++) {
      void* bone = m_getBoneData(entity, static_cast<uint16_t>(b));
      if (!bone) {
        allValid = false;
        break;
      }

      // Bone data at offset 0x18: [orient_x, orient_y, orient_z, orient_w, pos_x, pos_y, pos_z]
      float* boneFloats = reinterpret_cast<float*>(reinterpret_cast<CHAR*>(bone) + 0x18);
      boneData.bones[b][0] = boneFloats[0];  // orient x
      boneData.bones[b][1] = boneFloats[1];  // orient y
      boneData.bones[b][2] = boneFloats[2];  // orient z
      boneData.bones[b][3] = boneFloats[3];  // orient w
      boneData.bones[b][4] = boneFloats[4];  // pos x
      boneData.bones[b][5] = boneFloats[5];  // pos y
      boneData.bones[b][6] = boneFloats[6];  // pos z
    }

    boneData.valid = allValid;
  }
}

// ============================================================================
// Telemetry thread
// ============================================================================

void TelemetryStreamer::Run() {
  Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Telemetry thread started");

  // Wait for connection before sending header
  auto connectWaitStart = std::chrono::steady_clock::now();
  while (m_active.load(std::memory_order_relaxed) && !m_wsConnected.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() - connectWaitStart > std::chrono::seconds(10)) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.TELEMETRY] Timeout waiting for telemetry server connection");
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  if (m_wsConnected.load(std::memory_order_acquire)) {
    SendHeader();
  }

  auto frameInterval = std::chrono::microseconds(1000000 / m_rateHz);

  while (m_active.load(std::memory_order_acquire)) {
    auto frameStart = std::chrono::steady_clock::now();

    // Check if a new snapshot is ready
    if (m_snapshotReady.exchange(false, std::memory_order_acquire)) {
      // Read from the buffer the game thread is NOT writing to
      int readIdx = 1 - m_writeIndex.load(std::memory_order_relaxed);
      const TelemetrySnapshot& snap = m_snapshots[readIdx];

      if (m_wsConnected.load(std::memory_order_relaxed)) {
        BuildAndSendFrame(snap);
      }

      m_prevSnapshot = snap;
    }

    // Sleep until next frame interval
    auto elapsed = std::chrono::steady_clock::now() - frameStart;
    if (elapsed < frameInterval) {
      std::this_thread::sleep_for(frameInterval - elapsed);
    }
  }

  // Send footer on stop
  if (m_wsConnected.load(std::memory_order_relaxed)) {
    SendFooter();
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Telemetry thread exiting");
}

// ============================================================================
// Frame assembly + sending
// ============================================================================

int32_t TelemetryStreamer::MapGameStatus(uint64_t hash) {
  using namespace GameStatusHash;
  switch (hash) {
    case PRE_MATCH_1:
    case PRE_MATCH_2:
    case PRE_MATCH_3:
      return static_cast<int32_t>(telemetry::v2::GAME_STATUS_PRE_MATCH);
    case ROUND_START_1:
    case ROUND_START_2:
    case ROUND_START_3:
      return static_cast<int32_t>(telemetry::v2::GAME_STATUS_ROUND_START);
    case PLAYING_1:
    case PLAYING_2:
      return static_cast<int32_t>(telemetry::v2::GAME_STATUS_PLAYING);
    case SCORE_1:
    case SCORE_2:
      return static_cast<int32_t>(telemetry::v2::GAME_STATUS_SCORE);
    case ROUND_OVER_1:
    case ROUND_OVER_2:
      return static_cast<int32_t>(telemetry::v2::GAME_STATUS_ROUND_OVER);
    case POST_MATCH_1:
    case POST_MATCH_2:
      return static_cast<int32_t>(telemetry::v2::GAME_STATUS_POST_MATCH);
    case PRE_SUDDEN_DEATH_1:
    case PRE_SUDDEN_DEATH_2:
      return static_cast<int32_t>(telemetry::v2::GAME_STATUS_PRE_SUDDEN_DEATH);
    case SUDDEN_DEATH_1:
    case SUDDEN_DEATH_2:
      return static_cast<int32_t>(telemetry::v2::GAME_STATUS_SUDDEN_DEATH);
    case POST_SUDDEN_DEATH_1:
    case POST_SUDDEN_DEATH_2:
      return static_cast<int32_t>(telemetry::v2::GAME_STATUS_POST_SUDDEN_DEATH);
    default:
      return static_cast<int32_t>(telemetry::v2::GAME_STATUS_UNSPECIFIED);
  }
}

void TelemetryStreamer::BuildAndSendFrame(const TelemetrySnapshot& snap) {
  telemetry::v2::Envelope envelope;
  auto* frame = envelope.mutable_frame();

  frame->set_frame_index(m_frameIndex);

  auto elapsed = std::chrono::steady_clock::now() - m_sessionStartTime;
  frame->set_timestamp_offset_ms(
      static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));

  auto* arena = frame->mutable_echo_arena();

  // Game state
  arena->set_game_status(static_cast<telemetry::v2::GameStatus>(MapGameStatus(snap.gameStatusHash)));
  arena->set_game_clock(snap.gameClock);
  arena->set_blue_points(snap.bluePoints);
  arena->set_orange_points(snap.orangePoints);

  // Disc
  auto* disc = arena->mutable_disc();
  auto* discPose = disc->mutable_pose();
  auto* discPos = discPose->mutable_position();
  discPos->set_x(snap.discPos[0]);
  discPos->set_y(snap.discPos[1]);
  discPos->set_z(snap.discPos[2]);
  auto* discOri = discPose->mutable_orientation();
  discOri->set_x(snap.discOrient[0]);
  discOri->set_y(snap.discOrient[1]);
  discOri->set_z(snap.discOrient[2]);
  discOri->set_w(snap.discOrient[3]);
  auto* discVel = disc->mutable_velocity();
  discVel->set_x(snap.discVel[0]);
  discVel->set_y(snap.discVel[1]);
  discVel->set_z(snap.discVel[2]);
  disc->set_bounce_count(static_cast<uint32_t>(snap.discBounceCount));

  // Players
  for (uint16_t i = 0; i < snap.playerCount; i++) {
    const auto& pi = snap.playerInfo[i];
    if (pi.accountId == 0) continue;

    auto* ps = arena->add_players();
    ps->set_slot(i);
    ps->set_flags(pi.flags);
    ps->set_ping(pi.ping);

    // Transforms (zeroed for Phase 1, populated in Phase 2)
    const auto& pt = snap.players[i];

    auto* head = ps->mutable_head();
    auto* headPos = head->mutable_position();
    headPos->set_x(pt.headPos[0]);
    headPos->set_y(pt.headPos[1]);
    headPos->set_z(pt.headPos[2]);
    auto* headOri = head->mutable_orientation();
    headOri->set_x(pt.headRot[0]);
    headOri->set_y(pt.headRot[1]);
    headOri->set_z(pt.headRot[2]);
    headOri->set_w(pt.headRot[3]);

    auto* body = ps->mutable_body();
    auto* bodyPos = body->mutable_position();
    bodyPos->set_x(pt.bodyPos[0]);
    bodyPos->set_y(pt.bodyPos[1]);
    bodyPos->set_z(pt.bodyPos[2]);
    auto* bodyOri = body->mutable_orientation();
    bodyOri->set_x(pt.bodyRot[0]);
    bodyOri->set_y(pt.bodyRot[1]);
    bodyOri->set_z(pt.bodyRot[2]);
    bodyOri->set_w(pt.bodyRot[3]);

    auto* lh = ps->mutable_left_hand();
    auto* lhPos = lh->mutable_position();
    lhPos->set_x(pt.leftHandPos[0]);
    lhPos->set_y(pt.leftHandPos[1]);
    lhPos->set_z(pt.leftHandPos[2]);
    auto* lhOri = lh->mutable_orientation();
    lhOri->set_x(pt.leftHandRot[0]);
    lhOri->set_y(pt.leftHandRot[1]);
    lhOri->set_z(pt.leftHandRot[2]);
    lhOri->set_w(pt.leftHandRot[3]);

    auto* rh = ps->mutable_right_hand();
    auto* rhPos = rh->mutable_position();
    rhPos->set_x(pt.rightHandPos[0]);
    rhPos->set_y(pt.rightHandPos[1]);
    rhPos->set_z(pt.rightHandPos[2]);
    auto* rhOri = rh->mutable_orientation();
    rhOri->set_x(pt.rightHandRot[0]);
    rhOri->set_y(pt.rightHandRot[1]);
    rhOri->set_z(pt.rightHandRot[2]);
    rhOri->set_w(pt.rightHandRot[3]);

    auto* vel = ps->mutable_velocity();
    vel->set_x(pt.velocity[0]);
    vel->set_y(pt.velocity[1]);
    vel->set_z(pt.velocity[2]);

    // Bones
    const auto& bones = snap.playerBones[i];
    if (bones.valid) {
      auto* pb = arena->add_player_bones();
      pb->set_slot(i);

      // Pack transforms (23 bones × 3 floats = 276 bytes)
      std::string transforms(23 * 3 * sizeof(float), '\0');
      // Pack orientations (23 bones × 4 floats = 368 bytes)
      std::string orientations(23 * 4 * sizeof(float), '\0');

      for (int b = 0; b < 23; b++) {
        // bones[b] = [orient_x, orient_y, orient_z, orient_w, pos_x, pos_y, pos_z]
        std::memcpy(&orientations[b * 4 * sizeof(float)], &bones.bones[b][0], 4 * sizeof(float));
        std::memcpy(&transforms[b * 3 * sizeof(float)], &bones.bones[b][4], 3 * sizeof(float));
      }

      pb->set_transforms(std::move(transforms));
      pb->set_orientations(std::move(orientations));
    }
  }

  // Drain events from ring buffer (broadcaster-pushed events)
  TelemetryEvent event;
  while (m_eventBuffer.Pop(event)) {
    // Convert ring buffer events to proto EchoEvent
    auto* ev = arena->add_events();
    switch (event.type) {
      case TelemetryEvent::GoalScored: {
        auto* gs = ev->mutable_goal_scored();
        gs->set_team(static_cast<telemetry::v2::Role>(event.teamIndex));
        gs->set_point_amount(event.value);
        break;
      }
      case TelemetryEvent::PlayerJoined: {
        auto* pj = ev->mutable_player_joined();
        pj->set_slot(event.slot);
        break;
      }
      case TelemetryEvent::PlayerLeft: {
        auto* pl = ev->mutable_player_left();
        pl->set_player_slot(event.slot);
        break;
      }
      default:
        break;
    }
  }

  // Phase 4: detect events by comparing current and previous snapshots
  if (m_frameIndex > 0) {
    DetectEvents(snap, m_prevSnapshot, arena);
  }

  // Serialize and send
  std::string serialized;
  if (envelope.SerializeToString(&serialized)) {
    SendEnvelope(serialized);
    m_frameIndex++;
  } else {
    Log(EchoVR::LogLevel::Error, "[NEVR.TELEMETRY] Failed to serialize frame %u", m_frameIndex);
  }
}

void TelemetryStreamer::DetectEvents(const TelemetrySnapshot& curr, const TelemetrySnapshot& prev,
                                     telemetry::v2::EchoArenaFrame* frame) {
  // Detect game status change
  int32_t currStatus = MapGameStatus(curr.gameStatusHash);
  int32_t prevStatus = MapGameStatus(prev.gameStatusHash);
  if (currStatus != prevStatus && prevStatus != 0) {
    // Status transitions → specific events
    if (currStatus == static_cast<int32_t>(telemetry::v2::GAME_STATUS_ROUND_START)) {
      auto* ev = frame->add_events();
      ev->mutable_round_started();
    } else if (currStatus == static_cast<int32_t>(telemetry::v2::GAME_STATUS_ROUND_OVER)) {
      auto* ev = frame->add_events();
      ev->mutable_round_ended();
    } else if (currStatus == static_cast<int32_t>(telemetry::v2::GAME_STATUS_POST_MATCH)) {
      auto* ev = frame->add_events();
      ev->mutable_match_ended();
    }
  }

  // Detect score changes
  if (curr.bluePoints != prev.bluePoints || curr.orangePoints != prev.orangePoints) {
    auto* ev = frame->add_events();
    auto* sb = ev->mutable_scoreboard_updated();
    sb->set_blue_points(curr.bluePoints);
    sb->set_orange_points(curr.orangePoints);
  }

  // Detect goal scored (last score data changed)
  if (curr.lastScore.scorerPacked != prev.lastScore.scorerPacked &&
      curr.lastScore.pointAmount > 0) {
    auto* ev = frame->add_events();
    auto* gs = ev->mutable_goal_scored();
    gs->set_team(static_cast<telemetry::v2::Role>(curr.lastScore.team));
    gs->set_point_amount(curr.lastScore.pointAmount);
    gs->set_disc_speed(curr.lastScore.discSpeed);
    gs->set_distance_thrown(curr.lastScore.distanceThrown);
    gs->set_scorer_slot(curr.lastScore.scorerPacked & 0x1F);
    if ((curr.lastScore.assistPacked & 0x1F) != 0x1F) {
      gs->set_assist_slot(curr.lastScore.assistPacked & 0x1F);
    }
  }

  // Detect player join/leave
  for (uint16_t i = 0; i < 16; i++) {
    bool currPresent = (i < curr.playerCount && curr.playerInfo[i].accountId != 0);
    bool prevPresent = (i < prev.playerCount && prev.playerInfo[i].accountId != 0);

    if (currPresent && !prevPresent) {
      auto* ev = frame->add_events();
      auto* pj = ev->mutable_player_joined();
      pj->set_slot(i);
      pj->set_account_number(curr.playerInfo[i].accountId);
      pj->set_display_name(curr.playerInfo[i].displayName);
    } else if (!currPresent && prevPresent) {
      auto* ev = frame->add_events();
      auto* pl = ev->mutable_player_left();
      pl->set_player_slot(i);
    } else if (currPresent && prevPresent) {
      // Detect team switch
      if (curr.playerInfo[i].teamIndex != prev.playerInfo[i].teamIndex) {
        auto* ev = frame->add_events();
        auto* sw = ev->mutable_player_switched_team();
        sw->set_player_slot(i);
        sw->set_new_role(static_cast<telemetry::v2::Role>(curr.playerInfo[i].teamIndex));
      }
    }
  }
}

void TelemetryStreamer::SendEnvelope(const std::string& serialized) {
  if (!m_ws || !m_wsConnected.load(std::memory_order_relaxed)) return;

  // Wire format: [4-byte length LE][serialized protobuf]
  uint32_t len = static_cast<uint32_t>(serialized.size());
  std::string wire(sizeof(uint32_t) + serialized.size(), '\0');
  std::memcpy(&wire[0], &len, sizeof(uint32_t));
  std::memcpy(&wire[sizeof(uint32_t)], serialized.data(), serialized.size());

  m_ws->send(wire, true);  // binary=true
}

void TelemetryStreamer::SendHeader() {
  telemetry::v2::Envelope envelope;
  auto* header = envelope.mutable_header();

  header->set_capture_id(m_sessionId);
  header->set_format_version(2);

  auto* ts = header->mutable_created_at();
  auto now = std::chrono::system_clock::now();
  auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
  auto nanos =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()) - std::chrono::duration_cast<std::chrono::nanoseconds>(secs);
  ts->set_seconds(secs.count());
  ts->set_nanos(static_cast<int32_t>(nanos.count()));

  auto* arenaHeader = header->mutable_echo_arena();
  arenaHeader->set_session_id(m_sessionId);

  std::string serialized;
  if (envelope.SerializeToString(&serialized)) {
    SendEnvelope(serialized);
    Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Sent CaptureHeader (%zu bytes)", serialized.size());
  }
}

void TelemetryStreamer::SendFooter() {
  telemetry::v2::Envelope envelope;
  auto* footer = envelope.mutable_footer();

  footer->set_frame_count(m_frameIndex);

  auto elapsed = std::chrono::steady_clock::now() - m_sessionStartTime;
  footer->set_duration_ms(
      static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));

  std::string serialized;
  if (envelope.SerializeToString(&serialized)) {
    SendEnvelope(serialized);
    Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Sent CaptureFooter (frames=%u, duration=%ums)", m_frameIndex,
        footer->duration_ms());
  }
}

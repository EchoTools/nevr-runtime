#include "runtime/server/telemetry_streamer.h"

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXWebSocket.h>

#include <cstring>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "abi/echovr.h"
#include "abi/echovr_functions.h"
#include "telemetry/v2/capture.pb.h"
#include "telemetry/v2/echo_arena.pb.h"
#include "runtime/log/url_diagnostics.h"
#include "runtime/log/security_diagnostics.h"
#include "runtime/server/telemetry_events.h"
#include "runtime/server/telemetry_frame.h"
#include "runtime/server/telemetry_game_status.h"
#include "runtime/server/telemetry_game_type.h"
#include "runtime/server/telemetry_player_read.h"
#include "runtime/server/telemetry_session_read.h"

extern VOID Log(EchoVR::LogLevel level, const CHAR* format, ...);

// ============================================================================
// Lifecycle
// ============================================================================

TelemetryStreamer::TelemetryStreamer() {
  m_prevSnapshot.Clear();
}

TelemetryStreamer::~TelemetryStreamer() {
  Stop();
  Disconnect();
}

void TelemetryStreamer::SetBearerTokenRefresher(BearerReconnectAuth::Refresher refresher) {
  m_bearerRefresher = std::move(refresher);
}

bool TelemetryStreamer::Connect(const std::string& uri, const std::string& token) {
  if (uri.empty()) return false;

  m_token = token;
  m_ws = std::make_unique<ix::WebSocket>();
  m_ws->setUrl(uri);

  // Auth — send JWT on WebSocket upgrade request, and keep it fresh across auto-reconnects (#114)
  m_bearerAuth.Attach(*m_ws, m_token, m_bearerRefresher);

  // Heartbeat — detect dead connections faster than TCP timeout
  m_ws->setPingInterval(30);

  // Auto-reconnect with exponential backoff
  m_ws->enableAutomaticReconnection();
  m_ws->setMinWaitBetweenReconnectionRetries(1000);   // 1s
  m_ws->setMaxWaitBetweenReconnectionRetries(10000);   // 10s

  m_ws->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open:
        Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Connected to telemetry server");
        if (m_hasConnectedOnce) {
          m_reconnectCount++;
        }
        m_hasConnectedOnce = true;
        m_wsConnected.store(true, std::memory_order_release);
        m_headerEpoch.Require();
        break;
      case ix::WebSocketMessageType::Close:
        {
          const std::string diagnostic = nevr_log_diagnostics::FormatWebSocketCloseDiagnostic(
              "[NEVR.TELEMETRY] Disconnected from telemetry server ", msg->closeInfo.code,
              m_reconnectCount);
          Log(EchoVR::LogLevel::Info, "%s", diagnostic.c_str());
        }
        m_wsConnected.store(false, std::memory_order_release);
        break;
      case ix::WebSocketMessageType::Error:
        {
          const std::string diagnostic = nevr_log_diagnostics::FormatWebSocketErrorDiagnostic(
              "[NEVR.TELEMETRY] Connection error: ", msg->errorInfo.http_status,
              msg->errorInfo.retries, m_reconnectCount);
          Log(EchoVR::LogLevel::Error, "%s", diagnostic.c_str());
        }
        m_wsConnected.store(false, std::memory_order_release);
        m_bearerAuth.OnError(msg->errorInfo.http_status);
        break;
      case ix::WebSocketMessageType::Message:
        // Telemetry server responses (acks) — currently just log
        Log(EchoVR::LogLevel::Debug, "[NEVR.TELEMETRY] Received response (%zu bytes)", msg->str.size());
        break;
      default:
        break;
    }
  });

  const std::string diagnostic = nevr_log_diagnostics::FormatUrlDiagnostic("[NEVR.TELEMETRY] Connecting to ", uri);
  Log(EchoVR::LogLevel::Info, "%s", diagnostic.c_str());
  m_ws->start();
  return true;
}

void TelemetryStreamer::Start(const std::string& sessionId, uint32_t rateHz, const std::string& gameType) {
  std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
  if (m_active.load(std::memory_order_relaxed) || m_thread.joinable()) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.TELEMETRY] Already streaming, stopping previous session");
    StopLocked();
  }

  m_sessionId = sessionId;
  m_rateHz = rateHz > 0 ? rateHz : 30;
  m_gameType = gameType;
  m_frameIndex = 0;
  m_sessionStartTime = std::chrono::steady_clock::now();
  m_lastSnapshotTime = m_sessionStartTime;
  m_prevSnapshot.Clear();
  m_eventState = TelemetryEventState{};
  m_leftOutWarned = false;
  m_lastReadSnapshotSequence = 0;
  m_droppedFrames = 0;
  m_reconnectCount = 0;
  m_bytesSent = 0;
  m_hasConnectedOnce = false;
  m_stopping.store(false, std::memory_order_relaxed);

  if (!m_snapshotStore.Reset()) {
    Log(EchoVR::LogLevel::Error, "[NEVR.TELEMETRY] Cannot start while a snapshot lease is still active");
    return;
  }
  m_headerEpoch.Require();
  m_snapshotStore.Activate();
  m_active.store(true, std::memory_order_release);
  if (!StartTelemetryWorker(
          [this]() {
            m_thread = std::thread(&TelemetryStreamer::Run, this);
            return true;
          },
          [this]() {
            m_active.store(false, std::memory_order_release);
            m_snapshotStore.DeactivateAndQuiesce();
            (void)m_snapshotStore.Reset();
            Log(EchoVR::LogLevel::Error, "[NEVR.TELEMETRY] Failed to create telemetry worker thread");
          })) {
    return;
  }

  Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Started streaming session=%s at %uHz", sessionId.c_str(), m_rateHz);
}

void TelemetryStreamer::Stop() {
  std::lock_guard<std::mutex> lifecycleLock(m_lifecycleMutex);
  StopLocked();
}

void TelemetryStreamer::StopLocked() {
  if (!m_active.load(std::memory_order_relaxed) && !m_thread.joinable()) return;

  Log(EchoVR::LogLevel::Info, "[NEVR.TELEMETRY] Stopping telemetry stream");
  m_stopping.store(true, std::memory_order_release);
  m_active.store(false, std::memory_order_release);
  m_snapshotStore.DeactivateAndQuiesce();

  if (m_thread.joinable()) {
    m_thread.join();
  }

  // Disable auto-reconnect AFTER thread join — thread may reconnect during Run()
  if (m_ws) {
    m_ws->disableAutomaticReconnection();
  }

  const uint64_t skippedSnapshots = m_snapshotStore.skippedGenerations();
  if (!m_snapshotStore.Reset()) {
    Log(EchoVR::LogLevel::Error, "[NEVR.TELEMETRY] Snapshot store still leased after worker join");
  }

  Log(EchoVR::LogLevel::Info,
      "[NEVR.TELEMETRY] Stream stopped (sent %u frames, network-dropped %u, snapshots-overwritten %llu, reconnects %u, bytes %llu)",
      m_frameIndex, m_droppedFrames, static_cast<unsigned long long>(skippedSnapshots), m_reconnectCount,
      (unsigned long long)m_bytesSent);
}

void TelemetryStreamer::Disconnect() {
  if (m_ws) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.TELEMETRY] Disconnecting from telemetry server");
    m_bearerAuth.Cancel();  // stop() joins the thread a 401 mint would run on
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
  m_snapshotStore.CaptureIfActive([this](TelemetrySnapshot& snapshot) {
    if (!m_active.load(std::memory_order_acquire)) return false;
    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::microseconds(1000000 / m_rateHz);
    if (now - m_lastSnapshotTime < interval) return false;

    m_lastSnapshotTime = now;
    snapshot.Clear();
    SnapshotGameState(&snapshot);
    return true;
  });
}

void TelemetryStreamer::PushEvent(const TelemetryEvent& event) { m_eventBuffer.Push(event); }

void TelemetryStreamer::ProcessResponses() {
  // Currently a no-op — telemetry server responses are handled in the WS callback.
  // This method exists so the game thread Update() loop has a consistent call pattern.
}

// ============================================================================
// Game thread: Diagnostic mode (-telemetry-diag)
// ============================================================================

void TelemetryStreamer::RunDiagnostics() {
  auto now = std::chrono::steady_clock::now();
  if (now - m_lastDiagTime < std::chrono::seconds(1)) return;
  m_lastDiagTime = now;

  ResolveFunctionPointers();

  CHAR* base = EchoVR::g_GameBaseAddress;
  if (!base) {
    Log(EchoVR::LogLevel::Warning, "[TELEMETRY.DIAG] No game base address");
    return;
  }

  Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] ========== SNAPSHOT ==========");
  Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] Base=%p", base);

  // --- Pointer chain ---
  void** contextPtr = reinterpret_cast<void**>(base + nevr_game_offsets::GAME_CONTEXT_OFFSET);
  void* gameContext = *contextPtr;
  Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] GameContext=%p (base+0x%llX)", gameContext,
      (unsigned long long)nevr_game_offsets::GAME_CONTEXT_OFFSET);

  if (!gameContext) {
    Log(EchoVR::LogLevel::Warning, "[TELEMETRY.DIAG] GameContext is NULL — game not initialized yet");
    return;
  }

  CHAR* ctxBase = reinterpret_cast<CHAR*>(gameContext);
  void* netGame = *reinterpret_cast<void**>(ctxBase + nevr_game_offsets::NETGAME_OFFSET);
  Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] CR15NetGame=%p (ctx+0x%X)", netGame,
      (unsigned)nevr_game_offsets::NETGAME_OFFSET);

  if (!netGame) {
    Log(EchoVR::LogLevel::Warning, "[TELEMETRY.DIAG] CR15NetGame is NULL — not in game yet");
    return;
  }

  CHAR* ngBase = reinterpret_cast<CHAR*>(netGame);

  // GameStateData
  void* gsd = *reinterpret_cast<void**>(ngBase + nevr_game_offsets::GAME_STATE_DATA_OFFSET);
  Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] GameStateData=%p (ng+0x%X)", gsd,
      (unsigned)nevr_game_offsets::GAME_STATE_DATA_OFFSET);

  // The entity system: the handle resolver and the entity lookup's object (telemetry_player_pose.h)
  void* entitySystem = *reinterpret_cast<void**>(ngBase + nevr_game_offsets::ENTITY_SYSTEM_OFFSET);
  Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] EntitySystem=%p (ng+0x%X)", entitySystem,
      (unsigned)nevr_game_offsets::ENTITY_SYSTEM_OFFSET);

  // --- Game function calls ---
  if (!m_getSymbolHash) {
    Log(EchoVR::LogLevel::Warning, "[TELEMETRY.DIAG] GetSymbolHash fn not resolved");
  }
  const GameProperties props =
      ReadGameProperties({m_getSymbolHash, m_getFloatProperty, m_getIntProperty}, netGame);
  Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] GameStatus: hash=0x%016llX → enum=%d",
      (unsigned long long)props.statusHash, MapGameStatus(props.statusHash));
  Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] GameClock: %.2f", props.clock);
  Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] Scores: blue=%d, orange=%d", props.bluePoints,
      props.orangePoints);

  // --- Players, by game slot (the same reader as SnapshotGameState) ---
  m_diagSnapshot.Clear();
  const nevr_telemetry_player::PlayerReadResult read =
      nevr_telemetry_player::ReadPlayers(reinterpret_cast<const uint8_t*>(ngBase), &m_diagSnapshot, m_getUserName);
  Log(EchoVR::LogLevel::Debug,
      "[TELEMETRY.DIAG] players=%u not_players=%u slot_out_of_range=%u not_in_handle_table=%u no_identity=%u",
      m_diagSnapshot.playerCount, read.notPlayers, read.slotOutOfRange, read.notInHandleTable, read.noIdentity);

  // --- Disc state ---
  if (gsd) {
    CHAR* gsdBase = reinterpret_cast<CHAR*>(gsd);
    float* discPos = reinterpret_cast<float*>(gsdBase + nevr_game_offsets::DISC_POS_OFFSET);
    float* discVel = reinterpret_cast<float*>(gsdBase + nevr_game_offsets::DISC_VEL_OFFSET);
    float* discOri = reinterpret_cast<float*>(gsdBase + nevr_game_offsets::DISC_ORIENT_OFFSET);
    uint64_t bounce = *reinterpret_cast<uint64_t*>(gsdBase + nevr_game_offsets::DISC_BOUNCE_OFFSET);

    Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] Disc pos=(%.3f, %.3f, %.3f) vel=(%.3f, %.3f, %.3f)",
        discPos[0], discPos[1], discPos[2], discVel[0], discVel[1], discVel[2]);
    Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] Disc orient=(%.3f, %.3f, %.3f, %.3f) bounce=%llu",
        discOri[0], discOri[1], discOri[2], discOri[3], (unsigned long long)bounce);

    // Quaternion length check
    float qlen2 = discOri[0]*discOri[0] + discOri[1]*discOri[1] +
                   discOri[2]*discOri[2] + discOri[3]*discOri[3];
    if (qlen2 > 0.01f) {
      Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] Disc quat |q|^2=%.4f %s",
          qlen2, (qlen2 > 0.99f && qlen2 < 1.01f) ? "(OK)" : "(BAD - not unit quaternion!)");
    }
  }

  // --- Per-player info + entity resolution ---
  uint16_t logged = 0;
  for (uint8_t slot = 0; slot < nevr_telemetry_player::kMaxSlots && logged < 4; slot++) {  // Log first 4 players max
    const auto& info = m_diagSnapshot.playerInfo[slot];
    if (!info.present) continue;
    logged++;
    const uint64_t accountId = info.accountId;

    Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] Player slot=%u: name=%s, acct=%llu, flags=0x%04X, team=%u, ping=%u",
        slot, info.displayName, static_cast<unsigned long long>(accountId), info.flags, info.teamIndex, info.ping);
    // The pose, through the same reader as SnapshotGameState
    TelemetrySnapshot::PlayerTransform pose{};
    const nevr_telemetry_pose::PoseResult poseRead = nevr_telemetry_pose::ReadPlayerPose(
        reinterpret_cast<const uint8_t*>(ngBase), info.entrantIndex, m_poseFunctions, &pose);
    Log(EchoVR::LogLevel::Debug,
        "[TELEMETRY.DIAG]   Pose: entrant=%u entity=%d head=%d body=%d hands=%d head_pos=(%.3f, %.3f, %.3f) "
        "body_pos=(%.3f, %.3f, %.3f) velocity=(%.3f, %.3f, %.3f)",
        info.entrantIndex, poseRead.entity, poseRead.head, poseRead.body, poseRead.hands, pose.headPos[0],
        pose.headPos[1], pose.headPos[2], pose.bodyPos[0], pose.bodyPos[1], pose.bodyPos[2], pose.velocity[0],
        pose.velocity[1], pose.velocity[2]);

    // Stats
    const auto& stats = m_diagSnapshot.stats[slot];
    Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG]   Stats: pts=%u, goals=%u, ast=%u, stuns=%u",
        stats.points, stats.goals, stats.assists, stats.stuns);
  }

  if (m_diagSnapshot.playerCount > 4) {
    Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] ... and %u more players", m_diagSnapshot.playerCount - 4);
  }

  Log(EchoVR::LogLevel::Debug, "[TELEMETRY.DIAG] ==============================");
}

// ============================================================================
// Game thread: Snapshot capture
// ============================================================================

void TelemetryStreamer::ResolveFunctionPointers() {
  if (m_funcPtrsResolved) return;

  CHAR* base = EchoVR::g_GameBaseAddress;
  if (!base) return;

  m_getSymbolHash = reinterpret_cast<GetSymbolHashFn>(base + nevr_game_func_addr::GET_SYMBOL_HASH);
  m_getFloatProperty = reinterpret_cast<GetFloatPropertyFn>(base + nevr_game_func_addr::GET_FLOAT_PROPERTY);
  m_getIntProperty = reinterpret_cast<GetIntPropertyFn>(base + nevr_game_func_addr::GET_INT_PROPERTY);
  m_getUserName = reinterpret_cast<GetUserNameFn>(base + nevr_game_func_addr::GET_USER_NAME);
  namespace pose = nevr_telemetry_pose;
  namespace addr = nevr_game_func_addr;
  m_poseFunctions.resolveHandle = reinterpret_cast<pose::ResolveEntityHandleFn>(base + addr::RESOLVE_ENTITY_HANDLE);
  m_poseFunctions.lookupEntity = reinterpret_cast<pose::EntityLookupFn>(base + addr::ENTITY_LOOKUP);
  m_poseFunctions.headComponent = reinterpret_cast<pose::GetComponentFn>(base + addr::GET_HEAD_COMPONENT);
  m_poseFunctions.bodyComponent = reinterpret_cast<pose::GetComponentFn>(base + addr::GET_BODY_COMPONENT);
  m_poseFunctions.remotePlayerComponent =
      reinterpret_cast<pose::GetComponentFn>(base + addr::GET_REMOTE_PLAYER_COMPONENT);
  m_poseFunctions.readHead = reinterpret_cast<pose::ReadHeadTransformFn>(base + addr::READ_HEAD_TRANSFORM);
  m_poseFunctions.readBody = reinterpret_cast<pose::ReadBodyTransformFn>(base + addr::READ_BODY_TRANSFORM);
  m_poseFunctions.readLeftHand = reinterpret_cast<pose::ReadHandPositionFn>(base + addr::READ_LEFT_HAND_POSITION);
  m_poseFunctions.readRightHand = reinterpret_cast<pose::ReadHandPositionFn>(base + addr::READ_RIGHT_HAND_POSITION);
  m_poseFunctions.readLocalVelocity = reinterpret_cast<pose::ReadLocalVelocityFn>(base + addr::READ_LOCAL_VELOCITY);
  m_poseFunctions.readWorldTransform =
      reinterpret_cast<pose::ReadWorldTransformFn>(base + addr::READ_WORLD_TRANSFORM);

  m_funcPtrsResolved = true;
  Log(EchoVR::LogLevel::Debug, "[NEVR.TELEMETRY] Game function pointers resolved");
}

bool TelemetryStreamer::ResolveGamePointers() {
  CHAR* base = EchoVR::g_GameBaseAddress;
  if (!base) return false;

  void** contextPtr = reinterpret_cast<void**>(base + nevr_game_offsets::GAME_CONTEXT_OFFSET);
  void* gameContext = *contextPtr;
  if (!gameContext) return false;

  CHAR* contextBase = reinterpret_cast<CHAR*>(gameContext);
  m_netGame = *reinterpret_cast<void**>(contextBase + nevr_game_offsets::NETGAME_OFFSET);
  if (!m_netGame) return false;

  CHAR* netGameBase = reinterpret_cast<CHAR*>(m_netGame);
  m_gameStateData = *reinterpret_cast<void**>(netGameBase + nevr_game_offsets::GAME_STATE_DATA_OFFSET);
  return m_gameStateData != nullptr;
}

void TelemetryStreamer::SnapshotGameState(TelemetrySnapshot* snap) {
  ResolveFunctionPointers();

  if (!ResolveGamePointers()) return;
  snap->gameRead = true;

  CHAR* netGameBase = reinterpret_cast<CHAR*>(m_netGame);
  CHAR* gsdBase = reinterpret_cast<CHAR*>(m_gameStateData);

  // --- Game function calls (game thread only) ---
  const GameProperties props =
      ReadGameProperties({m_getSymbolHash, m_getFloatProperty, m_getIntProperty}, m_netGame);
  snap->gameStatusHash = props.statusHash;
  snap->gameClock = props.clock;
  snap->bluePoints = props.bluePoints;
  snap->orangePoints = props.orangePoints;

  // The level name and the round count, as /session reads them (telemetry_session_read.h)
  nevr_telemetry_session::ReadSessionState({m_getIntProperty}, m_netGame, snap);

  // --- Direct memory reads ---

  // Players by game slot: the name from the game's GetUserName, account id and ping from the lobby, stats
  // from the slot's block (telemetry_player_read.h)
  const nevr_telemetry_player::PlayerReadResult read =
      nevr_telemetry_player::ReadPlayers(reinterpret_cast<const uint8_t*>(netGameBase), snap, m_getUserName);
  const unsigned leftOut = read.slotOutOfRange + read.notInHandleTable + read.noIdentity;
  if (leftOut != 0 && !m_leftOutWarned) {
    m_leftOutWarned = true;
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.TELEMETRY] players left out of the stream session_id=%s slot_out_of_range=%u not_in_handle_table=%u "
        "no_identity=%u: a game slot of %u or more, an entrant its slot's handle-table entry does not name (empty, "
        "or naming another entrant), or a slot with no entry in the lobby's entrant table; their frames and events "
        "are not sent (logged once per session)",
        m_sessionId.c_str(), read.slotOutOfRange, read.notInHandleTable, read.noIdentity,
        nevr_telemetry_player::kMaxSlots);
  }

  // Disc state
  std::memcpy(snap->discOrient, gsdBase + nevr_game_offsets::DISC_ORIENT_OFFSET, sizeof(float) * 4);
  std::memcpy(snap->discPos, gsdBase + nevr_game_offsets::DISC_POS_OFFSET, sizeof(float) * 3);
  std::memcpy(snap->discVel, gsdBase + nevr_game_offsets::DISC_VEL_OFFSET, sizeof(float) * 3);
  snap->discBounceCount = *reinterpret_cast<uint64_t*>(gsdBase + nevr_game_offsets::DISC_BOUNCE_OFFSET);

  // Last score data
  snap->lastScore.team = *reinterpret_cast<int16_t*>(gsdBase + nevr_game_offsets::LAST_SCORE_TEAM_OFFSET);
  snap->lastScore.pointAmount = *reinterpret_cast<uint16_t*>(gsdBase + nevr_game_offsets::LAST_SCORE_POINT_AMOUNT_OFFSET);
  snap->lastScore.discSpeed = *reinterpret_cast<float*>(gsdBase + nevr_game_offsets::LAST_SCORE_DISC_SPEED_OFFSET);
  snap->lastScore.distanceThrown = *reinterpret_cast<float*>(gsdBase + nevr_game_offsets::LAST_SCORE_DISTANCE_THROWN_OFFSET);
  snap->lastScore.assistPacked = *reinterpret_cast<uint32_t*>(gsdBase + nevr_game_offsets::LAST_SCORE_ASSIST_PACKED_OFFSET);
  snap->lastScore.scorerPacked = *reinterpret_cast<uint32_t*>(gsdBase + nevr_game_offsets::LAST_SCORE_SCORER_PACKED_OFFSET);

  // Player poses through the game's own component readers, as /session reads them (telemetry_player_pose.h)
  nevr_telemetry_pose::ReadPlayerPoses(reinterpret_cast<const uint8_t*>(netGameBase), snap, m_poseFunctions);
}

// ============================================================================
// Telemetry thread
// ============================================================================

void TelemetryStreamer::Run() {
  Log(EchoVR::LogLevel::Debug, "[NEVR.TELEMETRY] Telemetry thread started");

  // Preserve the startup wait window, but continue polling after timeout. A
  // late connection must still get a header from a leased published snapshot.
  auto waitStart = std::chrono::steady_clock::now();
  while (m_active.load(std::memory_order_relaxed)) {
    const bool connected = m_wsConnected.load(std::memory_order_acquire);
    if (connected && m_snapshotStore.HasPublished()) break;
    if (std::chrono::steady_clock::now() - waitStart > std::chrono::seconds(30)) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.TELEMETRY] Timeout waiting for connection + snapshot");
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  auto frameInterval = std::chrono::microseconds(1000000 / m_rateHz);

  while (m_active.load(std::memory_order_acquire)) {
    auto frameStart = std::chrono::steady_clock::now();
    if (m_wsConnected.load(std::memory_order_acquire) && m_ws != nullptr) {
      const bool headerRequired = m_headerEpoch.Required();
      auto lease = m_snapshotStore.TryAcquireRead(m_lastReadSnapshotSequence, headerRequired);
      if (lease) {
        const TelemetrySnapshot& snapshot = lease.snapshot();
        bool maySendFrame = true;

        if (headerRequired) {
          const uint64_t headerEpoch = m_headerEpoch.Current();
          if (!SendHeaderWithSnapshot(snapshot) || !m_headerEpoch.MarkSent(headerEpoch)) {
            maySendFrame = false;
          } else {
            Log(EchoVR::LogLevel::Debug, "[NEVR.TELEMETRY] Sent required header for epoch %llu",
                static_cast<unsigned long long>(headerEpoch));
          }
        }

        // A reconnect callback can require a new header while this snapshot
        // is being serialized. Defer its frame so the new epoch gets a header
        // first on the next pass.
        if (maySendFrame && m_headerEpoch.Required()) maySendFrame = false;

        if (maySendFrame) {
          const size_t buffered = m_ws->bufferedAmount();
          if (buffered > kMaxWsBufferBytes) {
            ++m_droppedFrames;
            if (m_droppedFrames % 100 == 1) {
              Log(EchoVR::LogLevel::Warning,
                  "[NEVR.TELEMETRY] Backpressure: network-dropped %u frames (buffer=%zu bytes)",
                  m_droppedFrames, buffered);
            }
          } else {
            BuildAndSendFrame(snapshot);
          }
        }

        // Keep the reader lease alive through both serialization/send work and
        // this copy; release occurs when `lease` leaves scope. Only a read snapshot becomes the previous
        // one, so the next read one is compared with the last state the game gave (telemetry_events.h).
        if (snapshot.gameRead) m_prevSnapshot = snapshot;
        m_lastReadSnapshotSequence = lease.sequence();
      }
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

  Log(EchoVR::LogLevel::Debug, "[NEVR.TELEMETRY] Telemetry thread exiting");
}

// ============================================================================
// Frame assembly + sending
// ============================================================================

int32_t TelemetryStreamer::MapGameStatus(uint64_t hash) {
  return static_cast<int32_t>(GameStatusFromSymbol(hash));
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
  for (uint8_t i = 0; i < nevr_telemetry_player::kMaxSlots; i++) {  // i is the game slot
    const auto& pi = snap.playerInfo[i];
    if (!pi.present) continue;

    auto* ps = arena->add_players();
    ps->set_slot(i);
    ps->set_flags(pi.flags);
    ps->set_ping(pi.ping);

    // Poses, as the game's readers gave them (telemetry_player_pose.h); a part not read is zero
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

    // No player_bones: the snapshot holds no bone data (telemetry_snapshot.h).
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

  // Phase 4: detect events by comparing current and previous snapshots (telemetry_events.h)
  if (m_frameIndex > 0) {
    DetectTelemetryEvents(snap, m_prevSnapshot, m_eventState, arena);
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

bool TelemetryStreamer::SendEnvelope(const std::string& serialized) {
  if (!m_ws || !m_wsConnected.load(std::memory_order_relaxed)) return false;

  // Wire format: [4-byte length LE][serialized protobuf]
  uint32_t len = static_cast<uint32_t>(serialized.size());
  std::string wire(sizeof(uint32_t) + serialized.size(), '\0');
  std::memcpy(&wire[0], &len, sizeof(uint32_t));
  std::memcpy(&wire[sizeof(uint32_t)], serialized.data(), serialized.size());

  auto info = m_ws->send(wire, true);  // binary=true
  if (info.success) {
    m_bytesSent += info.wireSize;
    return true;
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.TELEMETRY] Send failed (payload=%zu)", info.payloadSize);
    return false;
  }
}

bool TelemetryStreamer::SendHeaderWithSnapshot(const TelemetrySnapshot& snap) {
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

  // The engine's game-type symbol for the session, from the settings JSON (#996).
  nevr_telemetry_header::SetCaptureGameType(header, m_gameType);
  // The session, its level and round count, the skeleton layout and the roster (telemetry_frame.h)
  nevr_telemetry_frame::BuildArenaHeader(snap, m_sessionId, header->mutable_echo_arena());

  std::string serialized;
  if (envelope.SerializeToString(&serialized)) {
    const bool sent = SendEnvelope(serialized);
    if (sent) {
      Log(EchoVR::LogLevel::Debug, "[NEVR.TELEMETRY] Sent CaptureHeader with roster (%zu bytes, %u players)",
          serialized.size(), snap.playerCount);
    }
    return sent;
  }
  Log(EchoVR::LogLevel::Error, "[NEVR.TELEMETRY] Failed to serialize CaptureHeader with roster");
  return false;
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
    Log(EchoVR::LogLevel::Debug, "[NEVR.TELEMETRY] Sent CaptureFooter (frames=%u, duration=%ums)", m_frameIndex,
        footer->duration_ms());
  }
}

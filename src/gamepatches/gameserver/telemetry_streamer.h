#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "event_ring_buffer.h"
#include "telemetry_snapshot.h"

namespace ix {
class WebSocket;
struct WebSocketMessage;
using WebSocketMessagePtr = std::unique_ptr<WebSocketMessage>;
}  // namespace ix

namespace GameServer {
class ServerContext;
}
namespace telemetry::v2 {
class EchoArenaFrame;
}

// Telemetry event pushed from broadcaster callbacks (game thread).
struct TelemetryEvent {
  enum Type : uint8_t {
    None = 0,
    GoalScored,
    PlayerJoined,
    PlayerLeft,
    StatUpdate,
  };
  Type type = None;
  uint8_t slot = 0;
  uint16_t teamIndex = 0;
  uint32_t value = 0;
};

// Streams game state telemetry over a dedicated WebSocket to a telemetry server.
//
// Game thread calls SnapshotIfDue() every Update() tick to capture state.
// A background thread serializes snapshots into protobuf frames and sends them.
// Uses double-buffered snapshots for lock-free game→telemetry transfer.
class TelemetryStreamer {
 public:
  TelemetryStreamer();
  ~TelemetryStreamer();

  // Connect to the telemetry ingest server. Non-blocking.
  // Optional token is sent as Bearer auth on the WebSocket upgrade request.
  bool Connect(const std::string& uri, const std::string& token = "");

  // Start streaming for a session. Launches the telemetry thread.
  void Start(const std::string& sessionId, uint32_t rateHz = 30, bool isPrivateMatch = false);

  // Stop streaming. Sends footer, joins telemetry thread.
  void Stop();

  // Disconnect from the telemetry server.
  void Disconnect();

  bool IsActive() const { return m_active.load(std::memory_order_relaxed); }
  bool IsConnected() const;

  // Called from Update() on game thread — snapshots game state if interval elapsed.
  void SnapshotIfDue();

  // Push a telemetry event from a broadcaster callback (game thread, lock-free).
  void PushEvent(const TelemetryEvent& event);

  // Process telemetry WS responses (call from game thread Update).
  void ProcessResponses();

  // Diagnostic mode: log snapshot data to console (game thread, rate-limited to 1Hz).
  // Called from Update() when -telemetrydiag is set. Does NOT require a WS connection.
  void RunDiagnostics();

 private:
  void Run();  // telemetry thread main loop

  // Game thread: snapshot all game state into the write buffer
  void SnapshotGameState(TelemetrySnapshot* snap);
  void SnapshotPlayerBones(TelemetrySnapshot* snap);

  // Telemetry thread: build protobuf frame from snapshot
  void BuildAndSendFrame(const TelemetrySnapshot& snap);
  void SendEnvelope(const std::string& serialized);
  void SendHeader();
  void SendHeaderWithSnapshot(const TelemetrySnapshot& snap);
  void SendFooter();

  // Map raw game status hash to proto enum value
  static int32_t MapGameStatus(uint64_t hash);

  // WebSocket (separate from ServerDB WS)
  std::unique_ptr<ix::WebSocket> m_ws;
  std::atomic<bool> m_wsConnected{false};

  // Double-buffered snapshot
  TelemetrySnapshot m_snapshots[2];
  std::atomic<int> m_writeIndex{0};     // game thread writes to m_snapshots[writeIndex]
  std::atomic<bool> m_snapshotReady{false};  // telemetry thread: new snapshot available

  // Thread + lifecycle
  std::thread m_thread;
  std::atomic<bool> m_active{false};
  std::atomic<bool> m_stopping{false};
  uint32_t m_rateHz{30};
  std::chrono::steady_clock::time_point m_lastSnapshotTime;

  // Diagnostics (game thread, 1Hz)
  std::chrono::steady_clock::time_point m_lastDiagTime;
  TelemetrySnapshot m_diagSnapshot;

  // Events from broadcaster callbacks
  EventRingBuffer<TelemetryEvent, 256> m_eventBuffer;

  // Reconnection
  std::atomic<bool> m_needsResendHeader{false};
  bool m_hasConnectedOnce{false};

  // Metrics
  uint32_t m_droppedFrames{0};
  uint32_t m_reconnectCount{0};
  uint64_t m_bytesSent{0};
  static constexpr size_t kMaxWsBufferBytes = 1024 * 1024;  // 1MB ~ 2s at 30Hz

  // Auth
  std::string m_token;

  // Match metadata
  bool m_isPrivateMatch{false};

  // Session tracking
  std::string m_sessionId;
  std::chrono::steady_clock::time_point m_sessionStartTime;
  uint32_t m_frameIndex{0};

  // Previous snapshot for event diffing (Phase 4)
  TelemetrySnapshot m_prevSnapshot;

  // Detect events by comparing current and previous snapshots
  void DetectEvents(const TelemetrySnapshot& curr, const TelemetrySnapshot& prev,
                    telemetry::v2::EchoArenaFrame* frame);

  // Game function pointers (resolved once on first snapshot)
  bool m_funcPtrsResolved{false};
  GetSymbolHashFn m_getSymbolHash{nullptr};
  GetFloatPropertyFn m_getFloatProperty{nullptr};
  GetIntPropertyFn m_getIntProperty{nullptr};
  EntityLookupFn m_entityLookup{nullptr};
  ResolveEntityHandleFn m_resolveEntityHandle{nullptr};
  GetBoneDataFn m_getBoneData{nullptr};
  GetTransformComponentFn m_getTransformComponent{nullptr};

  // Cached game pointers (resolved per snapshot)
  void* m_netGame{nullptr};
  void* m_gameStateData{nullptr};
  void* m_entityManager{nullptr};
  void* m_handleResolver{nullptr};

  void ResolveFunctionPointers();
  bool ResolveGamePointers();

  // Resolve entity for a player by account ID, returns entity pointer or nullptr
  void* ResolvePlayerEntity(uint64_t accountId);
};

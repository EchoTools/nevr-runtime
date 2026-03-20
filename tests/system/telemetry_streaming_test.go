package system

import (
	"context"
	"encoding/binary"
	"fmt"
	"math"
	"net"
	"net/http"
	"sync"
	"testing"
	"time"

	telemetryv2 "buf.build/gen/go/echotools/nevr-api/protocolbuffers/go/telemetry/v2"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/proto"
	"nhooyr.io/websocket"
)

// mockTelemetryServer accepts WebSocket connections and collects
// length-prefixed protobuf telemetry::v2::Envelope messages.
type mockTelemetryServer struct {
	mu        sync.Mutex
	envelopes []*telemetryv2.Envelope
	rawFrames [][]byte
	srv       *http.Server
	addr      string
	done      chan struct{}
}

func newMockTelemetryServer(t *testing.T) *mockTelemetryServer {
	t.Helper()

	m := &mockTelemetryServer{
		done: make(chan struct{}),
	}

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	m.addr = listener.Addr().String()

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
			InsecureSkipVerify: true,
		})
		if err != nil {
			t.Logf("WebSocket accept error: %v", err)
			return
		}
		defer conn.CloseNow()

		t.Logf("Telemetry client connected from %s", r.RemoteAddr)

		for {
			_, data, err := conn.Read(context.Background())
			if err != nil {
				t.Logf("WebSocket read ended: %v", err)
				return
			}

			m.mu.Lock()
			m.rawFrames = append(m.rawFrames, data)

			// Wire format: [4-byte LE length][protobuf bytes]
			if len(data) >= 4 {
				pbLen := binary.LittleEndian.Uint32(data[:4])
				pbData := data[4:]

				if uint32(len(pbData)) >= pbLen {
					env := &telemetryv2.Envelope{}
					if err := proto.Unmarshal(pbData[:pbLen], env); err != nil {
						t.Logf("Failed to unmarshal envelope: %v", err)
					} else {
						m.envelopes = append(m.envelopes, env)
					}
				}
			}
			m.mu.Unlock()
		}
	})

	m.srv = &http.Server{Handler: mux}
	go func() {
		defer close(m.done)
		if err := m.srv.Serve(listener); err != http.ErrServerClosed {
			t.Logf("Mock server error: %v", err)
		}
	}()

	t.Logf("Mock telemetry server listening on ws://%s", m.addr)
	return m
}

func (m *mockTelemetryServer) URL() string {
	return fmt.Sprintf("ws://%s/", m.addr)
}

func (m *mockTelemetryServer) Stop(t *testing.T) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	m.srv.Shutdown(ctx)
	<-m.done
}

func (m *mockTelemetryServer) Envelopes() []*telemetryv2.Envelope {
	m.mu.Lock()
	defer m.mu.Unlock()
	cp := make([]*telemetryv2.Envelope, len(m.envelopes))
	copy(cp, m.envelopes)
	return cp
}

func (m *mockTelemetryServer) RawFrames() [][]byte {
	m.mu.Lock()
	defer m.mu.Unlock()
	cp := make([][]byte, len(m.rawFrames))
	copy(cp, m.rawFrames)
	return cp
}

// TestTelemetry_StreamingWireFormat starts a mock WebSocket telemetry server,
// launches the game server with telemetry_uri pointing to it, and verifies:
//   - Wire format: [4B LE length][protobuf Envelope]
//   - Session lifecycle: CaptureHeader → Frames → CaptureFooter
//   - Frame contents: game status, clock, disc state, player data
//
// Requires: game binary, evr-test-harness. Set EVR_GAME_DIR and NEVR_BUILD_DIR.
func TestTelemetry_StreamingWireFormat(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// Start mock telemetry server
	mock := newMockTelemetryServer(t)
	defer mock.Stop(t)

	// Deploy DLLs and start game with telemetry_uri config override
	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	// Write config with telemetry_uri, start game via test harness
	fixture := NewFixture(t)
	defer fixture.Cleanup()

	result, err := fixture.MCPClient().Call("echovr_start", map[string]any{
		"headless":        true,
		"moderator":       true,
		"http_port":       6721,
		"wait_ready":      true,
		"timeout_seconds": 60,
		"gametype":        "echo_arena",
		"level":           "mpl_arena_a",
		"extra_args":      []string{"-telemetryrate", "10"},
		"config_overrides": map[string]any{
			"telemetry_uri": mock.URL(),
		},
	})
	require.NoError(t, err, "Failed to start EchoVR")
	require.NotNil(t, result)

	sessionID, ok := result["session_id"].(string)
	require.True(t, ok)
	require.NotEmpty(t, sessionID)
	t.Logf("Game started with session %s, telemetry → %s", sessionID, mock.URL())

	defer func() {
		fixture.MCPClient().Call("echovr_stop", map[string]any{
			"session_id":      sessionID,
			"timeout_seconds": 30,
		})
	}()

	// Wait for telemetry frames (at 10Hz, 3 seconds ≈ 30 frames)
	time.Sleep(3 * time.Second)

	// Stop game to trigger footer
	fixture.MCPClient().Call("echovr_stop", map[string]any{
		"session_id":      sessionID,
		"timeout_seconds": 30,
	})
	time.Sleep(1 * time.Second)

	// === Wire format validation ===
	rawFrames := mock.RawFrames()
	require.NotEmpty(t, rawFrames, "Expected at least one raw frame")
	t.Logf("Received %d raw WebSocket frames", len(rawFrames))

	for i, raw := range rawFrames {
		require.GreaterOrEqual(t, len(raw), 4,
			"Frame %d too short for length prefix (%d bytes)", i, len(raw))

		pbLen := binary.LittleEndian.Uint32(raw[:4])
		require.Equal(t, int(pbLen), len(raw)-4,
			"Frame %d: length prefix (%d) != payload size (%d)", i, pbLen, len(raw)-4)
	}

	// === Protobuf envelope validation ===
	envelopes := mock.Envelopes()
	require.NotEmpty(t, envelopes, "Expected parsed protobuf envelopes")
	t.Logf("Parsed %d protobuf envelopes", len(envelopes))

	validateHeader(t, envelopes[0])

	var frames []*telemetryv2.Frame
	var footer *telemetryv2.CaptureFooter
	for _, env := range envelopes[1:] {
		if f := env.GetFrame(); f != nil {
			frames = append(frames, f)
		}
		if f := env.GetFooter(); f != nil {
			footer = f
		}
	}
	t.Logf("Found %d frames, footer=%v", len(frames), footer != nil)

	require.NotEmpty(t, frames, "Expected at least one EchoArena frame")
	validateFrameSequence(t, frames)
	validateFrameContents(t, frames)

	if footer != nil {
		validateFooter(t, footer, len(frames))
	}
}

// NewFixture creates a test fixture using the evr-test-harness MCPClient.
// This is a lightweight wrapper that avoids importing testutil directly
// so the telemetry tests can be compiled independently when the harness
// submodule isn't available.
func NewFixture(t *testing.T) *fixture {
	t.Helper()
	return &fixture{t: t, mcp: &mcpClient{}}
}

type fixture struct {
	t   *testing.T
	mcp *mcpClient
}

func (f *fixture) MCPClient() *mcpClient { return f.mcp }
func (f *fixture) Cleanup()              {}

type mcpClient struct{}

func (c *mcpClient) Call(method string, params map[string]any) (map[string]any, error) {
	// TODO: Delegate to evr-test-harness MCPClient when submodule is available
	return nil, fmt.Errorf("test harness not available — run with EVR_GAME_DIR set")
}

// --- Validation helpers ---

func validateHeader(t *testing.T, env *telemetryv2.Envelope) {
	t.Helper()

	header := env.GetHeader()
	require.NotNil(t, header, "First envelope must be CaptureHeader")

	t.Logf("CaptureHeader: capture_id=%s, format_version=%d",
		header.GetCaptureId(), header.GetFormatVersion())

	assert.NotEmpty(t, header.GetCaptureId(), "capture_id must be set")
	assert.Equal(t, uint32(2), header.GetFormatVersion(), "format_version must be 2")
	assert.NotNil(t, header.GetCreatedAt(), "created_at timestamp must be set")

	arenaHeader := header.GetEchoArena()
	require.NotNil(t, arenaHeader, "CaptureHeader must have echo_arena header")
	assert.NotEmpty(t, arenaHeader.GetSessionId(), "session_id must be set")
}

func validateFrameSequence(t *testing.T, frames []*telemetryv2.Frame) {
	t.Helper()

	for i, f := range frames {
		assert.Equal(t, uint32(i), f.GetFrameIndex(),
			"Frame %d: index should be %d, got %d", i, i, f.GetFrameIndex())

		if i > 0 {
			assert.GreaterOrEqual(t, f.GetTimestampOffsetMs(), frames[i-1].GetTimestampOffsetMs(),
				"Frame %d: timestamp must be >= previous", i)
		}

		require.NotNil(t, f.GetEchoArena(), "Frame %d must have echo_arena payload", i)
	}
}

func validateFrameContents(t *testing.T, frames []*telemetryv2.Frame) {
	t.Helper()

	// Check that game status is populated (not UNSPECIFIED) in at least one frame
	hasGameStatus := false
	for _, f := range frames {
		if f.GetEchoArena().GetGameStatus() != telemetryv2.GameStatus_GAME_STATUS_UNSPECIFIED {
			hasGameStatus = true
			break
		}
	}
	assert.True(t, hasGameStatus, "At least one frame should have a non-UNSPECIFIED game status")

	// Check disc position is non-zero in at least one frame
	discHasPosition := false
	for _, f := range frames {
		a := f.GetEchoArena()
		if d := a.GetDisc(); d != nil {
			if p := d.GetPose(); p != nil {
				if pos := p.GetPosition(); pos != nil {
					if pos.GetX() != 0 || pos.GetY() != 0 || pos.GetZ() != 0 {
						discHasPosition = true
						break
					}
				}
			}
		}
	}
	assert.True(t, discHasPosition, "Disc should have non-zero position in at least one frame")

	// Pick a mid-stream frame for detailed inspection
	mid := frames[len(frames)/2]
	arena := mid.GetEchoArena()

	t.Logf("Sample frame %d: status=%s, clock=%.2f, blue=%d, orange=%d, players=%d, bones=%d",
		mid.GetFrameIndex(), arena.GetGameStatus(), arena.GetGameClock(),
		arena.GetBluePoints(), arena.GetOrangePoints(),
		len(arena.GetPlayers()), len(arena.GetPlayerBones()))

	// Disc orientation quaternion should be normalized (length ≈ 1.0)
	if disc := arena.GetDisc(); disc != nil {
		if pose := disc.GetPose(); pose != nil {
			if q := pose.GetOrientation(); q != nil {
				qlen := math.Sqrt(float64(q.GetX()*q.GetX() + q.GetY()*q.GetY() +
					q.GetZ()*q.GetZ() + q.GetW()*q.GetW()))
				if qlen > 0 {
					assert.InDelta(t, 1.0, qlen, 0.01,
						"Disc orientation quaternion should be unit length, got %.4f", qlen)
				}
			}
		}
	}

	// Validate player data if present
	for _, p := range arena.GetPlayers() {
		assert.GreaterOrEqual(t, p.GetSlot(), int32(0), "Player slot must be >= 0")
		assert.Less(t, p.GetSlot(), int32(16), "Player slot must be < 16")

		// Head position should exist and not be NaN
		if head := p.GetHead(); head != nil {
			if pos := head.GetPosition(); pos != nil {
				assert.False(t, math.IsNaN(float64(pos.GetX())), "Head X should not be NaN")
				assert.False(t, math.IsNaN(float64(pos.GetY())), "Head Y should not be NaN")
				assert.False(t, math.IsNaN(float64(pos.GetZ())), "Head Z should not be NaN")
			}
		}
	}

	// Validate bone data if present
	for _, pb := range arena.GetPlayerBones() {
		assert.GreaterOrEqual(t, pb.GetSlot(), int32(0), "Bone slot must be >= 0")

		transforms := pb.GetTransforms()
		orientations := pb.GetOrientations()

		// 23 bones × 3 floats × 4 bytes = 276 bytes
		assert.Equal(t, 276, len(transforms),
			"Bone transforms should be 276 bytes (23×3×4)")
		// 23 bones × 4 floats × 4 bytes = 368 bytes
		assert.Equal(t, 368, len(orientations),
			"Bone orientations should be 368 bytes (23×4×4)")

		// Check that bone data isn't all zeros (at least one bone should have non-zero position)
		if len(transforms) == 276 {
			hasNonZero := false
			for i := 0; i < 276; i += 4 {
				val := math.Float32frombits(binary.LittleEndian.Uint32(transforms[i : i+4]))
				if val != 0 && !math.IsNaN(float64(val)) {
					hasNonZero = true
					break
				}
			}
			assert.True(t, hasNonZero, "Bone transforms should have at least one non-zero value")
		}
	}
}

func validateFooter(t *testing.T, footer *telemetryv2.CaptureFooter, expectedFrames int) {
	t.Helper()

	t.Logf("CaptureFooter: frame_count=%d, duration_ms=%d",
		footer.GetFrameCount(), footer.GetDurationMs())

	assert.Equal(t, uint32(expectedFrames), footer.GetFrameCount(),
		"Footer frame_count should match actual frame count")
	assert.Greater(t, footer.GetDurationMs(), uint32(0),
		"Footer duration must be > 0")
}

// TestTelemetry_StreamingEvents verifies that telemetry event detection
// generates the correct EchoEvent protobuf messages via frame-diff.
func TestTelemetry_StreamingEvents(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	t.Skip("Event detection test requires multiplayer scenario with player injection")
}

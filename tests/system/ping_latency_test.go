package system

import (
	"crypto/rand"
	"encoding/binary"
	"fmt"
	"net"
	"sort"
	"testing"
	"time"

	"github.com/EchoTools/evr-test-harness/pkg/testutil"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

const (
	// Broadcaster ping protocol symbols (little-endian uint64)
	pingRequestSymbol = 0x997279DE065A03B0
	pingAckSymbol     = 0x4F7AE556E0B77891

	// Default broadcaster UDP port
	broadcasterPort = 6792

	// Ping test parameters
	pingCount   = 10
	pingTimeout = 2 * time.Second
)

// sendBroadcasterPing sends a single ping to the broadcaster and returns
// the round-trip time. The protocol is 16 bytes each direction:
//
//	Request:  [symbol 0x997279DE065A03B0 LE (8)] [random token (8)]
//	Response: [symbol 0x4F7AE556E0B77891 LE (8)] [echoed token (8)]
func sendBroadcasterPing(addr string, timeout time.Duration) (time.Duration, error) {
	conn, err := net.Dial("udp", addr)
	if err != nil {
		return 0, fmt.Errorf("dial: %w", err)
	}
	defer conn.Close()

	// Build request: symbol + random token
	var req [16]byte
	binary.LittleEndian.PutUint64(req[:8], pingRequestSymbol)
	if _, err := rand.Read(req[8:]); err != nil {
		return 0, fmt.Errorf("generate token: %w", err)
	}

	if err := conn.SetDeadline(time.Now().Add(timeout)); err != nil {
		return 0, fmt.Errorf("set deadline: %w", err)
	}

	start := time.Now()
	if _, err := conn.Write(req[:]); err != nil {
		return 0, fmt.Errorf("send: %w", err)
	}

	var resp [16]byte
	n, err := conn.Read(resp[:])
	rtt := time.Since(start)
	if err != nil {
		return 0, fmt.Errorf("recv: %w", err)
	}
	if n != 16 {
		return 0, fmt.Errorf("unexpected response size: %d", n)
	}

	// Validate ack symbol
	ackSym := binary.LittleEndian.Uint64(resp[:8])
	if ackSym != pingAckSymbol {
		return 0, fmt.Errorf("unexpected ack symbol: 0x%016X", ackSym)
	}

	// Validate echoed token
	for i := 8; i < 16; i++ {
		if req[i] != resp[i] {
			return 0, fmt.Errorf("token mismatch at byte %d", i)
		}
	}

	return rtt, nil
}

// measurePingLatencies sends multiple pings and returns all RTT values.
func measurePingLatencies(t *testing.T, addr string, count int) []time.Duration {
	t.Helper()
	var rtts []time.Duration

	for i := 0; i < count; i++ {
		rtt, err := sendBroadcasterPing(addr, pingTimeout)
		if err != nil {
			t.Logf("ping %d/%d failed: %v", i+1, count, err)
			continue
		}
		rtts = append(rtts, rtt)
		t.Logf("ping %d/%d: %v", i+1, count, rtt)
	}

	return rtts
}

// medianDuration returns the median of a sorted duration slice.
func medianDuration(rtts []time.Duration) time.Duration {
	sorted := make([]time.Duration, len(rtts))
	copy(sorted, rtts)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
	return sorted[len(sorted)/2]
}

// TestPingLatency_IdleServer validates that the server's idle tick rate
// causes measurably high ping latency. At 6Hz (166ms/tick), median RTT
// should exceed 80ms due to the packet waiting in the socket buffer
// until the next tick's recvfrom drain.
//
// This test validates the premise — it should PASS before the fix.
func TestPingLatency_IdleServer(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	f := testutil.NewFixture(t)
	defer f.Cleanup()

	// Start server — it enters lobby state at 6Hz idle tick rate
	result, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6780,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 60,
	})
	require.NoError(t, err, "Failed to start Echo VR server")

	sessionID := result["session_id"].(string)
	f.Sessions = append(f.Sessions, sessionID)

	// Wait for HTTP API (confirms server is in lobby state)
	err = f.WaitForHTTP("http://localhost:6780/session", 30*time.Second)
	require.NoError(t, err, "HTTP API did not become available")

	// Let the server settle into idle tick rate
	time.Sleep(2 * time.Second)

	// Measure ping latency
	addr := fmt.Sprintf("127.0.0.1:%d", broadcasterPort)
	rtts := measurePingLatencies(t, addr, pingCount)
	require.GreaterOrEqual(t, len(rtts), 5, "Need at least 5 successful pings")

	median := medianDuration(rtts)
	t.Logf("Median RTT: %v (from %d samples)", median, len(rtts))

	// At 6Hz idle, median should be well above 80ms.
	// Packets wait 0-166ms to be read, plus processing time.
	assert.Greater(t, median, 80*time.Millisecond,
		"Median RTT should exceed 80ms at 6Hz idle tick rate (got %v)", median)
}

// TestPingLatency_Fast asserts that ping latency stays below 50ms.
// This test should FAIL before the WSAPoll fix (proving the regression
// gate works) and PASS after.
func TestPingLatency_Fast(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	deployAllDLLs(t)
	defer cleanupAllDLLs(t)

	f := testutil.NewFixture(t)
	defer f.Cleanup()

	// Start server — enters lobby state
	result, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       6781,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 60,
	})
	require.NoError(t, err, "Failed to start Echo VR server")

	sessionID := result["session_id"].(string)
	f.Sessions = append(f.Sessions, sessionID)

	err = f.WaitForHTTP("http://localhost:6781/session", 30*time.Second)
	require.NoError(t, err, "HTTP API did not become available")

	time.Sleep(2 * time.Second)

	addr := fmt.Sprintf("127.0.0.1:%d", broadcasterPort)
	rtts := measurePingLatencies(t, addr, pingCount)
	require.GreaterOrEqual(t, len(rtts), 5, "Need at least 5 successful pings")

	median := medianDuration(rtts)
	t.Logf("Median RTT: %v (from %d samples)", median, len(rtts))

	// Regression gate: with WSAPoll, median should be well under 50ms
	assert.Less(t, median, 50*time.Millisecond,
		"Median RTT should be under 50ms with event-driven recv (got %v)", median)

	// No outlier should exceed 100ms
	sorted := make([]time.Duration, len(rtts))
	copy(sorted, rtts)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
	maxRTT := sorted[len(sorted)-1]
	t.Logf("Max RTT: %v", maxRTT)
	assert.Less(t, maxRTT, 100*time.Millisecond,
		"Max RTT should be under 100ms (got %v)", maxRTT)
}

package system

// #2109: an idle dedicated server answers a UDP raw ping as it arrives, idles at its idle tick, and spends little CPU
// doing so. This test measures all three; it reads no log line for the ping, which it times itself.
//
// Two ways to run it (it skips otherwise, and in -short):
//
//   - Attach to a server that is already running, on this host:
//       NEVR_TIMING_ADDR=127.0.0.1:6792   the server's broadcaster UDP address
//       NEVR_TIMING_PID=<pid>             optional: its echovr.exe process, for the CPU figure
//       NEVR_TIMING_LOG=<run log>         optional: its nevr-*.jsonl run log, for the loop rate (diagnostics.frame_stats
//                                         must be on in its config.yaml)
//     Nothing is started or written. This is how a build that is already deployed (v6.16.4, or this one) is measured.
//
//   - Start an offline server on a copy of the install:
//       NEVR_TIMING_START=1 EVR_GAME_DIR=<copy>/echovr NEVR_BUILD_DIR=<build dir with bin/BugSplat64.dll and
//       bin/nevr-runtime.dll>
//     It refuses a copy that already holds a config.yaml in its nevr root. It deploys the two DLLs, writes a config.yaml
//     with no service (services.socket_uri "" and services.allow_offline_server true, so it never logs in; an
//     auth.password, so it runs no device-code sign-in) and diagnostics.frame_stats on, starts `wine echovr.exe -server -headless -noovr -port <port>`, measures, stops the
//     game and the Wine server, and removes what it wrote. Run it in its own scope in nevr-live.slice (AGENTS.md).
//
// The thresholds assume the pinger and the server share a host (loopback): a round trip there is the server's own
// delay. They are package constants; NEVR_TIMING_CPU_MAX_PCT overrides the CPU limit for a host whose baseline differs.

import (
	"bufio"
	"crypto/rand"
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
)

const (
	// CMatSym::Hash("raw_ping_request") and CMatSym::Hash("raw_ping_acknowledge"), as the game compares and writes
	// them (src/runtime/patch/server_frame_wait.h; tools/tests/test_server_frame_wait_wiring.py recomputes both).
	rawPingRequest     = 0x997279de065a03b0
	rawPingAcknowledge = 0x4f7ae556e0b77891

	timingPingCount   = 60
	timingPingTimeout = 2 * time.Second

	// A ping answered in the frame wait comes back in well under a millisecond on loopback. One that waits for the
	// next frame comes back after up to a whole step: 8.3 ms at 120 Hz (median about 4 ms), 166 ms at the idle rate.
	timingPingMedianMax = 2 * time.Millisecond
	timingPingP90Max    = 5 * time.Millisecond

	// The idle rate is 6 a second (src/runtime/lifecycle/server_timestep.h kIdleTickRateHz): at most 60 frames in the
	// probe's 10 s window, with a fifth on top for the probe's own window edges. v6.16.4 reads 1201.
	timingIdleFramesMax = 72

	// Percent of one core over the CPU window. v6.16.4 idle measured 29% on a cloud vCPU and 155% on a desktop i9.
	timingCPUMaxPctDefault = 10.0
	timingCPUWindow        = 10 * time.Second
)

var frameStatsLine = regexp.MustCompile(`frame_stats window_s=(\d+) frames=(\d+)`)

// sendRawPing sends one raw ping and returns the round trip, checking the answer's symbol and token.
func sendRawPing(addr string, timeout time.Duration) (time.Duration, error) {
	conn, err := net.Dial("udp", addr)
	if err != nil {
		return 0, fmt.Errorf("dial: %w", err)
	}
	defer conn.Close()
	var req [16]byte
	binary.LittleEndian.PutUint64(req[:8], rawPingRequest)
	if _, err := rand.Read(req[8:]); err != nil {
		return 0, fmt.Errorf("token: %w", err)
	}
	if err := conn.SetDeadline(time.Now().Add(timeout)); err != nil {
		return 0, err
	}
	start := time.Now()
	if _, err := conn.Write(req[:]); err != nil {
		return 0, fmt.Errorf("send: %w", err)
	}
	var resp [64]byte
	n, err := conn.Read(resp[:])
	rtt := time.Since(start)
	if err != nil {
		return 0, fmt.Errorf("receive: %w", err)
	}
	if n != 16 {
		return 0, fmt.Errorf("answer is %d bytes, not 16", n)
	}
	if got := binary.LittleEndian.Uint64(resp[:8]); got != rawPingAcknowledge {
		return 0, fmt.Errorf("answer symbol 0x%016x, not the acknowledge symbol", got)
	}
	if string(resp[8:16]) != string(req[8:16]) {
		return 0, fmt.Errorf("answer token differs from the request's")
	}
	return rtt, nil
}

// measurePings sends timingPingCount pings, each after a random pause of up to 25 ms so they land at every phase of
// the server's frame, and returns the round trips of those answered.
func measurePings(t *testing.T, addr string) []time.Duration {
	t.Helper()
	var rtts []time.Duration
	var pause [1]byte
	for i := 0; i < timingPingCount; i++ {
		_, _ = rand.Read(pause[:])
		time.Sleep(time.Duration(pause[0]%25) * time.Millisecond)
		rtt, err := sendRawPing(addr, timingPingTimeout)
		if err != nil {
			t.Logf("ping %d: %v", i+1, err)
			continue
		}
		rtts = append(rtts, rtt)
	}
	return rtts
}

func percentile(sorted []time.Duration, p float64) time.Duration {
	if len(sorted) == 0 {
		return 0
	}
	i := int(p * float64(len(sorted)-1))
	return sorted[i]
}

// statTicks is utime + stime (fields 14 and 15) of a /proc stat file, a process's or one thread's, in clock ticks.
func statTicks(path string) (uint64, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	text := string(raw)
	end := strings.LastIndexByte(text, ')')
	if end < 0 {
		return 0, fmt.Errorf("unreadable %s", path)
	}
	fields := strings.Fields(text[end+2:])
	if len(fields) < 13 {
		return 0, fmt.Errorf("short %s", path)
	}
	utime, err1 := strconv.ParseUint(fields[11], 10, 64)
	stime, err2 := strconv.ParseUint(fields[12], 10, 64)
	if err1 != nil || err2 != nil {
		return 0, fmt.Errorf("bad times in %s", path)
	}
	return utime + stime, nil
}

// cpuTicks is utime + stime of a process, in clock ticks.
func cpuTicks(pid int) (uint64, error) { return statTicks(fmt.Sprintf("/proc/%d/stat", pid)) }

type threadTime struct {
	name  string
	ticks uint64
}

// threadTicks is utime + stime of every thread of a process, by tid, with the thread's name (/proc/<pid>/task/<tid>/comm,
// which Wine sets from the game's own thread names). A thread that ends between the two reads is left out.
func threadTicks(pid int) map[int]threadTime {
	out := map[int]threadTime{}
	entries, err := os.ReadDir(fmt.Sprintf("/proc/%d/task", pid))
	if err != nil {
		return out
	}
	for _, e := range entries {
		tid, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		ticks, err := statTicks(fmt.Sprintf("/proc/%d/task/%d/stat", pid, tid))
		if err != nil {
			continue
		}
		name, _ := os.ReadFile(fmt.Sprintf("/proc/%d/task/%d/comm", pid, tid))
		out[tid] = threadTime{name: strings.TrimSpace(string(name)), ticks: ticks}
	}
	return out
}

// threadBreakdown is the CPU each thread used between two samples, as a percent of one core, busiest first, as text:
// the main thread and the job workers run once per frame, so this says how much of an idle server's cost follows the
// frame and how much runs on a clock of its own.
func threadBreakdown(before, after map[int]threadTime, seconds float64, top int) string {
	type row struct {
		tid  int
		name string
		pct  float64
	}
	var rows []row
	for tid, a := range after {
		b, ok := before[tid]
		if !ok || a.ticks < b.ticks {
			continue
		}
		rows = append(rows, row{tid, a.name, float64(a.ticks-b.ticks) / 100.0 / seconds * 100.0})
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].pct > rows[j].pct })
	var parts []string
	for i, r := range rows {
		if i == top || r.pct == 0 {
			break
		}
		parts = append(parts, fmt.Sprintf("%d %q %.1f%%", r.tid, r.name, r.pct))
	}
	return fmt.Sprintf("%d threads; busiest: %s", len(after), strings.Join(parts, ", "))
}

// cpuPercent is the process's CPU over `window` as a percent of one core (USER_HZ is 100 on Linux x86-64), and the
// per-thread breakdown of the same window.
func cpuPercent(pid int, window time.Duration) (float64, string, error) {
	threadsBefore := threadTicks(pid)
	before, err := cpuTicks(pid)
	if err != nil {
		return 0, "", err
	}
	start := time.Now()
	time.Sleep(window)
	after, err := cpuTicks(pid)
	threadsAfter := threadTicks(pid)
	if err != nil {
		return 0, "", err
	}
	seconds := time.Since(start).Seconds()
	return float64(after-before) / 100.0 / seconds * 100.0, threadBreakdown(threadsBefore, threadsAfter, seconds, 10), nil
}

// frameStats returns the frames counted by each `frame_stats` line in the run log, oldest first.
func frameStats(logPath string) ([]int, error) {
	f, err := os.Open(logPath)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	var frames []int
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 0, 1<<16), 1<<20)
	for sc.Scan() {
		if m := frameStatsLine.FindStringSubmatch(sc.Text()); m != nil && m[1] == "10" {
			n, _ := strconv.Atoi(m[2])
			frames = append(frames, n)
		}
	}
	return frames, sc.Err()
}

// udpPortBound reports whether any local UDP socket (IPv4 or IPv6) is bound to `port`, from /proc/net/udp{,6}.
func udpPortBound(port int) bool {
	want := fmt.Sprintf(":%04X ", port)
	for _, table := range []string{"/proc/net/udp", "/proc/net/udp6"} {
		raw, err := os.ReadFile(table)
		if err != nil {
			continue
		}
		for _, line := range strings.Split(string(raw), "\n")[1:] {
			fields := strings.Fields(line)
			if len(fields) > 1 && strings.HasSuffix(fields[1]+" ", want) {
				return true
			}
		}
	}
	return false
}

// timingMeasure runs the three measurements against a running server and fails on each figure past its limit. The
// ping part is a subtest: when `pingPort` is above 0 and nothing on this host is bound to it (an offline server never
// begins multiplayer, so its broadcaster never binds), it is skipped with that reason, never passed.
func timingMeasure(t *testing.T, addr string, pingPort int, pid int, logPath func() string) {
	t.Helper()
	t.Run("raw_ping", func(t *testing.T) {
		if pingPort > 0 && !udpPortBound(pingPort) {
			t.Skipf("NOT MEASURED: nothing is bound to UDP %d on this host (an offline server never begins multiplayer, "+
				"so its broadcaster never binds); measure a registered server with NEVR_TIMING_ADDR", pingPort)
		}
		timingPings(t, addr)
	})
	timingIdle(t, pid, logPath)
}

func timingPings(t *testing.T, addr string) {
	rtts := measurePings(t, addr)
	if len(rtts) < timingPingCount/2 {
		t.Errorf("only %d of %d raw pings were answered at %s", len(rtts), timingPingCount, addr)
	} else {
		sort.Slice(rtts, func(i, j int) bool { return rtts[i] < rtts[j] })
		med, p90, max := percentile(rtts, 0.5), percentile(rtts, 0.9), rtts[len(rtts)-1]
		t.Logf("raw ping round trip: answered=%d/%d median=%v p90=%v max=%v", len(rtts), timingPingCount, med, p90, max)
		if med > timingPingMedianMax {
			t.Errorf("median ping round trip %v is above %v: pings wait for the next frame", med, timingPingMedianMax)
		}
		if p90 > timingPingP90Max {
			t.Errorf("p90 ping round trip %v is above %v", p90, timingPingP90Max)
		}
	}
}

func timingIdle(t *testing.T, pid int, logPath func() string) {
	if pid > 0 {
		maxPct := timingCPUMaxPctDefault
		if v := os.Getenv("NEVR_TIMING_CPU_MAX_PCT"); v != "" {
			if f, err := strconv.ParseFloat(v, 64); err == nil {
				maxPct = f
			}
		}
		pct, threads, err := cpuPercent(pid, timingCPUWindow)
		if err != nil {
			t.Errorf("CPU of pid %d: %v", pid, err)
		} else {
			t.Logf("idle CPU: %.1f%% of one core over %v (pid %d)", pct, timingCPUWindow, pid)
			t.Logf("idle CPU by thread: %s", threads)
			if pct > maxPct {
				t.Errorf("idle CPU %.1f%% of one core is above %.1f%%", pct, maxPct)
			}
		}
	}

	if logPath != nil {
		path := logPath()
		frames, err := frameStats(path)
		switch {
		case err != nil:
			t.Errorf("run log %s: %v", path, err)
		case len(frames) == 0:
			t.Errorf("no frame_stats line in %s (is diagnostics.frame_stats on?)", path)
		default:
			last := frames[len(frames)-1]
			t.Logf("idle loop: frame_stats frames=%d per 10 s (all windows: %v)", last, frames)
			if last > timingIdleFramesMax {
				t.Errorf("idle loop ran %d frames in 10 s, above %d: the server is not on its idle tick", last,
					timingIdleFramesMax)
			}
		}
	}
}

func TestServerTimingIdle(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping a live server measurement in short mode")
	}
	if addr := os.Getenv("NEVR_TIMING_ADDR"); addr != "" {
		pid, _ := strconv.Atoi(os.Getenv("NEVR_TIMING_PID"))
		var logPath func() string
		if p := os.Getenv("NEVR_TIMING_LOG"); p != "" {
			logPath = func() string { return p }
		}
		timingMeasure(t, addr, 0, pid, logPath)
		return
	}
	if os.Getenv("NEVR_TIMING_START") != "1" {
		t.Skip("set NEVR_TIMING_ADDR to measure a running server, or NEVR_TIMING_START=1 with EVR_GAME_DIR (a copy) to start one")
	}
	timingStartAndMeasure(t)
}

func timingStartAndMeasure(t *testing.T) {
	gameDir, err := filepath.Abs(getGameDir())
	if err != nil {
		t.Fatal(err)
	}
	exe := filepath.Join(gameDir, "bin", "win10", "echovr.exe")
	if _, err := os.Stat(exe); err != nil {
		t.Fatalf("EVR_GAME_DIR has no bin/win10/echovr.exe: %v", err)
	}
	refuseCopyCredentials(t)
	root := nevrRootDir(gameDir)
	configPath := filepath.Join(root, "config.yaml")
	if _, err := os.Stat(configPath); err == nil {
		t.Fatalf("%s exists; this test writes its own config and does not overwrite one", configPath)
	}
	for _, dll := range []string{"BugSplat64.dll", runtimeDLLName} {
		if err := deployDLL(getDLLPath(dll), gameDir); err != nil {
			t.Fatalf("deploy %s: %v", dll, err)
		}
	}
	defer cleanupAllDLLs(t)
	if err := os.MkdirAll(root, 0o755); err != nil {
		t.Fatal(err)
	}
	// No service at all: no socket_uri (no bridge, never logs in or registers), an auth.password (a server with one
	// runs no device-code sign-in, token_auth.cpp nevr_token_auth::Init) and an auth address nothing listens on, so
	// a mistake here still reaches no service.
	config := "services:\n  socket_uri: \"\"\n  allow_offline_server: true\n" +
		"auth:\n  password: \"offline-timing-test\"\n  http_uri: \"http://127.0.0.1:9\"\n" +
		"diagnostics:\n  frame_stats: true\n"
	if err := os.WriteFile(configPath, []byte(config), 0o644); err != nil {
		t.Fatal(err)
	}
	defer os.Remove(configPath)

	port := 6792
	if v := os.Getenv("NEVR_TIMING_PORT"); v != "" {
		port, _ = strconv.Atoi(v)
	}
	logDir := filepath.Join(root, "logs")
	started := time.Now()
	cmd := exec.Command("wine", exe, "-server", "-headless", "-noovr", "-port", strconv.Itoa(port))
	cmd.Dir = filepath.Dir(exe)
	env := []string{}
	for _, kv := range os.Environ() {
		if strings.HasPrefix(kv, "DISPLAY=") || strings.HasPrefix(kv, "WAYLAND_DISPLAY=") || strings.HasPrefix(kv, "WINEPREFIX=") {
			continue
		}
		env = append(env, kv)
	}
	cmd.Env = append(env, "WINEPREFIX="+filepath.Join(gameDir, ".wineprefix"), "WINEDEBUG=-all")
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	if err := cmd.Start(); err != nil {
		t.Fatalf("start the server: %v", err)
	}
	defer func() {
		_ = syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
		_ = cmd.Wait()
		kill := exec.Command("wineserver", "-k")
		kill.Env = cmd.Env
		_ = kill.Run()
	}()

	// The run log is the nevr-*.jsonl written after the start that holds the frame-time probe's lines (every build
	// with diagnostics.frame_stats logs them, the one being measured against included). The game needs 15-20 s of
	// splash before any runtime line (AGENTS.md "Startup timing"); give it 120 s.
	var runLog string
	deadline := time.Now().Add(120 * time.Second)
	for time.Now().Before(deadline) && runLog == "" {
		time.Sleep(2 * time.Second)
		matches, _ := filepath.Glob(filepath.Join(logDir, "nevr-*.jsonl"))
		for _, m := range matches {
			if st, err := os.Stat(m); err == nil && st.ModTime().After(started) {
				if raw, err := os.ReadFile(m); err == nil && strings.Contains(string(raw), "frame_stats") {
					runLog = m
				}
			}
		}
	}
	if runLog == "" {
		t.Fatalf("no run log under %s logged a frame_stats line within 120 s", logDir)
	}
	t.Logf("run log: %s", runLog)
	// Settle, then measure. The loop-rate check needs two full frame_stats windows after the boot.
	time.Sleep(25 * time.Second)
	timingMeasure(t, fmt.Sprintf("127.0.0.1:%d", port), port, cmd.Process.Pid, func() string { return runLog })
}

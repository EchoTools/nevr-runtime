package system

import (
	"os"
	"os/exec"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/require"
)

// TestCtrlC_ClientHandlerReArmed verifies RearmConsoleCtrlHandler is called
// in client mode so CTRL+C reaches our handler first.
// Regression test for 2026-08-04 bug: CTRL+C was ignored because our handler
// sat behind the game's, and RearmConsoleCtrlHandler was only called in server mode.
func TestCtrlC_ClientHandlerReArmed(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dllPath := getDLLPath("BugSplat64.dll")
	gameDir := getGameDir()

	_, err := os.Stat(dllPath)
	if os.IsNotExist(err) {
		t.Skipf("BugSplat64.dll not found at %s (build required)", dllPath)
	}
	require.NoError(t, err)

	err = deployDLL(dllPath, gameDir)
	require.NoError(t, err)
	defer cleanupAllDLLs(t)

	exePath := gameDir + "/bin/win10/echovr.exe"
	cmd := exec.Command("wine", exePath, "-noovr", "-spectatorstream")
	cmd.Dir = gameDir + "/bin/win10"
	cmd.Env = append(os.Environ(),
		"DISPLAY=:101",
		"WINEPREFIX="+gameDir+"/.wineprefix",
	)

	var stdout strings.Builder
	cmd.Stdout = &stdout
	cmd.Stderr = &stdout

	err = cmd.Start()
	require.NoError(t, err, "Failed to start echovr.exe")
	defer func() {
		cmd.Process.Kill()
		cmd.Wait()
	}()

	// Wait for the re-arm log message
	rearmed := make(chan bool, 1)
	go func() {
		for i := 0; i < 60; i++ {
			time.Sleep(500 * time.Millisecond)
			out := stdout.String()
			if strings.Contains(out, "console ctrl handler re-armed") {
				rearmed <- true
				return
			}
		}
		rearmed <- false
	}()

	select {
	case ok := <-rearmed:
		if !ok {
			t.Error("BUG: RearmConsoleCtrlHandler not called — CTRL+C handler is still behind the game's handler")
		}
	case <-time.After(35 * time.Second):
		t.Error("BUG: RearmConsoleCtrlHandler not called within 35s — CTRL+C handler not re-armed")
	}
}

// TestWindowedImpliesNoOvr verifies -windowed implies -noovr.
// Regression test for 2026-08-04 bug: -windowed alone tried to load Oculus DLLs.
func TestWindowedImpliesNoOvr(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	dllPath := getDLLPath("BugSplat64.dll")
	gameDir := getGameDir()

	_, err := os.Stat(dllPath)
	if os.IsNotExist(err) {
		t.Skipf("BugSplat64.dll not found at %s (build required)", dllPath)
	}
	require.NoError(t, err)

	err = deployDLL(dllPath, gameDir)
	require.NoError(t, err)
	defer cleanupAllDLLs(t)

	exePath := gameDir + "/bin/win10/echovr.exe"
	cmd := exec.Command("wine", exePath, "-windowed", "-mp")
	cmd.Dir = gameDir + "/bin/win10"
	cmd.Env = append(os.Environ(),
		"DISPLAY=:101",
		"WINEPREFIX="+gameDir+"/.wineprefix",
	)

	var stdout strings.Builder
	cmd.Stdout = &stdout
	cmd.Stderr = &stdout

	cmd.Start()
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()

	select {
	case <-done:
		out := stdout.String()
		require.NotContains(t, out, "Failed to initialize OVR library",
			"BUG: -windowed tried to load Oculus VR library — should imply -noovr")
		require.NotContains(t, out, "Unable to load LibOVRRT",
			"BUG: -windowed tried to load LibOVRRT")
	case <-time.After(25 * time.Second):
		out := stdout.String()
		require.NotContains(t, out, "Failed to initialize OVR library",
			"BUG: -windowed tried to load Oculus VR library")
		cmd.Process.Kill()
		cmd.Wait()
	}
}

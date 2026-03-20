package system

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// TestDLLLoading_Gamepatches verifies gamepatches.dll can be deployed to the game directory
func TestDLLLoading_Gamepatches(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// Setup
	dllPath := getDLLPath("gamepatches.dll")
	gameDir := getGameDir()

	// Verify source DLL exists
	_, err := os.Stat(dllPath)
	if os.IsNotExist(err) {
		t.Skipf("gamepatches.dll not found at %s (build required)", dllPath)
	}
	require.NoError(t, err, "Failed to stat gamepatches.dll")

	// Deploy DLL
	err = deployDLL(dllPath, gameDir)
	require.NoError(t, err, "Failed to deploy gamepatches.dll")

	// Cleanup
	defer cleanupAllDLLs(t)

	// Verify DLL was deployed
	targetPath := filepath.Join(gameDir, "bin", "win10", "gamepatches.dll")
	info, err := os.Stat(targetPath)
	require.NoError(t, err, "Deployed DLL not found at target location")
	assert.Greater(t, info.Size(), int64(0), "Deployed DLL has zero size")

	t.Logf("gamepatches.dll deployed successfully to %s (%d bytes)", targetPath, info.Size())
}

// TestDLLLoading_Gameserver verifies gameserver.dll can be deployed to the game directory
func TestDLLLoading_Gameserver(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// Setup
	dllPath := getDLLPath("gameserver.dll")
	gameDir := getGameDir()

	// Verify source DLL exists
	_, err := os.Stat(dllPath)
	if os.IsNotExist(err) {
		t.Skipf("gameserver.dll not found at %s (build required)", dllPath)
	}
	require.NoError(t, err, "Failed to stat gameserver.dll")

	// Deploy DLL
	err = deployDLL(dllPath, gameDir)
	require.NoError(t, err, "Failed to deploy gameserver.dll")

	// Cleanup
	defer cleanupAllDLLs(t)

	// Verify DLL was deployed
	targetPath := filepath.Join(gameDir, "bin", "win10", "gameserver.dll")
	info, err := os.Stat(targetPath)
	require.NoError(t, err, "Deployed DLL not found at target location")
	assert.Greater(t, info.Size(), int64(0), "Deployed DLL has zero size")

	t.Logf("gameserver.dll deployed successfully to %s (%d bytes)", targetPath, info.Size())
}

// TestDLLLoading_Telemetryagent verifies telemetryagent.dll can be deployed to the game directory
func TestDLLLoading_Telemetryagent(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// Setup
	dllPath := getDLLPath("telemetryagent.dll")
	gameDir := getGameDir()

	// Verify source DLL exists
	_, err := os.Stat(dllPath)
	if os.IsNotExist(err) {
		t.Skipf("telemetryagent.dll not found at %s (build required)", dllPath)
	}
	require.NoError(t, err, "Failed to stat telemetryagent.dll")

	// Deploy DLL
	err = deployDLL(dllPath, gameDir)
	require.NoError(t, err, "Failed to deploy telemetryagent.dll")

	// Cleanup
	defer cleanupAllDLLs(t)

	// Verify DLL was deployed
	targetPath := filepath.Join(gameDir, "bin", "win10", "telemetryagent.dll")
	info, err := os.Stat(targetPath)
	require.NoError(t, err, "Deployed DLL not found at target location")
	assert.Greater(t, info.Size(), int64(0), "Deployed DLL has zero size")

	t.Logf("telemetryagent.dll deployed successfully to %s (%d bytes)", targetPath, info.Size())
}

// TestDLLLoading_AllDLLs verifies all NEVR DLLs can be deployed together
func TestDLLLoading_AllDLLs(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	gameDir := getGameDir()

	// Verify all source DLLs exist
	dllNames := []string{"gamepatches.dll", "gameserver.dll", "telemetryagent.dll"}
	missingDLLs := []string{}

	for _, dllName := range dllNames {
		dllPath := getDLLPath(dllName)
		if _, err := os.Stat(dllPath); os.IsNotExist(err) {
			missingDLLs = append(missingDLLs, dllName)
		}
	}

	if len(missingDLLs) > 0 {
		t.Skipf("DLLs not found (build required): %v", missingDLLs)
	}

	// Deploy all DLLs
	deployAllDLLs(t)

	// Cleanup
	defer cleanupAllDLLs(t)

	// Verify all DLLs were deployed
	for _, dllName := range dllNames {
		targetPath := filepath.Join(gameDir, "bin", "win10", dllName)
		info, err := os.Stat(targetPath)
		require.NoError(t, err, "Deployed DLL %s not found at target location", dllName)
		assert.Greater(t, info.Size(), int64(0), "Deployed DLL %s has zero size", dllName)
		t.Logf("%s deployed successfully (%d bytes)", dllName, info.Size())
	}

	t.Log("All NEVR DLLs deployed successfully")
}

// TestDLLLoading_CleanupRemovesAllDLLs verifies cleanup function removes all deployed DLLs
func TestDLLLoading_CleanupRemovesAllDLLs(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	gameDir := getGameDir()
	dllNames := []string{"gamepatches.dll", "gameserver.dll", "telemetryagent.dll"}

	// Skip if DLLs don't exist
	missingDLLs := []string{}
	for _, dllName := range dllNames {
		dllPath := getDLLPath(dllName)
		if _, err := os.Stat(dllPath); os.IsNotExist(err) {
			missingDLLs = append(missingDLLs, dllName)
		}
	}

	if len(missingDLLs) > 0 {
		t.Skipf("DLLs not found (build required): %v", missingDLLs)
	}

	// Deploy all DLLs
	deployAllDLLs(t)

	// Verify DLLs exist
	for _, dllName := range dllNames {
		targetPath := filepath.Join(gameDir, "bin", "win10", dllName)
		_, err := os.Stat(targetPath)
		require.NoError(t, err, "DLL %s should exist after deployment", dllName)
	}

	// Cleanup
	cleanupAllDLLs(t)

	// Verify DLLs were removed
	for _, dllName := range dllNames {
		targetPath := filepath.Join(gameDir, "bin", "win10", dllName)
		_, err := os.Stat(targetPath)
		assert.True(t, os.IsNotExist(err), "DLL %s should not exist after cleanup", dllName)
	}

	t.Log("All NEVR DLLs cleaned up successfully")
}

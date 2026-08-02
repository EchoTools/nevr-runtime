package system

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// TestDLLLoading_BugSplat verifies BugSplat64.dll can be deployed to the game directory
func TestDLLLoading_BugSplat(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	// Setup
	dllPath := getDLLPath("BugSplat64.dll")
	gameDir := getGameDir()

	// Verify source DLL exists
	_, err := os.Stat(dllPath)
	if os.IsNotExist(err) {
		t.Skipf("BugSplat64.dll not found at %s (build required)", dllPath)
	}
	require.NoError(t, err, "Failed to stat BugSplat64.dll")

	// Deploy DLL
	err = deployDLL(dllPath, gameDir)
	require.NoError(t, err, "Failed to deploy BugSplat64.dll")

	// Cleanup
	defer cleanupAllDLLs(t)

	// Verify DLL was deployed
	targetPath := filepath.Join(gameDir, "bin", "win10", "BugSplat64.dll")
	info, err := os.Stat(targetPath)
	require.NoError(t, err, "Deployed DLL not found at target location")
	assert.Greater(t, info.Size(), int64(0), "Deployed DLL has zero size")

	t.Logf("BugSplat64.dll deployed successfully to %s (%d bytes)", targetPath, info.Size())
}

// TestDLLLoading_AllDLLs verifies the monolithic BugSplat64.dll can be deployed
func TestDLLLoading_AllDLLs(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	gameDir := getGameDir()

	// Verify source DLL exists
	dllNames := []string{"BugSplat64.dll"}
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

	t.Log("Monolithic BugSplat64.dll deployed successfully")
}

// TestDLLLoading_CleanupRemovesAllDLLs verifies cleanup removes the deployed DLL
func TestDLLLoading_CleanupRemovesAllDLLs(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	gameDir := getGameDir()
	dllNames := []string{"BugSplat64.dll"}

	// Skip if DLL doesn't exist
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

	// Verify DLL exists
	for _, dllName := range dllNames {
		targetPath := filepath.Join(gameDir, "bin", "win10", dllName)
		_, err := os.Stat(targetPath)
		require.NoError(t, err, "DLL %s should exist after deployment", dllName)
	}

	// Cleanup
	cleanupAllDLLs(t)

	// Verify DLL was removed
	for _, dllName := range dllNames {
		targetPath := filepath.Join(gameDir, "bin", "win10", dllName)
		_, err := os.Stat(targetPath)
		assert.True(t, os.IsNotExist(err), "DLL %s should not exist after cleanup", dllName)
	}

	t.Log("BugSplat64.dll cleaned up successfully")
}

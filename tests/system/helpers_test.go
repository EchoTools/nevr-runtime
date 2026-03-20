package system

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"testing"

	"github.com/EchoTools/evr-test-harness/pkg/testutil"
	"github.com/stretchr/testify/require"
)

// Environment variable constants for paths
const (
	// NEVR_BUILD_DIR points to the build output directory containing DLL files
	envBuildDir     = "NEVR_BUILD_DIR"
	defaultBuildDir = "../../dist"

	// EVR_GAME_DIR points to the Echo VR game installation directory
	envGameDir     = "EVR_GAME_DIR"
	defaultGameDir = "../../ready-at-dawn-echo-arena"
)

// Helper functions from evr-test-harness
var (
	AssertProcessRunning = testutil.AssertProcessRunning
	AssertProcessStopped = testutil.AssertProcessStopped
	AssertHTTPResponds   = testutil.AssertHTTPResponds
	AssertHTTPFails      = testutil.AssertHTTPFails
)

// getBuildDir returns the build directory from environment or default
func getBuildDir() string {
	if dir := os.Getenv(envBuildDir); dir != "" {
		return dir
	}
	return defaultBuildDir
}

// getGameDir returns the game directory from environment or default
func getGameDir() string {
	if dir := os.Getenv(envGameDir); dir != "" {
		return dir
	}
	return defaultGameDir
}

// getDLLPath resolves the full path to a DLL file in the build directory
// dllName: the name of the DLL file (e.g., "gamepatches.dll")
// Returns: absolute path to the DLL file
func getDLLPath(dllName string) string {
	buildDir := getBuildDir()
	absPath, err := filepath.Abs(filepath.Join(buildDir, dllName))
	if err != nil {
		return filepath.Join(buildDir, dllName)
	}
	return absPath
}

// deployDLL copies a DLL file to the game's bin/win10 directory
// dllPath: full path to the source DLL file
// gameDir: path to the game root directory
// Returns: error if copy fails
func deployDLL(dllPath, gameDir string) error {
	// Resolve game directory to absolute path
	absGameDir, err := filepath.Abs(gameDir)
	if err != nil {
		return fmt.Errorf("failed to resolve game directory: %w", err)
	}

	// Check if source DLL exists
	if _, err := os.Stat(dllPath); err != nil {
		return fmt.Errorf("source DLL not found: %w", err)
	}

	// Construct destination path
	destDir := filepath.Join(absGameDir, "bin", "win10")
	if err := os.MkdirAll(destDir, 0755); err != nil {
		return fmt.Errorf("failed to create destination directory: %w", err)
	}

	destPath := filepath.Join(destDir, filepath.Base(dllPath))

	// Open source file
	src, err := os.Open(dllPath)
	if err != nil {
		return fmt.Errorf("failed to open source DLL: %w", err)
	}
	defer src.Close()

	// Create destination file
	dst, err := os.Create(destPath)
	if err != nil {
		return fmt.Errorf("failed to create destination file: %w", err)
	}
	defer dst.Close()

	// Copy contents
	if _, err := io.Copy(dst, src); err != nil {
		return fmt.Errorf("failed to copy DLL: %w", err)
	}

	// Ensure write is flushed
	if err := dst.Sync(); err != nil {
		return fmt.Errorf("failed to sync destination file: %w", err)
	}

	return nil
}

// cleanupDLLs removes all NEVR DLL files from the game's bin/win10 directory
// gameDir: path to the game root directory
// Returns: error if cleanup fails
func cleanupDLLs(gameDir string) error {
	// Resolve game directory to absolute path
	absGameDir, err := filepath.Abs(gameDir)
	if err != nil {
		return fmt.Errorf("failed to resolve game directory: %w", err)
	}

	dllDir := filepath.Join(absGameDir, "bin", "win10")

	// List of NEVR DLL files to remove
	dllNames := []string{
		"gamepatches.dll",
		"gameserver.dll",
		"telemetryagent.dll",
	}

	var errors []error
	for _, dllName := range dllNames {
		dllPath := filepath.Join(dllDir, dllName)
		if err := os.Remove(dllPath); err != nil {
			if !os.IsNotExist(err) {
				errors = append(errors, fmt.Errorf("failed to remove %s: %w", dllName, err))
			}
			// Silently ignore "file not found" errors
		}
	}

	if len(errors) > 0 {
		return fmt.Errorf("cleanup failed with %d errors: %v", len(errors), errors)
	}

	return nil
}

// deployAllDLLs is a convenience helper that deploys all NEVR DLL files
func deployAllDLLs(t *testing.T) {
	gameDir := getGameDir()
	dllNames := []string{
		"gamepatches.dll",
		"gameserver.dll",
		"telemetryagent.dll",
	}

	for _, dllName := range dllNames {
		dllPath := getDLLPath(dllName)
		err := deployDLL(dllPath, gameDir)
		require.NoError(t, err, "Failed to deploy %s", dllName)
	}
}

// cleanupAllDLLs is a convenience helper that cleans up all NEVR DLL files
func cleanupAllDLLs(t *testing.T) {
	gameDir := getGameDir()
	err := cleanupDLLs(gameDir)
	require.NoError(t, err, "Failed to cleanup DLLs")
}

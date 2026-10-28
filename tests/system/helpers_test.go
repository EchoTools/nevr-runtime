package system

import (
	"fmt"
	"io"
	"os"
	"os/user"
	"path/filepath"
	"testing"

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

// getDLLPath resolves the full path to a DLL file in the build directory.
// The current build layout puts DLLs under bin/ (e.g. bin/BugSplat64.dll).
// dllName: the name of the DLL file (e.g., "BugSplat64.dll")
func getDLLPath(dllName string) string {
	buildDir := getBuildDir()
	absPath, err := filepath.Abs(filepath.Join(buildDir, "bin", dllName))
	if err != nil {
		return filepath.Join(buildDir, "bin", dllName)
	}
	return absPath
}

// runtimeDLLName is the runtime the host (BugSplat64.dll) loads. The host looks for it in the nevr root of the
// Windows user, never beside itself (ADR 0012), so a deployment puts it there.
const runtimeDLLName = "nevr-runtime.dll"

// nevrRootDir is the nevr root (%LOCALAPPDATA%\nevr\echovr) of the Wine prefix a test run uses: the prefix under the
// game root, with the Windows user named as the host user (what `id -un` gives, as launch-client.sh uses).
// gameDir: path to the game root directory
func nevrRootDir(gameDir string) string {
	name := os.Getenv("USER")
	if current, err := user.Current(); err == nil {
		name = current.Username
	}
	return filepath.Join(gameDir, ".wineprefix", "drive_c", "users", name, "AppData", "Local", "nevr", "echovr")
}

// deployedPath is where a deployed DLL lands: the runtime in the nevr root, every other DLL in bin/win10.
// gameDir: path to the game root directory
func deployedPath(gameDir, dllName string) string {
	if dllName == runtimeDLLName {
		return filepath.Join(nevrRootDir(gameDir), dllName)
	}
	return filepath.Join(gameDir, "bin", "win10", dllName)
}

// deployDLL copies a DLL file to where the game loads it from: the host to the game's bin/win10 directory, the
// runtime to the nevr root of the Wine prefix
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
	destPath := deployedPath(absGameDir, filepath.Base(dllPath))
	if err := os.MkdirAll(filepath.Dir(destPath), 0755); err != nil {
		return fmt.Errorf("failed to create destination directory: %w", err)
	}

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

// cleanupDLLs removes the deployed NEVR DLL files: the host from bin/win10, the runtime from the nevr root
// gameDir: path to the game root directory
// Returns: error if cleanup fails
func cleanupDLLs(gameDir string) error {
	// Resolve game directory to absolute path
	absGameDir, err := filepath.Abs(gameDir)
	if err != nil {
		return fmt.Errorf("failed to resolve game directory: %w", err)
	}

	// The pair: the host (BugSplat64.dll) and the runtime it loads (nevr-runtime.dll)
	dllNames := []string{
		runtimeDLLName,
		"BugSplat64.dll",
	}

	var errors []error
	for _, dllName := range dllNames {
		dllPath := deployedPath(absGameDir, dllName)
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
		"nevr-runtime.dll",
		"BugSplat64.dll",
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

// getPluginPath resolves the full path to a plugin DLL in the build directory's plugins/ subdirectory
// dllName: the name of the plugin DLL file (e.g., "server_timing.dll")
// Returns: absolute path to the plugin DLL file
func getPluginPath(dllName string) string {
	buildDir := getBuildDir()
	absPath, err := filepath.Abs(filepath.Join(buildDir, "plugins", dllName))
	if err != nil {
		return filepath.Join(buildDir, "plugins", dllName)
	}
	return absPath
}

// deployPlugin copies a plugin DLL to the plugins\ folder of the nevr root (ADR 0012), where the runtime loads plugins from
// dllPath: full path to the source plugin DLL file
// gameDir: path to the game root directory
// Returns: error if copy fails
func deployPlugin(dllPath, gameDir string) error {
	absGameDir, err := filepath.Abs(gameDir)
	if err != nil {
		return fmt.Errorf("failed to resolve game directory: %w", err)
	}

	if _, err := os.Stat(dllPath); err != nil {
		return fmt.Errorf("source plugin DLL not found: %w", err)
	}

	destDir := filepath.Join(nevrRootDir(absGameDir), "plugins")
	if err := os.MkdirAll(destDir, 0755); err != nil {
		return fmt.Errorf("failed to create plugins directory: %w", err)
	}

	destPath := filepath.Join(destDir, filepath.Base(dllPath))

	src, err := os.Open(dllPath)
	if err != nil {
		return fmt.Errorf("failed to open source plugin DLL: %w", err)
	}
	defer src.Close()

	dst, err := os.Create(destPath)
	if err != nil {
		return fmt.Errorf("failed to create destination file: %w", err)
	}
	defer dst.Close()

	if _, err := io.Copy(dst, src); err != nil {
		return fmt.Errorf("failed to copy plugin DLL: %w", err)
	}

	if err := dst.Sync(); err != nil {
		return fmt.Errorf("failed to sync destination file: %w", err)
	}

	return nil
}

// cleanupPlugins removes all .dll files from the plugins\ folder of the nevr root
// gameDir: path to the game root directory
// Returns: error if cleanup fails
func cleanupPlugins(gameDir string) error {
	absGameDir, err := filepath.Abs(gameDir)
	if err != nil {
		return fmt.Errorf("failed to resolve game directory: %w", err)
	}

	pluginsDir := filepath.Join(nevrRootDir(absGameDir), "plugins")

	entries, err := os.ReadDir(pluginsDir)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return fmt.Errorf("failed to read plugins directory: %w", err)
	}

	var errors []error
	for _, entry := range entries {
		if entry.IsDir() {
			continue
		}
		if filepath.Ext(entry.Name()) != ".dll" {
			continue
		}
		dllPath := filepath.Join(pluginsDir, entry.Name())
		if err := os.Remove(dllPath); err != nil {
			errors = append(errors, fmt.Errorf("failed to remove %s: %w", entry.Name(), err))
		}
	}

	if len(errors) > 0 {
		return fmt.Errorf("plugin cleanup failed with %d errors: %v", len(errors), errors)
	}

	return nil
}

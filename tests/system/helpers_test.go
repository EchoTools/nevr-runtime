package system

import (
	"fmt"
	"io"
	"os"
	"os/user"
	"path/filepath"
	"strings"
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

// copyCredentialPatternFile is the one definition of the sign-in file name pattern, read by the shell guard
// (tools/lib/game_install.sh), this suite and tools/winvm/systest.py. The test's working directory is tests/system.
const copyCredentialPatternFile = "../../tools/lib/copy_credential_pattern"

// copyCredentialPattern returns the pattern from copyCredentialPatternFile: the sign-in files the game would sign in with.
func copyCredentialPattern() (string, error) {
	raw, err := os.ReadFile(copyCredentialPatternFile)
	if err != nil {
		return "", fmt.Errorf("cannot read the sign-in pattern: %w", err)
	}
	pattern := strings.TrimSpace(string(raw))
	if pattern == "" {
		return "", fmt.Errorf("%s holds no pattern", copyCredentialPatternFile)
	}
	return pattern, nil
}

// copyCredentialFiles lists the sign-in files under echovrDir (a copy's echovr/ directory). A symbolic link counts and
// a directory does not; the walk does not follow links, as find does not by default. An error means the tree could not
// be searched, which is not the same as no file.
func copyCredentialFiles(echovrDir string) ([]string, error) {
	pattern, err := copyCredentialPattern()
	if err != nil {
		return nil, err
	}
	var found []string
	err = filepath.WalkDir(echovrDir, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		match, err := filepath.Match(pattern, d.Name())
		if err != nil {
			return err
		}
		if match {
			found = append(found, path)
		}
		return nil
	})
	return found, err
}

// checkCopyCredentials refuses a game copy that holds a sign-in file: the game would sign in as whoever the file
// belongs to. It applies only when EVR_GAME_DIR names a copy. Unset, the default is the main checkout's own install,
// which holds the owner's sign-in, and nothing is refused there (the rule refuse_copy_credentials applies when
// NEVR_GAME_ROOT is unset; AGENTS.md "The game install").
func checkCopyCredentials() error {
	if os.Getenv(envGameDir) == "" {
		return nil
	}
	gameDir := getGameDir()
	found, err := copyCredentialFiles(gameDir)
	if err != nil {
		return fmt.Errorf("cannot search %s for sign-in files: %w", gameDir, err)
	}
	if len(found) == 0 {
		return nil
	}
	return fmt.Errorf("the game copy holds sign-in file(s) the game would sign in with: %s (remove them: AGENTS.md \"The game install\", \"Make it safe\")",
		strings.Join(found, " "))
}

// refuseCopyCredentials fails the calling test before it deploys into or starts the game, when checkCopyCredentials
// refuses. Every test that starts echovr.exe calls it first.
func refuseCopyCredentials(t *testing.T) {
	t.Helper()
	if err := checkCopyCredentials(); err != nil {
		t.Fatalf("not starting the game: %v", err)
	}
}

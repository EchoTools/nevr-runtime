package plugins

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"testing"

	"github.com/EchoTools/evr-test-harness/pkg/testutil"
	"github.com/stretchr/testify/require"
)

const (
	// NEVR_PLUGINS_BUILD_DIR must point to a directory containing plugin DLLs.
	// Default assumes `just dist` was run in nevr-runtime-plugins.
	// If using CMake build output directly, set this to the build/mingw-release/plugins/ tree.
	envPluginsBuildDir     = "NEVR_PLUGINS_BUILD_DIR"
	defaultPluginsBuildDir = "../../../nevr-runtime-plugins/dist/nevr-plugins-v1.0.0/plugins"

	envBuildDir     = "NEVR_BUILD_DIR"
	defaultBuildDir = "../../dist"

	envGameDir     = "EVR_GAME_DIR"
	defaultGameDir = "../../ready-at-dawn-echo-arena"

	envReconstructionDir     = "ECHOVR_RECONSTRUCTION_DIR"
	defaultReconstructionDir = "../../../echovr-reconstruction"

	envEvrReconstructionDir     = "EVR_RECONSTRUCTION_DIR"
	defaultEvrReconstructionDir = "../../../evr-reconstruction"

	envPluginsSourceDir     = "NEVR_PLUGINS_SOURCE_DIR"
	defaultPluginsSourceDir = "../../../nevr-runtime-plugins"
)

var (
	AssertProcessRunning = testutil.AssertProcessRunning
	AssertProcessStopped = testutil.AssertProcessStopped
	AssertHTTPResponds   = testutil.AssertHTTPResponds
	AssertHTTPFails      = testutil.AssertHTTPFails
)

func getPluginsBuildDir() string {
	if dir := os.Getenv(envPluginsBuildDir); dir != "" {
		return dir
	}
	return defaultPluginsBuildDir
}

func getBuildDir() string {
	if dir := os.Getenv(envBuildDir); dir != "" {
		return dir
	}
	return defaultBuildDir
}

func getGameDir() string {
	if dir := os.Getenv(envGameDir); dir != "" {
		return dir
	}
	return defaultGameDir
}

func getReconstructionDir() string {
	if dir := os.Getenv(envReconstructionDir); dir != "" {
		return dir
	}
	return defaultReconstructionDir
}

func getEvrReconstructionDir() string {
	if dir := os.Getenv(envEvrReconstructionDir); dir != "" {
		return dir
	}
	return defaultEvrReconstructionDir
}

func getPluginsSourceDir() string {
	if dir := os.Getenv(envPluginsSourceDir); dir != "" {
		return dir
	}
	return defaultPluginsSourceDir
}

func getPluginDLLPath(pluginName string) string {
	buildDir := getPluginsBuildDir()
	dllName := pluginName + ".dll"
	absPath, err := filepath.Abs(filepath.Join(buildDir, dllName))
	if err != nil {
		return filepath.Join(buildDir, dllName)
	}
	return absPath
}

func getDLLPath(dllName string) string {
	buildDir := getBuildDir()
	absPath, err := filepath.Abs(filepath.Join(buildDir, dllName))
	if err != nil {
		return filepath.Join(buildDir, dllName)
	}
	return absPath
}

func copyFile(src, dst string) error {
	srcFile, err := os.Open(src)
	if err != nil {
		return fmt.Errorf("open source %s: %w", src, err)
	}
	defer srcFile.Close()

	if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
		return fmt.Errorf("create dest dir for %s: %w", dst, err)
	}

	dstFile, err := os.Create(dst)
	if err != nil {
		return fmt.Errorf("create dest %s: %w", dst, err)
	}
	defer dstFile.Close()

	if _, err := io.Copy(dstFile, srcFile); err != nil {
		return fmt.Errorf("copy %s -> %s: %w", src, dst, err)
	}

	return dstFile.Sync()
}

func deployCoreDLL(t *testing.T, dllName string) {
	t.Helper()
	src := getDLLPath(dllName)
	gameDir, err := filepath.Abs(getGameDir())
	require.NoError(t, err)
	dst := filepath.Join(gameDir, "bin", "win10", dllName)
	require.NoError(t, copyFile(src, dst), "Failed to deploy core DLL %s", dllName)
	t.Logf("Deployed core DLL: %s -> %s", src, dst)
}

func deployPlugin(t *testing.T, pluginName string) {
	t.Helper()
	src := getPluginDLLPath(pluginName)
	gameDir, err := filepath.Abs(getGameDir())
	require.NoError(t, err)
	pluginDir := filepath.Join(gameDir, "bin", "win10", "plugins")
	dst := filepath.Join(pluginDir, pluginName+".dll")
	require.NoError(t, copyFile(src, dst), "Failed to deploy plugin %s", pluginName)
	t.Logf("Deployed plugin: %s -> %s", src, dst)

	// Also deploy config file if it exists alongside the DLL in the build dir.
	// Try both naming conventions: "plugin_config.ext" and "plugin.ext"
	// (most plugins use _config suffix, game_rules_override uses bare name)
	buildDir := getPluginsBuildDir()
	for _, ext := range []string{".json", ".jsonc", ".yml", ".yaml"} {
		for _, suffix := range []string{"_config", ""} {
			configName := pluginName + suffix + ext
			configSrc := filepath.Join(buildDir, configName)
			if _, err := os.Stat(configSrc); err == nil {
				configDst := filepath.Join(pluginDir, configName)
				if err := copyFile(configSrc, configDst); err != nil {
					t.Logf("Warning: failed to deploy config %s: %v", configName, err)
				} else {
					t.Logf("Deployed config: %s -> %s", configSrc, configDst)
				}
			}
		}
	}
}

func deployPlugins(t *testing.T, pluginNames ...string) {
	t.Helper()
	for _, name := range pluginNames {
		deployPlugin(t, name)
	}
}

func deployAllCoreDLLs(t *testing.T) {
	t.Helper()
	for _, dll := range []string{"gamepatches.dll", "gameserver.dll", "telemetryagent.dll"} {
		deployCoreDLL(t, dll)
	}
}

func cleanupPlugins(t *testing.T) {
	t.Helper()
	gameDir, err := filepath.Abs(getGameDir())
	require.NoError(t, err)
	pluginDir := filepath.Join(gameDir, "bin", "win10", "plugins")

	entries, err := os.ReadDir(pluginDir)
	if os.IsNotExist(err) {
		return
	}
	require.NoError(t, err)

	for _, entry := range entries {
		if filepath.Ext(entry.Name()) == ".dll" {
			path := filepath.Join(pluginDir, entry.Name())
			if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
				t.Logf("Warning: failed to remove %s: %v", path, err)
			}
		}
	}
	t.Log("Cleaned up plugin DLLs")
}

func cleanupCoreDLLs(t *testing.T) {
	t.Helper()
	gameDir, err := filepath.Abs(getGameDir())
	require.NoError(t, err)
	dllDir := filepath.Join(gameDir, "bin", "win10")

	for _, dll := range []string{"gamepatches.dll", "gameserver.dll", "telemetryagent.dll"} {
		path := filepath.Join(dllDir, dll)
		if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
			t.Logf("Warning: failed to remove %s: %v", path, err)
		}
	}
	t.Log("Cleaned up core DLLs")
}

func cleanupAll(t *testing.T) {
	t.Helper()
	cleanupPlugins(t)
	cleanupCoreDLLs(t)
}

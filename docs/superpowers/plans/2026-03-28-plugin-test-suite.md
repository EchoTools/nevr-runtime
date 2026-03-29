# Plugin Test Suite Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a comprehensive test suite for all nevr-runtime-plugins in `~/src/nevr-runtime/tests/plugins/` with ground truth verification against reconstruction source and integration tests via MCP harness.

**Architecture:** Two-layer Go test suite: (1) ground truth cross-reference tests that parse C++ source and cross_binary_map.json to verify every binary assumption, (2) integration tests that deploy plugins to a running game and verify behavior via HTTP API. Both layers live in a new `tests/plugins/` Go module parallel to the existing `tests/system/`.

**Tech Stack:** Go 1.25.6, testify, evr-test-harness MCP client, regexp for C++ parsing, encoding/json for cross-binary map

---

## File Structure

```
~/src/nevr-runtime/tests/
├── system/                        # Existing E2E tests (DO NOT MODIFY)
└── plugins/                       # NEW: Plugin test suite
    ├── go.mod
    ├── go.sum
    ├── plugin_helpers_test.go     # Plugin deploy/cleanup helpers
    ├── groundtruth_test.go        # Binary/reconstruction cross-reference tests
    ├── superhyperturbo_test.go    # super-hyper-turbo integration tests
    └── testdata/                  # (empty initially, for future config files)
```

---

### Task 1: Create `tests/plugins/` Go Module

**Files:**

- Create: `tests/plugins/go.mod`
- Create: `tests/plugins/go.sum` (generated)
- Create: `tests/plugins/testdata/` (empty directory)

- [ ] **Step 1.1: Create directory structure**

```bash
cd ~/src/nevr-runtime
mkdir -p tests/plugins/testdata
```

- [ ] **Step 1.2: Create `~/src/nevr-runtime/tests/plugins/go.mod`**

```go
module github.com/EchoTools/nevr-runtime/tests/plugins

go 1.25.6

replace github.com/EchoTools/evr-test-harness => ../../extern/evr-test-harness

require (
	github.com/EchoTools/evr-test-harness v0.0.0-00010101000000-000000000000
	github.com/stretchr/testify v1.11.1
)

require (
	github.com/davecgh/go-spew v1.1.1 // indirect
	github.com/pmezard/go-difflib v1.0.0 // indirect
	go.uber.org/multierr v1.10.0 // indirect
	go.uber.org/zap v1.27.1 // indirect
	gopkg.in/yaml.v3 v3.0.1 // indirect
)
```

- [ ] **Step 1.3: Generate `go.sum`**

```bash
cd ~/src/nevr-runtime/tests/plugins && go mod tidy
```

Expected: `go.sum` created, no errors.

---

### Task 2: Write Plugin Deployment Helpers

**Files:**

- Create: `tests/plugins/plugin_helpers_test.go`

- [ ] **Step 2.1: Create `~/src/nevr-runtime/tests/plugins/plugin_helpers_test.go`**

```go
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
```

- [ ] **Step 2.2: Verify compilation**

```bash
cd ~/src/nevr-runtime/tests/plugins && go vet ./...
```

Expected: no output (success).

- [ ] **Step 2.3: Commit**

```bash
cd ~/src/nevr-runtime
git add tests/plugins/
git commit -m "tests/plugins: add Go module and plugin deploy/cleanup helpers"
```

---

### Task 3: Write Ground Truth Cross-Reference Tests

**Files:**

- Create: `tests/plugins/groundtruth_test.go`

- [ ] **Step 3.1: Create `~/src/nevr-runtime/tests/plugins/groundtruth_test.go`**

```go
package plugins

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// ============================================================================
// Cross-binary map types
// ============================================================================

type CrossBinaryMap struct {
	Metadata struct {
		TotalMatches int `json:"total_matches"`
	} `json:"metadata"`
	Matches map[string]CrossBinaryMatch `json:"matches"`
}

type CrossBinaryMatch struct {
	QuestSymbol string  `json:"quest_symbol"`
	QuestVA     string  `json:"quest_va"`
	Confidence  float64 `json:"confidence"`
	Method      string  `json:"method"`
	PCName      string  `json:"pc_name,omitempty"`
}

func loadCrossBinaryMap(t *testing.T) CrossBinaryMap {
	t.Helper()
	path := filepath.Join(getEvrReconstructionDir(), "data", "cross_binary_map.json")
	absPath, err := filepath.Abs(path)
	require.NoError(t, err)

	data, err := os.ReadFile(absPath)
	if os.IsNotExist(err) {
		t.Skipf("cross_binary_map.json not found at %s (evr-reconstruction repo required)", absPath)
	}
	require.NoError(t, err, "Failed to read cross_binary_map.json at %s", absPath)

	var cbm CrossBinaryMap
	require.NoError(t, json.Unmarshal(data, &cbm), "Failed to parse cross_binary_map.json")
	require.NotEmpty(t, cbm.Matches, "cross_binary_map.json has no matches")
	return cbm
}

func readSourceFile(t *testing.T, path string) string {
	t.Helper()
	absPath, err := filepath.Abs(path)
	require.NoError(t, err)

	data, err := os.ReadFile(absPath)
	if os.IsNotExist(err) {
		t.Skipf("Source file not found: %s", absPath)
	}
	require.NoError(t, err, "Failed to read source file: %s", absPath)
	return string(data)
}

// parseHashesFromFunction extracts uint64 hex literals from a named function body.
func parseHashesFromFunction(t *testing.T, source, funcName string) []uint64 {
	t.Helper()

	// Match function definitions (not comments). Require a return type keyword
	// before the function name to avoid matching comment references.
	escapedName := regexp.QuoteMeta(funcName)
	funcRe := regexp.MustCompile(`(?m)^[^\n/]*\b(?:bool|void|int|static)\b[^\n]*` + escapedName + `\s*\(`)
	loc := funcRe.FindStringIndex(source)
	require.NotNil(t, loc, "Function %q not found in source", funcName)

	rest := source[loc[0]:]
	braceIdx := strings.Index(rest, "{")
	require.True(t, braceIdx >= 0, "No opening brace found for %s", funcName)

	depth := 0
	bodyStart := braceIdx
	bodyEnd := -1
	for i := braceIdx; i < len(rest); i++ {
		if rest[i] == '{' {
			depth++
		} else if rest[i] == '}' {
			depth--
			if depth == 0 {
				bodyEnd = i + 1
				break
			}
		}
	}
	require.True(t, bodyEnd > bodyStart, "Could not find closing brace for %s", funcName)

	body := rest[bodyStart:bodyEnd]

	hexRe := regexp.MustCompile(`0x([0-9a-fA-F]{8,16})`)
	hexMatches := hexRe.FindAllStringSubmatch(body, -1)

	var hashes []uint64
	for _, m := range hexMatches {
		val, err := strconv.ParseUint(m[1], 16, 64)
		if err == nil {
			hashes = append(hashes, val)
		}
	}
	return hashes
}

func parseVAConstants(t *testing.T, source string) map[string]uint64 {
	t.Helper()
	re := regexp.MustCompile(`static\s+constexpr\s+uint64_t\s+(VA_\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*;`)
	matches := re.FindAllStringSubmatch(source, -1)
	result := make(map[string]uint64)
	for _, m := range matches {
		name := m[1]
		val, err := strconv.ParseUint(strings.TrimPrefix(m[2], "0x"), 16, 64)
		require.NoError(t, err, "Failed to parse VA constant %s = %s", name, m[2])
		result[name] = val
	}
	return result
}

func uint64ToHexKey(val uint64) string {
	return fmt.Sprintf("0x%x", val)
}

const imageBase uint64 = 0x140000000

// ============================================================================
// Ground Truth Tests
// ============================================================================

func TestGroundTruth_ArenaHashCompleteness(t *testing.T) {
	hashesPath := filepath.Join(getReconstructionDir(),
		"src", "NRadEngine", "Game", "CGameModeHashes.h")
	hashesSource := readSourceFile(t, hashesPath)
	groundTruthHashes := parseHashesFromFunction(t, hashesSource, "IsArenaMode")
	require.Len(t, groundTruthHashes, 10,
		"Expected 10 arena mode hashes in CGameModeHashes.h IsArenaMode")

	pluginPath := filepath.Join(getPluginsSourceDir(),
		"plugins", "super-hyper-turbo", "src", "superhyperturbo.cpp")
	pluginSource := readSourceFile(t, pluginPath)
	pluginHashes := parseHashesFromFunction(t, pluginSource, "IsArenaModeHash")
	require.Len(t, pluginHashes, 10,
		"Expected 10 arena mode hashes in superhyperturbo.cpp IsArenaModeHash")

	groundTruthSet := make(map[uint64]bool)
	for _, h := range groundTruthHashes {
		groundTruthSet[h] = true
	}
	pluginSet := make(map[uint64]bool)
	for _, h := range pluginHashes {
		pluginSet[h] = true
	}

	for h := range groundTruthSet {
		assert.True(t, pluginSet[h],
			"Arena hash 0x%016x from CGameModeHashes.h missing in plugin", h)
	}
	for h := range pluginSet {
		assert.True(t, groundTruthSet[h],
			"Arena hash 0x%016x in plugin not found in CGameModeHashes.h", h)
	}
}

func TestGroundTruth_CombatHashValidity(t *testing.T) {
	hashesPath := filepath.Join(getReconstructionDir(),
		"src", "NRadEngine", "Game", "CGameModeHashes.h")
	hashesSource := readSourceFile(t, hashesPath)

	combatHashes := parseHashesFromFunction(t, hashesSource, "IsCombatMode")
	require.Len(t, combatHashes, 7)

	aiHashes := parseHashesFromFunction(t, hashesSource, "IsAIMode")
	require.Len(t, aiHashes, 8)

	combatPracticeHashes := parseHashesFromFunction(t, hashesSource, "IsCombatOrPracticeMode")
	require.Len(t, combatPracticeHashes, 6)

	const hashCombatPracticeAI uint64 = 0x47c693e5ebaddea6

	combatSet := make(map[uint64]bool)
	for _, h := range combatHashes {
		combatSet[h] = true
	}
	assert.True(t, combatSet[hashCombatPracticeAI],
		"HASH_COMBAT_PRACTICE_AI must be in IsCombatMode")

	aiSet := make(map[uint64]bool)
	for _, h := range aiHashes {
		aiSet[h] = true
	}
	assert.True(t, aiSet[hashCombatPracticeAI],
		"HASH_COMBAT_PRACTICE_AI must be in IsAIMode")

	cpSet := make(map[uint64]bool)
	for _, h := range combatPracticeHashes {
		cpSet[h] = true
	}
	assert.True(t, cpSet[hashCombatPracticeAI],
		"HASH_COMBAT_PRACTICE_AI must be in IsCombatOrPracticeMode")

	pluginPath := filepath.Join(getPluginsSourceDir(),
		"plugins", "super-hyper-turbo", "src", "superhyperturbo.cpp")
	pluginSource := readSourceFile(t, pluginPath)

	re := regexp.MustCompile(`HASH_COMBAT_PRACTICE_AI\s*=\s*(0x[0-9a-fA-F]+)`)
	m := re.FindStringSubmatch(pluginSource)
	require.NotNil(t, m, "HASH_COMBAT_PRACTICE_AI not found in superhyperturbo.cpp")
	pluginVal, err := strconv.ParseUint(strings.TrimPrefix(m[1], "0x"), 16, 64)
	require.NoError(t, err)
	assert.Equal(t, hashCombatPracticeAI, pluginVal)
}

func TestGroundTruth_VACreateSessionIdentity(t *testing.T) {
	cbm := loadCrossBinaryMap(t)

	registryPath := filepath.Join(getPluginsSourceDir(),
		"common", "include", "address_registry.h")
	registrySource := readSourceFile(t, registryPath)
	vaConstants := parseVAConstants(t, registrySource)

	vaCreateSession, ok := vaConstants["VA_CREATE_SESSION"]
	require.True(t, ok, "VA_CREATE_SESSION not found in address_registry.h")
	assert.Equal(t, uint64(0x14015e920), vaCreateSession)

	key := uint64ToHexKey(vaCreateSession)
	match, found := cbm.Matches[key]
	require.True(t, found, "VA_CREATE_SESSION (%s) not found in cross_binary_map.json", key)

	assert.Contains(t, match.QuestSymbol, "Create",
		"VA_CREATE_SESSION symbol should contain 'Create', got %q", match.QuestSymbol)

	t.Logf("VA_CREATE_SESSION (%s) -> %s (confidence: %.2f, method: %s)",
		key, match.QuestSymbol, match.Confidence, match.Method)
}

func TestGroundTruth_CreateSessionSignature(t *testing.T) {
	netGamePath := filepath.Join(getReconstructionDir(),
		"src", "NRadEngine", "Game", "CR15NetGame.cpp")
	netGameSource := readSourceFile(t, netGamePath)

	sigRe := regexp.MustCompile(`void\s+CR15NetGame_CreateSession\s*\(([^)]*)\)`)
	m := sigRe.FindStringSubmatch(netGameSource)
	require.NotNil(t, m, "CR15NetGame_CreateSession signature not found")

	params := strings.Split(m[1], ",")
	var reconParams []string
	for _, p := range params {
		p = strings.TrimSpace(p)
		if p != "" {
			reconParams = append(reconParams, p)
		}
	}

	pluginPath := filepath.Join(getPluginsSourceDir(),
		"plugins", "super-hyper-turbo", "src", "superhyperturbo.cpp")
	pluginSource := readSourceFile(t, pluginPath)

	typedefRe := regexp.MustCompile(`typedef\s+void\s*\(\s*__fastcall\s*\*\s*CreateSession_t\s*\)\s*\(([^)]*)\)`)
	pm := typedefRe.FindStringSubmatch(pluginSource)
	require.NotNil(t, pm, "CreateSession_t typedef not found")

	var pluginParams []string
	for _, p := range strings.Split(pm[1], ",") {
		p = strings.TrimSpace(p)
		if p != "" {
			pluginParams = append(pluginParams, p)
		}
	}

	assert.Equal(t, len(reconParams), len(pluginParams),
		"CreateSession param count: reconstruction=%d (%v), plugin=%d (%v)",
		len(reconParams), reconParams, len(pluginParams), pluginParams)
}

func TestGroundTruth_GametypeHashOffset(t *testing.T) {
	netGamePath := filepath.Join(getReconstructionDir(),
		"src", "NRadEngine", "Game", "CR15NetGame.cpp")
	netGameSource := readSourceFile(t, netGamePath)

	pluginPath := filepath.Join(getPluginsSourceDir(),
		"plugins", "super-hyper-turbo", "src", "superhyperturbo.cpp")
	pluginSource := readSourceFile(t, pluginPath)

	createSessionIdx := strings.Index(netGameSource, "CR15NetGame_CreateSession")
	require.True(t, createSessionIdx >= 0)

	searchRegion := netGameSource[createSessionIdx:]
	if len(searchRegion) > 2000 {
		searchRegion = searchRegion[:2000]
	}

	offsetRe := regexp.MustCompile(`(?i)self\s*\+\s*0x6[aA]0`)
	assert.True(t, offsetRe.MatchString(searchRegion),
		"Offset 0x6A0 not found near CR15NetGame_CreateSession in reconstruction")

	pluginOffsetRe := regexp.MustCompile(`(?i)0x6[aA]0`)
	assert.True(t, pluginOffsetRe.MatchString(pluginSource),
		"Offset 0x6A0 not found in superhyperturbo.cpp")

	sessionHeaderPath := filepath.Join(getPluginsSourceDir(),
		"plugins", "session-api", "src", "session_api.h")
	sessionHeader := readSourceFile(t, sessionHeaderPath)

	gameModeHashRe := regexp.MustCompile(`GAME_MODE_HASH\s*=\s*(0x[0-9a-fA-F]+)`)
	m := gameModeHashRe.FindStringSubmatch(sessionHeader)
	require.NotNil(t, m, "GAME_MODE_HASH not found in session_api.h")
	val, err := strconv.ParseUint(strings.TrimPrefix(m[1], "0x"), 16, 64)
	require.NoError(t, err)
	assert.Equal(t, uint64(0x06A0), val,
		"session_api.h GAME_MODE_HASH offset should be 0x06A0")
}

func TestGroundTruth_AllVAsAboveImageBase(t *testing.T) {
	registryPath := filepath.Join(getPluginsSourceDir(),
		"common", "include", "address_registry.h")
	registrySource := readSourceFile(t, registryPath)
	vaConstants := parseVAConstants(t, registrySource)
	require.NotEmpty(t, vaConstants)

	for name, addr := range vaConstants {
		assert.True(t, addr > imageBase,
			"VA %s (0x%x) not above IMAGE_BASE (0x%x)", name, addr, imageBase)
	}
	t.Logf("Checked %d VA constants, all above IMAGE_BASE", len(vaConstants))
}

func TestGroundTruth_NoDuplicateVAs(t *testing.T) {
	registryPath := filepath.Join(getPluginsSourceDir(),
		"common", "include", "address_registry.h")
	registrySource := readSourceFile(t, registryPath)
	vaConstants := parseVAConstants(t, registrySource)
	require.NotEmpty(t, vaConstants)

	addrToNames := make(map[uint64][]string)
	for name, addr := range vaConstants {
		addrToNames[addr] = append(addrToNames[addr], name)
	}

	for addr, names := range addrToNames {
		if len(names) > 1 {
			// Known alias: VA_CONFIG_TABLE and VA_BALANCE_SETTINGS_TABLE share 0x1420d3a68
			if addr == 0x1420d3a68 {
				t.Logf("Known alias at 0x%x: %v", addr, names)
				continue
			}
			t.Errorf("Duplicate VA at 0x%x: %v", addr, names)
		}
	}
}

func TestGroundTruth_VAsInCrossBinaryMap(t *testing.T) {
	cbm := loadCrossBinaryMap(t)

	registryPath := filepath.Join(getPluginsSourceDir(),
		"common", "include", "address_registry.h")
	registrySource := readSourceFile(t, registryPath)
	vaConstants := parseVAConstants(t, registrySource)
	require.NotEmpty(t, vaConstants)

	found := 0
	notFound := 0
	for name, addr := range vaConstants {
		key := uint64ToHexKey(addr)
		if match, ok := cbm.Matches[key]; ok {
			found++
			t.Logf("FOUND: %s (%s) -> %s (confidence: %.2f)",
				name, key, match.QuestSymbol, match.Confidence)
		} else {
			notFound++
			t.Logf("NOT IN MAP: %s (%s)", name, key)
		}
	}

	t.Logf("VA cross-reference: %d/%d found in cross_binary_map.json", found, found+notFound)

	vaCreateSession := vaConstants["VA_CREATE_SESSION"]
	key := uint64ToHexKey(vaCreateSession)
	_, ok := cbm.Matches[key]
	assert.True(t, ok, "VA_CREATE_SESSION (%s) MUST be in cross_binary_map.json", key)
}

func TestGroundTruth_SessionApiModeHashInBuildSessionJson(t *testing.T) {
	sessionCppPath := filepath.Join(getPluginsSourceDir(),
		"plugins", "session-api", "src", "session_api.cpp")
	sessionSource := readSourceFile(t, sessionCppPath)

	funcIdx := strings.Index(sessionSource, "BuildSessionJson")
	require.True(t, funcIdx >= 0, "BuildSessionJson not found in session_api.cpp")

	rest := sessionSource[funcIdx:]
	jsonChainIdx := strings.Index(rest, "json <<")
	require.True(t, jsonChainIdx >= 0, "json << chain not found in BuildSessionJson")

	searchRegion := rest[jsonChainIdx:]
	if len(searchRegion) > 1000 {
		searchRegion = searchRegion[:1000]
	}

	assert.Contains(t, searchRegion, "game_mode_hash",
		"BuildSessionJson must include game_mode_hash in JSON output. "+
			"Fix: add game_mode_hash field to the json << chain in session_api.cpp")
}
```

- [ ] **Step 3.2: Run ground truth tests**

```bash
cd ~/src/nevr-runtime/tests/plugins && go test -v -run "TestGroundTruth" ./...
```

Expected: all pass EXCEPT `TestGroundTruth_SessionApiModeHashInBuildSessionJson` (fails until Task 4 fixes session_api.cpp).

- [ ] **Step 3.3: Commit**

```bash
cd ~/src/nevr-runtime
git add tests/plugins/groundtruth_test.go
git commit -m "tests/plugins: add ground truth cross-reference tests

Verify plugin binary assumptions against reconstruction source:
- Arena hash completeness (10 hashes, set comparison)
- Combat hash validity (in all 3 classifiers)
- VA_CREATE_SESSION identity in cross_binary_map.json
- CreateSession signature param count
- Gametype hash offset 0x6A0
- All VAs above IMAGE_BASE, no duplicates
- SessionApi game_mode_hash in response (expected fail until fix)"
```

---

### Task 4: Fix Plugin Bugs (nevr-runtime-plugins)

**Files:**

- Modify: `~/src/nevr-runtime-plugins/plugins/super-hyper-turbo/src/superhyperturbo.cpp:40-58`
- Modify: `~/src/nevr-runtime-plugins/plugins/session-api/src/session_api.cpp:109-154`

- [ ] **Step 4.1: Fix `superhyperturbo.cpp` — unchecked MH_Initialize**

In `~/src/nevr-runtime-plugins/plugins/super-hyper-turbo/src/superhyperturbo.cpp`, change line 43 from:

```cpp
    MH_Initialize();
```

To:

```cpp
    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
```

- [ ] **Step 4.2: Fix `superhyperturbo.cpp` — missing MH_RemoveHook**

In the same file, change `RemoveSuperHyperTurboHooks` (lines 54-58) from:

```cpp
void RemoveSuperHyperTurboHooks() {
    if (s_target_CreateSession) {
        MH_DisableHook(s_target_CreateSession);
    }
}
```

To:

```cpp
void RemoveSuperHyperTurboHooks() {
    if (s_target_CreateSession) {
        MH_DisableHook(s_target_CreateSession);
        MH_RemoveHook(s_target_CreateSession);
    }
}
```

- [ ] **Step 4.3: Fix `session_api.cpp` — add game_mode_hash to BuildSessionJson**

In `~/src/nevr-runtime-plugins/plugins/session-api/src/session_api.cpp`, in the `BuildSessionJson` function, after line 132 (`std::memcpy(&mode_hash, ...)`), add:

```cpp
    /* Format mode hash as hex string */
    char mode_hex[19];
    std::snprintf(mode_hex, sizeof(mode_hex), "0x%016llx",
                  static_cast<unsigned long long>(mode_hash));
```

Then in the `json <<` chain (line 141-153), add the `game_mode_hash` field after `game_status`:

```cpp
    json << "{"
         << "\"game_status\":\"" << state_str << "\","
         << "\"game_mode_hash\":\"" << mode_hex << "\","
         << "\"game_clock\":0.0,"
```

- [ ] **Step 4.4: Build plugins and create dist**

```bash
cd ~/src/nevr-runtime-plugins && just build && just dist
```

Expected: build succeeds, dist packages created. The `just dist` step is required because the default test plugin path points to `dist/nevr-plugins-v1.0.0/plugins/` (not the raw CMake build tree).

- [ ] **Step 4.5: Commit fixes**

```bash
cd ~/src/nevr-runtime-plugins
git add plugins/super-hyper-turbo/src/superhyperturbo.cpp plugins/session-api/src/session_api.cpp
git commit -m "fix: MH_Initialize check, MH_RemoveHook, expose game_mode_hash

superhyperturbo.cpp:
- Check MH_Initialize return; bail if not OK/ALREADY_INITIALIZED
- Add MH_RemoveHook in RemoveSuperHyperTurboHooks

session_api.cpp:
- Add game_mode_hash hex field to BuildSessionJson JSON output"
```

- [ ] **Step 4.6: Re-run ground truth tests — all should pass now**

```bash
cd ~/src/nevr-runtime/tests/plugins && go test -v -run "TestGroundTruth" ./...
```

Expected: ALL pass including `TestGroundTruth_SessionApiModeHashInBuildSessionJson`.

---

### Task 5: Write Super Hyper Turbo Integration Tests

**Files:**

- Create: `tests/plugins/superhyperturbo_test.go`

- [ ] **Step 5.1: Create `~/src/nevr-runtime/tests/plugins/superhyperturbo_test.go`**

```go
package plugins

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"testing"
	"time"

	"github.com/EchoTools/evr-test-harness/pkg/testutil"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// IMPORTANT: These integration tests must NOT use t.Parallel().
// All tests share a single session_api instance on port 6721, and only one
// game process can run at a time. Tests within a Go package run sequentially
// by default — do not add t.Parallel() to any test in this file.

type sessionResponse struct {
	GameStatus   string  `json:"game_status"`
	GameModeHash string  `json:"game_mode_hash"`
	GameClock    float64 `json:"game_clock"`
	BluePoints   int     `json:"blue_points"`
	OrangePoints int     `json:"orange_points"`
}

// sessionApiPort is the port session_api listens on (default from session_api.h).
// This is SEPARATE from the game's built-in HTTP API port (http_port in echovr_start).
// The game's API does NOT include game_mode_hash; session_api's does.
const sessionApiPort = 6721

// fetchSessionFromPlugin queries the session_api plugin's HTTP server (NOT the game's).
func fetchSessionFromPlugin(t *testing.T) sessionResponse {
	t.Helper()
	url := fmt.Sprintf("http://localhost:%d/session", sessionApiPort)
	resp, err := http.Get(url)
	require.NoError(t, err, "GET /session from session_api (port %d) failed", sessionApiPort)
	defer resp.Body.Close()
	require.Equal(t, 200, resp.StatusCode)

	body, err := io.ReadAll(resp.Body)
	require.NoError(t, err)

	var session sessionResponse
	require.NoError(t, json.Unmarshal(body, &session),
		"Failed to parse session JSON: %s", string(body))
	return session
}

func deployPluginTestStack(t *testing.T, plugins ...string) {
	t.Helper()
	deployAllCoreDLLs(t)
	deployPlugins(t, plugins...)
}

// waitForSessionApi waits for the session_api plugin's HTTP server to respond.
func waitForSessionApi(t *testing.T, f *testutil.TestFixture) {
	t.Helper()
	url := fmt.Sprintf("http://localhost:%d/session", sessionApiPort)
	err := f.WaitForHTTP(url, 30*time.Second)
	require.NoError(t, err,
		"session_api HTTP API (port %d) did not become available", sessionApiPort)
}

func TestSuperHyperTurbo_ArenaGetsCombatWeapons(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	deployPluginTestStack(t, "session_api", "super_hyper_turbo")
	defer cleanupAll(t)

	f := testutil.NewFixture(t)
	defer f.Cleanup()

	// http_port is the GAME's built-in HTTP API — used for readiness check only.
	// session_api runs its own server on port 6721 (sessionApiPort).
	const gameHttpPort = 6780
	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       gameHttpPort,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 120,
	})
	require.NoError(t, err, "Failed to start Echo VR")

	sessionID := startResult["session_id"].(string)
	pid := int(startResult["pid"].(float64))
	t.Logf("Game started: session=%s pid=%d", sessionID, pid)

	defer func() {
		_, err := f.MCPClient().Call("echovr_stop", map[string]any{
			"session_id":      sessionID,
			"timeout_seconds": 10,
		})
		if err != nil {
			t.Logf("Warning: stop failed: %v", err)
		}
	}()

	// Wait for session_api plugin's HTTP server (port 6721), NOT the game's
	waitForSessionApi(t, f)

	session := fetchSessionFromPlugin(t)
	t.Logf("Session: status=%s game_mode_hash=%s", session.GameStatus, session.GameModeHash)

	// Assert: game_mode_hash should be the COMBAT hash because super-hyper-turbo
	// swaps arena hashes to combat before CreateSession runs.
	assert.Equal(t, "0x47c693e5ebaddea6", session.GameModeHash,
		"Arena mode should be swapped to HASH_COMBAT_PRACTICE_AI")

	AssertProcessRunning(t, pid)
}

func TestSuperHyperTurbo_PluginInitSucceeds(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	deployPluginTestStack(t, "session_api", "super_hyper_turbo")
	defer cleanupAll(t)

	f := testutil.NewFixture(t)
	defer f.Cleanup()

	const gameHttpPort = 6781
	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       gameHttpPort,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 120,
	})
	require.NoError(t, err)

	sessionID := startResult["session_id"].(string)
	defer func() {
		_, _ = f.MCPClient().Call("echovr_stop", map[string]any{
			"session_id": sessionID,
		})
	}()

	// Wait for session_api's HTTP server
	waitForSessionApi(t, f)

	session := fetchSessionFromPlugin(t)

	// game_mode_hash present → session_api loaded
	assert.NotEmpty(t, session.GameModeHash, "game_mode_hash should be present")
	// game_mode_hash is combat → super_hyper_turbo loaded and hooked
	assert.Equal(t, "0x47c693e5ebaddea6", session.GameModeHash,
		"game_mode_hash should be combat hash (super_hyper_turbo loaded)")

	t.Log("Both session_api and super_hyper_turbo initialized successfully")
}

func TestSuperHyperTurbo_NonArenaModePassthrough(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	deployPluginTestStack(t, "session_api", "super_hyper_turbo")
	defer cleanupAll(t)

	f := testutil.NewFixture(t)
	defer f.Cleanup()

	const gameHttpPort = 6782
	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       gameHttpPort,
		"gametype":        "echo_combat_private",
		"wait_ready":      true,
		"timeout_seconds": 120,
	})
	require.NoError(t, err)

	sessionID := startResult["session_id"].(string)
	defer func() {
		_, _ = f.MCPClient().Call("echovr_stop", map[string]any{
			"session_id": sessionID,
		})
	}()

	waitForSessionApi(t, f)

	session := fetchSessionFromPlugin(t)
	t.Logf("Combat mode session: game_mode_hash=%s", session.GameModeHash)

	assert.NotEmpty(t, session.GameModeHash)

	// These 6 hashes appear ONLY in IsArenaMode, not in any combat classifier
	arenaOnlyHashes := map[string]bool{
		"0xcb60a4de7e1caf73": true,
		"0x09990965f4db8c03": true,
		"0xd53a9c73b8b2c760": true,
		"0x4dc1ddbd89b28531": true,
		"0x339c5dc66c03b81f": true,
		"0xd894afea8246bc6b": true,
	}

	assert.False(t, arenaOnlyHashes[session.GameModeHash],
		"Combat mode should not have arena-only hash, got %s", session.GameModeHash)
}

func TestSuperHyperTurbo_CleanShutdown(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping integration test in short mode")
	}

	deployPluginTestStack(t, "session_api", "super_hyper_turbo")
	defer cleanupAll(t)

	f := testutil.NewFixture(t)
	defer f.Cleanup()

	const gameHttpPort = 6783
	startResult, err := f.MCPClient().Call("echovr_start", map[string]any{
		"http_port":       gameHttpPort,
		"gametype":        "echo_arena_private",
		"wait_ready":      true,
		"timeout_seconds": 120,
	})
	require.NoError(t, err)

	sessionID := startResult["session_id"].(string)
	pid := int(startResult["pid"].(float64))

	waitForSessionApi(t, f)
	AssertProcessRunning(t, pid)

	stopResult, err := f.MCPClient().Call("echovr_stop", map[string]any{
		"session_id":      sessionID,
		"timeout_seconds": 10,
	})
	require.NoError(t, err)
	assert.Equal(t, "stopped", stopResult["status"])

	AssertProcessStopped(t, pid)

	// Verify session_api's HTTP server is down too
	sessionApiURL := fmt.Sprintf("http://localhost:%d/session", sessionApiPort)
	AssertHTTPFails(t, sessionApiURL)

	t.Log("Clean shutdown verified (MH_RemoveHook working)")
}
```

- [ ] **Step 5.2: Verify compilation**

```bash
cd ~/src/nevr-runtime/tests/plugins && go vet ./...
```

Expected: no output (success).

- [ ] **Step 5.3: Commit**

```bash
cd ~/src/nevr-runtime
git add tests/plugins/superhyperturbo_test.go
git commit -m "tests/plugins: add super-hyper-turbo integration tests

Four MCP harness tests:
- ArenaGetsCombatWeapons: hash swap verified via /session
- PluginInitSucceeds: both plugins load
- NonArenaModePassthrough: combat mode not incorrectly swapped
- CleanShutdown: game stops cleanly with hooks removed"
```

---

### Task 6: Add Justfile Targets

**Files:**

- Modify: `~/src/nevr-runtime/justfile` (after line 60, the `test-system-verbose` target)

- [ ] **Step 6.1: Add plugin test targets to justfile**

Add after the `test-system-verbose` target:

```just
# Run plugin ground truth tests (no game binary needed)
test-plugins-groundtruth:
    cd tests/plugins && go test -v -run "TestGroundTruth" ./...

# Run all plugin tests (needs game binary + MCP harness)
test-plugins:
    cd tests/plugins && go test -v -timeout 10m ./...

# Run plugin tests in short mode (skips integration, runs ground truth only)
test-plugins-short:
    cd tests/plugins && go test -v -short ./...

# Run plugin tests with verbose output, no cache
test-plugins-verbose:
    cd tests/plugins && go test -v -count=1 -timeout 10m ./...
```

- [ ] **Step 6.2: Verify targets**

```bash
cd ~/src/nevr-runtime && just --list | grep test-plugins
```

Expected:

```
test-plugins               Run all plugin tests (needs game binary + MCP harness)
test-plugins-groundtruth   Run plugin ground truth tests (no game binary needed)
test-plugins-short         Run plugin tests in short mode (skips integration, runs ground truth only)
test-plugins-verbose       Run plugin tests with verbose output, no cache
```

- [ ] **Step 6.3: Run ground truth via justfile**

```bash
cd ~/src/nevr-runtime && just test-plugins-groundtruth
```

Expected: all ground truth tests pass.

- [ ] **Step 6.4: Commit**

```bash
cd ~/src/nevr-runtime
git add justfile
git commit -m "justfile: add plugin test targets"
```

---

## Verification

```bash
# Ground truth only (no game binary)
cd ~/src/nevr-runtime && just test-plugins-groundtruth

# Short mode (skips integration)
cd ~/src/nevr-runtime && just test-plugins-short

# Full integration (needs game + MCP)
cd ~/src/nevr-runtime && just test-plugins

# Verbose, no cache
cd ~/src/nevr-runtime && just test-plugins-verbose
```

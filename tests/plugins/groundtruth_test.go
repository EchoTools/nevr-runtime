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

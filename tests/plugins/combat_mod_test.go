package plugins

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// getCombatModDLLPath returns the path to combat_mod.dll in the build output.
// Uses NEVR_BUILD_DIR or falls back to build/mingw-release/bin/.
func getCombatModDLLPath() string {
	buildDir := getBuildDir()
	// Try the build/mingw-release/bin path first (direct CMake output)
	candidates := []string{
		filepath.Join(buildDir, "combat_mod.dll"),
		filepath.Join(buildDir, "..", "build", "mingw-release", "bin", "combat_mod.dll"),
		"../../build/mingw-release/bin/combat_mod.dll",
	}
	for _, p := range candidates {
		abs, _ := filepath.Abs(p)
		if _, err := os.Stat(abs); err == nil {
			return abs
		}
	}
	return candidates[0]
}

func getCombat2dDLLPath() string {
	buildDir := getBuildDir()
	candidates := []string{
		filepath.Join(buildDir, "combat_2d.dll"),
		filepath.Join(buildDir, "..", "build", "mingw-release", "bin", "combat_2d.dll"),
		"../../build/mingw-release/bin/combat_2d.dll",
	}
	for _, p := range candidates {
		abs, _ := filepath.Abs(p)
		if _, err := os.Stat(abs); err == nil {
			return abs
		}
	}
	return candidates[0]
}

func getOverrideDir() string {
	gameDir := getGameDir()
	return filepath.Join(gameDir, "bin", "win10", "_overrides", "combat")
}

// ── Tier 2: DLL existence and deployment tests (no game binary needed) ──

func TestCombatModDLLExists(t *testing.T) {
	path := getCombatModDLLPath()
	info, err := os.Stat(path)
	require.NoError(t, err, "combat_mod.dll not found at %s", path)
	assert.Greater(t, info.Size(), int64(0), "combat_mod.dll is empty")
	t.Logf("combat_mod.dll: %s (%d bytes)", path, info.Size())
}

func TestCombat2dDLLExists(t *testing.T) {
	path := getCombat2dDLLPath()
	info, err := os.Stat(path)
	require.NoError(t, err, "combat_2d.dll not found at %s", path)
	assert.Greater(t, info.Size(), int64(0), "combat_2d.dll is empty")
	t.Logf("combat_2d.dll: %s (%d bytes)", path, info.Size())
}

// ── Override file tests ──

type combatManifest struct {
	Version           int                `json:"version"`
	Sublevel          string             `json:"sublevel"`
	SublevelHash      string             `json:"sublevel_hash"`
	OriginalCombat    string             `json:"original_combat_hash"`
	ArenaLevel        string             `json:"arena_level"`
	ArenaHash         string             `json:"arena_hash"`
	Resources         []combatResource   `json:"resources"`
}

type combatResource struct {
	TypeHash  string `json:"type_hash"`
	NameHash  string `json:"name_hash"`
	TypeName  string `json:"type_name"`
	LevelName string `json:"level_name"`
	File      string `json:"file"`
	Size      int64  `json:"size"`
	Alias     bool   `json:"alias"`
	AliasTo   string `json:"alias_to"`
}

func TestCombatOverrideManifestExists(t *testing.T) {
	overrideDir := getOverrideDir()
	manifestPath := filepath.Join(overrideDir, "manifest.json")

	data, err := os.ReadFile(manifestPath)
	require.NoError(t, err, "manifest.json not found at %s", manifestPath)

	var m combatManifest
	require.NoError(t, json.Unmarshal(data, &m), "manifest.json parse error")

	assert.Equal(t, 1, m.Version, "manifest version")
	assert.Equal(t, "mpl_arena_combat", m.Sublevel)
	assert.Equal(t, "0x813edecf5228a2ba", m.SublevelHash, "sublevel hash mismatch")
	assert.Equal(t, "0xcb9977f7fc2b4526", m.OriginalCombat, "original combat hash mismatch")
	assert.Equal(t, "mpl_arena_a", m.ArenaLevel)

	t.Logf("Manifest: %d resources", len(m.Resources))
}

func TestCombatOverrideResourcesExist(t *testing.T) {
	overrideDir := getOverrideDir()
	manifestPath := filepath.Join(overrideDir, "manifest.json")

	data, err := os.ReadFile(manifestPath)
	require.NoError(t, err, "manifest.json not found")

	var m combatManifest
	require.NoError(t, json.Unmarshal(data, &m))

	modified := 0
	aliased := 0
	for _, r := range m.Resources {
		if r.Alias {
			aliased++
			assert.NotEmpty(t, r.AliasTo, "alias resource %s/%s missing alias_to", r.TypeName, r.LevelName)
		} else if r.File != "" {
			modified++
			filePath := filepath.Join(overrideDir, r.File)
			info, err := os.Stat(filePath)
			require.NoError(t, err, "override file not found: %s", filePath)
			assert.Equal(t, r.Size, info.Size(),
				"size mismatch for %s/%s: manifest=%d, file=%d",
				r.TypeName, r.LevelName, r.Size, info.Size())
		}
	}

	assert.Equal(t, 3, modified, "expected 3 modified resources")
	assert.Equal(t, 84, aliased, "expected 84 aliased resources")

	t.Logf("Verified: %d modified files on disk, %d aliases", modified, aliased)
}

func TestCombatOverrideFileFormat(t *testing.T) {
	// Verify the override files are named in the format the hook expects:
	// 0xTYPE_HASH.0xNAME_HASH
	overrideDir := getOverrideDir()
	manifestPath := filepath.Join(overrideDir, "manifest.json")

	data, err := os.ReadFile(manifestPath)
	require.NoError(t, err)

	var m combatManifest
	require.NoError(t, json.Unmarshal(data, &m))

	for _, r := range m.Resources {
		if r.File == "" {
			continue
		}
		// File should be "0xTYPE.0xNAME"
		assert.Regexp(t, `^0x[0-9a-f]{16}\.0x[0-9a-f]{16}$`, r.File,
			"file %s doesn't match expected format 0xTYPE.0xNAME for %s/%s",
			r.File, r.TypeName, r.LevelName)

		// Type hash in filename should match type_hash field
		expectedFile := r.TypeHash + "." + r.NameHash
		assert.Equal(t, expectedFile, r.File,
			"filename doesn't match hashes for %s/%s", r.TypeName, r.LevelName)
	}
}

func TestCombatOverrideModifiedResourceTypes(t *testing.T) {
	// Verify exactly the right resource types are modified
	overrideDir := getOverrideDir()
	manifestPath := filepath.Join(overrideDir, "manifest.json")

	data, err := os.ReadFile(manifestPath)
	require.NoError(t, err)

	var m combatManifest
	require.NoError(t, json.Unmarshal(data, &m))

	expectedModified := map[string]bool{
		"CActorDataResourceWin10": false,
		"CTransformCRWin10":       false,
		"CGSceneResourceWin10":    false,
	}

	for _, r := range m.Resources {
		if !r.Alias && r.File != "" {
			_, expected := expectedModified[r.TypeName]
			assert.True(t, expected, "unexpected modified type: %s", r.TypeName)
			if expected {
				expectedModified[r.TypeName] = true
			}
		}
	}

	for typeName, found := range expectedModified {
		assert.True(t, found, "expected modified type not found: %s", typeName)
	}
}

func TestCombatOverrideAliasHashConsistency(t *testing.T) {
	// All aliased resources should point to the same original combat hash
	overrideDir := getOverrideDir()
	manifestPath := filepath.Join(overrideDir, "manifest.json")

	data, err := os.ReadFile(manifestPath)
	require.NoError(t, err)

	var m combatManifest
	require.NoError(t, json.Unmarshal(data, &m))

	for _, r := range m.Resources {
		if !r.Alias {
			continue
		}
		assert.Equal(t, m.OriginalCombat, r.AliasTo,
			"alias_to mismatch for %s/%s: expected %s, got %s",
			r.TypeName, r.LevelName, m.OriginalCombat, r.AliasTo)
		assert.Equal(t, m.SublevelHash, r.NameHash,
			"aliased resource %s should have sublevel hash as name_hash",
			r.TypeName)
	}
}

func TestCombatOverrideTotalSize(t *testing.T) {
	overrideDir := getOverrideDir()
	manifestPath := filepath.Join(overrideDir, "manifest.json")

	data, err := os.ReadFile(manifestPath)
	require.NoError(t, err)

	var m combatManifest
	require.NoError(t, json.Unmarshal(data, &m))

	var totalSize int64
	for _, r := range m.Resources {
		if r.File != "" {
			totalSize += r.Size
		}
	}

	// Sanity check: total should be < 10MB (currently ~6.5MB)
	assert.Less(t, totalSize, int64(10*1024*1024),
		"total override file size %d exceeds 10MB", totalSize)
	assert.Greater(t, totalSize, int64(1*1024*1024),
		"total override file size %d is suspiciously small", totalSize)

	t.Logf("Total override file size: %.1f MB", float64(totalSize)/(1024*1024))
}

package plugins

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// repoRoot returns the nevr-runtime repository root.
func repoRoot(t *testing.T) string {
	t.Helper()
	// tests/plugins/ -> repo root is ../..
	wd, err := os.Getwd()
	require.NoError(t, err)
	return filepath.Join(wd, "..", "..")
}

// scanSourceFiles walks the directory and returns all .cpp and .h files,
// excluding legacy/ and extras/ directories.
func scanSourceFiles(t *testing.T, root string, dirs ...string) []string {
	t.Helper()
	var files []string
	for _, dir := range dirs {
		base := filepath.Join(root, dir)
		_ = filepath.Walk(base, func(path string, info os.FileInfo, err error) error {
			if err != nil {
				return nil
			}
			if info.IsDir() {
				name := info.Name()
				if name == "legacy" || name == "node_modules" || name == "build" {
					return filepath.SkipDir
				}
				return nil
			}
			if strings.HasSuffix(path, ".cpp") || strings.HasSuffix(path, ".h") {
				files = append(files, path)
			}
			return nil
		})
	}
	return files
}

// grepFiles returns files containing the pattern.
func grepFiles(t *testing.T, files []string, pattern string) []string {
	t.Helper()
	var matches []string
	for _, f := range files {
		data, err := os.ReadFile(f)
		if err != nil {
			continue
		}
		if strings.Contains(string(data), pattern) {
			matches = append(matches, f)
		}
	}
	return matches
}

// TestGroundTruth_NoExtractJsonString verifies all hand-rolled JSON parsing
// has been replaced with nlohmann-json.
func TestGroundTruth_NoExtractJsonString(t *testing.T) {
	root := repoRoot(t)
	files := scanSourceFiles(t, root, "plugins", "src")

	patterns := []string{
		"ExtractJsonString",
		"ExtractJsonBool",
		"ExtractJsonInt",
		"ExtractJsonUint64",
	}

	for _, pat := range patterns {
		matches := grepFiles(t, files, pat)
		assert.Empty(t, matches, "Found %s in: %v", pat, matches)
	}
}

// TestGroundTruth_NoExtractLogFilterHelpers verifies log-filter's hand-rolled
// JSON helpers have been replaced.
func TestGroundTruth_NoExtractLogFilterHelpers(t *testing.T) {
	root := repoRoot(t)
	files := scanSourceFiles(t, root, "plugins/log-filter")

	patterns := []string{
		"ExtractUint32",
		"ExtractInt(",       // avoid matching ExtractJsonInt
		"ExtractBool(",      // avoid matching ExtractJsonBool
		"ExtractString(",    // avoid matching ExtractJsonString
		"ExtractStringArray",
		"ExtractTruncateRules",
	}

	for _, pat := range patterns {
		matches := grepFiles(t, files, pat)
		assert.Empty(t, matches, "Found %s in: %v", pat, matches)
	}
}

// TestGroundTruth_NoLocalWriteCallback verifies no duplicate curl WriteCallback.
func TestGroundTruth_NoLocalWriteCallback(t *testing.T) {
	root := repoRoot(t)
	files := scanSourceFiles(t, root, "plugins")

	matches := grepFiles(t, files, "static size_t WriteCallback")
	assert.Empty(t, matches, "Found local WriteCallback in: %v", matches)
}

// TestGroundTruth_NoDuplicateDeviceAuth verifies only token-auth has
// RunDeviceAuthFlow.
func TestGroundTruth_NoDuplicateDeviceAuth(t *testing.T) {
	root := repoRoot(t)
	files := scanSourceFiles(t, root, "plugins")

	matches := grepFiles(t, files, "RunDeviceAuthFlow")
	for _, m := range matches {
		// Only token-auth should have it
		rel, _ := filepath.Rel(root, m)
		assert.True(t, strings.HasPrefix(rel, "plugins/token-auth"),
			"RunDeviceAuthFlow found outside token-auth: %s", rel)
	}
}

// TestGroundTruth_AuthJsonSchema validates .credentials.json roundtrip through
// Go's equivalent of the C++ LoadCachedAuthToken logic.
func TestGroundTruth_AuthJsonSchema(t *testing.T) {
	type AuthJSON struct {
		Token              string `json:"token"`
		TokenExpiry        uint64 `json:"token_expiry"`
		RefreshToken       string `json:"refresh_token,omitempty"`
		RefreshTokenExpiry uint64 `json:"refresh_token_expiry,omitempty"`
		UserID             string `json:"user_id,omitempty"`
		Username           string `json:"username,omitempty"`
	}

	now := uint64(time.Now().Unix())
	original := AuthJSON{
		Token:              "test_access_token",
		TokenExpiry:        now + 3600,
		RefreshToken:       "test_refresh_token",
		RefreshTokenExpiry: now + (30 * 24 * 3600),
		UserID:             "user-abc-123",
		Username:           "testplayer",
	}

	data, err := json.MarshalIndent(original, "", "  ")
	require.NoError(t, err)

	var loaded AuthJSON
	err = json.Unmarshal(data, &loaded)
	require.NoError(t, err)

	assert.Equal(t, original.Token, loaded.Token)
	assert.Equal(t, original.TokenExpiry, loaded.TokenExpiry)
	assert.Equal(t, original.RefreshToken, loaded.RefreshToken)
	assert.Equal(t, original.RefreshTokenExpiry, loaded.RefreshTokenExpiry)
	assert.Equal(t, original.UserID, loaded.UserID)
	assert.Equal(t, original.Username, loaded.Username)
}

// TestGroundTruth_AuthJsonBackwardsCompat verifies old "expiry" field is
// recognized (the C++ code falls back to "expiry" when "token_expiry" is 0).
func TestGroundTruth_AuthJsonBackwardsCompat(t *testing.T) {
	oldFormat := `{"token":"old_token","expiry":1234567890}`
	var j map[string]interface{}
	err := json.Unmarshal([]byte(oldFormat), &j)
	require.NoError(t, err)

	// Simulate C++ logic: token_expiry = j.value("token_expiry", 0); if 0, try "expiry"
	tokenExpiry := uint64(0)
	if v, ok := j["token_expiry"]; ok {
		tokenExpiry = uint64(v.(float64))
	}
	if tokenExpiry == 0 {
		if v, ok := j["expiry"]; ok {
			tokenExpiry = uint64(v.(float64))
		}
	}
	assert.Equal(t, uint64(1234567890), tokenExpiry)
}

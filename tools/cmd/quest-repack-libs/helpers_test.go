package main

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"regexp"
	"testing"
)

// reManifestPin is the data step's pin in src/quest/data/plan.h (questrepack's own pattern, kept here for the tests).
var reManifestPin = regexp.MustCompile(`kManifestPath\s*=\s*"_data/([A-Za-z0-9._-]+)/rad15/android/manifests/([A-Za-z0-9._-]+)"`)

// pinsOf is the data version folder and the manifest id of header's kManifestPath.
func pinsOf(t *testing.T, header string) (string, string) {
	t.Helper()
	raw, err := os.ReadFile(filepath.Clean(header))
	if err != nil {
		t.Fatalf("%v", err)
	}
	m := reManifestPin.FindSubmatch(raw)
	if m == nil {
		t.Fatalf("%s names no kManifestPath", header)
	}
	return string(m[1]), string(m[2])
}

// sha256Hex is the sha-256 of data, lower-case hex.
func sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// exists reports whether path exists.
func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

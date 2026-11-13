package questapk

import (
	"archive/zip"
	"bytes"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"testing"
)

// Expected values are written out by hand from the rule, as the Python tests wrote them, and kept as digit strings so no
// numeric literal sits in a test (ADR 0044: a number is a named constant; a table's expected value is data).
const (
	installerCodeText   = "4987570"
	storeCodeText       = "4987566"
	int32LimitText      = "2147483647"
	playCeilingText     = "2100000000"
	decimalBase         = 10
	releaseTagLimitText = "209"
	repoRootSteps       = "../../.."
	justfileName        = "justfile"
)

// mustInt parses an expected value written as digits.
func mustInt(t *testing.T, s string) int {
	t.Helper()
	v, err := strconv.Atoi(s)
	if err != nil {
		t.Fatalf("expected value %q is not an integer: %v", s, err)
	}
	return v
}

// repoRoot is the checkout this package is in: the three directories above it. It fails the test when the root has no
// justfile, so a test never runs against the wrong tree.
func repoRoot(t *testing.T) string {
	t.Helper()
	root, err := filepath.Abs(repoRootSteps)
	if err != nil {
		t.Fatalf("repo root: %v", err)
	}
	if _, err := exec.LookPath("git"); err != nil {
		t.Skipf("git is not on PATH: %v", err)
	}
	if !fileExists(filepath.Join(root, justfileName)) {
		t.Fatalf("repo root %s has no %s", root, justfileName)
	}
	return root
}

// fileExists reports whether a path exists.
func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

// contains reports whether s holds the substring want, failing the test with s when it does not.
func contains(t *testing.T, s, want string) {
	t.Helper()
	if !strings.Contains(s, want) {
		t.Fatalf("want %q in:\n%s", want, s)
	}
}

// notContains reports whether s lacks the substring, failing the test when it holds it.
func notContains(t *testing.T, s, unwanted string) {
	t.Helper()
	if strings.Contains(s, unwanted) {
		t.Fatalf("did not want %q in:\n%s", unwanted, s)
	}
}

// writeZip writes an archive of entries, in name order, to path.
func writeZip(t *testing.T, path string, entries map[string][]byte) {
	t.Helper()
	out, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer out.Close()
	zw := zip.NewWriter(out)
	names := make([]string, 0, len(entries))
	for name := range entries {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		w, err := zw.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := io.Copy(w, bytes.NewReader(entries[name])); err != nil {
			t.Fatal(err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
}

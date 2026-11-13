package questrepack

import (
	"archive/zip"
	"bytes"
	"crypto/md5" //nolint:gosec // the test's md5 of a fixture is the runtime's checksum (#1737)
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/questrepack/repackfixture"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// TestMain makes the test binary the stand-in patcher's modes too (testkit.Main, ADR 0044 rule 10).
func TestMain(m *testing.M) {
	testkit.Main(m, repackfixture.Fakes())
}

// entry is one file of a zip the tests make: its name and its bytes, in the order written.
type entry struct {
	name string
	data []byte
}

// zipOf writes the entries to path, each deflated, and returns the path. The entries carry no clock: the zip is the
// same bytes every time.
func zipOf(t *testing.T, path string, entries ...entry) string {
	t.Helper()
	f, err := os.Create(path)
	if err != nil {
		t.Fatalf("%v", err)
	}
	defer f.Close()
	zw := zip.NewWriter(f)
	for _, e := range entries {
		w, err := zw.CreateHeader(&zip.FileHeader{Name: e.name, Method: zip.Deflate})
		if err != nil {
			t.Fatalf("%v", err)
		}
		if _, err := w.Write(e.data); err != nil {
			t.Fatalf("%v", err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatalf("%v", err)
	}
	return path
}

// writeFile writes data at path under the test's directory and returns the path.
func writeFile(t *testing.T, path string, data []byte) string {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatalf("%v", err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatalf("%v", err)
	}
	return path
}

// sha is the sha-256 of data, lower-case hex.
func sha(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// md5Hex is the md5 of data, lower-case hex.
func md5Hex(data []byte) string {
	sum := md5.Sum(data) //nolint:gosec // see the import note
	return hex.EncodeToString(sum[:])
}

// aarch64SO is the smallest 64-bit little-endian AArch64 ELF header the staging check reads, then tag, so each file is
// distinct.
func aarch64SO(tag []byte) []byte {
	const (
		headerLen = 64
		emOffset  = 18
	)
	header := make([]byte, headerLen)
	copy(header, "\x7fELF")
	header[4] = elfClass64
	header[5] = elfDataLE
	binary.LittleEndian.PutUint16(header[emOffset:], emAArch64)
	return append(header, tag...)
}

// readFile reads path and fails the test on an error.
func readFile(t *testing.T, path string) []byte {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("%v", err)
	}
	return data
}

// hasAll reports whether every needle is a substring of one of the problems, and each one is a problem's whole (sub)text.
func hasSub(problems []string, needle string) bool {
	for _, p := range problems {
		if strings.Contains(p, needle) {
			return true
		}
	}
	return false
}

// need reports the tests' requirement: the test fails with the problems when none of them holds the needle.
func need(t *testing.T, problems []string, needle string) {
	t.Helper()
	if !hasSub(problems, needle) {
		t.Fatalf("want a problem containing %q, got %q", needle, problems)
	}
}

// requireTools skips the test when one of the named tools is missing (the Python tests' AARCH64_MISSING).
func requireTools(t *testing.T, names ...string) {
	t.Helper()
	var missing []string
	for _, n := range names {
		if _, err := exec.LookPath(n); err != nil {
			missing = append(missing, n)
		}
	}
	if len(missing) > 0 {
		t.Skipf("needs %s", strings.Join(missing, ", "))
	}
}

// bytesOf is the bytes of s, for a table written as strings.
func bytesOf(s string) []byte { return []byte(s) }

// exists reports whether path exists.
func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

// bytesEqual is bytes.Equal, named for the tests' reads.
func bytesEqual(a, b []byte) bool { return bytes.Equal(a, b) }

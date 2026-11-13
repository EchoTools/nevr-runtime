package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// File and directory modes of the files a test writes.
const (
	filePerm = 0o644
)

func setup(t *testing.T) (manifest, dir string) {
	t.Helper()
	dir = t.TempDir()
	body := []byte("zip bytes")
	sum := sha256.Sum256(body)
	if err := os.WriteFile(filepath.Join(dir, "a.zip"), body, filePerm); err != nil {
		t.Fatal(err)
	}
	manifest = filepath.Join(t.TempDir(), "manifest.json")
	js := fmt.Sprintf(`{"releases":{"1.2.3":{"artifacts":[{"kind":"windows-zip","path":"a.zip","size":%d,"sha256":"%s"}]}}}`, len(body), hex.EncodeToString(sum[:]))
	if err := os.WriteFile(manifest, []byte(js), filePerm); err != nil {
		t.Fatal(err)
	}
	return
}

func do(m, v, d string) (int, string) {
	var o, e bytes.Buffer
	c := execute([]string{"-manifest", m, "-version", v, "-dir", d}, &o, &e)
	return c, o.String() + e.String()
}

func TestGreenEveryFileOK(t *testing.T) {
	m, d := setup(t)
	c, out := do(m, "1.2.3", d)
	if c != 0 || !strings.Contains(out, "OK windows-zip a.zip") || !strings.Contains(out, "manifest_sha256=") {
		t.Fatalf("%d %s", c, out)
	}
}

// Planted violations: a flipped byte, a padded file, a missing file, a wrong version, a path that escapes.
func TestPlantedViolationsFail(t *testing.T) {
	m, d := setup(t)
	p := filepath.Join(d, "a.zip")
	os.WriteFile(p, []byte("zip byteX"), filePerm) // same size, one byte flipped
	if c, out := do(m, "1.2.3", d); c != 1 || !strings.Contains(out, "FAIL") || !strings.Contains(out, "sha256 is") {
		t.Errorf("flipped: %d %s", c, out)
	}
	os.WriteFile(p, []byte("zip bytes!"), filePerm)
	if c, out := do(m, "1.2.3", d); c != 1 || !strings.Contains(out, "size is 10") {
		t.Errorf("padded: %d %s", c, out)
	}
	os.Remove(p)
	if c, out := do(m, "1.2.3", d); c != 1 || !strings.Contains(out, "missing") {
		t.Errorf("missing: %d %s", c, out)
	}
	if c, _ := do(m, "9.9.9", d); c != 1 {
		t.Errorf("wrong version: %d", c)
	}
	raw, _ := os.ReadFile(m)
	os.WriteFile(m, []byte(strings.Replace(string(raw), `"a.zip"`, `"../a.zip"`, 1)), filePerm)
	if c, out := do(m, "1.2.3", d); c != 1 || !strings.Contains(out, "leaves") {
		t.Errorf("escape: %d %s", c, out)
	}
}

func TestBadInput(t *testing.T) {
	var o, e bytes.Buffer
	if execute(nil, &o, &e) != exitUsage {
		t.Fatal("flags")
	}
	bad := filepath.Join(t.TempDir(), "m")
	os.WriteFile(bad, []byte("not json"), filePerm)
	if c, _ := do(bad, "1", t.TempDir()); c != exitUsage {
		t.Fatalf("bad manifest: %d", c)
	}
}

// Planted violation of the logging rule: a run that leaves no record in the tools log fails here. The run is the real
// command through its own entry point, on a state directory of the test's own.
func TestARunIsRecordedInTheToolsLog(t *testing.T) {
	env := testkit.Env(t)
	m, d := setup(t)
	args := []string{"-manifest", m, "-version", "1.2.3", "-dir", d}
	var o, e bytes.Buffer
	code := run(args, testkit.Getenv(env), &o, &e)
	if code != 0 {
		t.Fatalf("exit %d: %s%s", code, o.String(), e.String())
	}
	testkit.RequireRunRecorded(t, env, "release-verify", code, args)
}

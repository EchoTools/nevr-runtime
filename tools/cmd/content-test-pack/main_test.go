package main

// #1424: content-test-pack makes a pack the runtime's rules accept, from a fake install this test writes: a manifest with
// the stock layout (a 24-byte ZSTD header, three table descriptors, three tables) and two script DLLs. Nothing of the
// game's is read.

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/klauspost/compress/zstd"
)

const set = "48037dc70b0ecab2"

func fakeManifest(t *testing.T, packages uint32, resources, files, chunks uint64) ([]byte, []byte) {
	t.Helper()
	le := binary.LittleEndian
	plain := make([]byte, tablesStart)
	le.PutUint32(plain[0:], packages)
	le.PutUint32(plain[4:], 0x80000)
	for _, d := range []struct{ base, count, stride uint64 }{{resourceDesc, resources, 0x20}, {fileDesc, files, 0x28}, {chunkDesc, chunks, 0x10}} {
		le.PutUint64(plain[d.base+0x08:], d.count*d.stride)
		le.PutUint64(plain[d.base+0x28:], d.count)
		le.PutUint64(plain[d.base+0x30:], d.count)
	}
	plain = append(plain, bytes.Repeat([]byte{0x11}, int(resources*0x20))...)
	plain = append(plain, bytes.Repeat([]byte{0x22}, int(files*0x28))...)
	for i := uint64(0); i < chunks; i++ {
		rec := make([]byte, 16)
		le.PutUint32(rec[0:], uint32(i)%packages)
		le.PutUint32(rec[4:], uint32(i*100))
		le.PutUint32(rec[8:], 10)
		le.PutUint32(rec[12:], 20)
		plain = append(plain, rec...)
	}
	enc, _ := zstd.NewWriter(nil)
	packed := enc.EncodeAll(plain, nil)
	enc.Close()
	header := make([]byte, headerSize)
	copy(header, "ZSTD")
	le.PutUint32(header[4:], 0x10)
	le.PutUint64(header[8:], uint64(len(plain)))
	le.PutUint64(header[16:], uint64(len(packed)))
	return append(header, packed...), plain
}

type fixture struct {
	game, content string
	stock, plain  []byte
	env           map[string]string
}

func newFixture(t *testing.T) fixture {
	t.Helper()
	dir := t.TempDir()
	f := fixture{game: filepath.Join(dir, "echovr"), content: filepath.Join(dir, "content"),
		env: map[string]string{"XDG_STATE_HOME": filepath.Join(dir, "state")}}
	f.stock, f.plain = fakeManifest(t, 3, 2, 2, 4)
	mustWrite(t, filepath.Join(f.game, filepath.FromSlash(dataDir), "manifests", set), f.stock)
	scripts := filepath.Join(f.game, "bin", "win10", "scripts")
	mustWrite(t, filepath.Join(scripts, "190ed14fe6c74dd3.dll"), []byte("script one"))
	mustWrite(t, filepath.Join(scripts, "0000000000000001.dll"), []byte("script two"))
	mustWrite(t, filepath.Join(scripts, "notascript.dll"), []byte("other"))
	return f
}

func mustWrite(t *testing.T, path string, data []byte) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

func (f fixture) run(args ...string) (int, string, string) {
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), args, func(k string) string { return f.env[k] }, nil, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

func TestTheManifestGainsOnePackageAndItsEndRecordAndNothingElse(t *testing.T) {
	f := newFixture(t)
	out := filepath.Join(f.content, "testpack")
	if code, _, stderr := f.run("-game-dir", f.game, "-out", out); code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	made, err := os.ReadFile(filepath.Join(out, "manifests", set))
	if err != nil {
		t.Fatal(err)
	}
	le := binary.LittleEndian
	if !bytes.Equal(made[:4], []byte("ZSTD")) || le.Uint32(made[4:]) != 0x10 || le.Uint64(made[16:]) != uint64(len(made)-headerSize) {
		t.Fatalf("header %x", made[:headerSize])
	}
	dec, _ := zstd.NewReader(nil)
	defer dec.Close()
	plain, err := dec.DecodeAll(made[headerSize:], nil)
	if err != nil {
		t.Fatal(err)
	}
	if len(plain) != len(f.plain)+16 || uint64(len(plain)) != le.Uint64(made[8:]) {
		t.Fatalf("length %d, stock %d", len(plain), len(f.plain))
	}
	if got := le.Uint32(plain[0:]); got != 4 {
		t.Errorf("package count %d, want 4", got)
	}
	if got := le.Uint64(plain[chunkDesc+0x08:]); got != 5*16 {
		t.Errorf("chunk bytes %d", got)
	}
	if capacity, c := le.Uint64(plain[chunkDesc+0x28:]), le.Uint64(plain[chunkDesc+0x30:]); c != 5 || capacity != 5 {
		t.Errorf("chunk count %d capacity %d", c, capacity)
	}
	end := plain[len(plain)-16:]
	if le.Uint32(end[0:]) != 3 || le.Uint32(end[4:]) != addedSize || le.Uint32(end[8:]) != 0 || le.Uint32(end[12:]) != 0 {
		t.Errorf("end record %x", end)
	}
	if !bytes.Equal(plain[tablesStart:len(plain)-16], f.plain[tablesStart:]) {
		t.Error("a stock byte past the counts changed")
	}
	pkg, _ := os.ReadFile(filepath.Join(out, "packages", set+"_3"))
	if !bytes.Equal(pkg, make([]byte, addedSize)) {
		t.Errorf("added package %x", pkg)
	}
}

func TestContentJsonListsEveryFileByItsDigestAndNamesTheStockManifest(t *testing.T) {
	f := newFixture(t)
	out := filepath.Join(f.content, "testpack")
	code, stdout, stderr := f.run("-game-dir", f.game, "-out", out)
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	var result made
	if err := json.Unmarshal([]byte(stdout), &result); err != nil || result.Scripts != 2 || result.Files != 4 {
		t.Fatalf("stdout %q (%v)", stdout, err)
	}
	text, _ := os.ReadFile(filepath.Join(out, "content.json"))
	var content struct {
		Format    int    `json:"format"`
		ID        string `json:"id"`
		Manifests []struct {
			StockSha256   string   `json:"stock_sha256"`
			StockPackages int      `json:"stock_packages"`
			Packages      []string `json:"packages"`
		} `json:"manifests"`
		Scripts []string          `json:"scripts"`
		Files   map[string]string `json:"files"`
	}
	if err := json.Unmarshal(text, &content); err != nil {
		t.Fatal(err)
	}
	m := content.Manifests[0]
	if content.Format != 1 || content.ID != "testpack" || m.StockSha256 != digest(f.stock) || m.StockPackages != 3 ||
		strings.Join(m.Packages, ",") != "packages/"+set+"_3" {
		t.Fatalf("content.json %s", text)
	}
	if strings.Join(content.Scripts, ",") != "scripts/0000000000000001.dll,scripts/190ed14fe6c74dd3.dll" {
		t.Errorf("scripts %v", content.Scripts)
	}
	shaped := regexp.MustCompile(`^(manifests|packages|scripts)/[0-9a-f_]+(\.dll)?$`)
	for rel, want := range content.Files {
		data, err := os.ReadFile(filepath.Join(out, filepath.FromSlash(rel)))
		if err != nil || digest(data) != want || !shaped.MatchString(rel) {
			t.Errorf("%s: %v", rel, err)
		}
	}
	if len(content.Files) != 4 {
		t.Errorf("%d files", len(content.Files))
	}
	log, err := os.ReadFile(filepath.Join(f.env["XDG_STATE_HOME"], "nevr-runtime", "content-test-pack.jsonl"))
	if err != nil || !strings.Contains(string(log), `"event":"pack_made"`) {
		t.Errorf("run log %q (%v)", log, err)
	}
}

func TestRefusals(t *testing.T) {
	f := newFixture(t)
	existing := filepath.Join(f.content, "testpack")
	if err := os.MkdirAll(existing, 0o755); err != nil {
		t.Fatal(err)
	}
	broken, _ := fakeManifest(t, 3, 2, 2, 4)
	copy(broken, "XXXX")
	brokenGame := filepath.Join(t.TempDir(), "echovr")
	mustWrite(t, filepath.Join(brokenGame, filepath.FromSlash(dataDir), "manifests", set), broken)
	cases := []struct {
		name string
		args []string
		want string
	}{
		{"an existing folder", []string{"-game-dir", f.game, "-out", existing}, "exists"},
		{"an id with upper case", []string{"-game-dir", f.game, "-out", filepath.Join(f.content, "Bad"), "-id", "Bad"}, "not a pack id"},
		{"a folder not named by the id", []string{"-game-dir", f.game, "-out", filepath.Join(f.content, "other")}, "named testpack"},
		{"a header that is not ZSTD", []string{"-game-dir", brokenGame, "-out", filepath.Join(f.content, "fresh"), "-id", "fresh"}, "not ZSTD"},
	}
	for _, c := range cases {
		code, _, stderr := f.run(c.args...)
		if code != 1 || !strings.Contains(stderr, c.want) {
			t.Errorf("%s: exit %d, stderr %q", c.name, code, stderr)
		}
	}
}

func TestThePackIdRuleIsTheRuntimes(t *testing.T) {
	header, err := os.ReadFile(filepath.Join("..", "..", "..", "src", "runtime", "content", "pack_rules.h"))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(header), "[a-z0-9][a-z0-9._-]{0,63}") || packID.String() != `^[a-z0-9][a-z0-9._-]{0,63}$` {
		t.Error("the pack id rule here and in runtime/content/pack_rules.h differ")
	}
}

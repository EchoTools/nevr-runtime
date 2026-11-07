package main

// content-pack-build over a fake install the test writes (tools/internal/radpack WriteStockSet: two client sets with the
// shipped layout, and two script DLLs). Nothing of the game's is read.

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

type fixture struct {
	game, root string
	env        map[string]string
}

func newFixture(t *testing.T) fixture {
	t.Helper()
	dir := t.TempDir()
	f := fixture{game: filepath.Join(dir, "echovr"), root: filepath.Join(dir, "root"), env: map[string]string{"XDG_STATE_HOME": filepath.Join(dir, "state")}}
	data := filepath.Join(f.game, filepath.FromSlash(DataDir))
	if err := radpack.WriteStockSet(data, Sets[0], []map[radpack.Key][]byte{
		{{Type: 1, Name: 1}: []byte("stock settings"), {Type: 2, Name: 7}: []byte("stock table")},
		{{Type: 3, Name: 9}: []byte("stock model")},
	}); err != nil {
		t.Fatal(err)
	}
	if err := radpack.WriteStockSet(data, Sets[1], []map[radpack.Key][]byte{{{Type: 4, Name: 4}: []byte("stock bank")}}); err != nil {
		t.Fatal(err)
	}
	scripts := filepath.Join(f.game, "bin", "win10", "scripts")
	if err := os.MkdirAll(scripts, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(scripts, "190ed14fe6c74dd3.dll"), []byte("MZ stock script"), 0o644); err != nil {
		t.Fatal(err)
	}
	return f
}

func (f fixture) out(id string) string { return filepath.Join(f.root, "content", id) }

func (f fixture) run(list []Step, args ...string) (int, string, string) {
	var stdout, stderr bytes.Buffer
	code := runWith(list, args, func(k string) string { return f.env[k] }, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

// A step of the kind the EchoCombat slices add: read stock, change it, put it back; add a resource; change a script.
var testSteps = []Step{
	{Name: "rewrite the table", Apply: func(p *Pack) error {
		b, err := p.Read(Sets[0], 2, 7)
		if err != nil {
			return err
		}
		return p.Put(Sets[0], 2, 7, append(b, []byte(" + a row")...))
	}},
	{Name: "add a resource", Apply: func(p *Pack) error {
		// A later step reads the pack as the earlier steps left it.
		b, err := p.Read(Sets[0], 2, 7)
		if err != nil {
			return err
		}
		if string(b) != "stock table + a row" {
			return fmt.Errorf("the table reads %q, not the first step's change", b)
		}
		return p.Put(Sets[0], 2, 8, []byte("new"))
	}},
	{Name: "patch a script", Apply: func(p *Pack) error {
		b, err := p.ReadScript("190ed14fe6c74dd3")
		if err != nil {
			return err
		}
		return p.PutScript("190ed14fe6c74dd3", append(b, '!'))
	}},
}

func TestThePackServesTheStepsChangesInTheRuntimesFormat(t *testing.T) {
	f := newFixture(t)
	out := f.out("testpack")
	code, stdout, stderr := f.run(testSteps, "-game-dir", f.game, "-out", out, "-id", "testpack", "-version", "1.0.0")
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	var res Result
	if err := json.Unmarshal([]byte(stdout), &res); err != nil || res.Manifests != 1 || res.Resources != 2 || res.Scripts != 1 || res.Files != 3 {
		t.Fatalf("stdout %q (%v)", stdout, err)
	}
	text, err := os.ReadFile(filepath.Join(out, "content.json"))
	if err != nil {
		t.Fatal(err)
	}
	var cj contentJSON
	if err := json.Unmarshal(text, &cj); err != nil {
		t.Fatal(err)
	}
	stockRaw, _ := os.ReadFile(filepath.Join(f.game, filepath.FromSlash(DataDir), "manifests", Sets[0]))
	sum := sha256.Sum256(stockRaw)
	if cj.Format != 1 || cj.ID != "testpack" || cj.Version != "1.0.0" || cj.DataDir != DataDir || len(cj.Manifests) != 1 {
		t.Fatalf("content.json %s", text)
	}
	m := cj.Manifests[0]
	if m.Package != Sets[0] || m.StockSha256 != hex.EncodeToString(sum[:]) || m.StockPackages != 2 || m.File != "manifests/"+Sets[0] ||
		strings.Join(m.Packages, ",") != "packages/"+Sets[0]+"_2" {
		t.Errorf("manifest entry %+v", m)
	}
	if strings.Join(cj.Scripts, ",") != "scripts/190ed14fe6c74dd3.dll" {
		t.Errorf("scripts %v", cj.Scripts)
	}
	for rel, want := range cj.Files {
		b, err := os.ReadFile(filepath.Join(out, filepath.FromSlash(rel)))
		got := sha256.Sum256(b)
		if err != nil || hex.EncodeToString(got[:]) != want {
			t.Errorf("%s: %v", rel, err)
		}
	}
	// The pack's set as the runtime serves it: its manifest over the stock packages plus its own.
	view := filepath.Join(t.TempDir(), "view")
	stockData := filepath.Join(f.game, filepath.FromSlash(DataDir))
	copyFile(t, filepath.Join(out, "manifests", Sets[0]), filepath.Join(view, "manifests", Sets[0]))
	for _, p := range []string{Sets[0] + "_0", Sets[0] + "_1"} {
		copyFile(t, filepath.Join(stockData, "packages", p), filepath.Join(view, "packages", p))
	}
	copyFile(t, filepath.Join(out, "packages", Sets[0]+"_2"), filepath.Join(view, "packages", Sets[0]+"_2"))
	s, _, err := radpack.OpenSet(view, Sets[0])
	if err != nil {
		t.Fatal(err)
	}
	for k, want := range map[radpack.Key]string{{Type: 2, Name: 7}: "stock table + a row", {Type: 2, Name: 8}: "new",
		{Type: 1, Name: 1}: "stock settings", {Type: 3, Name: 9}: "stock model"} {
		got, ok, err := s.Read(k.Type, k.Name)
		if err != nil || !ok || string(got) != want {
			t.Errorf("%v: %q %v %v", k, got, ok, err)
		}
	}
	if dll, _ := os.ReadFile(filepath.Join(out, "scripts", "190ed14fe6c74dd3.dll")); string(dll) != "MZ stock script!" {
		t.Errorf("script %q", dll)
	}
	// The untouched set is not in the pack.
	if _, err := os.Stat(filepath.Join(out, "manifests", Sets[1])); !errors.Is(err, os.ErrNotExist) {
		t.Error("a set no step changed was written")
	}
	log, err := os.ReadFile(filepath.Join(f.env["XDG_STATE_HOME"], "nevr-runtime", "content-pack-build.jsonl"))
	if err != nil || !strings.Contains(string(log), `"event":"pack_made"`) || strings.Count(string(log), `"event":"step_applied"`) != 3 {
		t.Errorf("run log %s (%v)", log, err)
	}
}

func copyFile(t *testing.T, from, to string) {
	t.Helper()
	b, err := os.ReadFile(from)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Dir(to), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(to, b, 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestWithNoStepThatChangesAnythingNothingIsWritten(t *testing.T) {
	f := newFixture(t)
	out := f.out("emptypack")
	code, _, stderr := f.run(nil, "-game-dir", f.game, "-out", out, "-id", "emptypack", "-version", "1.0.0")
	if code != 1 || !strings.Contains(stderr, "nothing to pack") {
		t.Errorf("exit %d, stderr %q", code, stderr)
	}
	if _, err := os.Stat(out); !errors.Is(err, os.ErrNotExist) {
		t.Error("the pack folder was made")
	}
}

func TestRefusalsWriteNothing(t *testing.T) {
	f := newFixture(t)
	failing := append(append([]Step(nil), testSteps[0]), Step{Name: "fails", Apply: func(p *Pack) error {
		_, err := p.Read(Sets[0], 99, 99)
		return err
	}})
	scriptsOnly := []Step{testSteps[2]}
	exists := f.out("exists")
	if err := os.MkdirAll(exists, 0o755); err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name string
		list []Step
		args []string
		want string
	}{
		{"a step that fails", failing, []string{"-out", f.out("p"), "-id", "p", "-version", "1"}, "lists no resource"},
		{"scripts only", scriptsOnly, []string{"-out", f.out("p"), "-id", "p", "-version", "1"}, "needs a manifest"},
		{"an existing folder", testSteps, []string{"-out", exists, "-id", "exists", "-version", "1"}, "exists"},
		{"a bad id", testSteps, []string{"-out", f.out("Bad"), "-id", "Bad", "-version", "1"}, "not a pack id"},
		{"a folder not named by the id", testSteps, []string{"-out", f.out("other"), "-id", "p", "-version", "1"}, "named p"},
		{"a version with a quote", testSteps, []string{"-out", f.out("p"), "-id", "p", "-version", `1"`}, "not a version"},
		{"a script name that is not hex", []Step{{Name: "bad", Apply: func(p *Pack) error { return p.PutScript("nothex", []byte("x")) }}},
			[]string{"-out", f.out("p"), "-id", "p", "-version", "1"}, "not a script name"},
		{"no stock install", testSteps, []string{"-out", f.out("p"), "-id", "p", "-version", "1", "-game-dir", t.TempDir()}, "stock set"},
	}
	for _, c := range cases {
		args := append([]string{"-game-dir", f.game}, c.args...)
		code, _, stderr := f.run(c.list, args...)
		if code != 1 || !strings.Contains(stderr, c.want) {
			t.Errorf("%s: exit %d, stderr %q", c.name, code, stderr)
		}
	}
	if _, err := os.Stat(f.out("p")); !errors.Is(err, os.ErrNotExist) {
		t.Error("a refused build left its folder")
	}
}

func TestTheProductionStepsAreTheEchoCombatOnesInOrder(t *testing.T) {
	want := []string{
		"drone bullet takes the assault bullet's effects",
		"drone bullet gets the assault bullet's particle rows",
		"drone actor runs script 190ed14fe6c74dd3",
	}
	if len(steps) != len(want) {
		t.Fatalf("steps has %d entries, want %d: each EchoCombat step lands in its own slice with its own tests", len(steps), len(want))
	}
	for i, w := range want {
		if steps[i].Name != w {
			t.Errorf("step %d is %q, want %q", i, steps[i].Name, w)
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

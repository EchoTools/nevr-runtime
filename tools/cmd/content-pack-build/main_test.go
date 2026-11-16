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
		"DOT FIELD magazine drops its leftover rigid-attach rows",
		"DOT FIELD magazine drops its leftover parented-constraint rows",
		"DOT FIELD arms by remote trigger, not on impact",
		"drone bullet takes the assault bullet's effects",
		"drone bullet gets the assault bullet's particle rows",
		"drone actor runs script 190ed14fe6c74dd3",
		"damageable health of actor 10e40287a9bf5a13 9999 to 70",
		"equipment lists offer magnum, smg, chain, rifle, loc, dot and drone",
		"RULES page labels the capture point's two times",
	}
	// Steps of the other data pieces (steps_<feature>.go, each registered from its own init) may sit between these and
	// are checked by their own tests; these must keep their relative order and each must be present once.
	seen := map[string]int{}
	for _, st := range steps {
		seen[st.Name]++
	}
	for _, w := range want {
		if seen[w] != 1 {
			t.Errorf("step %q is registered %d times, want once", w, seen[w])
		}
	}
	at := 0
	for _, st := range steps {
		if at < len(want) && st.Name == want[at] {
			at++
		}
	}
	if at != len(want) {
		t.Fatalf("the production steps do not hold %q in order from step %d of %d", want[at], at, len(want))
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

func TestAnOutInsideTheGameFolderIsRefused(t *testing.T) {
	f := newFixture(t)
	link := filepath.Join(t.TempDir(), "game-link")
	if err := os.Symlink(f.game, link); err != nil {
		t.Fatal(err)
	}
	for _, out := range []string{
		filepath.Join(f.game, "content", "p"),
		filepath.Join(f.game, "bin", "win10", "p"), // an existing folder of the game
		filepath.Join(link, "content", "p"),        // through a link to the game folder
		filepath.Join(f.game, "..x", "p"),          // a folder of the game whose name starts with ..
	} {
		code, _, stderr := f.run(testSteps, "-game-dir", f.game, "-out", out, "-id", "p", "-version", "1")
		if code != 1 || !strings.Contains(stderr, "inside -game-dir") {
			t.Errorf("%s: exit %d, stderr %q", out, code, stderr)
		}
		if _, err := os.Lstat(out); !errors.Is(err, os.ErrNotExist) {
			t.Errorf("%s was written", out)
		}
	}
	// -game-dir given as a link, -out by the real path.
	code, _, stderr := f.run(testSteps, "-game-dir", link, "-out", filepath.Join(f.game, "content", "p"), "-id", "p", "-version", "1")
	if code != 1 || !strings.Contains(stderr, "inside -game-dir") {
		t.Errorf("-game-dir as a link: exit %d, stderr %q", code, stderr)
	}
	if _, err := os.Lstat(filepath.Join(f.game, "content")); !errors.Is(err, os.ErrNotExist) {
		t.Error("a folder was made in the game folder")
	}
	// A sibling whose name starts with the game folder's is not inside it.
	out := filepath.Join(f.game+"-packs", "content", "p")
	if code, _, stderr := f.run(testSteps, "-game-dir", f.game, "-out", out, "-id", "p", "-version", "1"); code != 0 {
		t.Errorf("a folder beside the game folder was refused: %s", stderr)
	}
}

func TestAFailedWriteLeavesNoOut(t *testing.T) {
	f := newFixture(t)
	out := f.out("p")
	n := 0
	writeFile = func(name string, b []byte, mode os.FileMode) error {
		if n++; n == 2 {
			return errors.New("disk full")
		}
		return os.WriteFile(name, b, mode)
	}
	t.Cleanup(func() { writeFile = os.WriteFile })
	code, _, stderr := f.run(testSteps, "-game-dir", f.game, "-out", out, "-id", "p", "-version", "1")
	if code != 1 || !strings.Contains(stderr, "disk full") {
		t.Errorf("exit %d, stderr %q", code, stderr)
	}
	if _, err := os.Lstat(out); !errors.Is(err, os.ErrNotExist) {
		t.Error("a failed write left -out")
	}
	left, err := os.ReadDir(filepath.Dir(out))
	if err != nil {
		t.Fatal(err)
	}
	if len(left) != 0 {
		t.Errorf("the staging folder was left: %v", left[0].Name())
	}
	// With the writes working, the same build writes -out whole and leaves no staging folder beside it.
	writeFile = os.WriteFile
	if code, _, stderr := f.run(testSteps, "-game-dir", f.game, "-out", out, "-id", "p", "-version", "1"); code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	if left, _ = os.ReadDir(filepath.Dir(out)); len(left) != 1 || left[0].Name() != "p" {
		t.Errorf("beside -out: %v", left)
	}
	if fi, err := os.Stat(out); err != nil || fi.Mode().Perm() != 0o755 {
		t.Errorf("-out: %v, %v", fi, err)
	}
	if _, err := os.Stat(filepath.Join(out, "content.json")); err != nil {
		t.Error(err)
	}
}

func TestAFolderMadeAtOutWhileThePackIsWrittenIsNotReplaced(t *testing.T) {
	f := newFixture(t)
	out := f.out("p")
	writeFile = func(name string, b []byte, mode os.FileMode) error {
		if err := os.MkdirAll(out, 0o755); err != nil {
			return err
		}
		return os.WriteFile(name, b, mode)
	}
	t.Cleanup(func() { writeFile = os.WriteFile })
	code, _, stderr := f.run(testSteps, "-game-dir", f.game, "-out", out, "-id", "p", "-version", "1")
	if code != 1 || !strings.Contains(stderr, "exists") {
		t.Errorf("exit %d, stderr %q", code, stderr)
	}
	if left, _ := os.ReadDir(out); len(left) != 0 {
		t.Errorf("the folder made at -out was written into: %v", left)
	}
	if left, _ := os.ReadDir(filepath.Dir(out)); len(left) != 1 {
		t.Errorf("beside -out: %v", left)
	}
}

// A link planted at the name the pack writes next, while the write fails, is unlinked by name and its target survives:
// the staging cleanup removes only the names writeStaged recorded, and never follows a link.
func TestAFailedWriteNeverFollowsALinkPlantedInStaging(t *testing.T) {
	f := newFixture(t)
	out := f.out("p")
	canary := filepath.Join(t.TempDir(), "canary.txt")
	if err := os.WriteFile(canary, []byte("canary\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	n := 0
	writeFile = func(name string, b []byte, mode os.FileMode) error {
		if n++; n == 2 {
			if err := os.Symlink(canary, name); err != nil {
				t.Fatal(err)
			}
			return errors.New("disk full")
		}
		return os.WriteFile(name, b, mode)
	}
	t.Cleanup(func() { writeFile = os.WriteFile })
	code, _, stderr := f.run(testSteps, "-game-dir", f.game, "-out", out, "-id", "p", "-version", "1")
	if code != 1 || !strings.Contains(stderr, "disk full") {
		t.Errorf("exit %d, stderr %q", code, stderr)
	}
	if b, err := os.ReadFile(canary); err != nil || string(b) != "canary\n" {
		t.Errorf("the canary changed: %q, %v", b, err)
	}
	if left, _ := os.ReadDir(filepath.Dir(out)); len(left) != 0 {
		t.Errorf("the staging folder was left: %v", left)
	}
}

const (
	rekeyCopy   = "00000000000000c1"
	rekeySource = "190ed14fe6c74dd3"
)

func TestARekeyIsListedWithTheSha256OfItsStockScriptAndCarriesNoByte(t *testing.T) {
	f := newFixture(t)
	out := f.out("rekeypack")
	list := append([]Step{{Name: "copy a stock script", Apply: func(p *Pack) error { return p.Rekey(rekeyCopy, rekeySource) }}}, testSteps[:2]...)
	code, stdout, stderr := f.run(list, "-game-dir", f.game, "-out", out, "-id", "rekeypack", "-version", "1.0.0")
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	var res Result
	if err := json.Unmarshal([]byte(stdout), &res); err != nil || res.Rekeys != 1 || res.Scripts != 0 {
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
	sum := sha256.Sum256([]byte("MZ stock script"))
	want := RekeyEntry{Name: rekeyCopy, Platform: "windows-x86_64", StockSource: rekeySource, StockSha256: hex.EncodeToString(sum[:])}
	if len(cj.Rekeys) != 1 || cj.Rekeys[0] != want {
		t.Fatalf("rekeys %+v, want [%+v]", cj.Rekeys, want)
	}
	if len(cj.Scripts) != 0 {
		t.Errorf("scripts %v: a rekey carries no script", cj.Scripts)
	}
	for rel := range cj.Files {
		if strings.HasPrefix(rel, "scripts/") {
			t.Errorf("the pack carries %s", rel)
		}
	}
	if _, err := os.Stat(filepath.Join(out, "scripts")); !errors.Is(err, os.ErrNotExist) {
		t.Errorf("scripts folder written: %v", err)
	}
}

func TestARekeyThatCannotBeMadeIsRefusedAndWritesNothing(t *testing.T) {
	cases := map[string]func(p *Pack) error{
		"stock script absent": func(p *Pack) error { return p.Rekey(rekeyCopy, "94ba7ca39fc97e1a") },
		"bad source":          func(p *Pack) error { return p.Rekey(rekeyCopy, "../x") },
		"bad name":            func(p *Pack) error { return p.Rekey("C1", rekeySource) },
		"itself":              func(p *Pack) error { return p.Rekey(rekeySource, rekeySource) },
		"twice": func(p *Pack) error {
			if err := p.Rekey(rekeyCopy, rekeySource); err != nil {
				return err
			}
			return p.Rekey(rekeyCopy, rekeySource)
		},
		"a name the pack carries": func(p *Pack) error {
			if err := p.PutScript(rekeyCopy, []byte("MZ")); err != nil {
				return err
			}
			return p.Rekey(rekeyCopy, rekeySource)
		},
	}
	for what, apply := range cases {
		f := newFixture(t)
		out := f.out("rekeypack")
		// A step that changes a resource comes first, so only the rekey can be what refuses the build.
		list := []Step{{Name: "add a resource", Apply: func(p *Pack) error { return p.Put(Sets[0], 2, 8, []byte("new")) }}, {Name: what, Apply: apply}}
		code, _, stderr := f.run(list, "-game-dir", f.game, "-out", out, "-id", "rekeypack", "-version", "1.0.0")
		if code != 1 || !strings.Contains(stderr, "step "+what) {
			t.Errorf("%s: exit %d, want 1 from that step: %s", what, code, stderr)
		}
		if _, err := os.Stat(out); !errors.Is(err, os.ErrNotExist) {
			t.Errorf("%s: out written", what)
		}
	}
}

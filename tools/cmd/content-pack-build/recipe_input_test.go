package main

// Recipe inputs (recipe_input.go): the -input name=path flag and Pack.ReadInput. The tests write their own fixture files into
// t.TempDir(); the one test that reads a real asset is skipped unless NEVR_DRONE4_GLB names it.

import (
	"bytes"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const (
	drone4Env    = "NEVR_DRONE4_GLB"                                                  // the same variable tools/internal/gltf's test reads
	drone4Bytes  = 4435328                                                            // Drone4.glb as measured (stat of the file)
	drone4SHA256 = "31f2c14299a146cbab2c54b9b23e2ff87fdb6764c11418d84627278c5ed3a8d3" // sha256sum of the file, measured
	clipInput    = "clip"                                                             // the input name the fixture step reads
	clipContent  = "recipe clip bytes\n"
)

// recipeInputFile writes b into a new file of t's temporary folder and returns its path.
func recipeInputFile(t *testing.T, name string, b []byte) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), name)
	if err := os.WriteFile(path, b, 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// A Pack with the flag `-input clip=<path>` given, as Build sets it, and the step named "reads" running.
func inputPack(path string) *Pack {
	return &Pack{inputFiles: map[string]string{clipInput: path}, step: "reads"}
}

// ReadInput returns the bytes of the file the flag named when its sha-256 is the pin, and records one pinned input with the
// step that read it.
func TestReadInputReturnsTheFlaggedBytesWhenTheDigestMatches(t *testing.T) {
	b := []byte(clipContent)
	p := inputPack(recipeInputFile(t, "clip.bin", b))
	got, err := p.ReadInput(clipInput, digest(b))
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != clipContent {
		t.Fatalf("ReadInput returned %q, want %q", got, clipContent)
	}
	if len(p.inputs) != 1 || p.inputs[0] != (PinnedInput{Step: "reads", File: "clip.bin", Sha256: digest(b)}) {
		t.Fatalf("pinned inputs %+v", p.inputs)
	}
}

// A step that reads an input with no -input flag for it is refused, naming the flag the way -gear-icons-dir is named, and
// records nothing.
func TestReadInputRefusesAnAbsentFlagNamingIt(t *testing.T) {
	p := &Pack{step: "reads"}
	got, err := p.ReadInput(clipInput, digest([]byte(clipContent)))
	if err == nil || got != nil || !strings.Contains(err.Error(), "-input clip=<file> is not set") || !strings.Contains(err.Error(), "is required") {
		t.Fatalf("got %q, error %v", got, err)
	}
	if len(p.inputs) != 0 {
		t.Fatalf("pinned inputs %+v after a refusal", p.inputs)
	}
}

// A file whose sha-256 is not the pin is refused, and the error gives both digests; nothing is returned or recorded.
func TestReadInputRefusesADigestThatDiffers(t *testing.T) {
	b := []byte(clipContent)
	p := inputPack(recipeInputFile(t, "clip.bin", b))
	pin := digest([]byte("some other bytes"))
	got, err := p.ReadInput(clipInput, pin)
	if err == nil || got != nil {
		t.Fatalf("got %q, error %v", got, err)
	}
	if !strings.Contains(err.Error(), "sha256 is "+digest(b)) || !strings.Contains(err.Error(), "the pinned value is "+pin) {
		t.Fatalf("error does not name both digests: %v", err)
	}
	if len(p.inputs) != 0 {
		t.Fatalf("pinned inputs %+v after a refusal", p.inputs)
	}
}

// A flagged file that is not there is refused with the open error, and nothing is recorded.
func TestReadInputRefusesAMissingFile(t *testing.T) {
	p := inputPack(filepath.Join(t.TempDir(), "absent.bin"))
	if got, err := p.ReadInput(clipInput, digest([]byte(clipContent))); err == nil || got != nil || len(p.inputs) != 0 {
		t.Fatalf("got %q, error %v, inputs %+v", got, err, p.inputs)
	}
}

// A flag that names a directory is refused: an input is one regular file.
func TestReadInputRefusesADirectory(t *testing.T) {
	p := inputPack(t.TempDir())
	_, err := p.ReadInput(clipInput, digest(nil))
	if err == nil || !strings.Contains(err.Error(), "not a regular file") {
		t.Fatalf("error %v, want the regular-file refusal", err)
	}
}

// A file over maxRecipeInputBytes is refused by its size, before its digest is compared. The file is sparse, so the test
// writes no bytes to the disk.
func TestReadInputRefusesAFileOverTheCap(t *testing.T) {
	path := filepath.Join(t.TempDir(), "big.bin")
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := f.Truncate(maxRecipeInputBytes + 1); err != nil {
		t.Fatal(err)
	}
	if err := f.Close(); err != nil {
		t.Fatal(err)
	}
	p := inputPack(path)
	_, err = p.ReadInput(clipInput, digest(nil))
	if err == nil || !strings.Contains(err.Error(), "is over") {
		t.Fatalf("error %v, want the size refusal", err)
	}
}

// A name that is not an input name, and a pin that is not 64 lowercase hex digits, are refused before anything is read.
func TestReadInputRefusesABadNameOrPin(t *testing.T) {
	p := inputPack(recipeInputFile(t, "clip.bin", []byte(clipContent)))
	for _, c := range []struct{ name, pin, want string }{
		{"../clip", digest([]byte(clipContent)), "is not an input name"},
		{"Clip", digest([]byte(clipContent)), "is not an input name"},
		{clipInput, "ABC", "not 64 lowercase hex digits"},
		{clipInput, strings.ToUpper(digest([]byte(clipContent))), "not 64 lowercase hex digits"},
	} {
		if _, err := p.ReadInput(c.name, c.pin); err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("ReadInput(%q, %q): error %v, want %q", c.name, c.pin, err, c.want)
		}
	}
	if len(p.inputs) != 0 {
		t.Fatalf("pinned inputs %+v after refusals", p.inputs)
	}
}

// The flag is name=path, repeatable: a value with no '=', a name that is not an input name, and a name given twice exit 2
// before any step runs.
func TestTheInputFlagIsNameEqualsPathAndRefusesMalformedValues(t *testing.T) {
	f := newFixture(t)
	base := []string{"-game-dir", f.game, "-out", f.out("p"), "-id", "p", "-version", "1"}
	for _, bad := range [][]string{{"-input", "clip"}, {"-input", "clip="}, {"-input", "Clip=/x"}, {"-input", "clip=/x", "-input", "clip=/y"}} {
		code, _, stderr := f.run(testSteps, append(append([]string{}, base...), bad...)...)
		if code != 2 {
			t.Errorf("%v: exit %d (%s), want 2", bad, code, stderr)
		}
	}
}

// The flag's usage line shows the names it was given: String writes the map, so a name it holds appears in the text.
func TestTheInputFlagPrintsTheNamesItHolds(t *testing.T) {
	if s := (inputFlag{clipInput: "/some/clip.bin"}).String(); !strings.Contains(s, clipInput) || !strings.Contains(s, "/some/clip.bin") {
		t.Fatalf("String() = %q, want the name and its path", s)
	}
}

// A step reads a flagged input through Build: the summary lists it with its step and digest, the run log records input_pinned,
// the step gets the file's bytes, and without the flag the build refuses and writes nothing.
func TestAStepReadsAFlaggedInputThroughBuild(t *testing.T) {
	f := newFixture(t)
	clip := recipeInputFile(t, "clip.bin", []byte(clipContent))
	var got []byte
	list := []Step{{Name: "read the clip", Apply: func(p *Pack) error {
		b, err := p.ReadInput(clipInput, digest([]byte(clipContent)))
		if err != nil {
			return err
		}
		got = b
		return p.Put(Sets[0], 2, 9, b)
	}}}

	out := f.out("inputpack")
	code, stdout, stderr := f.run(list, "-game-dir", f.game, "-out", out, "-id", "inputpack", "-version", "1.0.0", "-input", clipInput+"="+clip)
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	if string(got) != clipContent {
		t.Fatalf("the step read %q, want %q", got, clipContent)
	}
	var res Result
	if err := json.Unmarshal([]byte(stdout), &res); err != nil {
		t.Fatalf("stdout %q: %v", stdout, err)
	}
	want := PinnedInput{Step: "read the clip", File: "clip.bin", Sha256: digest([]byte(clipContent))}
	if len(res.Inputs) != 1 || res.Inputs[0] != want {
		t.Fatalf("summary inputs %+v, want %+v", res.Inputs, want)
	}
	log, err := os.ReadFile(filepath.Join(f.env["XDG_STATE_HOME"], "nevr-runtime", "content-pack-build.jsonl"))
	if err != nil || !strings.Contains(string(log), `"event":"input_pinned"`) {
		t.Errorf("run log has no input_pinned record (%v): %s", err, log)
	}

	without := f.out("nopin")
	code, _, stderr = f.run(list, "-game-dir", f.game, "-out", without, "-id", "nopin", "-version", "1.0.0")
	if code != 1 || !strings.Contains(stderr, "-input clip=<file> is not set") {
		t.Errorf("without the flag: exit %d, stderr %q", code, stderr)
	}
	if _, err := os.Stat(without); !errors.Is(err, os.ErrNotExist) {
		t.Error("a refused build wrote its -out")
	}
}

// The real asset: when NEVR_DRONE4_GLB names Drone4.glb, ReadInput reads it with its measured sha-256 and the size is the
// measured one. Read only; the file is never written.
func TestDrone4GLBIsReadAsAPinnedInput(t *testing.T) {
	path := os.Getenv(drone4Env)
	if path == "" {
		t.Skipf("%s is not set (the path of Drone4.glb, an external asset that is not in the repository)", drone4Env)
	}
	st, err := os.Stat(path)
	if err != nil {
		t.Fatal(err)
	}
	if st.Size() != drone4Bytes {
		t.Fatalf("%s is %d bytes, want %d", path, st.Size(), drone4Bytes)
	}
	p := &Pack{inputFiles: map[string]string{"drone4": path}, step: "drone model"}
	b, err := p.ReadInput("drone4", drone4SHA256)
	if err != nil {
		t.Fatal(err)
	}
	if int64(len(b)) != drone4Bytes || !bytes.Equal(b[:4], []byte("glTF")) {
		t.Fatalf("read %d bytes, starting %q", len(b), b[:min(4, len(b))])
	}
}

package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const (
	nameAlpha  = "section01_first_speed_update"
	nameBeta   = "ambient.clothirradianceintensity"
	nameGamma  = "mpl_arena_a"
	namePinned = "0x%016x"
	filePerm   = 0o600
	shaLen     = 40
)

// corpusFile writes a corpus JSON whose keys are the real hashes of names.
func corpusFile(t *testing.T, dir, file string, names ...string) string {
	t.Helper()
	body := map[string]string{}
	for _, name := range names {
		body[fmt.Sprintf(namePinned, csymbol64(name))] = name
	}
	return rawFile(t, dir, file, mustJSON(t, body))
}

func mustJSON(t *testing.T, value any) string {
	t.Helper()
	data, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	return string(data)
}

func rawFile(t *testing.T, dir, file, body string) string {
	t.Helper()
	path := filepath.Join(dir, file)
	if err := os.WriteFile(path, []byte(body), filePerm); err != nil {
		t.Fatal(err)
	}
	return path
}

func call(args ...string) (string, string, error) {
	var out, errOut bytes.Buffer
	err := run(args, &out, &errOut)
	return out.String(), errOut.String(), err
}

func TestRunRequiresACommand(t *testing.T) {
	_, errOut, err := call()
	if err == nil || !strings.Contains(err.Error(), "command is required") {
		t.Fatalf("err = %v", err)
	}
	if !strings.Contains(errOut, "usage: symcache") {
		t.Fatalf("usage not printed: %q", errOut)
	}
}

func TestRunRejectsAnUnknownCommand(t *testing.T) {
	_, errOut, err := call("frobnicate")
	if err == nil || !strings.Contains(err.Error(), `unknown command "frobnicate"`) {
		t.Fatalf("err = %v", err)
	}
	if !strings.Contains(errOut, "usage: symcache") {
		t.Fatalf("usage not printed: %q", errOut)
	}
}

func TestRunDispatchesEachCommandToItsHandler(t *testing.T) {
	// Each handler's own first refusal proves the dispatch reached it.
	cases := map[string]string{
		"hash":     "hash: at least one name",
		"validate": "validate: --input is required",
		"diff":     "diff: --old and --new are required",
		"resolve":  "resolve: at least one hash",
	}
	for command, want := range cases {
		_, _, err := call(command)
		if err == nil || !strings.Contains(err.Error(), want) {
			t.Errorf("%s: err = %v, want %q", command, err, want)
		}
	}
}

func TestHashPrintsEachNameWithItsHash(t *testing.T) {
	out, _, err := call("hash", nameAlpha, nameBeta)
	if err != nil {
		t.Fatal(err)
	}
	want := fmt.Sprintf("0x%016x\t%s\n0x%016x\t%s\n", csymbol64(nameAlpha), nameAlpha, csymbol64(nameBeta), nameBeta)
	if out != want {
		t.Fatalf("out = %q, want %q", out, want)
	}
	// The anchor hash from the corpus tests ties this to the real algorithm.
	if !strings.Contains(out, "0x00027229b6c95edd\t"+nameAlpha) {
		t.Fatalf("hash of %s is not the known value: %q", nameAlpha, out)
	}
}

func generatedFile(t *testing.T, dir string, corpus Corpus) string {
	t.Helper()
	var buf bytes.Buffer
	if err := WriteGenerated(&buf, corpus, "src.json", strings.Repeat("a", shaLen)); err != nil {
		t.Fatal(err)
	}
	return rawFile(t, dir, "symcache_data.cpp", buf.String())
}

func TestResolveNamesKnownHashesAndMarksUnknownOnes(t *testing.T) {
	dir := t.TempDir()
	data := generatedFile(t, dir, Corpus{csymbol64(nameAlpha): nameAlpha})
	known := fmt.Sprintf("0x%016X", csymbol64(nameAlpha)) // upper case, with prefix
	bare := fmt.Sprintf("%x", csymbol64(nameAlpha))       // no prefix
	prefixed := "0X" + bare                               // an upper-case prefix is accepted too
	out, _, err := call("resolve", "--data", data, known, bare, prefixed, "0x1")
	if err != nil {
		t.Fatal(err)
	}
	line := fmt.Sprintf("0x%016x\t%s\n", csymbol64(nameAlpha), nameAlpha)
	want := line + line + line + "0x0000000000000001\t<unknown>\n"
	if out != want {
		t.Fatalf("out = %q, want %q", out, want)
	}
}

func TestResolveRefusesBadInput(t *testing.T) {
	dir := t.TempDir()
	data := generatedFile(t, dir, Corpus{csymbol64(nameAlpha): nameAlpha})
	if _, _, err := call("resolve", "--data", data, "0xzz"); err == nil || !strings.Contains(err.Error(), `resolve "0xzz"`) {
		t.Fatalf("bad hex: err = %v", err)
	}
	if _, _, err := call("resolve", "--data", filepath.Join(dir, "absent.cpp"), "0x1"); !os.IsNotExist(unwrapPath(err)) {
		t.Fatalf("missing table: err = %v", err)
	}
	broken := rawFile(t, dir, "broken.cpp", "not a generated table\n")
	if _, _, err := call("resolve", "--data", broken, "0x1"); err == nil {
		t.Fatal("an unparseable table was accepted")
	}
}

func unwrapPath(err error) error {
	if pe, ok := err.(*os.PathError); ok {
		return pe
	}
	return err
}

func TestValidateAcceptsAConsistentCorpus(t *testing.T) {
	dir := t.TempDir()
	in := corpusFile(t, dir, "pc.json", nameAlpha, nameBeta)
	out, _, err := call("validate", "--input", in)
	if err != nil {
		t.Fatal(err)
	}
	want := fmt.Sprintf("symcache: %s: 2 entries, all names hash to their keys\n", in)
	if out != want {
		t.Fatalf("out = %q, want %q", out, want)
	}
}

func TestValidateRefusals(t *testing.T) {
	dir := t.TempDir()
	good := corpusFile(t, dir, "pc.json", nameAlpha, nameBeta)
	wrong := rawFile(t, dir, "wrong.json", `{"0x0000000000000001": "not_that_hash"}`)
	if _, _, err := call("validate", "--input", wrong); err == nil || !strings.Contains(err.Error(), "validation") {
		t.Fatalf("mismatched hash: err = %v", err)
	}
	if _, _, err := call("validate", "--input", good, "--previous", "100"); err == nil || !strings.Contains(err.Error(), "20%") {
		t.Fatalf("count delta: err = %v", err)
	}
	if _, _, err := call("validate", "--input", good, "--previous", "2"); err != nil {
		t.Fatalf("an equal count was refused: %v", err)
	}
	if _, _, err := call("validate", "--input", filepath.Join(dir, "absent.json")); err == nil {
		t.Fatal("a missing input was accepted")
	}
	if _, _, err := call("validate", "--input", rawFile(t, dir, "bad.json", "{")); err == nil {
		t.Fatal("malformed JSON was accepted")
	}
}

func TestValidateMergesTheQuestSubset(t *testing.T) {
	dir := t.TempDir()
	pc := corpusFile(t, dir, "pc.json", nameAlpha, nameBeta)
	quest := corpusFile(t, dir, "quest.json", nameBeta, nameGamma)
	out, _, err := call("validate", "--input", pc, "--quest-input", quest)
	if err != nil {
		t.Fatal(err)
	}
	want := fmt.Sprintf("symcache: %s + %s: 3 entries, all names hash to their keys, 0 case-only collisions kept the PC name\n", pc, quest)
	if out != want {
		t.Fatalf("out = %q, want %q", out, want)
	}
	// The count check applies to the merged corpus, not to either half.
	if _, _, err := call("validate", "--input", pc, "--quest-input", quest, "--previous", "100"); err == nil || !strings.Contains(err.Error(), "20%") {
		t.Fatalf("merged count delta: err = %v", err)
	}
	// Each half is validated on its own.
	wrong := rawFile(t, dir, "wrong.json", `{"0x0000000000000001": "not_that_hash"}`)
	if _, _, err := call("validate", "--input", pc, "--quest-input", wrong); err == nil || !strings.Contains(err.Error(), "validation") {
		t.Fatalf("bad quest half: err = %v", err)
	}
	if _, _, err := call("validate", "--input", wrong, "--quest-input", quest); err == nil || !strings.Contains(err.Error(), "validation") {
		t.Fatalf("bad pc half: err = %v", err)
	}
	if _, _, err := call("validate", "--input", pc, "--quest-input", filepath.Join(dir, "absent.json")); err == nil {
		t.Fatal("a missing quest input was accepted")
	}
}

func TestDiffPrintsTheMarkdown(t *testing.T) {
	dir := t.TempDir()
	oldPath := corpusFile(t, dir, "old.json", nameAlpha, nameGamma)
	newPath := corpusFile(t, dir, "new.json", nameAlpha, nameBeta)
	out, _, err := call("diff", "--old", oldPath, "--new", newPath)
	if err != nil {
		t.Fatal(err)
	}
	oldCorpus, _ := loadCorpusFile(oldPath)
	newCorpus, _ := loadCorpusFile(newPath)
	if want := Diff(oldCorpus, newCorpus).Markdown(); out != want {
		t.Fatalf("out = %q, want %q", out, want)
	}
	if !strings.Contains(out, nameBeta) || !strings.Contains(out, nameGamma) {
		t.Fatalf("added and removed names missing: %q", out)
	}
}

func TestDiffRefusals(t *testing.T) {
	dir := t.TempDir()
	a := corpusFile(t, dir, "a.json", nameAlpha)
	if _, _, err := call("diff", "--old", a); err == nil || !strings.Contains(err.Error(), "--old and --new are required") {
		t.Fatalf("--new missing: err = %v", err)
	}
	if _, _, err := call("diff", "--new", a); err == nil || !strings.Contains(err.Error(), "--old and --new are required") {
		t.Fatalf("--old missing: err = %v", err)
	}
	if _, _, err := call("diff", "--old", filepath.Join(dir, "absent.json"), "--new", a); err == nil {
		t.Fatal("a missing old file was accepted")
	}
	if _, _, err := call("diff", "--old", a, "--new", filepath.Join(dir, "absent.json")); err == nil {
		t.Fatal("a missing new file was accepted")
	}
	for _, only := range [][]string{{"--old-quest", a}, {"--new-quest", a}} {
		args := append([]string{"diff", "--old", a, "--new", a}, only...)
		if _, _, err := call(args...); err == nil || !strings.Contains(err.Error(), "must be given together") {
			t.Fatalf("%v: err = %v", only, err)
		}
	}
}

func TestDiffMergesTheQuestSubsetOnBothSides(t *testing.T) {
	dir := t.TempDir()
	oldPC := corpusFile(t, dir, "old.json", nameAlpha)
	newPC := corpusFile(t, dir, "new.json", nameAlpha)
	oldQuest := corpusFile(t, dir, "oldq.json", nameGamma)
	newQuest := corpusFile(t, dir, "newq.json", nameGamma, nameBeta)
	out, _, err := call("diff", "--old", oldPC, "--new", newPC, "--old-quest", oldQuest, "--new-quest", newQuest)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out, nameBeta) {
		t.Fatalf("the quest-only addition is missing: %q", out)
	}
	if strings.Contains(out, nameGamma) {
		t.Fatalf("a name present in both merged sides was reported: %q", out)
	}
	// A quest side that conflicts with its PC side fails the merge.
	clash := rawFile(t, dir, "clash.json", fmt.Sprintf(`{"0x%016x": "other_name"}`, csymbol64(nameAlpha)))
	if _, _, err := call("diff", "--old", oldPC, "--new", newPC, "--old-quest", clash, "--new-quest", newQuest); err == nil {
		t.Fatal("a conflicting old quest side was merged")
	}
	if _, _, err := call("diff", "--old", oldPC, "--new", newPC, "--old-quest", oldQuest, "--new-quest", clash); err == nil {
		t.Fatal("a conflicting new quest side was merged")
	}
	if _, _, err := call("diff", "--old", oldPC, "--new", newPC, "--old-quest", filepath.Join(dir, "absent.json"), "--new-quest", newQuest); err == nil {
		t.Fatal("a missing old quest file was accepted")
	}
	if _, _, err := call("diff", "--old", oldPC, "--new", newPC, "--old-quest", oldQuest, "--new-quest", filepath.Join(dir, "absent.json")); err == nil {
		t.Fatal("a missing new quest file was accepted")
	}
}

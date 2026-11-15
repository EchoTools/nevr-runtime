package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/questapk"
	"github.com/EchoTools/nevr-runtime/tools/internal/questapkfix"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The Go twin of the Python CLI and recipe cases of tools/tests/test_quest_apk_rename_package.py (CliTest, LabelCliTest and
// JustfileRecipeTest). The tool runs in-process through run.

const (
	projectLabel    = "Echo VR"
	referenceLabel  = "Echo Combat (port)"
	libr15Count     = 5
	assetPatchCount = 2
	tooLongLabel    = "Echo Combat (port) and more"
	recipeName      = "quest-reference-base"
	rootDir         = "../../.."
	tagRename       = "NEVR.QUEST.RENAME"
	eventRefused    = "package_rename_refused"
	eventRenamed    = "package_renamed"
	libr15Name      = "lib/arm64-v8a/libr15.so"
	bodyLabelFlag   = `--label "{{ label }}"`
	recipeCall      = "build/go-tools/quest-apk-rename-package rename"
	goToolsDep      = "go-tools"
	fixtureLibr15   = 4
)

// runTool runs the tool in-process with a hermetic environment and returns its exit code and both streams.
func runTool(t *testing.T, args ...string) (int, string, string) {
	t.Helper()
	env := testkit.Env(t)
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), args, testkit.Getenv(env), nil, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

// lastRecord parses a stream's one JSON line.
func lastRecord(t *testing.T, text string) map[string]any {
	t.Helper()
	var got map[string]any
	if err := json.Unmarshal([]byte(strings.TrimSpace(text)), &got); err != nil {
		t.Fatalf("not one JSON line: %v\n%s", err, text)
	}
	return got
}

// fixturePaths is one test's input and output APK paths.
type fixturePaths struct{ src, out string }

// newPaths is a fresh input and output path in a temporary directory.
func newPaths(t *testing.T) fixturePaths {
	dir := t.TempDir()
	return fixturePaths{src: filepath.Join(dir, "in.apk"), out: filepath.Join(dir, "out.apk")}
}

// writeReference writes the reference fixture with the counts given and the manifest (nil: the reference's).
func (p fixturePaths) writeReference(t *testing.T, libr15, assetPatch int, manifest []byte) {
	t.Helper()
	questapkfix.Write(t, p.src, questapkfix.ReferenceEntries(libr15, assetPatch, manifest))
}

// refusedRecord asserts a refusal (exit 1, nothing on standard output, no output file) and returns the record.
func (p fixturePaths) refusedRecord(t *testing.T, args ...string) map[string]any {
	t.Helper()
	code, stdout, stderr := runTool(t, args...)
	if code != exitRefused || stdout != "" {
		t.Fatalf("exit %d, stdout %q, stderr %s", code, stdout, stderr)
	}
	if _, err := os.Stat(p.out); err == nil {
		t.Fatal("a refusal wrote the output")
	}
	if _, err := os.Stat(p.out + ".part"); err == nil {
		t.Fatal("a refusal left its partial file")
	}
	return lastRecord(t, stderr)
}

// TestRenameLogsOneJSONLine.
func TestRenameLogsOneJSONLine(t *testing.T) {
	t.Parallel()
	p := newPaths(t)
	p.writeReference(t, libr15Count, assetPatchCount, nil)
	code, stdout, stderr := runTool(t, "rename", "--apk", p.src, "--out", p.out)
	if code != 0 {
		t.Fatalf("exit %d, stderr %s", code, stderr)
	}
	if strings.Count(strings.TrimRight(stdout, "\n"), "\n") != 0 {
		t.Fatalf("more than one line on stdout: %q", stdout)
	}
	got := lastRecord(t, stdout)
	if got["tag"] != tagRename || got["event"] != eventRenamed {
		t.Fatalf("record = %v", got)
	}
	if _, err := os.Stat(p.out); err != nil {
		t.Fatal("no output written")
	}
}

// TestARefusalIsOneJSONLineOnStderrAndExit1.
func TestARefusalIsOneJSONLineOnStderrAndExit1(t *testing.T) {
	t.Parallel()
	p := newPaths(t)
	p.writeReference(t, fixtureLibr15, assetPatchCount, nil)
	got := p.refusedRecord(t, "rename", "--apk", p.src, "--out", p.out)
	if got["event"] != eventRefused || !strings.Contains(got["reason"].(string), libr15Name) {
		t.Fatalf("record = %v", got)
	}
}

// TestAMissingInputIsARefusalNotATraceback.
func TestAMissingInputIsARefusalNotATraceback(t *testing.T) {
	t.Parallel()
	p := newPaths(t)
	got := p.refusedRecord(t, "rename", "--apk", filepath.Join(filepath.Dir(p.src), "nope.apk"), "--out", p.out)
	if got["event"] != eventRefused {
		t.Fatalf("record = %v", got)
	}
}

// TestFromAndToDefaultToAfkAndR15: the defaults and the explicit names write the same APK.
func TestFromAndToDefaultToAfkAndR15(t *testing.T) {
	t.Parallel()
	p := newPaths(t)
	p.writeReference(t, libr15Count, assetPatchCount, nil)
	explicit := filepath.Join(filepath.Dir(p.src), "explicit.apk")
	if code, _, stderr := runTool(t, "rename", "--apk", p.src, "--out", p.out); code != 0 {
		t.Fatalf("default run: exit %d: %s", code, stderr)
	}
	if code, _, stderr := runTool(t, "rename", "--apk", p.src, "--out", explicit, "--from", questapk.DefaultFrom, "--to", questapk.DefaultTo); code != 0 {
		t.Fatalf("explicit run: exit %d: %s", code, stderr)
	}
	a, err := os.ReadFile(p.out)
	if err != nil {
		t.Fatal(err)
	}
	b, err := os.ReadFile(explicit)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(zipListing(t, p.out), zipListing(t, explicit)) || len(a) == 0 || len(b) == 0 {
		t.Fatal("the default and the explicit names wrote different APKs")
	}
}

// zipListing is the entries of an APK, with names, methods and bytes, for comparing two outputs.
func zipListing(t *testing.T, path string) []byte {
	t.Helper()
	order, _ := questapkfix.Read(t, path)
	var out bytes.Buffer
	for _, e := range order {
		out.WriteString(e.Name)
		out.Write([]byte{0})
		out.Write(e.Data)
		out.Write([]byte{0})
	}
	return out.Bytes()
}

// labelledReference writes the reference fixture with the reference build's label manifest.
func (p fixturePaths) labelledReference(t *testing.T) {
	t.Helper()
	p.writeReference(t, libr15Count, assetPatchCount, questapkfix.LabelledManifest(questapk.DefaultFrom, referenceLabel, false))
}

// TestLabelFlagSetsTheLabelAndTheRecordNamesBoth.
func TestLabelFlagSetsTheLabelAndTheRecordNamesBoth(t *testing.T) {
	t.Parallel()
	p := newPaths(t)
	p.labelledReference(t)
	code, stdout, stderr := runTool(t, "rename", "--apk", p.src, "--out", p.out, "--label", projectLabel)
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	got := lastRecord(t, stdout)
	if got["event"] != eventRenamed || got["label_from"] != referenceLabel || got["label_to"] != projectLabel {
		t.Fatalf("record = %v", got)
	}
	order, byName := questapkfix.Read(t, p.out)
	if len(order) == 0 {
		t.Fatal("empty output")
	}
	label, err := questapk.LabelOf(byName[questapkfix.ManifestName].Data)
	if err != nil || label != projectLabel {
		t.Fatalf("label = %q, %v", label, err)
	}
}

// TestNoLabelFlagLeavesTheLabel.
func TestNoLabelFlagLeavesTheLabel(t *testing.T) {
	t.Parallel()
	p := newPaths(t)
	p.labelledReference(t)
	code, stdout, stderr := runTool(t, "rename", "--apk", p.src, "--out", p.out)
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	if _, has := lastRecord(t, stdout)["label_to"]; has {
		t.Fatal("a label_to field without a label")
	}
	_, byName := questapkfix.Read(t, p.out)
	label, err := questapk.LabelOf(byName[questapkfix.ManifestName].Data)
	if err != nil || label != referenceLabel {
		t.Fatalf("label = %q, %v", label, err)
	}
}

// TestARefusedLabelIsOneJSONLineOnStderrAndExit1.
func TestARefusedLabelIsOneJSONLineOnStderrAndExit1(t *testing.T) {
	t.Parallel()
	p := newPaths(t)
	p.labelledReference(t)
	got := p.refusedRecord(t, "rename", "--apk", p.src, "--out", p.out, "--label", tooLongLabel)
	if got["event"] != eventRefused || !strings.Contains(got["reason"].(string), "only edits in place") {
		t.Fatalf("record = %v", got)
	}
	if _, err := os.Stat(p.out + ".part"); err == nil {
		t.Fatal("a partial file was left")
	}
}

// TestTheRecipeCallsTheRenameWithTheLabelAndDependsOnTheGoTools reads the justfile: the recipe runs the Go command, takes
// the label as its parameter and builds the tools first (ADR 0044 rule 7).
func TestTheRecipeCallsTheRenameWithTheLabelAndDependsOnTheGoTools(t *testing.T) {
	t.Parallel()
	root, err := filepath.Abs(rootDir)
	if err != nil {
		t.Fatal(err)
	}
	text, err := os.ReadFile(filepath.Join(root, "justfile"))
	if err != nil {
		t.Fatal(err)
	}
	found := regexp.MustCompile(`(?m)^(` + recipeName + `[^\n]*)\n((?:[ \t]+[^\n]*\n)+)`).FindStringSubmatch(string(text))
	if found == nil {
		t.Fatal("the justfile has no " + recipeName + " recipe")
	}
	header, body := found[1], found[2]
	if !strings.Contains(header, `label="Echo VR"`) || !strings.Contains(header, goToolsDep) {
		t.Fatalf("header = %s", header)
	}
	if !strings.Contains(body, recipeCall) || !strings.Contains(body, bodyLabelFlag) {
		t.Fatalf("body = %s", body)
	}
}

package main

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/questapk"
	"github.com/EchoTools/nevr-runtime/tools/internal/questapkfix"
	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// The label command has no Python test of its own (the Python label tool was tested through the rename): these cases
// cover its actions and its refusals directly. The package is the Go port of tools/quest_apk_label.py.

const (
	labelPackage   = "com.readyatdawn.r15"
	labelReference = "Echo Combat (port)"
	labelProject   = "Echo VR"
	labelTooLong   = "Echo Combat (port) and more"
	labelTagName   = "NEVR.QUEST.LABEL"
)

// runTool runs the tool in-process with a hermetic environment and returns its exit code and both streams.
func runTool(t *testing.T, args ...string) (int, string, string) {
	t.Helper()
	env := testkit.Env(t)
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), args, testkit.Getenv(env), nil, &stdout, &stderr)
	return code, stdout.String(), stderr.String()
}

// oneRecord parses a stream's one JSON line.
func oneRecord(t *testing.T, text string) map[string]any {
	t.Helper()
	var got map[string]any
	if err := json.Unmarshal([]byte(strings.TrimSpace(text)), &got); err != nil {
		t.Fatalf("not one JSON line: %v\n%s", err, text)
	}
	return got
}

// manifestFile writes the label fixture manifest to a temporary file and returns its path.
func manifestFile(t *testing.T) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), questapkfix.ManifestName)
	if err := os.WriteFile(path, questapkfix.LabelledManifest(labelPackage, labelReference, false), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

// TestShowPrintsTheLabel.
func TestShowPrintsTheLabel(t *testing.T) {
	t.Parallel()
	code, stdout, stderr := runTool(t, "show", manifestFile(t))
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	got := oneRecord(t, stdout)
	if got["tag"] != labelTagName || got["event"] != "label" || got["label"] != labelReference {
		t.Fatalf("record = %v", got)
	}
}

// TestSetWritesTheRelabelledManifestAndNamesBothLabels: the output is the input with its label replaced, nothing else.
func TestSetWritesTheRelabelledManifestAndNamesBothLabels(t *testing.T) {
	t.Parallel()
	in := manifestFile(t)
	out := filepath.Join(filepath.Dir(in), "out.xml")
	code, stdout, stderr := runTool(t, "set", in, out, "-label", labelProject)
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	got := oneRecord(t, stdout)
	if got["event"] != "label_set" || got["before"] != labelReference || got["after"] != labelProject || got["out"] != "out.xml" {
		t.Fatalf("record = %v", got)
	}
	written, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	source, err := os.ReadFile(in)
	if err != nil {
		t.Fatal(err)
	}
	want, err := questapk.SetLabel(source, labelProject)
	if err != nil || !bytes.Equal(written, want) {
		t.Fatalf("the output is not the input relabelled: %v", err)
	}
}

// TestSetRefusesALongerLabelAndWritesNothing.
func TestSetRefusesALongerLabelAndWritesNothing(t *testing.T) {
	t.Parallel()
	in := manifestFile(t)
	out := filepath.Join(filepath.Dir(in), "out.xml")
	code, stdout, stderr := runTool(t, "set", in, out, "-label", labelTooLong)
	if code != exitRefused || stdout != "" {
		t.Fatalf("exit %d, stdout %q", code, stdout)
	}
	got := oneRecord(t, stderr)
	if got["event"] != "label_refused" || !strings.Contains(got["reason"].(string), "only edits in place") {
		t.Fatalf("record = %v", got)
	}
	if _, err := os.Stat(out); err == nil {
		t.Fatal("a refusal wrote the output")
	}
}

// TestSetNeedsTheLabelFlag: a missing -label is a bad command line.
func TestSetNeedsTheLabelFlag(t *testing.T) {
	t.Parallel()
	in := manifestFile(t)
	if code, _, _ := runTool(t, "set", in, filepath.Join(filepath.Dir(in), "out.xml")); code != exitUsage {
		t.Fatalf("exit %d, want %d", code, exitUsage)
	}
}

// TestAMissingManifestIsARefusal.
func TestAMissingManifestIsARefusal(t *testing.T) {
	t.Parallel()
	code, _, stderr := runTool(t, "show", filepath.Join(t.TempDir(), "nope.xml"))
	if code != exitRefused || oneRecord(t, stderr)["event"] != "label_refused" {
		t.Fatalf("exit %d, stderr %s", code, stderr)
	}
}

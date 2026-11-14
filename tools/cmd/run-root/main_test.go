package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// call runs the command with the given environment and returns its exit code and both streams. The run log goes to a
// directory the test owns, never to the user's state directory.
func call(t *testing.T, env map[string]string, args ...string) (int, string, string) {
	t.Helper()
	full := map[string]string{"XDG_STATE_HOME": t.TempDir()}
	for k, v := range env {
		full[k] = v
	}
	var out, errb bytes.Buffer
	code := run(args, func(k string) string { return full[k] }, &out, &errb)
	return code, out.String(), errb.String()
}

func TestMakeThenRemoveRoundTripExitsZero(t *testing.T) {
	wr := t.TempDir()
	code, out, errs := call(t, nil, "-work-root", wr, "make", "round-trip")
	if code != 0 {
		t.Fatalf("make exit %d, stderr %q", code, errs)
	}
	p := strings.TrimSuffix(out, "\n")
	if !strings.HasPrefix(p, filepath.Join(wr, "run-roots", "round-trip-")) {
		t.Fatalf("make printed %q, not a run root under the work root", p)
	}
	if _, err := os.Stat(p); err != nil {
		t.Fatalf("printed run root does not exist: %v", err)
	}
	code, _, errs = call(t, nil, "-work-root", wr, "remove", p)
	if code != 0 {
		t.Fatalf("remove exit %d, stderr %q", code, errs)
	}
	if _, err := os.Lstat(p); !os.IsNotExist(err) {
		t.Fatalf("run root still there after remove: %v", err)
	}
}

// TestRemoveRefusesALinkAndTheTargetSurvives: a run root that is a symbolic link to a directory is refused with exit 1,
// the reason is printed, and the target is untouched.
func TestRemoveRefusesALinkAndTheTargetSurvives(t *testing.T) {
	wr := t.TempDir()
	target := t.TempDir()
	keep := filepath.Join(target, "keep.txt")
	if err := os.WriteFile(keep, []byte("keep"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(filepath.Join(wr, "run-roots"), 0o755); err != nil {
		t.Fatal(err)
	}
	link := filepath.Join(wr, "run-roots", "link-20261114T030405Z-1")
	if err := os.Symlink(target, link); err != nil {
		t.Fatal(err)
	}
	code, _, errs := call(t, nil, "-work-root", wr, "remove", link)
	if code != 1 {
		t.Fatalf("remove of a link: exit %d, want 1; stderr %q", code, errs)
	}
	if !strings.Contains(errs, "run-root") || !strings.Contains(errs, "symbolic link") {
		t.Errorf("the refusal must name the reason on stderr, got %q", errs)
	}
	if got, err := os.ReadFile(keep); err != nil || string(got) != "keep" {
		t.Fatalf("the link's target did not survive: %v", err)
	}
}

// TestTheRunLogRecordsEachInvocation: a deleter logs (run-copy's rule). The log is written beside the state directory the
// test gave, and a refusal is recorded with the sentinel's name.
func TestTheRunLogRecordsEachInvocation(t *testing.T) {
	wr := t.TempDir()
	state := t.TempDir()
	var out, errb bytes.Buffer
	getenv := func(k string) string {
		if k == "XDG_STATE_HOME" {
			return state
		}
		return ""
	}
	if code := run([]string{"-work-root", wr, "remove", filepath.Join(wr, "run-roots", "nope-20261114T030405Z-1")}, getenv, &out, &errb); code != 1 {
		t.Fatalf("exit %d, want 1; stderr %q", code, errb.String())
	}
	log, err := os.ReadFile(filepath.Join(state, "nevr-runtime", "run-root.jsonl"))
	if err != nil {
		t.Fatalf("no run log: %v", err)
	}
	if !strings.Contains(string(log), "ErrMissing") || !strings.Contains(string(log), `"invoked"`) {
		t.Errorf("run log lacks the refusal or the invocation: %s", log)
	}
}

func TestWorkRootComesFromTheEnvironmentWithoutTheFlag(t *testing.T) {
	wr := t.TempDir()
	code, out, errs := call(t, map[string]string{"NEVR_WORK_ROOT": wr}, "make", "envroot")
	if code != 0 {
		t.Fatalf("make exit %d, stderr %q", code, errs)
	}
	if !strings.HasPrefix(out, filepath.Join(wr, "run-roots", "envroot-")) {
		t.Fatalf("make printed %q, not under NEVR_WORK_ROOT", out)
	}
}

func TestExitCodes(t *testing.T) {
	wr := t.TempDir()
	cases := []struct {
		name string
		env  map[string]string
		args []string
		want int
	}{
		{"no arguments is usage", nil, nil, 2},
		{"unknown subcommand is usage", nil, []string{"bogus"}, 2},
		{"make without a label is usage", nil, []string{"-work-root", wr, "make"}, 2},
		{"remove without a path is usage", nil, []string{"-work-root", wr, "remove"}, 2},
		{"a flag it does not know is usage", nil, []string{"-nope", "make", "x"}, 2},
		{"no work root and no HOME is usage", map[string]string{}, []string{"make", "x"}, 2},
		{"a bad label is a refusal", nil, []string{"-work-root", wr, "make", "Bad Label"}, 1},
		{"removing a missing run root is a refusal", nil, []string{"-work-root", wr, "remove", filepath.Join(wr, "run-roots", "nope-20261114T030405Z-1")}, 1},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			code, _, errs := call(t, c.env, c.args...)
			if code != c.want {
				t.Fatalf("exit %d, want %d; stderr %q", code, c.want, errs)
			}
			if c.want == 1 && !strings.Contains(errs, "run-root") {
				t.Errorf("a refusal must name the reason on stderr, got %q", errs)
			}
		})
	}
}

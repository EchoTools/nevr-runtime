package main

import (
	"bytes"
	"context"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/gtestdeadline"
	"github.com/EchoTools/nevr-runtime/tools/internal/testshelf"
)

const (
	filePerm   = 0o644
	scriptPerm = 0o755
)

const shelfText = `{"schema":1,"about":[],"ceiling_s":1,"entries":[
 {"id":"gtest:test_a#S.Slow","reason":"r","measured":"m","date":"2026-11-16"},
 {"id":"gtest:test_a#S.AlsoSlow","reason":"r","measured":"m","date":"2026-11-16"}]}`

func shelf(t *testing.T) *testshelf.Shelf {
	t.Helper()
	s, err := testshelf.Parse([]byte(shelfText))
	if err != nil {
		t.Fatal(err)
	}
	return s
}

func TestTheShelvedTestsOfTheBinaryAreFilteredOut(t *testing.T) {
	prog := []string{"wine", "a.exe"}
	cases := []struct {
		binary, base string
		want         []string
	}{
		{"test_a", "", []string{"wine", "a.exe", "--gtest_filter=-S.AlsoSlow:S.Slow"}},
		{"test_a", "S.*", []string{"wine", "a.exe", "--gtest_filter=S.*-S.AlsoSlow:S.Slow"}},
		{"test_a", "S.*-S.x", []string{"wine", "a.exe", "--gtest_filter=S.*-S.x:S.AlsoSlow:S.Slow"}},
		{"test_b", "", prog},
		{"test_b", "S.*", prog},
	}
	for _, c := range cases {
		if got := Command(shelf(t), c.binary, c.base, prog); !reflect.DeepEqual(got, c.want) {
			t.Errorf("Command(%s, %q) = %q, want %q", c.binary, c.base, got, c.want)
		}
	}
}

func TestRunHoldsTheProgramsTestsToTheShelfsCeiling(t *testing.T) {
	dir := t.TempDir()
	shelfFile := filepath.Join(dir, "shelf.json")
	if err := os.WriteFile(shelfFile, []byte(shelfText), filePerm); err != nil {
		t.Fatal(err)
	}
	prog := filepath.Join(dir, "prog.sh")
	// The program echoes the filter it was given, then starts a test that outlasts the shelf's 1 s ceiling.
	script := "#!/bin/sh\necho \"args: $*\"\necho '[ RUN      ] S.Long'\nsleep 20\n"
	if err := os.WriteFile(prog, []byte(script), scriptPerm); err != nil {
		t.Fatal(err)
	}
	var out, errb bytes.Buffer
	code := run(context.Background(), []string{"-shelf", shelfFile, "-binary", "test_a", "--", prog}, os.Environ(), "", strings.NewReader(""), &out, &errb)
	if code != gtestdeadline.ExitOverCeiling {
		t.Errorf("exit %d, want %d; stderr %q", code, gtestdeadline.ExitOverCeiling, errb.String())
	}
	if !strings.Contains(out.String(), "--gtest_filter=-S.AlsoSlow:S.Slow") || !strings.Contains(errb.String(), "S.Long ran longer than 1s") {
		t.Errorf("stdout %q, stderr %q", out.String(), errb.String())
	}
}

func TestAMissingBinaryFlagOrShelfIsAUsageError(t *testing.T) {
	var out, errb bytes.Buffer
	if code := run(context.Background(), []string{"--", "x"}, nil, "", strings.NewReader(""), &out, &errb); code != exitUsage {
		t.Errorf("no -binary: exit %d", code)
	}
	if code := run(context.Background(), []string{"-shelf", filepath.Join(t.TempDir(), "none"), "-binary", "b", "--", "x"}, nil, "", strings.NewReader(""), &out, &errb); code != exitUsage {
		t.Errorf("no shelf: exit %d", code)
	}
}

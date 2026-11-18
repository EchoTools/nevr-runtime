package main

import (
	"bytes"
	"strings"
	"testing"
)

func notRunning() (bool, error) { return false, nil }

func getenv(string) string { return "" }

func TestUsageErrorsExitTwo(t *testing.T) {
	for _, args := range [][]string{nil, {"bogus"}, {"package"}, {"install", "-nope"}, {"restore", "-nope"}} {
		var out, errOut bytes.Buffer
		if code := run(args, getenv, notRunning, &out, &errOut); code != exitUsage {
			t.Errorf("run(%v) = %d, want %d (stderr %q)", args, code, exitUsage, errOut.String())
		}
	}
}

func TestInstallAndRestoreWithoutARootRefuse(t *testing.T) {
	for _, sub := range []string{"install", "restore"} {
		var out, errOut bytes.Buffer
		if code := run([]string{sub}, getenv, notRunning, &out, &errOut); code != exitFailed {
			t.Errorf("%s = %d, want %d", sub, code, exitFailed)
		}
		if !strings.Contains(errOut.String(), "ERROR: ") {
			t.Errorf("%s stderr = %q", sub, errOut.String())
		}
	}
}

func TestPackageOfAMissingBuildRefuses(t *testing.T) {
	var out, errOut bytes.Buffer
	code := run([]string{"package", "-build-dir", t.TempDir(), "-tool", "x"}, getenv, notRunning, &out, &errOut)
	if code != exitFailed || !strings.Contains(errOut.String(), "no CMakeCache.txt") {
		t.Fatalf("code %d stderr %q", code, errOut.String())
	}
}

func TestRestoreRefusesWhileTheGameRuns(t *testing.T) {
	var out, errOut bytes.Buffer
	running := func() (bool, error) { return true, nil }
	code := run([]string{"restore", "-root", t.TempDir()}, getenv, running, &out, &errOut)
	if code != exitFailed || !strings.Contains(errOut.String(), "echovr.exe is running") {
		t.Fatalf("code %d stderr %q", code, errOut.String())
	}
}

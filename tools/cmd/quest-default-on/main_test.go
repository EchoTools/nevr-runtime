package main

import (
	"archive/zip"
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/afkdefaulton"
)

const (
	shipped    = "../../quest-afk-default-on.txt"
	baseList   = "# qa-1104-aj4-w2z22\npfxwarm.on\necplayo.on\n"
	otherEntry = "lib/arm64-v8a/libr15.so"
)

// apkWith writes a zip with the given entries and returns its path.
func apkWith(t *testing.T, dir, name string, entries map[string]string) string {
	t.Helper()
	var buf bytes.Buffer
	zw := zip.NewWriter(&buf)
	for n, body := range entries {
		w, err := zw.Create(n)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := w.Write([]byte(body)); err != nil {
			t.Fatal(err)
		}
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, buf.Bytes(), filePerm); err != nil {
		t.Fatal(err)
	}
	return p
}

func runTool(args ...string) (int, string) {
	var out, errb bytes.Buffer
	code := execute(args, &out, &errb)
	return code, out.String() + errb.String()
}

// The reference build's APK holds the entry: the repository's list is staged, and the base's bytes are not.
func TestStageWritesTheRepositorysListNotTheBases(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	ref := apkWith(t, dir, "ref.apk", map[string]string{"assets/afk_default_on.txt": baseList, otherEntry: "x"})
	stage := filepath.Join(dir, "stage")
	code, out := runTool("stage", "-apk", ref, "-list", shipped, "-stage", stage)
	if code != 0 {
		t.Fatalf("exit %d: %s", code, out)
	}
	got, err := os.ReadFile(filepath.Join(stage, "assets", "afk_default_on.txt"))
	if err != nil {
		t.Fatal(err)
	}
	want, _ := os.ReadFile(shipped)
	names, err := afkdefaulton.Parse(string(got))
	if err != nil || !bytes.Equal(got, want) {
		t.Fatalf("staged %q (%v)", got, err)
	}
	for _, n := range names {
		if n == "pfxwarm.on" {
			t.Fatal("the base's pfxwarm.on was staged")
		}
	}
}

// The store APK has no entry; nothing is staged, and the arena build stays free of it.
func TestStageStagesNothingForAnApkWithoutTheEntry(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	store := apkWith(t, dir, "store.apk", map[string]string{otherEntry: "x"})
	stage := filepath.Join(dir, "stage")
	if code, out := runTool("stage", "-apk", store, "-list", shipped, "-stage", stage); code != 0 {
		t.Fatalf("exit %d: %s", code, out)
	}
	if _, err := os.Stat(filepath.Join(stage, "assets")); !os.IsNotExist(err) {
		t.Fatalf("something was staged: %v", err)
	}
}

// A list with a TEST-only switch is refused by both subcommands, naming the source line.
func TestAListWithATestOnlySwitchIsRefused(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.txt")
	if err := os.WriteFile(bad, []byte(baseList), filePerm); err != nil {
		t.Fatal(err)
	}
	ref := apkWith(t, dir, "ref.apk", map[string]string{"assets/afk_default_on.txt": baseList})
	code, out := runTool("stage", "-apk", ref, "-list", bad, "-stage", filepath.Join(dir, "stage"))
	if code != exitRefused || !strings.Contains(out, "pfxwarm.on is TEST only (runtime/native/afkxr/afk_pfx.cpp:31929") {
		t.Fatalf("exit %d: %s", code, out)
	}
	if _, err := os.Stat(filepath.Join(dir, "stage")); !os.IsNotExist(err) {
		t.Fatal("a refused list staged something")
	}
	code, out = runTool("check", "-apk", ref, "-source", ref, "-list", bad)
	if code != exitRefused {
		t.Fatalf("check exit %d: %s", code, out)
	}
}

func TestCheckAcceptsTheListAndRefusesTheBases(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	want, _ := os.ReadFile(shipped)
	ref := apkWith(t, dir, "ref.apk", map[string]string{"assets/afk_default_on.txt": baseList})
	good := apkWith(t, dir, "good.apk", map[string]string{"assets/afk_default_on.txt": string(want)})
	if code, out := runTool("check", "-apk", good, "-source", ref, "-list", shipped); code != 0 {
		t.Fatalf("exit %d: %s", code, out)
	}
	code, out := runTool("check", "-apk", ref, "-source", ref, "-list", shipped)
	if code != exitRefused || !strings.Contains(out, "is not the repository's list") {
		t.Fatalf("the base's list passed: exit %d: %s", code, out)
	}
	none := apkWith(t, dir, "none.apk", map[string]string{otherEntry: "x"})
	if code, _ := runTool("check", "-apk", none, "-source", ref, "-list", shipped); code != exitRefused {
		t.Fatalf("an APK that lost the entry passed: exit %d", code)
	}
	store := apkWith(t, dir, "store.apk", map[string]string{otherEntry: "x"})
	if code, out := runTool("check", "-apk", none, "-source", store, "-list", shipped); code != 0 {
		t.Fatalf("a source without the entry and a result without it: exit %d: %s", code, out)
	}
}

// The runtime ignores a list over its cap whole, so a list over it is refused here.
func TestAListOverTheRuntimesCapIsRefused(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	big := filepath.Join(dir, "big.txt")
	body := strings.Repeat("fxdecal.on\n", afkdefaulton.MaxBytes)
	if err := os.WriteFile(big, []byte(body), filePerm); err != nil {
		t.Fatal(err)
	}
	ref := apkWith(t, dir, "ref.apk", map[string]string{"assets/afk_default_on.txt": baseList})
	code, out := runTool("stage", "-apk", ref, "-list", big, "-stage", filepath.Join(dir, "stage"))
	if code != exitRefused || !strings.Contains(out, "the runtime ignores a list over") {
		t.Fatalf("exit %d: %s", code, out)
	}
}

func TestUsageErrors(t *testing.T) {
	t.Parallel()
	for _, args := range [][]string{{}, {"nope"}, {"stage"}, {"check", "-apk", "a", "-list", "b"}} {
		if code, _ := runTool(args...); code != exitUsage {
			t.Errorf("%v: exit %d, want %d", args, code, exitUsage)
		}
	}
}

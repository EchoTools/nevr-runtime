package main

import (
	"context"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const testSHA = "27fd4ce81e1fc64315dcb4cf935086b3f80c2d81"

func TestRegenerateFetchesAtThePinnedSHA(t *testing.T) {
	var gotPath string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotPath = r.URL.Path
		w.Write([]byte(validJSON))
	}))
	defer server.Close()

	dir := t.TempDir()
	out := filepath.Join(dir, "symcache_data.cpp")
	result, err := Regenerate(context.Background(), RegenerateOptions{
		Pin:     testSHA,
		BaseURL: server.URL,
		OutPath: out,
	})
	if err != nil {
		t.Fatalf("Regenerate: %v", err)
	}
	if gotPath != "/"+testSHA+"/subsets/echo_vr-pc.json" {
		t.Fatalf("fetched %q, want the pinned sha path", gotPath)
	}
	if result.Count != 2 {
		t.Fatalf("count = %d, want 2", result.Count)
	}
	data, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), testSHA) {
		t.Fatalf("output does not record the pin:\n%s", data)
	}
}

func TestRegenerateRejectsNonSHA(t *testing.T) {
	for _, pin := range []string{"", "deadbeef", strings.Repeat("g", 40), strings.Repeat("a", 39)} {
		if _, err := Regenerate(context.Background(), RegenerateOptions{Pin: pin, OutPath: filepath.Join(t.TempDir(), "x.cpp")}); err == nil {
			t.Fatalf("Regenerate accepted pin %q", pin)
		}
	}
}

func TestRegenerateRefusesBigCountChange(t *testing.T) {
	dir := t.TempDir()
	out := filepath.Join(dir, "symcache_data.cpp")
	opts := RegenerateOptions{Pin: testSHA, InputPath: writeFixture(t, validJSON), OutPath: out, PreviousCount: 100, SourceName: "echo_vr-pc.json"}
	if _, err := Regenerate(context.Background(), opts); err == nil {
		t.Fatal("Regenerate accepted 2 entries against a previous count of 100")
	}
	opts.AllowCountChange = true
	if _, err := Regenerate(context.Background(), opts); err != nil {
		t.Fatalf("AllowCountChange should permit it: %v", err)
	}
}

func TestRegenerateIfPinChangedSkipsSamePin(t *testing.T) {
	dir := t.TempDir()
	out := filepath.Join(dir, "symcache_data.cpp")
	opts := RegenerateOptions{Pin: testSHA, InputPath: writeFixture(t, validJSON), OutPath: out, SourceName: "echo_vr-pc.json"}
	if _, err := Regenerate(context.Background(), opts); err != nil {
		t.Fatal(err)
	}
	before, _ := os.ReadFile(out)
	opts.IfPinChanged = true
	result, err := Regenerate(context.Background(), opts)
	if err != nil {
		t.Fatal(err)
	}
	if !result.Skipped {
		t.Fatal("same pin should skip")
	}
	after, _ := os.ReadFile(out)
	if string(before) != string(after) {
		t.Fatal("skipped regeneration changed the file")
	}
}

func TestReadPin(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "symbols.pin")
	os.WriteFile(path, []byte(testSHA+"\n"), 0o644)
	got, err := ReadPin(path)
	if err != nil || got != testSHA {
		t.Fatalf("ReadPin = %q, %v", got, err)
	}
	os.WriteFile(path, []byte("# comment\n"), 0o644)
	if _, err := ReadPin(path); err == nil {
		t.Fatal("ReadPin accepted a comment-only pin")
	}
}

func writeFixture(t *testing.T, body string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "fixture.json")
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

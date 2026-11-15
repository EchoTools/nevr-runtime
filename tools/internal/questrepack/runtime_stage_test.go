package questrepack

import (
	"archive/zip"
	"os"
	"path/filepath"
	"testing"
)

// writeStoreAPK writes a zip with the named entries (each with one byte of content) and returns its path.
func writeStoreAPK(t *testing.T, dir string, entries ...string) string {
	t.Helper()
	path := filepath.Join(dir, "store.apk")
	f, err := os.Create(path)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	z := zip.NewWriter(f)
	for _, name := range entries {
		w, err := z.Create(name)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := w.Write([]byte{0x7f}); err != nil {
			t.Fatal(err)
		}
	}
	if err := z.Close(); err != nil {
		t.Fatal(err)
	}
	return path
}

// TestStageRuntimeStagesOnlyWhenTheStoreHoldsNoRuntime: a non-empty runtime is staged under lib/arm64-v8a when the store
// APK has no library of that name, and a store that holds one refuses it, writing nothing.
func TestStageRuntimeStagesOnlyWhenTheStoreHoldsNoRuntime(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	runtime := filepath.Join(dir, "runtime.so")
	if err := os.WriteFile(runtime, []byte("baseline"), 0o644); err != nil {
		t.Fatal(err)
	}
	stage := filepath.Join(dir, "stage")

	if problems := StageRuntime(runtime, stage, writeStoreAPK(t, dir, "lib/arm64-v8a/libr15.so")); len(problems) != 0 {
		t.Fatalf("staging refused: %v", problems)
	}
	got, err := os.ReadFile(filepath.Join(stage, abiDir, runtimeName))
	if err != nil || string(got) != "baseline" {
		t.Fatalf("staged runtime = %q, %v; want the baseline bytes", got, err)
	}

	clash := filepath.Join(dir, "clash")
	problems := StageRuntime(runtime, clash, writeStoreAPK(t, dir, "lib/arm64-v8a/"+runtimeName))
	if len(problems) == 0 {
		t.Fatal("a store that holds the runtime's name was not refused")
	}
	if _, err := os.Stat(filepath.Join(clash, abiDir, runtimeName)); !os.IsNotExist(err) {
		t.Fatalf("a refused stage wrote the runtime: %v", err)
	}
}

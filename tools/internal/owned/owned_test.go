package owned

import (
	"os"
	"path/filepath"
	"testing"
)

// fixture makes a canary outside the tree the tool removes from, and the tree itself.
func fixture(t *testing.T) (root, canary string) {
	t.Helper()
	base := t.TempDir()
	canary = filepath.Join(base, "canary")
	if err := os.Mkdir(canary, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(canary, "keep.txt"), []byte("canary\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	root = filepath.Join(base, "root")
	if err := os.Mkdir(root, 0o755); err != nil {
		t.Fatal(err)
	}
	return root, canary
}

func keptCanary(t *testing.T, canary string) {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(canary, "keep.txt"))
	if err != nil || string(b) != "canary\n" {
		t.Fatalf("canary changed: %q, %v", b, err)
	}
}

func TestParentLinkIsRefusedCanarySurvives(t *testing.T) {
	root, canary := fixture(t)
	if err := os.Symlink(canary, filepath.Join(root, "link")); err != nil {
		t.Fatal(err)
	}
	got := RemoveOwned(root, []string{"link/keep.txt"})
	if len(got) != 1 || got[0].Outcome != RefusedLink {
		t.Fatalf("want refused_link, got %+v", got)
	}
	keptCanary(t, canary)
}

func TestLastComponentLinkIsUnlinkedTargetSurvives(t *testing.T) {
	root, canary := fixture(t)
	if err := os.Symlink(filepath.Join(canary, "keep.txt"), filepath.Join(root, "ln")); err != nil {
		t.Fatal(err)
	}
	got := RemoveOwned(root, []string{"ln"})
	if len(got) != 1 || got[0].Outcome != Removed {
		t.Fatalf("want removed, got %+v", got)
	}
	keptCanary(t, canary)
}

func TestNamesThatAreNotPlainAreRefused(t *testing.T) {
	root, canary := fixture(t)
	for _, name := range []string{"", ".", "..", "a/../b", "/abs", "a\\b", "C:x", "a//b"} {
		got := RemoveOwned(root, []string{name})
		if len(got) != 1 || got[0].Outcome != RefusedName {
			t.Fatalf("name %q: want refused_name, got %+v", name, got)
		}
	}
	keptCanary(t, canary)
}

func TestFolderHoldingAnUnlistedFileIsKept(t *testing.T) {
	root, _ := fixture(t)
	item := filepath.Join(root, "item")
	if err := os.Mkdir(item, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(item, "listed.txt"), []byte("a"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(item, "unlisted.txt"), []byte("b"), 0o644); err != nil {
		t.Fatal(err)
	}
	got := RemoveOwned(root, []string{"item/listed.txt", "item"})
	if got[0].Outcome != Removed || got[1].Outcome != NotEmpty {
		t.Fatalf("want removed then not_empty, got %+v", got)
	}
	if _, err := os.Stat(filepath.Join(item, "unlisted.txt")); err != nil {
		t.Fatalf("unlisted file was removed: %v", err)
	}
}

func TestRemoveCheckedInsideRemovesOutsideRefuses(t *testing.T) {
	base := t.TempDir()
	build := filepath.Join(base, "build")
	if err := os.MkdirAll(filepath.Join(build, "preset", "sub"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := RemoveChecked(build, filepath.Join(build, "preset")); err != nil {
		t.Fatalf("inside: %v", err)
	}
	if _, err := os.Lstat(filepath.Join(build, "preset")); !os.IsNotExist(err) {
		t.Fatalf("tree still there: %v", err)
	}
	canary := filepath.Join(base, "canary")
	if err := os.Mkdir(canary, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := RemoveChecked(build, canary); err == nil {
		t.Fatal("outside: want refusal")
	}
	if err := RemoveChecked(build, build); err == nil {
		t.Fatal("the parent itself: want refusal")
	}
	if _, err := os.Stat(canary); err != nil {
		t.Fatalf("canary removed: %v", err)
	}
}

func TestRemoveCheckedLinkComponentRefusedCanarySurvives(t *testing.T) {
	base := t.TempDir()
	build := filepath.Join(base, "build")
	if err := os.Mkdir(build, 0o755); err != nil {
		t.Fatal(err)
	}
	canary := filepath.Join(base, "canary")
	if err := os.Mkdir(canary, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(canary, "keep.txt"), []byte("canary\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(canary, filepath.Join(build, "linked")); err != nil {
		t.Fatal(err)
	}
	if err := RemoveChecked(build, filepath.Join(build, "linked")); err == nil {
		t.Fatal("link component: want refusal")
	}
	keptCanary(t, canary)
}

func TestControlBytesAreRefused(t *testing.T) {
	root, canary := fixture(t)
	for _, name := range []string{"a\x00b", "a\x01b", "a\x7fb", "a/"} {
		if got := RemoveOwned(root, []string{name}); got[0].Outcome != RefusedName {
			t.Fatalf("name %q: want refused_name, got %+v", name, got)
		}
	}
	keptCanary(t, canary)
}

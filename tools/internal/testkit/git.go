package testkit

import (
	"os/exec"
	"path/filepath"
	"strings"
)

// Git runs git -C repo with args in env and returns its standard output. A failure fails the test with git's output.
func Git(t T, env []string, repo string, args ...string) string {
	t.Helper()
	cmd := exec.Command("git", append([]string{"-C", repo}, args...)...)
	cmd.Env = env
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("git -C %s %s: %v\n%s", repo, strings.Join(args, " "), err, out)
	}
	return string(out)
}

// GitRepo makes a throwaway repository below t.TempDir() with one empty commit and the given release tags, each a
// lightweight tag made by ref (a tag made by `git tag` opens an editor when tag.gpgSign is set), and returns its path.
func GitRepo(t T, tags ...string) string {
	t.Helper()
	env := Env(t)
	repo := filepath.Join(t.TempDir(), "repo")
	cmd := exec.Command("git", "init", "-q", repo)
	cmd.Env = env
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git init %s: %v\n%s", repo, err, out)
	}
	Git(t, env, repo, "commit", "-q", "--allow-empty", "-m", "root")
	for _, name := range tags {
		Git(t, env, repo, "update-ref", "refs/tags/"+name, "HEAD")
	}
	return repo
}

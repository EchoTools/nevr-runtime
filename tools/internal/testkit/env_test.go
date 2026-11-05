package testkit

import (
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
)

func lookup(env []string, key string) (string, bool) {
	value, ok := "", false
	for _, kv := range env {
		if k, v, found := strings.Cut(kv, "="); found && k == key {
			value, ok = v, true
		}
	}
	return value, ok
}

func TestEnvIsolatesGitFromUserAndSystemConfig(t *testing.T) {
	t.Parallel()
	env := Env(t)
	for key, want := range map[string]string{
		"GIT_CONFIG_GLOBAL":   "/dev/null",
		"GIT_CONFIG_NOSYSTEM": "1",
		"GIT_TERMINAL_PROMPT": "0",
	} {
		if got, ok := lookup(env, key); !ok || got != want {
			t.Errorf("%s = %q (set %v), want %q", key, got, ok, want)
		}
	}
}

func TestEnvSetsAFixedIdentityAndNoSigningThroughGitConfigCount(t *testing.T) {
	t.Parallel()
	env := Env(t)
	count, ok := lookup(env, "GIT_CONFIG_COUNT")
	if !ok {
		t.Fatal("GIT_CONFIG_COUNT is not set")
	}
	cfg := map[string]string{}
	for i := 0; ; i++ {
		k, kok := lookup(env, "GIT_CONFIG_KEY_"+strconv.Itoa(i))
		v, vok := lookup(env, "GIT_CONFIG_VALUE_"+strconv.Itoa(i))
		if !kok && !vok {
			if strconv.Itoa(i) != count {
				t.Errorf("GIT_CONFIG_COUNT = %s, but %d key/value pairs are set", count, i)
			}
			break
		}
		if kok != vok {
			t.Fatalf("pair %d: key set %v, value set %v", i, kok, vok)
		}
		cfg[k] = v
	}
	for key, want := range map[string]string{
		"user.name":          "Test User",
		"user.email":         "test@example.invalid",
		"commit.gpgsign":     "false",
		"tag.gpgsign":        "false",
		"init.defaultBranch": "main",
	} {
		if got, ok := cfg[key]; !ok || got != want {
			t.Errorf("git config %s = %q (set %v), want %q", key, got, ok, want)
		}
	}
}

func TestEnvPutsHomeStateAndTempUnderTheTestsDirectory(t *testing.T) {
	t.Parallel()
	tmp := t.TempDir()
	env := Env(&dirT{T: t, dir: tmp})
	for _, key := range []string{"HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "NEVR_WORK_ROOT", "TMPDIR"} {
		got, ok := lookup(env, key)
		if !ok {
			t.Errorf("%s is not set", key)
			continue
		}
		if rel, err := filepath.Rel(tmp, got); err != nil || strings.HasPrefix(rel, "..") || rel == "." {
			t.Errorf("%s = %q, want a directory below %q", key, got, tmp)
		}
		if fi, err := os.Stat(got); err != nil || !fi.IsDir() {
			t.Errorf("%s = %q does not exist as a directory (%v)", key, got, err)
		}
	}
	home, _ := lookup(env, "HOME")
	state, _ := lookup(env, "XDG_STATE_HOME")
	if home == state {
		t.Errorf("HOME and XDG_STATE_HOME are both %q; each is its own directory", home)
	}
}

// dirT makes Env use a known directory, so the test can compare against it.
type dirT struct {
	*testing.T
	dir string
}

func (d *dirT) TempDir() string { return d.dir }

func TestEnvSeparateCallsGetSeparateDirectories(t *testing.T) {
	t.Parallel()
	a, _ := lookup(Env(t), "HOME")
	b, _ := lookup(Env(t), "HOME")
	if a == b {
		t.Errorf("two Env calls share HOME %q", a)
	}
}

// Not parallel: it sets variables of the test process, which t.Setenv forbids with t.Parallel.
func TestEnvDropsTheCallersEnvironmentAndKeepsOnlyPATH(t *testing.T) {
	for key, value := range map[string]string{
		"DISPLAY":               ":0",
		"WAYLAND_DISPLAY":       "wayland-1",
		"GNUPGHOME":             "/nonexistent/gnupg",
		"GIT_DIR":               "/nonexistent/git",
		"GIT_WORK_TREE":         "/nonexistent/tree",
		"GIT_INDEX_FILE":        "/nonexistent/index",
		"GIT_CONFIG_PARAMETERS": "'user.signingkey=owner'",
		"GIT_AUTHOR_NAME":       "Owner",
		"GIT_COMMITTER_EMAIL":   "owner@example.invalid",
		"SSH_AUTH_SOCK":         "/nonexistent/agent",
		"HOME":                  "/nonexistent/home",
		"NEVR_TEST_LEAK":        "1",
	} {
		t.Setenv(key, value)
	}
	env := Env(t)
	for _, kv := range env {
		key, value, _ := strings.Cut(kv, "=")
		switch key {
		case "DISPLAY", "WAYLAND_DISPLAY", "GNUPGHOME", "GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE",
			"GIT_CONFIG_PARAMETERS", "GIT_AUTHOR_NAME", "GIT_COMMITTER_EMAIL", "SSH_AUTH_SOCK", "NEVR_TEST_LEAK":
			t.Errorf("%s=%s reached the environment Env made", key, value)
		case "HOME":
			if strings.HasPrefix(value, "/nonexistent") {
				t.Errorf("HOME is the caller's %q", value)
			}
		}
	}
	if got, _ := lookup(env, "PATH"); got != os.Getenv("PATH") || got == "" {
		t.Errorf("PATH = %q, want the caller's %q (a command is found through it)", got, os.Getenv("PATH"))
	}
}

func TestEnvAddsAndOverridesWithoutDuplicates(t *testing.T) {
	t.Parallel()
	env := Env(t, "FOO=bar", "GIT_CONFIG_NOSYSTEM=0", "PATH=/only/this")
	counts := map[string]int{}
	for _, kv := range env {
		k, _, _ := strings.Cut(kv, "=")
		counts[k]++
	}
	for k, n := range counts {
		if n != 1 {
			t.Errorf("%s appears %d times", k, n)
		}
	}
	for key, want := range map[string]string{"FOO": "bar", "GIT_CONFIG_NOSYSTEM": "0", "PATH": "/only/this"} {
		if got, _ := lookup(env, key); got != want {
			t.Errorf("%s = %q, want %q", key, got, want)
		}
	}
}

func TestEnvRejectsAnAdditionWithoutAnEqualsSign(t *testing.T) {
	t.Parallel()
	ft := newFakeT(t.Context(), t.TempDir())
	msg := fatalOf(func() { Env(ft, "NOEQUALS") })
	mustContain(t, "Fatalf message", msg, "NOEQUALS", "KEY=VALUE")
}

func TestWithPathPutsDirectoriesFirst(t *testing.T) {
	t.Parallel()
	env := WithPath([]string{"A=1", "PATH=/usr/bin"}, "/x", "/y")
	if got, _ := lookup(env, "PATH"); got != "/x:/y:/usr/bin" {
		t.Errorf("PATH = %q, want /x:/y:/usr/bin", got)
	}
	if got, _ := lookup(env, "A"); got != "1" {
		t.Errorf("A = %q, want it kept", got)
	}
	if got, _ := lookup(WithPath(nil, "/x"), "PATH"); got != "/x" {
		t.Errorf("PATH with no PATH before = %q, want /x", got)
	}
	if got, _ := lookup(WithPath([]string{"PATH="}, "/x"), "PATH"); got != "/x" {
		t.Errorf("PATH with an empty PATH before = %q, want /x with no empty entry after it", got)
	}
}

func TestGetenvReadsTheLastValueOfAKey(t *testing.T) {
	t.Parallel()
	get := Getenv([]string{"A=1", "B=2", "A=3", "C="})
	if get("A") != "3" || get("B") != "2" || get("C") != "" || get("MISSING") != "" {
		t.Errorf("Getenv gave A=%q B=%q C=%q MISSING=%q", get("A"), get("B"), get("C"), get("MISSING"))
	}
}

// The reason the package exists: a user whose config signs every commit and whose key is unavailable still gets a commit.
func TestGitCommitWithEnvSucceedsWithNoKeyAndNoUserConfig(t *testing.T) {
	t.Parallel()
	git, err := exec.LookPath("git")
	if err != nil {
		t.Fatalf("git is a dependency of the tool suite: %v", err)
	}
	hostileHome := t.TempDir()
	hostile := "[commit]\n\tgpgsign = true\n[gpg]\n\tprogram = /nonexistent/gpg\n[user]\n\tname = Owner\n\temail = owner@example.invalid\n"
	if err := os.WriteFile(filepath.Join(hostileHome, ".gitconfig"), []byte(hostile), 0o600); err != nil {
		t.Fatal(err)
	}
	repo := t.TempDir()
	run := func(env []string, args ...string) (string, error) {
		cmd := exec.CommandContext(t.Context(), git, args...)
		cmd.Dir = repo
		cmd.Env = env
		out, err := cmd.CombinedOutput()
		return string(out), err
	}
	// Control: with only the hostile HOME the commit fails, so the passing case below is not passing for another reason.
	ctrl := []string{"HOME=" + hostileHome, "PATH=" + os.Getenv("PATH")}
	if out, err := run(ctrl, "init", "-q"); err != nil {
		t.Fatalf("git init: %v\n%s", err, out)
	}
	if out, err := run(ctrl, "commit", "-q", "--allow-empty", "-m", "control"); err == nil {
		t.Fatalf("the control commit succeeded; the hostile config signs nothing, so this test proves nothing:\n%s", out)
	}
	env := Env(t, "HOME="+hostileHome) // even with the hostile HOME handed back, GIT_CONFIG_GLOBAL=/dev/null wins
	if out, err := run(env, "commit", "-q", "--allow-empty", "-m", "hermetic"); err != nil {
		t.Fatalf("git commit with testkit.Env failed: %v\n%s", err, out)
	}
	out, err := run(env, "log", "-1", "--format=%an <%ae>|%G?")
	if err != nil {
		t.Fatalf("git log: %v\n%s", err, out)
	}
	if got := strings.TrimSpace(out); got != "Test User <test@example.invalid>|N" {
		t.Errorf("the commit's author and signature status = %q, want the fixed identity and N (unsigned)", got)
	}
}

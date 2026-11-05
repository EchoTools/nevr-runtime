package testkit

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// gitConfig is the configuration every command gets through GIT_CONFIG_COUNT: a fixed identity, and no signing, so a
// commit needs neither the owner's key nor a gpg-agent. Order is the order of the GIT_CONFIG_KEY_n variables.
var gitConfig = [][2]string{
	{"user.name", "Test User"},
	{"user.email", "test@example.invalid"},
	{"commit.gpgsign", "false"},
	{"tag.gpgsign", "false"},
	{"init.defaultBranch", "main"},
}

// Env returns the environment for a command a test starts, as exec.Cmd.Env wants it. It starts from nothing: the
// caller's variables do not reach the command, except PATH, which a command is found through. It sets:
//
//   - GIT_CONFIG_GLOBAL=/dev/null, GIT_CONFIG_NOSYSTEM=1 and GIT_TERMINAL_PROMPT=0, and the identity and the
//     commit.gpgsign=false, tag.gpgsign=false and init.defaultBranch=main of gitConfig through GIT_CONFIG_COUNT;
//   - HOME, XDG_CONFIG_HOME, XDG_DATA_HOME, XDG_STATE_HOME, XDG_CACHE_HOME, NEVR_WORK_ROOT and TMPDIR, each an existing
//     directory of its own below t.TempDir().
//
// DISPLAY, WAYLAND_DISPLAY, GNUPGHOME, GIT_DIR, GIT_CONFIG_PARAMETERS, the author and committer variables and the rest
// are not set, so a test that needs one passes it in extra ("KEY=VALUE"; a key already present is replaced, so the
// result has each key once). Each call makes new directories.
func Env(t T, extra ...string) []string {
	t.Helper()
	root := filepath.Join(t.TempDir(), "env")
	dirs := [][2]string{
		{"HOME", "home"}, {"XDG_CONFIG_HOME", "config"}, {"XDG_DATA_HOME", "data"}, {"XDG_STATE_HOME", "state"},
		{"XDG_CACHE_HOME", "cache"}, {"NEVR_WORK_ROOT", "work"}, {"TMPDIR", "tmp"},
	}
	var e envList
	if path, ok := os.LookupEnv("PATH"); ok {
		e.set("PATH", path)
	}
	e.set("GIT_CONFIG_GLOBAL", "/dev/null")
	e.set("GIT_CONFIG_NOSYSTEM", "1")
	e.set("GIT_TERMINAL_PROMPT", "0")
	e.set("GIT_CONFIG_COUNT", strconv.Itoa(len(gitConfig)))
	for i, kv := range gitConfig {
		e.set("GIT_CONFIG_KEY_"+strconv.Itoa(i), kv[0])
		e.set("GIT_CONFIG_VALUE_"+strconv.Itoa(i), kv[1])
	}
	for _, d := range dirs {
		path := filepath.Join(root, d[1])
		if err := os.MkdirAll(path, 0o755); err != nil {
			t.Fatalf("testkit.Env: %v", err)
		}
		e.set(d[0], path)
	}
	for _, kv := range extra {
		key, value, ok := strings.Cut(kv, "=")
		if !ok || key == "" {
			t.Fatalf("testkit.Env: %q is not KEY=VALUE", kv)
		}
		e.set(key, value)
	}
	return e.list
}

// WithPath returns env with dirs put first on its PATH, in the order given (a PATH that env lacks becomes just dirs).
// env is not changed.
func WithPath(env []string, dirs ...string) []string {
	var e envList
	for _, kv := range env {
		if key, value, ok := strings.Cut(kv, "="); ok {
			e.set(key, value)
		}
	}
	path := strings.Join(dirs, string(os.PathListSeparator))
	if old, ok := e.get("PATH"); ok && old != "" {
		path += string(os.PathListSeparator) + old
	}
	e.set("PATH", path)
	return e.list
}

// Getenv returns the lookup a tool takes in place of os.Getenv: the value of a key in env, the last one when it
// appears twice, and "" when it is absent.
func Getenv(env []string) func(string) string {
	return func(key string) string {
		value := ""
		for _, kv := range env {
			if k, v, ok := strings.Cut(kv, "="); ok && k == key {
				value = v
			}
		}
		return value
	}
}

// envList is an environment that keeps each key once, in the order it was first set.
type envList struct{ list []string }

func (e *envList) set(key, value string) {
	for i, kv := range e.list {
		if k, _, _ := strings.Cut(kv, "="); k == key {
			e.list[i] = key + "=" + value
			return
		}
	}
	e.list = append(e.list, key+"="+value)
}

func (e *envList) get(key string) (string, bool) {
	for _, kv := range e.list {
		if k, v, _ := strings.Cut(kv, "="); k == key {
			return v, true
		}
	}
	return "", false
}

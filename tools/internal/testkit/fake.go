package testkit

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"testing"
)

// Fakes maps a command name to the Go function that plays that command.
type Fakes map[string]func(*Call) int

// Call is one run of a fake: what the command was started with, and the streams it was started on. The function
// returns the exit code.
type Call struct {
	Name    string   // the command name, as registered with Main
	Args    []string // the arguments, without the name
	Dir     string   // the working directory
	Environ []string // the whole environment, KEY=VALUE
	Stdin   io.Reader
	Stdout  io.Writer
	Stderr  io.Writer

	// FakeDir is the directory the fake was found in: the one Fake made, which a test may leave files in for the fake
	// to read (what it should print, what code to exit with). Exec does not search it.
	FakeDir string
}

// Record is one line of a fake's log, written before its function runs: the call, and the values the test asked to see
// of the environment (Fake's envKeys that were set).
type Record struct {
	Name string            `json:"name"`
	Args []string          `json:"args"`
	Dir  string            `json:"dir"`
	Env  map[string]string `json:"env,omitempty"`
}

var (
	registryMu sync.Mutex
	registry   Fakes
)

// Main belongs in a package's TestMain: `func TestMain(m *testing.M) { testkit.Main(m, fakes) }`. When the test binary
// was started under the name of one of fakes (a link made by Fake), it runs that fake and exits with its code, without
// starting the tests. Otherwise it records fakes as the names Fake accepts and runs the tests.
func Main(m *testing.M, fakes Fakes) {
	if fn, ok := fakes[filepath.Base(os.Args[0])]; ok {
		os.Exit(runFake(filepath.Base(os.Args[0]), fn))
	}
	registryMu.Lock()
	registry = fakes
	registryMu.Unlock()
	os.Exit(m.Run())
}

// Fake makes the command name available to the test: the test binary, linked under that name in a directory of its
// own, which Dir returns for the caller to put first on PATH (WithPath). name must be one of the fakes given to Main.
// envKeys are the environment variables whose values each call records, when set. The directory is under t.TempDir().
func Fake(t T, name string, envKeys ...string) *FakeCommand {
	t.Helper()
	if name == "" || name == "." || name == ".." || filepath.Base(name) != name {
		t.Fatalf("testkit.Fake: %q is not a plain command name", name)
	}
	registryMu.Lock()
	_, ok := registry[name]
	registryMu.Unlock()
	if !ok {
		t.Fatalf("testkit.Fake: no fake named %q was passed to testkit.Main in this package's TestMain", name)
	}
	exe, err := os.Executable()
	if err != nil {
		t.Fatalf("testkit.Fake: %v", err)
	}
	dir := filepath.Join(t.TempDir(), "fake-"+name)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatalf("testkit.Fake: %v", err)
	}
	if err := os.Symlink(exe, filepath.Join(dir, name)); err != nil {
		t.Fatalf("testkit.Fake: %v", err)
	}
	f := &FakeCommand{name: name, dir: dir, log: filepath.Join(dir, name+".calls.jsonl")}
	side, err := json.Marshal(sidecar{Log: f.log, Env: envKeys})
	if err != nil {
		t.Fatalf("testkit.Fake: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, name+sidecarSuffix), side, 0o644); err != nil {
		t.Fatalf("testkit.Fake: %v", err)
	}
	return f
}

// FakeCommand is a fake made by Fake.
type FakeCommand struct{ name, dir, log string }

// Dir is the directory that holds the command under its name; put it first on PATH, or run Dir()/name by path.
func (f *FakeCommand) Dir() string { return f.dir }

// LogPath is the file the calls are recorded in, one JSON Record per line.
func (f *FakeCommand) LogPath() string { return f.log }

// Calls returns the calls recorded so far, oldest first.
func (f *FakeCommand) Calls(t T) []Record {
	t.Helper()
	raw, err := os.ReadFile(f.log)
	if errors.Is(err, fs.ErrNotExist) {
		return nil
	}
	if err != nil {
		t.Fatalf("testkit: read the calls of %s: %v", f.name, err)
	}
	var calls []Record
	sc := bufio.NewScanner(bytes.NewReader(raw))
	sc.Buffer(nil, 1<<20)
	for sc.Scan() {
		var r Record
		if err := json.Unmarshal(sc.Bytes(), &r); err != nil {
			t.Fatalf("testkit: a call of %s is not a Record: %v: %q", f.name, err, sc.Text())
		}
		calls = append(calls, r)
	}
	if err := sc.Err(); err != nil {
		t.Fatalf("testkit: read the calls of %s: %v", f.name, err)
	}
	return calls
}

// Exec replaces the fake's process with the real program of that name: the first executable file called program on the
// fake's PATH outside the directory the fake was found in (a program with a slash in its name is used as it is). The
// arguments are args, and the environment is the fake's own. It returns only when it could not, with the exit code 127
// and a line on standard error.
func (c *Call) Exec(program string, args ...string) int {
	path, err := lookPath(program, c.Environ, c.FakeDir)
	if err == nil {
		err = syscall.Exec(path, append([]string{program}, args...), c.Environ)
	}
	fmt.Fprintf(c.Stderr, "testkit: fake %s cannot replace itself with %s: %v\n", c.Name, program, err)
	return 127
}

func lookPath(program string, environ []string, skipDir string) (string, error) {
	if strings.ContainsRune(program, '/') {
		return program, nil
	}
	skip, _ := filepath.Abs(skipDir)
	var path string
	for _, kv := range environ {
		if v, ok := strings.CutPrefix(kv, "PATH="); ok {
			path = v
		}
	}
	for _, dir := range filepath.SplitList(path) {
		if dir == "" {
			continue
		}
		if abs, err := filepath.Abs(dir); err == nil && abs == skip {
			continue
		}
		candidate := filepath.Join(dir, program)
		if fi, err := os.Stat(candidate); err == nil && fi.Mode().IsRegular() && fi.Mode().Perm()&0o111 != 0 {
			return candidate, nil
		}
	}
	return "", fmt.Errorf("no executable %q on PATH outside %s", program, skipDir)
}

const sidecarSuffix = ".fake.json"

// sidecar sits beside the link and tells the fake process where to record and which environment keys to record.
type sidecar struct {
	Log string   `json:"log"`
	Env []string `json:"env"`
}

// findSidecar locates the description Fake wrote: beside argv[0] when it has a directory part, else in the first
// directory of PATH that holds one for name (the directory a shell would have found the link in).
func findSidecar(name string) (sidecar, string, error) {
	var dirs []string
	if strings.ContainsRune(os.Args[0], '/') {
		dirs = []string{filepath.Dir(os.Args[0])}
	} else {
		dirs = filepath.SplitList(os.Getenv("PATH"))
	}
	for _, dir := range dirs {
		if dir == "" {
			continue
		}
		raw, err := os.ReadFile(filepath.Join(dir, name+sidecarSuffix))
		if err != nil {
			continue
		}
		var s sidecar
		if err := json.Unmarshal(raw, &s); err != nil {
			return sidecar{}, "", fmt.Errorf("%s: %w", filepath.Join(dir, name+sidecarSuffix), err)
		}
		return s, dir, nil
	}
	return sidecar{}, "", fmt.Errorf("no %s%s beside it or on PATH", name, sidecarSuffix)
}

// exitFakeSetup is the exit code of a fake that was not set up by Fake, or whose call could not be recorded.
const exitFakeSetup = 125

func runFake(name string, fn func(*Call) int) int {
	side, dir, err := findSidecar(name)
	if err != nil {
		fmt.Fprintf(os.Stderr, "testkit: the test binary was started as the fake %q without testkit.Fake setting it up: %v\n", name, err)
		return exitFakeSetup
	}
	wd, err := os.Getwd()
	if err != nil {
		fmt.Fprintf(os.Stderr, "testkit: fake %s: %v\n", name, err)
		return exitFakeSetup
	}
	args := append([]string{}, os.Args[1:]...)
	rec := Record{Name: name, Args: args, Dir: wd}
	for _, key := range side.Env {
		if v, ok := os.LookupEnv(key); ok {
			if rec.Env == nil {
				rec.Env = map[string]string{}
			}
			rec.Env[key] = v
		}
	}
	line, err := json.Marshal(rec)
	if err == nil {
		err = appendLine(side.Log, append(line, '\n'))
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "testkit: fake %s cannot record its call: %v\n", name, err)
		return exitFakeSetup
	}
	return fn(&Call{Name: name, Args: args, Dir: wd, Environ: os.Environ(), Stdin: os.Stdin, Stdout: os.Stdout, Stderr: os.Stderr, FakeDir: dir})
}

// appendLine writes line to path with one write on an append-mode file, so calls made at once do not interleave.
func appendLine(path string, line []byte) error {
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_APPEND|os.O_CREATE, 0o644)
	if err != nil {
		return err
	}
	if _, err := f.Write(line); err != nil {
		f.Close()
		return err
	}
	return f.Close()
}

package main

import (
	"bytes"
	"encoding/base64"
	"fmt"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// Fixture values. The keystore is made per test, with the real keytool; the real Quest key is never touched.
const (
	passphrase    = "fixture-pass-7f3a91"
	wrongPass     = "not-the-passphrase-41c9"
	keySizeBits   = "2048"
	validityDays  = "30"
	fixtureDName  = "CN=Quest Keystore Test, O=test"
	fingerprintOK = "SHA-256"
	sha256Marker  = "SHA256"
	sleepSeconds  = "30"
	exitSeven     = 7
	keyBlobChars  = 40
	toolFakeName  = "quest-keystore"
	leakyFakeName = "leaky-quest-keystore"
	leakyEnv      = "LEAK"
	leakyReadyEnv = "LEAK_READY"
	leakyPIDEnv   = "LEAK_PIDFILE"
	leakArgv      = "argv"
	leakChild     = "child"
	leakLate      = "late"
	leakIgnore    = "ignore"
	sigExitBase   = 128
	pollStep      = 5 * time.Millisecond
	holdBound     = 10 * time.Second
	stopBound     = 20 * time.Second
	ignoreBound   = time.Second
	pidFileMode   = 0o644
	procDir       = "/proc"
	procStatState = 2 // the state letter follows ") " in /proc/<pid>/stat
	procPPIDField = 1 // the parent pid is the second field after ") "
	waitidOp      = 247
	waitidPID     = 1
	waitidExited  = 4
	waitidNoHang  = 1
	waitidNoWait  = 0x1000000
	sigInt        = syscall.SIGINT
	sigTerm       = syscall.SIGTERM
	sigHup        = syscall.SIGHUP
)

// fixture is one test's world: a keystore made with the real keytool, the fakes keytool and shred on PATH, and a
// runtime directory the tool copies the key into.
type fixture struct {
	t        *testing.T
	env      []string
	keystore string
	fp       string
	runtime  string
	keytool  *testkit.FakeCommand
	shred    *testkit.FakeCommand
}

// newFixture makes the keystore and the environment for one test. The keytool and shred fakes are first on PATH.
func newFixture(t *testing.T) *fixture {
	t.Helper()
	ks := filepath.Join(t.TempDir(), "fixture.jks")
	runCmd(t, "keytool", "-genkeypair", "-keystore", ks, "-alias", alias, "-keyalg", "RSA", "-keysize", keySizeBits,
		"-validity", validityDays, "-storepass", passphrase, "-keypass", passphrase, "-dname", fixtureDName)
	runtime := filepath.Join(t.TempDir(), "run")
	if err := os.Mkdir(runtime, keyDirMode); err != nil {
		t.Fatal(err)
	}
	keytool := testkit.Fake(t, keytoolName, envKSPass)
	shred := testkit.Fake(t, shredName)
	base := testkit.Env(t, envKeystore+"="+ks, envPass+"="+passphrase, envRuntimeDir+"="+runtime)
	return &fixture{
		t:        t,
		env:      testkit.WithPath(base, keytool.Dir(), shred.Dir()),
		keystore: ks,
		fp:       fingerprintOf(t, ks, passphrase),
		runtime:  runtime,
		keytool:  keytool,
		shred:    shred,
	}
}

// result is one in-process run: the exit code and what it wrote.
type result struct {
	code           int
	stdout, stderr string
}

// run calls the tool in this process with env, and returns what it did.
func (f *fixture) run(env []string, args ...string) result {
	f.t.Helper()
	var out, errb bytes.Buffer
	code := run(args, env, &out, &errb)
	return result{code: code, stdout: out.String(), stderr: errb.String()}
}

// runCmd runs a real program with the test's own environment (the keystore is made before any fake is on PATH).
func runCmd(t *testing.T, name string, args ...string) []byte {
	t.Helper()
	path, err := exec.LookPath(name)
	if err != nil {
		t.Skipf("%s is not on PATH: %v", name, err)
	}
	out, err := exec.Command(path, args...).CombinedOutput()
	if err != nil {
		t.Fatalf("%s %v: %v\n%s", name, redact(args), err, out)
	}
	return out
}

// redact hides the passphrase in an argument list a failure message would print.
func redact(args []string) []string {
	out := make([]string, len(args))
	for i, a := range args {
		out[i] = strings.ReplaceAll(a, passphrase, "<passphrase>")
	}
	return out
}

// fingerprintOf reads the signer's SHA-256 the way the fixture needs it: keytool's SHA-256 line, colons removed, lower case.
func fingerprintOf(t *testing.T, ks, pass string) string {
	t.Helper()
	out := runCmd(t, "keytool", "-list", "-keystore", ks, "-alias", alias, "-storepass", pass)
	for _, line := range strings.Split(string(out), "\n") {
		if strings.Contains(line, fingerprintOK) || strings.Contains(line, sha256Marker) {
			value := line[strings.Index(line, ":")+1:]
			return strings.ToLower(strings.NewReplacer(":", "", " ", "", "\t", "").Replace(value))
		}
	}
	t.Fatalf("keytool printed no SHA-256 line: %s", out)
	return ""
}

// without returns env with the keys removed.
func without(env []string, keys ...string) []string {
	drop := map[string]bool{}
	for _, k := range keys {
		drop[k] = true
	}
	var out []string
	for _, kv := range env {
		if k, _, ok := strings.Cut(kv, "="); !ok || !drop[k] {
			out = append(out, kv)
		}
	}
	return out
}

// with returns env with KEY=VALUE pairs set (replacing any old value).
func with(env []string, kv ...string) []string {
	for _, pair := range kv {
		k, v, _ := strings.Cut(pair, "=")
		env = setEnv(env, k, v)
	}
	return env
}

// leftover lists what is under dir now: a run that ends must leave nothing.
func leftover(t *testing.T, dir string) []string {
	t.Helper()
	var out []string
	err := filepath.WalkDir(dir, func(p string, _ fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if p != dir {
			out = append(out, p)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return out
}

// keyFiles lists the regular files under dir: the keystore copies a run has made.
func keyFiles(dir string) []string {
	var out []string
	_ = filepath.WalkDir(dir, func(p string, d fs.DirEntry, err error) error {
		if err == nil && d.Type().IsRegular() {
			out = append(out, p)
		}
		return nil
	})
	return out
}

// shredded reports whether the fake shred was called with -u on path (the one call that removes the key).
func (f *fixture) shredded(path string) bool {
	for _, c := range f.shred.Calls(f.t) {
		hasU := false
		for _, a := range c.Args {
			if a == "-u" {
				hasU = true
			}
		}
		if hasU && containsString(c.Args, path) {
			return true
		}
	}
	return false
}

func containsString(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}

// keyBlob is the base64 of the keystore's bytes: its first keyBlobChars must never reach a log or an output.
func (f *fixture) keyBlob() string {
	raw, err := os.ReadFile(f.keystore)
	if err != nil {
		f.t.Fatal(err)
	}
	return base64.StdEncoding.EncodeToString(raw)
}

// processInfo is one line of /proc: its pid, its parent and its command line.
type processInfo struct {
	pid, ppid int
	argv      []string
}

// processTable reads every process this user can read.
func processTable() map[int]processInfo {
	table := map[int]processInfo{}
	entries, err := os.ReadDir(procDir)
	if err != nil {
		return table
	}
	for _, e := range entries {
		pid, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		stat, err := os.ReadFile(filepath.Join(procDir, e.Name(), "stat"))
		if err != nil {
			continue
		}
		raw, err := os.ReadFile(filepath.Join(procDir, e.Name(), "cmdline"))
		if err != nil {
			continue
		}
		fields := strings.Fields(string(stat[strings.LastIndex(string(stat), ")")+procStatState:]))
		ppid := 0
		if len(fields) > procPPIDField {
			ppid, _ = strconv.Atoi(fields[procPPIDField])
		}
		var argv []string
		for _, part := range strings.Split(string(raw), "\x00") {
			if part != "" {
				argv = append(argv, part)
			}
		}
		table[pid] = processInfo{pid: pid, ppid: ppid, argv: argv}
	}
	return table
}

// processTree is root and every process below it right now.
func processTree(root int) []processInfo {
	table := processTable()
	var members []processInfo
	queue := []int{root}
	for len(queue) > 0 {
		pid := queue[len(queue)-1]
		queue = queue[:len(queue)-1]
		if p, ok := table[pid]; ok {
			members = append(members, p)
		}
		for _, p := range table {
			if p.ppid == pid {
				queue = append(queue, p.pid)
			}
		}
	}
	return members
}

// alive is true while pid is running or stopped: a zombie, or a pid that is gone, is not.
func alive(pid int) bool {
	stat, err := os.ReadFile(filepath.Join(procDir, strconv.Itoa(pid), "stat"))
	if err != nil {
		return false
	}
	s := string(stat)
	state := s[strings.LastIndex(s, ")")+procStatState]
	return state != 'Z' && state != 'X'
}

// exitedUnreaped blocks until pid has exited without reaping it (waitid with WNOWAIT), so its pid still names it.
func exitedUnreaped(t *testing.T, pid int) {
	t.Helper()
	var info [siginfoSize]byte
	if _, _, errno := syscall.Syscall6(waitidOp, waitidPID, uintptr(pid), uintptr(unsafe.Pointer(&info[0])), waitidExited|waitidNoWait, 0, 0); errno != 0 {
		t.Fatalf("waitid %d: %v", pid, errno)
	}
}

// siginfoSize is the size of the kernel's siginfo_t on Linux.
const siginfoSize = 128

// minTreeSize is the fewest processes a tool's tree has while its wrapped command runs: the tool and that command.
const minTreeSize = 2

// session is a tool run in a session of its own, so its process group is its pid and everything it starts is in it.
type session struct {
	t    *testing.T
	cmd  *exec.Cmd
	done chan error
	log  string
}

// startSession starts path with args and env, in a session of its own, with its output in a file under t.TempDir().
func startSession(t *testing.T, path string, args []string, env []string) *session {
	t.Helper()
	logPath := filepath.Join(t.TempDir(), "session.log")
	out, err := os.Create(logPath)
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(path, args...)
	cmd.Env = env
	cmd.Stdout = out
	cmd.Stderr = out
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if err := cmd.Start(); err != nil {
		t.Fatalf("start %s: %v", path, err)
	}
	out.Close()
	s := &session{t: t, cmd: cmd, done: make(chan error, 1), log: logPath}
	go func() { s.done <- cmd.Wait() }()
	t.Cleanup(func() { killGroup(cmd.Process.Pid) })
	return s
}

// killGroup sends SIGKILL to every process of the group led by pid; a group with no members is not an error.
func killGroup(pid int) {
	_ = syscall.Kill(-pid, syscall.SIGKILL)
}

// stop sends SIGTERM to the tool alone, waits for it to exit within bound, then stops its whole group. A tool still
// running after bound is an error that names the wait and carries the tool's log.
func (s *session) stop(bound time.Duration) error {
	_ = s.cmd.Process.Signal(sigTerm)
	ctx := testkit.Within(s.t, bound).Context()
	select {
	case <-s.done:
		killGroup(s.cmd.Process.Pid)
		return nil
	case <-ctx.Done():
		killGroup(s.cmd.Process.Pid)
		<-s.done
		logText, _ := os.ReadFile(s.log)
		return fmt.Errorf("the tool (pid %d) did not exit within %v of SIGTERM; its log: %q", s.cmd.Process.Pid, bound, logText)
	}
}

// passphraseHolders runs the tool at path with args in a session, looks at every process of its tree while the
// command it wraps runs, and returns the processes and keytool calls that carry the passphrase. after runs once the
// look is over, before the stop, so a test can start a process the look did not see.
func (f *fixture) passphraseHolders(path string, args []string, env []string, after func()) []string {
	f.t.Helper()
	s := startSession(f.t, path, args, env)
	testkit.WaitFor(f.t, "the wrapped command to start", func() bool {
		return hasArgv0(processTree(s.cmd.Process.Pid), "sleep")
	})
	tree := processTree(s.cmd.Process.Pid)
	if len(tree) < minTreeSize {
		f.t.Fatalf("the scan saw %v", tree)
	}
	var found []string
	for _, p := range tree {
		if strings.Contains(strings.Join(p.argv, " "), passphrase) {
			found = append(found, fmt.Sprintf("%d %d %s", p.pid, p.ppid, strings.Join(p.argv, " ")))
		}
	}
	if after != nil {
		after()
	}
	if err := s.stop(stopBound); err != nil {
		f.t.Fatal(err)
	}
	calls := f.keytool.Calls(f.t)
	if len(calls) == 0 {
		f.t.Fatal("the tool never called keytool, so there was nothing to check")
	}
	for _, c := range calls {
		if line := strings.Join(c.Args, " "); strings.Contains(line, passphrase) {
			found = append(found, "keytool "+line)
		}
	}
	return found
}

// waitFileExists polls for path until bound; the leaky fakes use it to wait on a file the test writes.
func waitFileExists(path string, bound time.Duration) bool {
	tick := time.NewTicker(pollStep)
	defer tick.Stop()
	deadline := time.NewTimer(bound)
	defer deadline.Stop()
	for {
		if _, err := os.Stat(path); err == nil {
			return true
		}
		select {
		case <-tick.C:
		case <-deadline.C:
			return false
		}
	}
}

// hasArgv0 reports whether a process of tree runs the program name.
func hasArgv0(tree []processInfo, name string) bool {
	for _, p := range tree {
		if len(p.argv) > 0 && p.argv[0] == name {
			return true
		}
	}
	return false
}

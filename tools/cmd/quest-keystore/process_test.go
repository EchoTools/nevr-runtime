package main

import (
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// fakeKeytool plays keytool for the tool under test: it records each call (testkit), then runs the real keytool.
func fakeKeytool(c *testkit.Call) int { return c.Exec(keytoolName, c.Args...) }

// fakeShred plays shred: it records each call, then runs the real shred.
func fakeShred(c *testkit.Call) int { return c.Exec(shredName, c.Args...) }

// fakeTool is the tool itself, linked under its name: the process boundary the signal cases start.
func fakeTool(c *testkit.Call) int {
	return run(c.Args, c.Environ, c.Stdout, c.Stderr)
}

// fakeLeaky is a tool that leaks the passphrase in one of the ways the holder check must catch (LEAK selects it),
// then runs its wrapped command, sleep, as the tool does. It has the shape of the tool's own log line, so a test can
// read its stderr.
func fakeLeaky(c *testkit.Call) int {
	fmt.Fprintf(c.Stderr, "%skeystore ready alias=%s\n", logPrefix, alias)
	if envIn(c.Environ, leakyEnv) == leakIgnore {
		signal.Ignore(sigTerm) // before anything else: the test sends SIGTERM once the wrapped command is up
	}
	pass := envIn(c.Environ, envPass)
	// The open check every run makes, with the passphrase in the environment as the tool passes it.
	open := exec.Command(keytoolName, "-list", "-keystore", "/dev/null", "-alias", alias, "-storepass:env", envKSPass)
	open.Env = setEnv(c.Environ, envKSPass, pass)
	_ = open.Run()
	switch envIn(c.Environ, leakyEnv) {
	case leakArgv:
		kt := exec.Command(keytoolName, "-list", "-keystore", "/dev/null", "-alias", alias, "-storepass", pass)
		kt.Env = c.Environ
		_ = kt.Run()
	case leakChild:
		holder := exec.Command("bash", "-c", "sleep "+sleepSeconds+"; :", pass)
		holder.Env = c.Environ
		if err := holder.Start(); err != nil {
			return exitRefused
		}
	case leakLate:
		// The holder starts only after the test's look; SIGTERM waits for its pid file, as the tool's cleanup does.
		go startLateHolder(c.Environ, envIn(c.Environ, leakyReadyEnv), envIn(c.Environ, leakyPIDEnv), pass)
		go stopOnTerm(envIn(c.Environ, leakyPIDEnv))
	}
	cmd := exec.Command("sleep", sleepSeconds)
	cmd.Env = c.Environ
	if err := cmd.Start(); err != nil {
		return exitRefused
	}
	return exitStatus(cmd.Wait())
}

// startLateHolder starts a process that holds the passphrase once the test has taken its look (the ready file), and
// writes its pid where the test reads it.
func startLateHolder(environ []string, ready, pidFile, pass string) {
	if !waitFileExists(ready, holdBound) {
		return
	}
	holder := exec.Command("bash", "-c", "sleep "+sleepSeconds+"; :", pass)
	holder.Env = environ
	if err := holder.Start(); err != nil {
		return
	}
	_ = os.WriteFile(pidFile, []byte(strconv.Itoa(holder.Process.Pid)), pidFileMode)
}

// stopOnTerm makes SIGTERM end the fake only after its holder has written its pid: the tool's cleanup waits for
// that file the same way, so the holder exists before the check stops the group.
func stopOnTerm(pidFile string) {
	term := make(chan os.Signal, 1)
	signal.Notify(term, sigTerm)
	<-term
	waitFileExists(pidFile, holdBound)
	os.Exit(sigExitBase + int(sigTerm))
}

// envIn returns the value of key in environ, the last one when it appears twice.
func envIn(environ []string, key string) string {
	return envLookup(environ)(key)
}

// testkitFake links the test binary under name and returns the directory of the link.
func testkitFake(t *testing.T, name string) string {
	t.Helper()
	return testkit.Fake(t, name).Dir()
}

// bashPath is bash's absolute path, found before any test changes PATH.
func bashPath(t *testing.T) string {
	t.Helper()
	p, err := exec.LookPath("bash")
	if err != nil {
		t.Skipf("bash is not on PATH: %v", err)
	}
	return p
}

// readFile is the whole of path, failing the test when it cannot be read.
func readFile(t *testing.T, path string) string {
	t.Helper()
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}

// itoa is strconv.Itoa, for the places a command line is built.
func itoa(n int) string { return strconv.Itoa(n) }

// sha256Hex is the SHA-256 of the file at path, as lower-case hex.
func sha256Hex(t *testing.T, path string) string {
	t.Helper()
	out, err := exec.Command("sha256sum", path).Output()
	if err != nil {
		t.Fatal(err)
	}
	return strings.Fields(string(out))[0]
}

// signalCase starts the tool as a process on a long command, sends it sig once the copy exists, and checks that the
// copy was shredded, that nothing was left behind, and that the tool exited with the signal's status.
func signalCase(t *testing.T, sig syscall.Signal) {
	t.Helper()
	f := newFixture(t)
	cmd := exec.Command(toolBinary(t), "with", "sleep", sleepSeconds)
	cmd.Env = f.env
	out, err := os.Create(filepath.Join(t.TempDir(), "tool.log"))
	if err != nil {
		t.Fatal(err)
	}
	defer out.Close()
	cmd.Stdout = out
	cmd.Stderr = out
	if err := cmd.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { killGroup(cmd.Process.Pid) })
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()

	// The signal must arrive while the wrapped command runs: a signal during the keytool check stops the run before
	// the command starts, which is a different path. So the test waits for the command, and the copy with it.
	testkit.WaitFor(t, "the wrapped command to start", func() bool {
		return hasArgv0(processTree(cmd.Process.Pid), "sleep") && len(keyFiles(f.runtime)) > 0
	})
	key := keyFiles(f.runtime)[0]
	if err := cmd.Process.Signal(sig); err != nil {
		t.Fatal(err)
	}
	var waitErr error
	select {
	case waitErr = <-done:
	case <-testkit.Within(t, stopBound).Context().Done():
		t.Fatalf("the tool did not exit within %v of %v", stopBound, sig)
	}
	if got, want := exitStatus(waitErr), sigExitBase+int(sig); got != want {
		t.Errorf("exit %d after %v, want %d", got, sig, want)
	}
	assertShredded(t, f, key)
	if left := leftover(t, f.runtime); len(left) != 0 {
		t.Fatalf("left behind: %v", left)
	}
}

// toolBinary is the tool linked under its name, the process the signal cases start.
func toolBinary(t *testing.T) string {
	t.Helper()
	return filepath.Join(testkitFake(t, toolFakeName), toolFakeName)
}

func TestTheCopyIsGoneAfterSIGTERM(t *testing.T) {
	t.Parallel()
	signalCase(t, sigTerm)
}

func TestShredIsCalledOnSIGTERM(t *testing.T) {
	t.Parallel()
	signalCase(t, sigTerm)
}

func TestShredIsCalledOnSIGINT(t *testing.T) {
	t.Parallel()
	signalCase(t, sigInt)
}

// This case changes the test process's own SIGINT disposition, which every test in the process shares, so it does
// not call t.Parallel: a parallel test would see the change.
func TestShredIsCalledOnSIGINTWhenTheTestProcessIgnoresSIGINT(t *testing.T) {
	signal.Ignore(sigInt)
	t.Cleanup(func() { signal.Reset(sigInt) })
	signalCase(t, sigInt)
}

func TestShredIsCalledOnSIGHUP(t *testing.T) {
	t.Parallel()
	signalCase(t, sigHup)
}

func TestAProcessStartedAfterTheCheckLookedIsStoppedBeforeTheCheckReturns(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	ready := filepath.Join(t.TempDir(), "late-ready")
	pidFile := filepath.Join(t.TempDir(), "late-child.pid")
	env := with(f.env, leakyEnv+"="+leakLate, leakyReadyEnv+"="+ready, leakyPIDEnv+"="+pidFile)
	// The look runs before the holder exists, so it sees no passphrase; the check's stop must still reach the holder
	// that the test's after-look signal makes the tool start.
	f.passphraseHolders(leakyBinary(t), []string{"with", "sleep", sleepSeconds}, env, func() {
		if err := os.WriteFile(ready, nil, pidFileMode); err != nil {
			t.Fatal(err)
		}
	})
	raw, err := os.ReadFile(pidFile)
	if err != nil {
		t.Fatal("the holder never started its child:", err)
	}
	late, _ := strconv.Atoi(strings.TrimSpace(string(raw)))
	testkit.WaitFor(t, fmt.Sprintf("the child started after the look (pid %d) to be gone", late), func() bool {
		return !alive(late)
	})
}

// leakyBinary is the leaky tool linked under its name.
func leakyBinary(t *testing.T) string {
	t.Helper()
	return filepath.Join(testkitFake(t, leakyFakeName), leakyFakeName)
}

func TestTheCheckFailsWhenTheToolPutsThePassphraseOnTheCommandLineOfKeytool(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	env := with(f.env, leakyEnv+"="+leakArgv)
	holders := f.passphraseHolders(leakyBinary(t), []string{"with", "sleep", sleepSeconds}, env, nil)
	if !hasKeytoolHolder(holders) {
		t.Fatalf("no keytool call with the passphrase was found: %v", holders)
	}
}

func TestTheCheckFailsWhenAProcessTheToolStartsHoldsThePassphrase(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	env := with(f.env, leakyEnv+"="+leakChild)
	holders := f.passphraseHolders(leakyBinary(t), []string{"with", "sleep", sleepSeconds}, env, nil)
	found := false
	for _, h := range holders {
		if strings.Contains(h, "bash -c sleep "+sleepSeconds+"; :") {
			found = true
		}
	}
	if !found {
		t.Fatalf("the holder started by the tool was not found: %v", holders)
	}
}

// hasKeytoolHolder reports whether one of the holder lines is a keytool call that carries the passphrase.
func hasKeytoolHolder(holders []string) bool {
	for _, h := range holders {
		if strings.HasPrefix(h, "keytool ") {
			return true
		}
	}
	return false
}

func TestAliveIsFalseForAZombieAndTrueForARunningProcess(t *testing.T) {
	t.Parallel()
	zombie := exec.Command("true")
	if err := zombie.Start(); err != nil {
		t.Fatal(err)
	}
	exitedUnreaped(t, zombie.Process.Pid)
	if alive(zombie.Process.Pid) {
		t.Error("a zombie counted as alive")
	}
	_ = zombie.Wait()
	runner := exec.Command("sleep", sleepSeconds)
	if err := runner.Start(); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = runner.Process.Kill(); _ = runner.Wait() })
	if !alive(runner.Process.Pid) {
		t.Error("a running process counted as dead")
	}
}

func TestAToolThatIgnoresSIGTERMFailsTheCheckAndNamesTheWait(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	env := with(f.env, leakyEnv+"="+leakIgnore)
	s := startSession(t, leakyBinary(t), []string{"with", "sleep", sleepSeconds}, env)
	testkit.WaitFor(t, "the wrapped command to start", func() bool {
		return hasArgv0(processTree(s.cmd.Process.Pid), "sleep")
	})
	err := s.stop(ignoreBound)
	if err == nil {
		t.Fatal("a tool that ignores SIGTERM stopped within its bound")
	}
	msg := err.Error()
	if !strings.Contains(msg, "did not exit within "+ignoreBound.String()+" of SIGTERM") {
		t.Errorf("the message does not name the wait: %s", msg)
	}
	if !strings.Contains(msg, logPrefix+"keystore ready") {
		t.Errorf("the message does not carry the tool's log: %s", msg)
	}
}

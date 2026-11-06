package testkit

import (
	"bytes"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

// The fakes of these tests. Each is a command a test below runs by name.
var testFakes = Fakes{
	// Prints its arguments, one per line, and exits with the code of its first argument ("exit=7").
	"tk-args": func(c *Call) int {
		for _, a := range c.Args {
			fmt.Fprintln(c.Stdout, a)
			if code, ok := strings.CutPrefix(a, "exit="); ok {
				var n int
				fmt.Sscan(code, &n)
				return n
			}
		}
		return 0
	},
	// Copies standard input to standard output, then prints its working directory to standard error.
	"tk-cat": func(c *Call) int {
		io.Copy(c.Stdout, c.Stdin)
		fmt.Fprintln(c.Stderr, c.Dir)
		return 0
	},
	// Replaces itself with the real basename, which a later directory of PATH holds under the same name (a shell has no
	// builtin of that name), and gives it other arguments than it was called with.
	"basename": func(c *Call) int {
		return c.Exec("basename", "some/dir/real-basename-ran")
	},
	// Replaces itself with a shell that prints the FOO it was started with.
	"tk-envexec": func(c *Call) int {
		return c.Exec("sh", "-c", "echo exec-saw-$FOO")
	},
	// Replaces itself with itself by path: Exec refuses.
	"tk-selfexec": func(c *Call) int {
		return c.Exec(filepath.Join(c.FakeDir, "tk-selfexec"))
	},
	// Replaces itself with a program that does not exist.
	"tk-noexec": func(c *Call) int {
		return c.Exec("tk-no-such-program-anywhere")
	},
	// Prints the directory it was found in, and the contents of the file "note" beside it.
	"tk-where": func(c *Call) int {
		fmt.Fprintln(c.Stdout, c.FakeDir)
		note, err := os.ReadFile(filepath.Join(c.FakeDir, "note"))
		if err != nil {
			fmt.Fprintln(c.Stderr, err)
			return 1
		}
		c.Stdout.Write(note)
		return 0
	},
	// Reports the environment key FOO it was started with.
	"tk-env": func(c *Call) int {
		for _, kv := range c.Environ {
			if v, ok := strings.CutPrefix(kv, "FOO="); ok {
				fmt.Fprintln(c.Stdout, v)
			}
		}
		return 0
	},
}

func TestMain(m *testing.M) { Main(m, testFakes) }

func sh(t *testing.T, env []string, script string, stdin string) (stdout, stderr string, code int) {
	t.Helper()
	cmd := exec.CommandContext(t.Context(), "sh", "-c", script)
	cmd.Env = env
	cmd.Dir = t.TempDir()
	cmd.Stdin = strings.NewReader(stdin)
	var out, errb bytes.Buffer
	cmd.Stdout, cmd.Stderr = &out, &errb
	err := cmd.Run()
	code = 0
	if ee, ok := err.(*exec.ExitError); ok {
		code = ee.ExitCode()
	} else if err != nil {
		t.Errorf("sh -c %q: %v", script, err) // Errorf, not Fatalf: concurrent tests call sh from goroutines
	}
	return out.String(), errb.String(), code
}

func TestAFakeIsFoundByItsBareNameAndRunsItsFunction(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-args")
	env := WithPath(Env(t), f.Dir())
	out, _, code := sh(t, env, "tk-args one 'two words' exit=7", "")
	if code != 7 {
		t.Errorf("exit code = %d, want 7", code)
	}
	if want := "one\ntwo words\nexit=7\n"; out != want {
		t.Errorf("stdout = %q, want %q", out, want)
	}
}

func TestAFakeRunsByAbsolutePathToo(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-args")
	cmd := exec.CommandContext(t.Context(), filepath.Join(f.Dir(), "tk-args"), "a", "exit=3")
	cmd.Env = Env(t)
	out, err := cmd.Output()
	ee, ok := err.(*exec.ExitError)
	if !ok || ee.ExitCode() != 3 {
		t.Fatalf("err = %v, want exit status 3", err)
	}
	if string(out) != "a\nexit=3\n" {
		t.Errorf("stdout = %q", out)
	}
}

func TestAFakeGetsStandardInputAndTheWorkingDirectory(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-cat")
	out, errOut, code := sh(t, WithPath(Env(t), f.Dir()), "tk-cat", "hello\n")
	if code != 0 || out != "hello\n" {
		t.Fatalf("exit %d, stdout %q", code, out)
	}
	if !filepath.IsAbs(strings.TrimSpace(errOut)) {
		t.Errorf("working directory printed = %q, want an absolute path", errOut)
	}
}

func TestEveryCallIsRecordedAsOneJSONLineWithArgsDirAndTheNamedEnvironmentKeys(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-args", "FOO", "UNSET_KEY")
	env := WithPath(Env(t, "FOO=bar", "OTHER=ignored"), f.Dir())
	sh(t, env, "tk-args first; tk-args second 'with space'", "")
	calls := f.Calls(t)
	if len(calls) != 2 {
		t.Fatalf("recorded %d calls, want 2: %+v", len(calls), calls)
	}
	if calls[0].Name != "tk-args" || strings.Join(calls[0].Args, "|") != "first" {
		t.Errorf("first call = %+v", calls[0])
	}
	if strings.Join(calls[1].Args, "|") != "second|with space" {
		t.Errorf("second call args = %q", calls[1].Args)
	}
	if !filepath.IsAbs(calls[0].Dir) {
		t.Errorf("recorded dir %q is not absolute", calls[0].Dir)
	}
	if len(calls[0].Env) != 1 || calls[0].Env["FOO"] != "bar" {
		t.Errorf("recorded env = %v, want exactly FOO=bar (OTHER is not named; UNSET_KEY is not set)", calls[0].Env)
	}
	raw, err := os.ReadFile(f.LogPath())
	if err != nil {
		t.Fatal(err)
	}
	if n := strings.Count(string(raw), "\n"); n != 2 || !strings.HasSuffix(string(raw), "\n") {
		t.Errorf("log has %d newline-terminated lines, want 2:\n%s", n, raw)
	}
}

func TestACallWithNoArgumentsRecordsAnEmptyArgumentList(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-args")
	sh(t, WithPath(Env(t), f.Dir()), "tk-args", "")
	raw, _ := os.ReadFile(f.LogPath())
	mustContain(t, "log line", string(raw), `"args":[]`)
}

func TestACallIsRecordedEvenWhenTheFakeFails(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-args")
	sh(t, WithPath(Env(t), f.Dir()), "tk-args exit=9", "")
	if got := len(f.Calls(t)); got != 1 {
		t.Errorf("recorded %d calls, want 1", got)
	}
}

func TestConcurrentCallsAreRecordedWholeAndNoneIsLost(t *testing.T) {
	t.Parallel()
	const n = 24
	f := Fake(t, "tk-args")
	env := WithPath(Env(t), f.Dir())
	var wg sync.WaitGroup
	for i := range n {
		wg.Add(1)
		go func() {
			defer wg.Done()
			sh(t, env, fmt.Sprintf("tk-args call-%d", i), "")
		}()
	}
	wg.Wait()
	calls := f.Calls(t)
	if len(calls) != n {
		t.Fatalf("recorded %d calls, want %d", len(calls), n)
	}
	seen := map[string]bool{}
	for _, c := range calls {
		seen[strings.Join(c.Args, " ")] = true
	}
	if len(seen) != n {
		t.Errorf("%d distinct calls recorded, want %d", len(seen), n)
	}
}

func TestAFakeCanReplaceItselfWithTheRealProgramOfTheSameName(t *testing.T) {
	t.Parallel()
	f := Fake(t, "basename")
	out, _, code := sh(t, WithPath(Env(t), f.Dir()), "basename a b", "")
	if code != 0 || out != "real-basename-ran\n" {
		t.Errorf("exit %d, stdout %q, want the real basename's \"real-basename-ran\"", code, out)
	}
	if calls := f.Calls(t); len(calls) != 1 || strings.Join(calls[0].Args, " ") != "a b" {
		t.Errorf("recorded %+v, want the one call before the replacement", calls)
	}
}

func TestReplacingItselfWithAMissingProgramExits127AndSaysSo(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-noexec")
	_, errOut, code := sh(t, WithPath(Env(t), f.Dir()), "tk-noexec", "")
	if code != 127 {
		t.Errorf("exit code = %d, want 127", code)
	}
	mustContain(t, "stderr", errOut, "tk-no-such-program-anywhere")
}

func TestAFakeSeesTheEnvironmentItWasStartedWith(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-env")
	out, _, _ := sh(t, WithPath(Env(t, "FOO=xyz"), f.Dir()), "tk-env", "")
	if out != "xyz\n" {
		t.Errorf("stdout = %q, want xyz", out)
	}
}

func TestFakeRefusesANameMainWasNotGiven(t *testing.T) {
	t.Parallel()
	ft := newFakeT(t.Context(), t.TempDir())
	msg := fatalOf(func() { Fake(ft, "tk-not-registered") })
	mustContain(t, "Fatalf message", msg, "tk-not-registered", "testkit.Main")
}

func TestFakeRefusesANameThatIsNotAPlainFileName(t *testing.T) {
	t.Parallel()
	for _, name := range []string{"", "a/b", ".", ".."} {
		ft := newFakeT(t.Context(), t.TempDir())
		msg := fatalOf(func() { Fake(ft, name) })
		if !strings.Contains(msg, "plain command name") { // not the later "no fake of that name", which would also fail
			t.Errorf("Fake(%q) failed with %q, want it to say the name is not a plain command name", name, msg)
		}
	}
}

// The test binary under a fake's name but not set up by Fake (no description beside it) must not run silently.
func TestAFakeWithNoDescriptionBesideItFailsLoudly(t *testing.T) {
	t.Parallel()
	exe, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	dir := t.TempDir()
	link := filepath.Join(dir, "tk-args")
	if err := os.Symlink(exe, link); err != nil {
		t.Fatal(err)
	}
	cmd := exec.CommandContext(t.Context(), link, "x")
	cmd.Env = Env(t)
	out, err := cmd.CombinedOutput()
	ee, ok := err.(*exec.ExitError)
	if !ok || ee.ExitCode() != 125 {
		t.Fatalf("err = %v, want exit status 125", err)
	}
	mustContain(t, "output", string(out), "tk-args", "testkit.Fake")
}

func TestCallsAreEmptyBeforeAnyCall(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-args")
	if calls := f.Calls(t); len(calls) != 0 {
		t.Errorf("calls = %+v, want none", calls)
	}
}

func TestAFakeKnowsTheDirectoryItWasFoundInSoATestCanLeaveItAFileBeside(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-where")
	if err := os.WriteFile(filepath.Join(f.Dir(), "note"), []byte("scenario one\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	for how, script := range map[string]string{"by name": "tk-where", "by path": f.Dir() + "/tk-where"} {
		out, errOut, code := sh(t, WithPath(Env(t), f.Dir()), script, "")
		if code != 0 || out != f.Dir()+"\nscenario one\n" {
			t.Errorf("%s: exit %d, stdout %q, stderr %q", how, code, out, errOut)
		}
	}
}

func TestExecHandsTheRealProgramTheFakesEnvironment(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-envexec")
	out, _, code := sh(t, WithPath(Env(t, "FOO=carried"), f.Dir()), "tk-envexec", "")
	if code != 0 || out != "exec-saw-carried\n" {
		t.Errorf("exit %d, stdout %q, want the environment to reach the replacement", code, out)
	}
}

// Without this a fake that finds itself would start itself for ever.
func TestExecRefusesToStartTheTestBinaryAgain(t *testing.T) {
	t.Parallel()
	f := Fake(t, "tk-selfexec")
	_, errOut, code := sh(t, WithPath(Env(t), f.Dir()), "tk-selfexec", "")
	if code != 127 {
		t.Errorf("exit %d, want 127", code)
	}
	mustContain(t, "stderr", errOut, "test binary")
	if n := len(f.Calls(t)); n != 1 {
		t.Errorf("the fake ran %d times, want once", n)
	}
}

package main

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// TestMain makes this test binary the fakes the tests need: keytool and shred record their calls and run the real
// program, the tool itself runs as a process under its own name, and a leaky tool plays the leaks the holder check
// must catch.
// What the command in TestWithGivesTheCommandAPrivateCopyOfTheKeystore writes, in order, and the modes it must show.
const (
	seenMode = iota
	seenDirMode
	seenDigest
	seenAlias
	seenFields
)

const (
	wantFileMode = "600"
	wantDirMode  = "700"
)

func TestMain(m *testing.M) {
	testkit.Main(m, testkit.Fakes{
		keytoolName:   fakeKeytool,
		shredName:     fakeShred,
		toolFakeName:  fakeTool,
		leakyFakeName: fakeLeaky,
	})
}

// The in-process cases call run in the test process. A signal or a process group is a process boundary, so those
// cases are in process_test.go and start the tool as a process.

func TestWithGivesTheCommandAPrivateCopyOfTheKeystore(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	out := filepath.Join(t.TempDir(), "seen.txt")
	cmd := `f="$NEVR_QUEST_KS"; printf "%s %s %s %s" "$(stat -c %a "$f")" "$(stat -c %a "$(dirname "$f")")" ` +
		`"$(sha256sum < "$f" | cut -d" " -f1)" "$NEVR_QUEST_KS_ALIAS" > ` + out
	r := f.run(f.env, "with", "bash", "-c", cmd)
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	fields := strings.Fields(readFile(t, out))
	if len(fields) != seenFields {
		t.Fatalf("the command wrote %q", fields)
	}
	if fields[seenMode] != wantFileMode || fields[seenDirMode] != wantDirMode {
		t.Errorf("copy mode %s, directory mode %s; want %s and %s", fields[seenMode], fields[seenDirMode], wantFileMode, wantDirMode)
	}
	if fields[seenDigest] != sha256Hex(t, f.keystore) {
		t.Errorf("the copy's digest %s is not the keystore's", fields[seenDigest])
	}
	if fields[seenAlias] != alias {
		t.Errorf("NEVR_QUEST_KS_ALIAS = %q", fields[seenAlias])
	}
}

func TestTheCopyIsGoneAfterTheCommandSucceeds(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	if r := f.run(f.env, "with", "true"); r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if left := leftover(t, f.runtime); len(left) != 0 {
		t.Fatalf("left behind: %v", left)
	}
}

func TestTheCopyIsGoneAfterTheCommandFailsAndItsStatusIsKept(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	r := f.run(f.env, "with", "bash", "-c", "exit "+itoa(exitSeven))
	if r.code != exitSeven {
		t.Fatalf("exit %d, want %d", r.code, exitSeven)
	}
	if left := leftover(t, f.runtime); len(left) != 0 {
		t.Fatalf("left behind: %v", left)
	}
}

func TestNeitherThePassphraseNorTheKeyIsPrintedOrOnACommandLine(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	if holders := f.passphraseHolders(toolBinary(t), []string{"with", "sleep", sleepSeconds}, f.env, nil); len(holders) != 0 {
		t.Fatalf("the passphrase was on a command line of the tool: %v", holders)
	}
	r := f.run(f.env, "with", "true")
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	blob := f.keyBlob()
	for _, text := range []string{r.stdout, r.stderr} {
		if strings.Contains(text, passphrase) {
			t.Errorf("the passphrase was printed: %q", text)
		}
		if strings.Contains(text, blob[:keyBlobChars]) {
			t.Errorf("the key was printed: %q", text)
		}
	}
}

func TestAProcessThatIsNotTheToolsAndNamesThePassphraseDoesNotFailTheCheck(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	// The name is argument 0 of bash's -c script, so it stays on the command line: a process that is not the tool's.
	startSession(t, bashPath(t), []string{"-c", "sleep " + sleepSeconds + "; :", passphrase}, f.env)
	found := false
	for _, p := range processTable() {
		if strings.Contains(strings.Join(p.argv, " "), passphrase) {
			found = true
		}
	}
	if !found {
		t.Fatal("the unrelated process is not in the process table, so this test proves nothing")
	}
	if holders := f.passphraseHolders(toolBinary(t), []string{"with", "sleep", sleepSeconds}, f.env, nil); len(holders) != 0 {
		t.Fatalf("the check counted a process that is not the tool's: %v", holders)
	}
}

func TestAMissingKeystoreVariableStopsBeforeTheCommand(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	marker := filepath.Join(t.TempDir(), "ran")
	r := f.run(without(f.env, envKeystore), "with", "bash", "-c", "touch "+marker)
	assertStopped(t, f, r, marker, envKeystore+" is not set")
}

func TestAKeystorePathThatIsNotAReadableFileStopsBeforeTheCommand(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	marker := filepath.Join(t.TempDir(), "ran")
	missing := filepath.Join(t.TempDir(), "nope.jks")
	r := f.run(with(f.env, envKeystore+"="+missing), "with", "bash", "-c", "touch "+marker)
	assertStopped(t, f, r, marker, envKeystore, missing)
}

func TestAMissingPassphraseVariableStopsBeforeTheCommand(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	marker := filepath.Join(t.TempDir(), "ran")
	r := f.run(without(f.env, envPass), "with", "bash", "-c", "touch "+marker)
	assertStopped(t, f, r, marker, envPass+" is not set")
}

func TestAWrongPassphraseStopsBeforeTheCommandAndDoesNotPrintIt(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	marker := filepath.Join(t.TempDir(), "ran")
	r := f.run(with(f.env, envPass+"="+wrongPass), "with", "bash", "-c", "touch "+marker)
	assertStopped(t, f, r, marker, "cannot be opened with "+envPass)
	if strings.Contains(r.stdout+r.stderr, wrongPass) {
		t.Error("the wrong passphrase was printed")
	}
}

func TestAFailedTemporaryDirectoryStopsTheToolBeforeTheCommand(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	marker := filepath.Join(t.TempDir(), "ran")
	var out, errb bytes.Buffer
	code := runWith([]string{"with", "bash", "-c", "touch " + marker}, f.env, &out, &errb,
		func(string, string) (string, error) { return "", errors.New("no directory for you") })
	if code != exitRefused {
		t.Fatalf("exit %d, want %d: %s", code, exitRefused, errb.String())
	}
	if !strings.Contains(errb.String(), "cannot create a private temporary directory") {
		t.Errorf("stderr: %s", errb.String())
	}
	if _, err := os.Stat(marker); err == nil {
		t.Error("the command ran")
	}
}

func TestShredIsCalledOnTheKeyAfterTheCommandSucceeds(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	seen := filepath.Join(t.TempDir(), "seen-key")
	r := f.run(f.env, "with", "bash", "-c", `printf "%s" "$NEVR_QUEST_KS" > `+seen)
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	assertShredded(t, f, readFile(t, seen))
	if left := leftover(t, f.runtime); len(left) != 0 {
		t.Fatalf("left behind: %v", left)
	}
}

func TestShredIsCalledOnTheKeyAfterTheCommandFails(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	seen := filepath.Join(t.TempDir(), "seen-key")
	r := f.run(f.env, "with", "bash", "-c", `printf "%s" "$NEVR_QUEST_KS" > `+seen+`; exit `+itoa(exitSeven))
	if r.code != exitSeven {
		t.Fatalf("exit %d, want %d", r.code, exitSeven)
	}
	assertShredded(t, f, readFile(t, seen))
	if left := leftover(t, f.runtime); len(left) != 0 {
		t.Fatalf("left behind: %v", left)
	}
}

func TestFingerprintPrintsOnlyTheSignerSHA256(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	r := f.run(f.env, "fingerprint")
	if r.code != 0 {
		t.Fatalf("exit %d: %s", r.code, r.stderr)
	}
	if r.stdout != f.fp+"\n" {
		t.Fatalf("stdout %q, want %q", r.stdout, f.fp+"\n")
	}
	if left := leftover(t, f.runtime); len(left) != 0 {
		t.Fatalf("left behind: %v", left)
	}
}

func TestCheckSucceedsSilentlyWhenTheKeystoreOpens(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	r := f.run(f.env, "check")
	if r.code != 0 || r.stdout != "" {
		t.Fatalf("exit %d, stdout %q: %s", r.code, r.stdout, r.stderr)
	}
	if left := leftover(t, f.runtime); len(left) != 0 {
		t.Fatalf("left behind: %v", left)
	}
}

func TestCheckNamesTheVariableThatIsMissing(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	r := f.run(without(f.env, envPass), "check")
	if r.code != exitRefused {
		t.Fatalf("exit %d, want %d", r.code, exitRefused)
	}
	if !strings.Contains(r.stderr, envPass+" is not set") {
		t.Errorf("stderr: %s", r.stderr)
	}
}

func TestCheckFailsOnAWrongPassphrase(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	r := f.run(with(f.env, envPass+"=wrong-7e2d"), "check")
	if r.code != exitRefused {
		t.Fatalf("exit %d, want %d", r.code, exitRefused)
	}
	if !strings.Contains(r.stderr, "cannot be opened") {
		t.Errorf("stderr: %s", r.stderr)
	}
	if strings.Contains(r.stdout+r.stderr, "wrong-7e2d") {
		t.Error("the wrong passphrase was printed")
	}
}

func TestImportIsNoLongerAModeOfTheTool(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	r := f.run(f.env, "import", f.keystore)
	if r.code != exitUsage {
		t.Fatalf("exit %d, want %d", r.code, exitUsage)
	}
	if !strings.Contains(r.stderr, "usage") {
		t.Errorf("stderr: %s", r.stderr)
	}
}

// assertStopped checks that a run stopped before its command: the exit code, the ERROR line and each named text, and
// that the command never ran and nothing was left behind.
func assertStopped(t *testing.T, f *fixture, r result, marker string, names ...string) {
	t.Helper()
	if r.code != exitRefused {
		t.Fatalf("exit %d, want %d: %s", r.code, exitRefused, r.stderr)
	}
	if !strings.Contains(r.stderr, errPrefix) {
		t.Errorf("stderr has no ERROR line: %s", r.stderr)
	}
	for _, name := range names {
		if !strings.Contains(r.stderr, name) {
			t.Errorf("stderr does not name %q: %s", name, r.stderr)
		}
	}
	if _, err := os.Stat(marker); err == nil {
		t.Error("the command ran")
	}
	if left := leftover(t, f.runtime); len(left) != 0 {
		t.Errorf("left behind: %v", left)
	}
}

// assertShredded checks that the fake shred was called with -u on the key path that a command read from its environment.
func assertShredded(t *testing.T, f *fixture, keyPath string) {
	t.Helper()
	if len(f.shred.Calls(t)) == 0 {
		t.Fatal("shred was never called")
	}
	if !f.shredded(keyPath) {
		t.Fatalf("no shred -u on %s in %v", keyPath, f.shred.Calls(t))
	}
}

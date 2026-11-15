package main

import (
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// Paths from this package's directory (go test runs the test in it) to the files the shape checks read.
const (
	mainSource   = "main.go"
	wrapperPath  = "../../quest_keystore.sh"
	justfilePath = "../../../justfile"
	recipeStart  = "\n"
	recipeEnd    = "\n\n"
)

// wrapperExitCheck is the status the wrapper must hand back for `with bash -c "exit 7"`.
const wrapperExitCheck = exitSeven

// TestTheScriptReadsTheStoreOnlyFromTheTwoVariablesAndNeverFromPass checks the names the tool reads and exports and
// that no path reads the store from anywhere else.
func TestTheScriptReadsTheStoreOnlyFromTheTwoVariablesAndNeverFromPass(t *testing.T) {
	t.Parallel()
	src := readFile(t, mainSource) + readFile(t, wrapperPath)
	for _, banned := range []string{"pass show", "import-quest-keystore", "nevrdebug"} {
		if strings.Contains(src, banned) {
			t.Errorf("the tool or its script mentions %q", banned)
		}
	}
	for _, want := range []string{
		`alias\s+= "nevr"`, `envKeystore\s+= "NEVR_QUEST_KEYSTORE"`, `envPass\s+= "NEVR_QUEST_KEYSTORE_PASS"`,
	} {
		if !regexp.MustCompile(want).MatchString(readFile(t, mainSource)) {
			t.Errorf("main.go does not declare %s", want)
		}
	}
}

// TestThePackageRecipesAskTheScriptForTheKeyInsteadOfNamingItsEntry checks the justfile still hands the signing
// recipes to the script, and that the entry it names is gone.
func TestThePackageRecipesAskTheScriptForTheKeyInsteadOfNamingItsEntry(t *testing.T) {
	t.Parallel()
	text := readFile(t, justfilePath)
	if strings.Contains(text, "pass show services/echovrce") {
		t.Error("the justfile names the password-store entry")
	}
	for _, name := range []string{"package-dev", "_release-quest-apk"} {
		if !strings.Contains(recipe(t, text, name), "tools/quest_keystore.sh check") {
			t.Errorf("recipe %s does not ask tools/quest_keystore.sh for the key", name)
		}
	}
}

// TestTheRepackNeverGeneratesAKeyAndSignsThroughTheKeystoreScript checks the repack recipe signs with the store and
// generates nothing.
func TestTheRepackNeverGeneratesAKeyAndSignsThroughTheKeystoreScript(t *testing.T) {
	t.Parallel()
	text := readFile(t, justfilePath)
	body := recipe(t, text, "android-repack-apk")
	if strings.Contains(body, "keytool -genkeypair") {
		t.Error("android-repack-apk generates a key")
	}
	if strings.Contains(text, "genkeypair") {
		t.Error("the justfile generates a key somewhere")
	}
	if !strings.Contains(body, "tools/quest_keystore.sh") {
		t.Error("android-repack-apk does not sign through tools/quest_keystore.sh")
	}
	if !strings.Contains(body, "--ks-pass env:") {
		t.Error("android-repack-apk does not read the passphrase from the environment")
	}
}

// TestTheRecipesToReadTheFingerprintExistAndImportIsGone checks the fingerprint recipe is there and import is not.
func TestTheRecipesToReadTheFingerprintExistAndImportIsGone(t *testing.T) {
	t.Parallel()
	text := readFile(t, justfilePath)
	if !strings.Contains(text, "quest-keystore-fingerprint") {
		t.Error("the justfile has no quest-keystore-fingerprint recipe")
	}
	if strings.Contains(text, "import-quest-keystore") {
		t.Error("the justfile still has import-quest-keystore")
	}
}

// recipe is the text of the justfile recipe name: from its line to the blank line after it.
func recipe(t *testing.T, text, name string) string {
	t.Helper()
	start := strings.Index(text, recipeStart+name+" ")
	if start < 0 {
		t.Fatalf("the justfile has no recipe %s", name)
	}
	end := strings.Index(text[start:], recipeEnd)
	if end < 0 {
		t.Fatalf("recipe %s is not followed by a blank line", name)
	}
	return text[start : start+end]
}

// TestWrapperRunsTheToolAndKeepsItsExitStatus is the wrapper's own check (added with the port): it builds the tool
// from the current source, runs it, and hands back the wrapped command's status unchanged.
func TestWrapperRunsTheToolAndKeepsItsExitStatus(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	cmd := exec.Command(bashPath(t), wrapperPath, "with", "bash", "-c", "exit "+itoa(wrapperExitCheck))
	cmd.Env = f.env
	err := cmd.Run()
	var ee *exec.ExitError
	if !errors.As(err, &ee) || ee.ExitCode() != wrapperExitCheck {
		t.Fatalf("the wrapper exited %v, want %d", err, wrapperExitCheck)
	}
	if left := leftover(t, f.runtime); len(left) != 0 {
		t.Fatalf("left behind: %v", left)
	}
}

// TestWrapperNamesAMissingGo is the wrapper's own check for a machine without go: it stops with its own message,
// before anything else runs.
func TestWrapperNamesAMissingGo(t *testing.T) {
	t.Parallel()
	bin := t.TempDir()
	if err := os.Symlink(bashPath(t), filepath.Join(bin, "bash")); err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(bashPath(t), wrapperPath, "check")
	cmd.Env = setEnv(testkit.Env(t), envPath, bin)
	var stderr strings.Builder
	cmd.Stderr = &stderr
	err := cmd.Run()
	var ee *exec.ExitError
	if !errors.As(err, &ee) || ee.ExitCode() != exitRefused {
		t.Fatalf("exit %v, want %d: %s", err, exitRefused, stderr.String())
	}
	if !strings.Contains(stderr.String(), "go is required") {
		t.Errorf("stderr: %s", stderr.String())
	}
}

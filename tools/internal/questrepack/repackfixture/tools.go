package repackfixture

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
)

// toolDir is the directory ToolsSetup built the Go tools into. Recipe runs the tools from it.
var toolDir string

// ToolsSetup is the setup of a package's TestMain (testkit.MainWith). It builds the commands of tools/cmd once, for
// every test of the package, into a directory the test binary makes and removes at the end: the recipe runs those
// binaries (Recipe passes the directory as go_tools), so no test writes the checkout's build/go-tools (ADR 0044
// rule 8). A build that fails ends the run, since no test can run without the tools.
func ToolsSetup(getenv func(string) string) func() {
	undo, dir, err := buildTools(getenv)
	if err != nil {
		fmt.Fprintf(os.Stderr, "repackfixture: the Go tools the recipe runs did not build: %v\n", err)
		os.Exit(1)
	}
	toolDir = dir
	return undo
}

// buildTools runs `go build` of tools/cmd into a new directory, with testkit's hermetic environment, and returns the
// function that removes the directory and the directory.
func buildTools(getenv func(string) string) (func(), string, error) {
	goBin, err := exec.LookPath("go")
	if err != nil {
		return nil, "", fmt.Errorf("go is not on PATH: %w", err)
	}
	repo, err := findCheckout()
	if err != nil {
		return nil, "", err
	}
	root, err := os.MkdirTemp("", "repack-tools-")
	if err != nil {
		return nil, "", err
	}
	undo := func() { _ = os.RemoveAll(root) }
	dir := filepath.Join(root, "bin")
	if err := os.Mkdir(dir, 0o755); err != nil {
		undo()
		return nil, "", err
	}
	env, err := testkit.EnvAt(filepath.Join(root, "env"), getenv, goBin)
	if err != nil {
		undo()
		return nil, "", err
	}
	cmd := exec.Command(goBin, "-C", filepath.Join(repo, "tools"), "build", "-trimpath",
		"-o", dir+string(filepath.Separator), "./cmd/...")
	cmd.Env = env
	if out, err := cmd.CombinedOutput(); err != nil {
		undo()
		return nil, "", fmt.Errorf("go build of tools/cmd: %w\n%s", err, out)
	}
	return undo, dir, nil
}

// findCheckout walks up from the test's directory to the checkout: the directory that holds the justfile and tools/go.mod.
func findCheckout() (string, error) {
	dir, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for {
		_, errJust := os.Stat(filepath.Join(dir, "justfile"))
		_, errMod := os.Stat(filepath.Join(dir, "tools", "go.mod"))
		if errJust == nil && errMod == nil {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", fmt.Errorf("no checkout (a directory with justfile and tools/go.mod) above %s", dir)
		}
		dir = parent
	}
}

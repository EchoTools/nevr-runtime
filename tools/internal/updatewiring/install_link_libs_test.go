package updatewiring

import (
	"os"
	"os/exec"
	"os/user"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

const (
	installTarget   = "x86_64-pc-windows-gnu"
	nativeLibsNote  = "native-static-libs:"
	installLibsFile = "src/runtime/CMakeLists.txt"
	// What rustc prints when the std of the target is not installed: the only failure the test skips for.
	targetMissingNote = "target may not be installed"
)

var installLinkList = regexp.MustCompile(`INTERFACE_LINK_LIBRARIES "([^"]+)"`)

// The Windows libraries nevr-runtime.dll must link beside libnevr_install.a are the ones the Rust toolchain in use says
// the static library needs (`--print native-static-libs`). CMake keeps them as a fixed list, so a newer toolchain that
// needs one more (Rust 1.100's std wants -lsynchronization for WaitOnAddress/WakeByAddress*) breaks the DLL's link
// with undefined references, not the library's build.
func TestRuntimeLinksEveryLibraryTheCoreInstallLibraryNames(t *testing.T) {
	if testing.Short() {
		t.Skip("builds the installer library with cargo")
	}
	cargo, err := exec.LookPath("cargo")
	if err != nil {
		t.Skipf("no cargo on PATH: %v", err)
	}
	listed := installLinkList.FindStringSubmatch(read(t, installLibsFile))
	if listed == nil {
		t.Fatalf("%s: no INTERFACE_LINK_LIBRARIES list for nevr_install", installLibsFile)
	}
	linked := map[string]bool{}
	for _, name := range strings.Split(listed[1], ";") {
		linked[name] = true
	}
	cmd := exec.Command(cargo, "rustc", "--locked", "--release", "--target", installTarget, "--lib", "--crate-type", "staticlib",
		"--manifest-path", root(t)+"/installer/Cargo.toml", "--target-dir", t.TempDir(), "--", "--print", "native-static-libs")
	cmd.Env = cargoEnv(t)
	out, err := cmd.CombinedOutput()
	if err != nil && strings.Contains(string(out), targetMissingNote) {
		t.Skipf("the %s target is not installed here: %v", installTarget, err)
	}
	if err != nil {
		t.Fatalf("cargo cannot build the core install library for %s: %v\n%s", installTarget, err, out)
	}
	_, after, found := strings.Cut(string(out), nativeLibsNote)
	if !found {
		t.Fatalf("cargo printed no %q line:\n%s", nativeLibsNote, out)
	}
	libsLine, _, _ := strings.Cut(after, "\n")
	for _, flag := range strings.Fields(libsLine) {
		if name, ok := strings.CutPrefix(flag, "-l"); ok && !linked[name] {
			t.Errorf("libnevr_install.a needs -l%s and %s links only %q", name, installLibsFile, listed[1])
		}
	}
}

// cargoEnv is the test process's environment with the Rust toolchain found again: `just test-tools` starts the tests with
// a fresh HOME, where rustup's cargo proxy has no toolchain to choose, so RUSTUP_HOME and CARGO_HOME (when the caller set
// none) name the account's own directories, from the account database and not from $HOME.
func cargoEnv(t *testing.T) []string {
	t.Helper()
	env := os.Environ()
	account, err := user.Current()
	if err != nil {
		t.Fatalf("the current account: %v", err)
	}
	for key, dir := range map[string]string{"RUSTUP_HOME": ".rustup", "CARGO_HOME": ".cargo"} {
		if os.Getenv(key) == "" {
			env = append(env, key+"="+filepath.Join(account.HomeDir, dir))
		}
	}
	return env
}

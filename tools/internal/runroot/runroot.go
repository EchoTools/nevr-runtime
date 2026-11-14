// Package runroot makes and removes the run root of one test binary run (ADR 0044; DESIGN.md P0b).
//
// The work root (WorkRoot) is where every tool of this repository keeps what it writes outside the checkout. A test
// run gets one run root under it, <work root>/run-roots/<label>-<UTC stamp>-<pid>, made by Make at the start of the
// run and removed by Remove when the run ends. The work-root contract is the one of tools/work_root.py and
// tools/lib/work_root.sh: the same default, and a README.md that says what a directory is for, whose it is and when
// it is removed, written once and never overwritten.
//
// Remove is the one sanctioned recursive removal in the repository (ORDER.md, "The manifest rule"). Every other
// deleter removes only the entries it recorded making, one by name. A run root is not that: the harness never sees
// what the tests in it made, so it removes the whole tree once, and only after it has proved the path is a real
// directory inside the work root's real path, reached through no link.
package runroot

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"os/user"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

const (
	envWorkRoot = "NEVR_WORK_ROOT"
	runRoots    = "run-roots"

	// The README texts. The root's purpose and removed strings are the ones tools/work_root.py holds.
	rootPurpose     = "Working files of the nevr-runtime tools and the agents that run them."
	rootRemoved     = "each item when its named end comes; its creator removes it that turn."
	runRootsPurpose = "One run root per test binary run; the harness removes each once when its run ends."
	runRootsRemoved = "each run root when its run ends (run-root remove); a leftover from an interrupted run by run-root remove."
)

// The reasons Remove refuses. Each refusal wraps exactly one of these, so a caller and a test can tell them apart with
// errors.Is. A refusal removes nothing.
var (
	// ErrEmptyWorkRoot: the work root is the empty string.
	ErrEmptyWorkRoot = errors.New("work root is empty")
	// ErrPath: the run root path has a "." or ".." element, or a trailing separator, or is empty. Such a path is not
	// named the way Make names a run root, and filepath.Abs would clean it into a different path than the one given.
	ErrPath = errors.New("run root path is not a plain path (a . or .. element, a trailing separator, or empty)")
	// ErrMissing: the run root, the run-roots directory, or the work root does not exist.
	ErrMissing = errors.New("path does not exist")
	// ErrLink: the run root, or the run-roots directory, is a symbolic link where a real directory is required.
	ErrLink = errors.New("symbolic link where a real directory is required")
	// ErrNotDir: the run root, or the run-roots directory, is not a directory (a regular file, say).
	ErrNotDir = errors.New("not a directory")
	// ErrOutside: the run root is not run-roots/<name> directly under the work root as written, or its real path is not
	// directly inside the work root's real run-roots directory.
	ErrOutside = errors.New("run root is not directly inside the run-roots directory of the work root")
	// ErrName: the run root's base name does not match the grammar Make gives run roots.
	ErrName = errors.New("run root name does not match the run root name grammar")
)

var (
	labelPattern = regexp.MustCompile(`^[a-z0-9][a-z0-9-]{0,63}$`)
	// namePattern is the grammar of a run root's base name: <label>-<YYYYMMDDTHHMMSSZ>-<pid>, as Make writes it.
	namePattern = regexp.MustCompile(`^[a-z0-9][a-z0-9-]{0,63}-\d{8}T\d{6}Z-\d+$`)
)

// WorkRoot is $NEVR_WORK_ROOT, else $HOME/src/nevr-runtime-work. It reads the environment through getenv, so a test
// can supply its own. With neither set it is an error: a tool does not guess a home directory.
func WorkRoot(getenv func(string) string) (string, error) {
	if v := getenv(envWorkRoot); v != "" {
		return v, nil
	}
	home := getenv("HOME")
	if home == "" {
		return "", fmt.Errorf("HOME is not set and %s is not: set %s", envWorkRoot, envWorkRoot)
	}
	return filepath.Join(home, "src", "nevr-runtime-work"), nil
}

// Make creates <workRoot>/run-roots/<label>-<now in UTC, YYYYMMDDTHHMMSSZ>-<pid> and returns its path as made (not
// resolved). The label must match ^[a-z0-9][a-z0-9-]{0,63}$. The run root itself must be new: an existing path is an
// error. The work root and run-roots are created when missing, and each gets its README.md when it has none.
//
// The READMEs are written before the run root is made, so a README that cannot be written leaves no run root behind.
func Make(workRoot, label string, now time.Time, pid int) (string, error) {
	if !labelPattern.MatchString(label) {
		return "", fmt.Errorf("label %q must match %s", label, labelPattern)
	}
	if workRoot == "" {
		return "", ErrEmptyWorkRoot
	}
	runs := filepath.Join(workRoot, runRoots)
	if err := os.MkdirAll(runs, 0o755); err != nil {
		return "", fmt.Errorf("cannot make %s: %w", runs, err)
	}
	if err := writeReadme(workRoot, rootPurpose, rootRemoved); err != nil {
		return "", err
	}
	if err := writeReadme(runs, runRootsPurpose, runRootsRemoved); err != nil {
		return "", err
	}
	path := filepath.Join(runs, fmt.Sprintf("%s-%s-%d", label, now.UTC().Format("20060102T150405Z"), pid))
	if err := os.Mkdir(path, 0o755); err != nil {
		return "", fmt.Errorf("cannot make run root: %w", err)
	}
	return path, nil
}

// writeReadme writes dir/README.md in the work-root format, exclusively: an existing README is kept.
func writeReadme(dir, purpose, removed string) error {
	readme := filepath.Join(dir, "README.md")
	text := fmt.Sprintf("# %s\n\n%s\n\nOwner: %s\nRemoved: %s\n", filepath.Base(dir), purpose, owner(), removed)
	f, err := os.OpenFile(readme, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o644)
	if errors.Is(err, fs.ErrExist) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("cannot write %s: %w", readme, err)
	}
	if _, err := f.WriteString(text); err != nil {
		f.Close()
		return fmt.Errorf("cannot write %s: %w", readme, err)
	}
	if err := f.Close(); err != nil {
		return fmt.Errorf("cannot write %s: %w", readme, err)
	}
	return nil
}

// owner is the user the README names. It asks the environment first, in the order Python's getpass does, so the
// READMEs of the Go and Python tools agree.
func owner() string {
	for _, k := range []string{"LOGNAME", "USER", "LNAME", "USERNAME"} {
		if v := os.Getenv(k); v != "" {
			return v
		}
	}
	if u, err := user.Current(); err == nil && u.Username != "" {
		return u.Username
	}
	return "the user who ran the tool"
}

// refuse is the error of a refusal: it wraps the sentinel that names the reason and says what was seen.
func refuse(reason error, format string, args ...any) error {
	return fmt.Errorf("refused: %w: %s", reason, fmt.Sprintf(format, args...))
}

// hasPlainElements reports whether p has no "." or ".." element and no trailing separator.
func hasPlainElements(p string) bool {
	if p == "" || strings.HasSuffix(p, "/") || strings.HasSuffix(p, string(filepath.Separator)) {
		return false
	}
	for _, seg := range strings.Split(filepath.ToSlash(p), "/") {
		if seg == "." || seg == ".." {
			return false
		}
	}
	return true
}

// Remove removes runRoot, a run root Make made under workRoot, and nothing else. It refuses, with an error that wraps
// the sentinel naming the reason (ErrEmptyWorkRoot, ErrPath, ErrMissing, ErrLink, ErrNotDir, ErrOutside, ErrName), and
// removes nothing, unless all of these hold, in this order:
//
//  0. workRoot is not empty; runRoot has no "." or ".." element and no trailing separator.
//     a. runRoot exists, is a directory, and is not a symbolic link.
//     b. runRoot is run-roots/<base> under workRoot as written (no link or ".." between them), <base> matches the
//     run-root name grammar, run-roots under workRoot is a real directory (not a link) whose real path is the real
//     run-roots of the work root, and runRoot's real path is that directory's child <base>.
//     c. that real path is strictly inside the real run-roots directory, and is not that directory.
//
// A work root that is itself a symbolic link to a real directory is accepted: the removal happens inside that
// directory's real run-roots, and only there. The work root is where the user keeps their own files, so the one link
// the check does not refuse is the one the user chose.
//
// Only then does it call os.RemoveAll. On Linux that walks with openat and unlinkat and does not follow links
// (/usr/lib/go/src/os/removeall_at.go), so a link that a run made inside the run root is unlinked, and its target
// survives. This is the one sanctioned recursive removal of the repository.
//
// Residual race: the checks are made by path, and os.RemoveAll then opens its parent by path. If run-roots, or the
// work root, is replaced by a link between the checks and the removal, RemoveAll opens the parent through that link
// (removeall_at.go:46) and removes a same-named directory there. Closing that needs the parent opened once and held
// (openat from a directory descriptor), which the standard library does not offer for RemoveAll. The exposure needs
// write access to the user's own work root, and is accepted for this tool.
func Remove(workRoot, runRoot string) error {
	if workRoot == "" {
		return fmt.Errorf("refused: %w", ErrEmptyWorkRoot)
	}
	if !hasPlainElements(runRoot) {
		return refuse(ErrPath, "%q", runRoot)
	}
	absW, err := filepath.Abs(workRoot)
	if err != nil {
		return fmt.Errorf("refused: work root %s: %w", workRoot, err)
	}
	absR, err := filepath.Abs(runRoot)
	if err != nil {
		return fmt.Errorf("refused: run root %s: %w", runRoot, err)
	}

	// a. The run root itself: no link, a directory.
	fi, err := os.Lstat(absR)
	if errors.Is(err, fs.ErrNotExist) {
		return refuse(ErrMissing, "run root %s", absR)
	}
	if err != nil {
		return fmt.Errorf("refused: run root %s: %w", absR, err)
	}
	if fi.Mode()&fs.ModeSymlink != 0 {
		return refuse(ErrLink, "run root %s", absR)
	}
	if !fi.IsDir() {
		return refuse(ErrNotDir, "run root %s", absR)
	}

	// b. The path, lexically, is exactly workRoot/run-roots/<base>, and the names match the grammar.
	base := filepath.Base(absR)
	rel, err := filepath.Rel(absW, absR)
	if err != nil || rel != filepath.Join(runRoots, base) {
		return refuse(ErrOutside, "run root %s is not %s/<name> directly under the work root %s", absR, runRoots, absW)
	}
	if !namePattern.MatchString(base) {
		return refuse(ErrName, "%q does not match %s", base, namePattern)
	}
	realW, err := filepath.EvalSymlinks(absW)
	if errors.Is(err, fs.ErrNotExist) {
		return refuse(ErrMissing, "work root %s", absW)
	}
	if err != nil {
		return fmt.Errorf("refused: work root %s: %w", absW, err)
	}
	tr := filepath.Join(absW, runRoots)
	trFi, err := os.Lstat(tr)
	if errors.Is(err, fs.ErrNotExist) {
		return refuse(ErrMissing, "%s", tr)
	}
	if err != nil {
		return fmt.Errorf("refused: %s: %w", tr, err)
	}
	if trFi.Mode()&fs.ModeSymlink != 0 {
		return refuse(ErrLink, "%s", tr)
	}
	if !trFi.IsDir() {
		return refuse(ErrNotDir, "%s", tr)
	}
	realRuns, err := filepath.EvalSymlinks(tr)
	if err != nil {
		return fmt.Errorf("refused: %s: %w", tr, err)
	}
	if realRuns != filepath.Join(realW, runRoots) {
		return refuse(ErrOutside, "%s resolves to %s, not inside the work root's real path %s", tr, realRuns, realW)
	}
	realR, err := filepath.EvalSymlinks(absR)
	if err != nil {
		return fmt.Errorf("refused: run root %s: %w", absR, err)
	}
	if filepath.Dir(realR) != realRuns || filepath.Base(realR) != base {
		return refuse(ErrOutside, "run root %s resolves to %s, outside %s", absR, realR, realRuns)
	}

	// c. Strictly inside the real run-roots directory.
	if !strings.HasPrefix(realR, realRuns+string(filepath.Separator)) || realR == realRuns {
		return refuse(ErrOutside, "run root %s is not strictly inside %s", realR, realRuns)
	}

	// d. The one removal.
	if err := os.RemoveAll(absR); err != nil {
		return fmt.Errorf("run root %s: remove: %w", absR, err)
	}
	return nil
}

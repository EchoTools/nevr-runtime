package policy

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// ReasonMarker opens the line in a Python file's header that records why the file is Python (ADR 0044, rule 2):
//
//	# ADR 0044 recorded reason: <the reason, in words>
//
// What counts as a reason is the ADR's rule 2, not this file's.
const ReasonMarker = "ADR 0044 recorded reason:"

// HeaderLines is how many lines from the top of a file count as its header.
const HeaderLines = 20

// HasRecordedReason reports whether the header of a Python source (its first HeaderLines lines) carries ReasonMarker
// followed by a reason of at least MinReasonLen characters.
func HasRecordedReason(src string) bool {
	lines := strings.SplitN(src, "\n", HeaderLines+1)
	if len(lines) > HeaderLines {
		lines = lines[:HeaderLines]
	}
	for _, line := range lines {
		_, reason, found := strings.Cut(line, ReasonMarker)
		if found && len(strings.TrimSpace(strings.TrimRight(reason, "\"'"))) >= MinReasonLen {
			return true
		}
	}
	return false
}

// ReasonedFiles returns the files of paths (relative to root) whose header carries a recorded reason. A file that cannot
// be read is not reasoned.
func ReasonedFiles(root string, paths []string) []string {
	var out []string
	for _, p := range paths {
		if src, err := os.ReadFile(filepath.Join(root, p)); err == nil && HasRecordedReason(string(src)) {
			out = append(out, p)
		}
	}
	return out
}

// AddedPython lists the Python files under tools/ that this change adds relative to the merge-base of HEAD and
// origin/main: committed, staged, unstaged or untracked. A renamed file is not an addition. skipped is a reason when
// there is no origin/main to compare with; err is set when origin/main exists but shares no history with HEAD (a shallow
// clone), which must not look like a pass.
func AddedPython(root string, env []string) (added []string, skipped string, err error) {
	git := func(args ...string) (string, error) {
		cmd := exec.Command("git", args...)
		cmd.Dir, cmd.Env = root, env
		out, err := cmd.Output()
		return strings.TrimSpace(string(out)), err
	}
	if _, err := git("rev-parse", "--verify", "--quiet", "refs/remotes/origin/main"); err != nil {
		return nil, "there is no origin/main in this clone: added Python files cannot be found; they are checked wherever origin/main exists", nil
	}
	base, err := git("merge-base", "HEAD", "origin/main")
	if err != nil || base == "" {
		return nil, "", fmt.Errorf("origin/main exists but git merge-base HEAD origin/main found nothing (%v): the history is too shallow to find the Python files this change adds; fetch more history (git fetch --deepen=100 origin main, or --unshallow)", err)
	}
	for _, args := range [][]string{
		{"diff", "--name-only", "-z", "--diff-filter=A", base, "--", "tools/*.py"},
		{"ls-files", "--others", "-z", "--exclude-standard", "--", "tools/*.py"},
	} {
		out, err := git(args...)
		if err != nil {
			return nil, "", fmt.Errorf("git %s in %s: %w", strings.Join(args, " "), root, err)
		}
		for _, f := range strings.Split(out, "\x00") {
			if f != "" {
				added = append(added, f)
			}
		}
	}
	return added, "", nil
}

// WithoutReason returns the files of added (relative to root) whose header carries no recorded reason.
func WithoutReason(root string, added []string) []string {
	have := toSet(ReasonedFiles(root, added))
	var out []string
	for _, f := range added {
		if !have[f] {
			out = append(out, f)
		}
	}
	return out
}

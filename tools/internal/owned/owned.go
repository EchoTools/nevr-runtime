// Package owned removes only what a tool recorded making (ORDER.md, "The manifest rule"). It is the Go twin of
// tools/owned_remove.py: the same entry grammar, the same refusals, and the same checked tree removal for a build tree.
//
// RemoveOwned removes each recorded entry by name, one at a time: a file or a link is unlinked, a folder is removed
// only when it is empty (Outcome NotEmpty otherwise, and what is in it stays). A parent component that is a link is
// refused, and nothing is recursed into.
//
// RemoveChecked is the recursive removal allowed for a build tree: it removes path only when its real path is strictly
// inside the real path of allowedParent, and no component between the two is a symbolic link. It is not for a tool's
// own scratch under the work root, which is runroot.Remove.
//
// Limits, stated: the checks are by path. A component replaced between the check and the removal is not caught.
package owned

import (
	"errors"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"strings"
	"syscall"
)

// Outcome names what happened to one recorded entry.
type Outcome string

const (
	Removed            Outcome = "removed"
	Absent             Outcome = "absent"
	NotEmpty           Outcome = "not_empty"
	RefusedName        Outcome = "refused_name"
	RefusedLink        Outcome = "refused_link"
	RefusedOutsideRoot Outcome = "refused_outside_root"
	Failed             Outcome = "failed"
)

// Result is what RemoveOwned did with one entry. Err is set for Failed and RefusedLink.
type Result struct {
	Entry   string
	Outcome Outcome
	Err     error
}

// Plain reports whether entry is a name a tool may record: the grammar of src/core/owned_remove_rules.h ValidEntry
// (the same as installer/src/util.rs valid_entry, plus the control bytes the C++ header refuses): non-empty, not
// absolute, no byte below 0x20 or equal to 0x7F, no backslash, no ':', and no empty, "." or ".." component.
func Plain(entry string) bool {
	if entry == "" || strings.HasPrefix(entry, "/") || strings.ContainsAny(entry, "\\:") {
		return false
	}
	for i := 0; i < len(entry); i++ {
		if entry[i] < 0x20 || entry[i] == 0x7f {
			return false
		}
	}
	for _, part := range strings.Split(entry, "/") {
		if part == "" || part == "." || part == ".." {
			return false
		}
	}
	return true
}

func isLink(fi fs.FileInfo) bool { return fi.Mode()&fs.ModeSymlink != 0 }

// RemoveOwned removes the recorded entries under root, in the order given, and reports one Result per entry.
func RemoveOwned(root string, entries []string) []Result {
	results := make([]Result, 0, len(entries))
	if strings.IndexFunc(root, func(r rune) bool { return r < 0x20 || r == 0x7f }) >= 0 {
		for _, e := range entries {
			results = append(results, Result{Entry: e, Outcome: RefusedName, Err: fmt.Errorf("root has a control byte")})
		}
		return results
	}
	rootInfo, err := os.Lstat(root)
	if err == nil && isLink(rootInfo) {
		for _, e := range entries {
			results = append(results, Result{Entry: e, Outcome: RefusedLink, Err: fmt.Errorf("%s is a symbolic link", root)})
		}
		return results
	}
	for _, e := range entries {
		results = append(results, removeOne(root, e))
	}
	return results
}

func removeOne(root, entry string) Result {
	if !Plain(entry) {
		return Result{Entry: entry, Outcome: RefusedName}
	}
	parts := strings.Split(entry, "/")
	current := root
	rootDev, rootOK := deviceOf(root)
	for _, part := range parts[:len(parts)-1] {
		current = filepath.Join(current, part)
		fi, err := os.Lstat(current)
		if err == nil && isLink(fi) {
			return Result{Entry: entry, Outcome: RefusedLink, Err: fmt.Errorf("%s is a symbolic link", current)}
		}
		if err == nil && rootOK {
			if dev, ok := deviceOfInfo(fi); ok && dev != rootDev {
				return Result{Entry: entry, Outcome: RefusedOutsideRoot, Err: fmt.Errorf("%s is on another device", current)}
			}
		}
	}
	target := filepath.Join(append([]string{root}, parts...)...)
	if _, err := os.Lstat(target); errors.Is(err, fs.ErrNotExist) {
		return Result{Entry: entry, Outcome: Absent}
	} else if err != nil {
		return Result{Entry: entry, Outcome: Failed, Err: err}
	}
	// A folder is removed only when empty (os.Remove), and a file or a link is unlinked, never followed.
	if err := os.Remove(target); err != nil {
		if errors.Is(err, syscall.ENOTEMPTY) || errors.Is(err, syscall.EEXIST) {
			return Result{Entry: entry, Outcome: NotEmpty, Err: err}
		}
		return Result{Entry: entry, Outcome: Failed, Err: err}
	}
	return Result{Entry: entry, Outcome: Removed}
}

// RemoveChecked removes the build tree at path, recursively, if and only if its real path is strictly inside the real
// path of allowedParent and no component from allowedParent down to path is a symbolic link. A missing path is not an
// error. Otherwise it returns an error and removes nothing.
func RemoveChecked(allowedParent, path string) error {
	parent, err := filepath.Abs(allowedParent)
	if err != nil {
		return err
	}
	target, err := filepath.Abs(path)
	if err != nil {
		return err
	}
	if fi, err := os.Lstat(parent); err == nil && isLink(fi) {
		return fmt.Errorf("refused: %s is a symbolic link", parent)
	}
	rel, err := filepath.Rel(parent, target)
	if err != nil || rel == "." || rel == ".." || strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
		return fmt.Errorf("refused: %s is not inside %s", target, parent)
	}
	current := parent
	for _, part := range strings.Split(rel, string(filepath.Separator)) {
		current = filepath.Join(current, part)
		fi, err := os.Lstat(current)
		if errors.Is(err, fs.ErrNotExist) {
			return nil
		}
		if err != nil {
			return err
		}
		if isLink(fi) {
			return fmt.Errorf("refused: %s is a symbolic link", current)
		}
	}
	realParent, err := filepath.EvalSymlinks(parent)
	if err != nil {
		return err
	}
	realTarget, err := filepath.EvalSymlinks(target)
	if err != nil {
		return err
	}
	if realTarget == realParent || !strings.HasPrefix(realTarget, realParent+string(filepath.Separator)) {
		return fmt.Errorf("refused: real path %s is not strictly inside %s", realTarget, realParent)
	}
	return os.RemoveAll(target)
}

//go:build unix

package owned

import (
	"io/fs"
	"syscall"
)

// deviceOf reports the device of the directory at path (following nothing: the root is checked by the caller).
func deviceOf(path string) (uint64, bool) {
	var st syscall.Stat_t
	if err := syscall.Stat(path, &st); err != nil {
		return 0, false
	}
	return uint64(st.Dev), true
}

// deviceOfInfo reports the device a FileInfo was read from.
func deviceOfInfo(fi fs.FileInfo) (uint64, bool) {
	st, ok := fi.Sys().(*syscall.Stat_t)
	if !ok {
		return 0, false
	}
	return uint64(st.Dev), true
}

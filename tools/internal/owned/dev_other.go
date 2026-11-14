//go:build !unix

package owned

import "io/fs"

// Without a device number the crossing check is not made on this platform; the link checks still are.
func deviceOf(string) (uint64, bool) { return 0, false }

func deviceOfInfo(fs.FileInfo) (uint64, bool) { return 0, false }

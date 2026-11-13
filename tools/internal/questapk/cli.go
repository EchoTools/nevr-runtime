package questapk

import (
	"crypto/sha256"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
)

// LogTag is the tag of the version tool's lines (the Python tool's NEVR.QUEST.APK).
const LogTag = "NEVR.QUEST.APK"

// LabelTag is the tag of the label tool's lines.
const LabelTag = "NEVR.QUEST.LABEL"

// Emit writes one JSON log line to w: the tag and the event, then the fields, in that order.
func Emit(w io.Writer, tag, event string, fields ...Field) {
	line, err := Line(append([]Field{{Key: "tag", Value: tag}, {Key: "event", Value: event}}, fields...)...)
	if err != nil {
		fmt.Fprintf(w, "{\"tag\":%q,\"event\":%q,\"encode_error\":%q}\n", tag, event, err.Error())
		return
	}
	fmt.Fprintln(w, line)
}

// ParseMixed parses args with fl, taking flags and positional arguments in any order, as argparse does, and returns the
// positional arguments. It stops at the first error fl reports.
func ParseMixed(fl *flag.FlagSet, args []string) ([]string, error) {
	var pos []string
	for {
		if err := fl.Parse(args); err != nil {
			return nil, err
		}
		rest := fl.Args()
		if len(rest) == 0 {
			return pos, nil
		}
		pos = append(pos, rest[0])
		args = rest[1:]
	}
}

// MajorOf is the major of a version that parses, and whether it does.
func MajorOf(version string) (int, bool) {
	p, ok := parseParts(version)
	return p.major, ok
}

// Sha256Hex is the lower-case hex sha256 of data.
func Sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// NewestSource is the newest release of the repository at repo, read from git when a stamp asks for the floor.
func NewestSource(repo string) ReleaseSource {
	return func() (string, int, error) { return NewestRelease(repo, nil) }
}

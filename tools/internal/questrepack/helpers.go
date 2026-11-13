package questrepack

import (
	"archive/zip"
	"bytes"
	"crypto/md5" //nolint:gosec // the runtime's checksum list is md5 by design (#1737)
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
)

// zipArchive is a zip file opened for reading, with its entries by name. A name that occurs twice takes the last
// entry, as Python's zipfile does when it opens a name.
type zipArchive struct {
	rc     *zip.ReadCloser
	byName map[string]*zip.File
}

func openZip(path string) (*zipArchive, error) {
	rc, err := zip.OpenReader(path)
	if err != nil {
		return nil, err
	}
	z := &zipArchive{rc: rc, byName: make(map[string]*zip.File, len(rc.File))}
	for _, f := range rc.File {
		z.byName[f.Name] = f
	}
	return z, nil
}

func (z *zipArchive) Close() error { return z.rc.Close() }

// names returns every entry name in archive order (directories included).
func (z *zipArchive) names() []string {
	out := make([]string, 0, len(z.rc.File))
	for _, f := range z.rc.File {
		out = append(out, f.Name)
	}
	return out
}

func (z *zipArchive) has(name string) bool {
	_, ok := z.byName[name]
	return ok
}

// readAll returns the bytes of the entry name.
func (z *zipArchive) readAll(name string) ([]byte, error) {
	f, ok := z.byName[name]
	if !ok {
		return nil, fmt.Errorf("no entry %s", name)
	}
	rc, err := f.Open()
	if err != nil {
		return nil, err
	}
	defer rc.Close()
	return io.ReadAll(rc)
}

// entryDigest is the sha-256 of the entry name, read through the archive.
func (z *zipArchive) entryDigest(name string) (string, error) {
	f, ok := z.byName[name]
	if !ok {
		return "", fmt.Errorf("no entry %s", name)
	}
	return streamDigest(f)
}

// entryMD5 is the md5 of the entry name, read through the archive.
func (z *zipArchive) entryMD5(name string) (string, error) {
	f, ok := z.byName[name]
	if !ok {
		return "", fmt.Errorf("no entry %s", name)
	}
	rc, err := f.Open()
	if err != nil {
		return "", err
	}
	defer rc.Close()
	h := md5.New() //nolint:gosec // see the import note
	if _, err := io.Copy(h, rc); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func streamDigest(f *zip.File) (string, error) {
	rc, err := f.Open()
	if err != nil {
		return "", err
	}
	defer rc.Close()
	h := sha256.New()
	if _, err := io.Copy(h, rc); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// fileDigest is the sha-256 of the file at path.
func fileDigest(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

func sha256Hex(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// libEntries is every file entry under lib/, in archive order.
func libEntries(z *zipArchive) []string {
	var out []string
	for _, name := range z.names() {
		if strings.HasPrefix(name, "lib/") && !strings.HasSuffix(name, "/") {
			out = append(out, name)
		}
	}
	return out
}

// pyRepr is Python's repr of a string, for the messages that quote one: single quotes, and the escapes repr writes.
func pyRepr(s string) string {
	var b strings.Builder
	b.WriteByte('\'')
	for _, r := range s {
		switch {
		case r == '\\':
			b.WriteString(`\\`)
		case r == '\'':
			b.WriteString(`\'`)
		case r == '\n':
			b.WriteString(`\n`)
		case r == '\r':
			b.WriteString(`\r`)
		case r == '\t':
			b.WriteString(`\t`)
		case r < asciiControlMax || r == ascii7DEL:
			fmt.Fprintf(&b, `\x%02x`, r)
		default:
			b.WriteRune(r)
		}
	}
	b.WriteByte('\'')
	return b.String()
}

// pyList is Python's repr of a list of strings: ['a', 'b'].
func pyList(items []string) string {
	quoted := make([]string, len(items))
	for i, s := range items {
		quoted[i] = pyRepr(s)
	}
	return "[" + strings.Join(quoted, ", ") + "]"
}

// readLines is the non-empty stripped lines of a text file, as Python's read_text().splitlines() with a strip gives.
func readLines(path string) ([]string, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var out []string
	for _, line := range strings.Split(string(raw), "\n") {
		line = strings.TrimSpace(line)
		if line != "" {
			out = append(out, line)
		}
	}
	return out, nil
}

// marshalSorted is json.dumps(value, sort_keys=True, separators=(",", ":")) with a trailing newline, as bytes.
func marshalSorted(value any) ([]byte, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetEscapeHTML(false)
	if err := enc.Encode(value); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// sortedNames returns the names of a set, sorted.
func sortedNames(set map[string]bool) []string {
	out := make([]string, 0, len(set))
	for k := range set {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

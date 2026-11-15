package questlifts

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

const (
	// ScriptCount is how many scripts EchoCombat changes: 14 of the game's and the four its tools make.
	ScriptCount = 18
	// tableHeading opens the section whose table lists the scripts.
	tableHeading = "## The lifts"
	// scriptHashHex is the length of a script's CSymbol64 hash in hex.
	scriptHashHex = 16
	// columnsBeforeDigests is the cells before the two digests: the hash and the PC name.
	columnsBeforeDigests = 2
	// digestColumns is Cat's image digest, then the lift's.
	digestColumns = 2
)

var (
	digestHex  = hex.EncodedLen(sha256.Size)
	rowStart   = regexp.MustCompile("^\\| `([^`]*)` \\|")
	scriptRe   = regexp.MustCompile(fmt.Sprintf("^[0-9a-f]{%d}$", scriptHashHex))
	digestRe   = regexp.MustCompile(fmt.Sprintf("^`[0-9a-f]{%d}`$", digestHex))
	nextHeader = regexp.MustCompile(`^## `)
)

// Row is one script of the table.
type Row struct {
	Script      string // 16 lowercase hex: the script's hash, lib<Script>.so on Quest
	ImageSHA256 string // Cat's x64 image
	LiftSHA256  string // the reference build's aarch64 lift
	Line        int    // 1-based line in the page
}

// Parse returns the rows of the table under tableHeading, in page order.
func Parse(page string) ([]Row, error) {
	lines := strings.Split(page, "\n")
	start := -1
	for i, l := range lines {
		if strings.TrimSpace(l) == tableHeading {
			start = i + 1
			break
		}
	}
	if start < 0 {
		return nil, fmt.Errorf("no %q section", tableHeading)
	}
	var rows []Row
	var errs []error
	seen := map[string]int{}
	for i := start; i < len(lines) && !nextHeader.MatchString(lines[i]); i++ {
		m := rowStart.FindStringSubmatch(lines[i])
		if m == nil {
			continue
		}
		line := i + 1
		script := m[1]
		if !scriptRe.MatchString(script) {
			errs = append(errs, fmt.Errorf("line %d: script %q is not %d lowercase hex", line, script, scriptHashHex))
			continue
		}
		cells := strings.Split(strings.Trim(lines[i], "|"), "|")
		if len(cells) < columnsBeforeDigests+digestColumns {
			errs = append(errs, fmt.Errorf("line %d: script %s has %d cells, fewer than %d", line, script, len(cells),
				columnsBeforeDigests+digestColumns))
			continue
		}
		digests := make([]string, digestColumns)
		bad := false
		for k := range digestColumns {
			c := strings.TrimSpace(cells[columnsBeforeDigests+k])
			if !digestRe.MatchString(c) {
				errs = append(errs, fmt.Errorf("line %d: script %s: cell %d %q is not a backticked %d-hex sha256", line, script,
					columnsBeforeDigests+k+1, c, digestHex))
				bad = true
				continue
			}
			digests[k] = strings.Trim(c, "`")
		}
		if bad {
			continue
		}
		if first, ok := seen[script]; ok {
			errs = append(errs, fmt.Errorf("line %d: script %s is named again (first at line %d)", line, script, first))
			continue
		}
		seen[script] = line
		rows = append(rows, Row{Script: script, ImageSHA256: digests[0], LiftSHA256: digests[1], Line: line})
	}
	if len(errs) == 0 && len(rows) != ScriptCount {
		errs = append(errs, fmt.Errorf("the table has %d scripts, want %d", len(rows), ScriptCount))
	}
	if len(errs) > 0 {
		return nil, errors.Join(errs...)
	}
	return rows, nil
}

// CheckLibs requires dir/lib<Script>.so to exist for every row and to hash to the row's LiftSHA256.
func CheckLibs(dir string, rows []Row) error {
	var errs []error
	for _, r := range rows {
		path := filepath.Join(dir, "lib"+r.Script+".so")
		b, err := os.ReadFile(path)
		if err != nil {
			errs = append(errs, fmt.Errorf("script %s (line %d): %w", r.Script, r.Line, err))
			continue
		}
		sum := sha256.Sum256(b)
		if got := hex.EncodeToString(sum[:]); got != r.LiftSHA256 {
			errs = append(errs, fmt.Errorf("script %s (line %d): %s has sha256 %s, the page says %s", r.Script, r.Line, path, got,
				r.LiftSHA256))
		}
	}
	return errors.Join(errs...)
}

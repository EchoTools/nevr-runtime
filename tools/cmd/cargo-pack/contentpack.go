package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const (
	contentJSON       = "content.json"
	contentFormat     = 1 // src/runtime/content/content_json.h, format 1
	manifestsDir      = "manifests"
	packagesDir       = "packages"
	decimalBase       = 10      // a package number is written in decimal
	packageNumberBits = 32      // and is a chunk record's 32-bit package field (radpack.Chunk.Package)
	maxContentJSON    = 1 << 20 // a content.json is a few KiB; a bound on a file read whole
)

var (
	scriptHex = regexp.MustCompile("^[0-9a-f]{16}$") // a script's name: the game's 16-hex file name
	sha256Hex = regexp.MustCompile("^[0-9a-f]{64}$")
)

var packageID = regexp.MustCompile(fmt.Sprintf("^[0-9a-f]{%d}$", hexDigits))

type contentManifest struct {
	Package       string   `json:"package"`
	StockPackages uint32   `json:"stock_packages"`
	File          string   `json:"file"`
	Packages      []string `json:"packages"`
}

type contentDoc struct {
	Format    int               `json:"format"`
	ID        string            `json:"id"`
	Manifests []contentManifest `json:"manifests"`
	Rekeys    []scriptDoc       `json:"rekeys"`
	Files     map[string]string `json:"files"`
}

// checkListedFile refuses a pack file whose sha-256 is not the one content.json lists for it (a file it does not list
// has no sha-256 to match).
func checkListedFile(dir, rel string, files map[string]string) error {
	f, err := os.Open(filepath.Join(dir, filepath.FromSlash(rel)))
	if err != nil {
		return err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return err
	}
	if got, want := hex.EncodeToString(h.Sum(nil)), files[rel]; got != want {
		return fmt.Errorf("%s hashes to %s, content.json lists %q", rel, got, want)
	}
	return nil
}

// packageNumber is n of "packages/<set>_<n>" in its canonical spelling (decimal digits, no leading zero, so the file the pack
// lists is the file the records are read from), and false for any other spelling.
func packageNumber(p, set string) (uint32, bool) {
	digits, ok := strings.CutPrefix(p, packagesDir+"/"+set+"_")
	if !ok {
		return 0, false
	}
	n, err := strconv.ParseUint(digits, decimalBase, packageNumberBits)
	return uint32(n), err == nil && strconv.FormatUint(n, decimalBase) == digits
}

// readContentDoc reads and decodes the pack's content.json, and refuses one that is not a format 1 pack that serves manifests.
func readContentDoc(dir string) (contentDoc, error) {
	raw, err := os.ReadFile(filepath.Join(dir, contentJSON))
	if err != nil {
		return contentDoc{}, err
	}
	if len(raw) > maxContentJSON {
		return contentDoc{}, fmt.Errorf("%s is %d bytes, more than %d", contentJSON, len(raw), maxContentJSON)
	}
	var doc contentDoc
	if err := json.Unmarshal(raw, &doc); err != nil {
		return contentDoc{}, fmt.Errorf("%s: %w", contentJSON, err)
	}
	if doc.Format != contentFormat || len(doc.Manifests) == 0 {
		return contentDoc{}, fmt.Errorf("%s: format %d with %d manifests is not a format %d pack that serves manifests", contentJSON, doc.Format, len(doc.Manifests), contentFormat)
	}
	return doc, nil
}

// contentPackScripts is the label's `scripts` list: the pack's `rekeys` of the label's platform, each a copy the runtime makes
// from a stock script on the player's machine (src/cdn/cdn_manifest.h). Only the entry is read; no script byte is in the pack
// or in the set. An entry whose name or stock source is not 16 lowercase hex, whose stock_sha256 is not 64 lowercase hex, whose
// copy is its own source, whose platform is unknown, or whose name is listed twice is refused. The list is sorted by name.
func contentPackScripts(dir, label, platformName string, log *slog.Logger) ([]scriptDoc, error) {
	if !labelRe.MatchString(label) {
		return nil, fmt.Errorf("label %q is not 1 to 48 of [a-z0-9_]", label)
	}
	if !platform[platformName] {
		return nil, fmt.Errorf("platform %q is not android-arm64 or windows-x86_64", platformName)
	}
	doc, err := readContentDoc(dir)
	if err != nil {
		return nil, err
	}
	var out []scriptDoc
	seen := map[string]bool{}
	for _, r := range doc.Rekeys {
		if !scriptHex.MatchString(r.Name) || !scriptHex.MatchString(r.StockSource) || !sha256Hex.MatchString(r.StockSha256) {
			return nil, fmt.Errorf("%s: rekey %q from %q (sha-256 %q) is not a 16-hex name, a 16-hex stock source and a 64-hex sha-256", contentJSON, r.Name, r.StockSource, r.StockSha256)
		}
		if !platform[r.Platform] {
			return nil, fmt.Errorf("%s: rekey %s names platform %q, not android-arm64 or windows-x86_64", contentJSON, r.Name, r.Platform)
		}
		if r.Name == r.StockSource {
			return nil, fmt.Errorf("%s: rekey %s is a copy of itself", contentJSON, r.Name)
		}
		if seen[r.Name+r.Platform] {
			return nil, fmt.Errorf("%s: rekey %s is listed twice for %s", contentJSON, r.Name, r.Platform)
		}
		seen[r.Name+r.Platform] = true
		if r.Platform == platformName {
			out = append(out, r)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	log.Info("pack scripts read", "label", label, "platform", platformName, "rekeys", len(doc.Rekeys), "listed", len(out))
	return out, nil
}

// contentPackRecords is the records a pack's own packages store: for each manifest of the pack, every resource whose chunk is
// in a listed package numbered from stock_packages on (a pack's packages include every record it adds or replaces, and may include
// others). Nothing of the pack is run; the scripts are not read.
func contentPackRecords(dir, label, platformName string, log *slog.Logger) ([]record, error) {
	if !labelRe.MatchString(label) {
		return nil, fmt.Errorf("label %q is not 1 to 48 of [a-z0-9_]", label)
	}
	if !platform[platformName] {
		return nil, fmt.Errorf("platform %q is not android-arm64 or windows-x86_64", platformName)
	}
	doc, err := readContentDoc(dir)
	if err != nil {
		return nil, err
	}
	chunks, err := radpack.NewChunks()
	if err != nil {
		return nil, err
	}
	defer chunks.Close()
	var recs []record
	index := map[string]int{} // key -> position in recs: a pack's sets may list the same record
	for _, m := range doc.Manifests {
		if !packageID.MatchString(m.Package) || m.File != manifestsDir+"/"+m.Package || len(m.Packages) == 0 {
			return nil, fmt.Errorf("%s: manifest entry %q is not a set id, its file %q, with packages", contentJSON, m.Package, m.File)
		}
		if err := checkListedFile(dir, m.File, doc.Files); err != nil {
			return nil, err
		}
		// A package is packages/<set>_<n> with n at least stock_packages (content_json.h): plain digits, so no part of the path
		// can be "." or "..", and never a stock package.
		listed := map[uint32]bool{}
		for _, p := range m.Packages {
			n, ok := packageNumber(p, m.Package)
			if !ok || n < m.StockPackages {
				return nil, fmt.Errorf("%s: package %q is not packages/%s_<n> with n at least stock_packages (%d)", contentJSON, p, m.Package, m.StockPackages)
			}
			listed[n] = true
			if err := checkListedFile(dir, p, doc.Files); err != nil {
				return nil, err
			}
		}
		set, _, err := radpack.OpenSet(dir, m.Package)
		if err != nil {
			return nil, err
		}
		plain := map[uint32][]byte{}
		added, duplicates := 0, 0
		for _, r := range set.Manifest.Resources {
			if int(r.Chunk) >= len(set.Manifest.Chunks) {
				return nil, fmt.Errorf("set %s: resource %016x.%016x names chunk %d of %d", m.Package, uint64(r.Type), uint64(r.Name), r.Chunk, len(set.Manifest.Chunks))
			}
			pkg := set.Manifest.Chunks[r.Chunk].Package
			if pkg < m.StockPackages {
				continue
			}
			if !listed[pkg] {
				return nil, fmt.Errorf("set %s: resource %016x.%016x is in package %d, which content.json does not list", m.Package, uint64(r.Type), uint64(r.Name), pkg)
			}
			p, ok := plain[r.Chunk]
			if !ok {
				var err error
				if p, err = chunks.Plain(set, int(r.Chunk)); err != nil {
					return nil, err
				}
				plain[r.Chunk] = p
			}
			if uint64(r.Offset)+uint64(r.Size) > uint64(len(p)) {
				return nil, fmt.Errorf("set %s: resource %016x.%016x runs past its chunk", m.Package, uint64(r.Type), uint64(r.Name))
			}
			data := p[r.Offset : r.Offset+r.Size]
			rec := record{label: label, platform: platformName, typ: fmt.Sprintf("0x%016x", uint64(r.Type)),
				name: fmt.Sprintf("0x%016x", uint64(r.Name)), align: r.Alignment, data: data, source: "set " + m.Package}
			// Two sets of the pack that store the same (type, name) are one record when their bytes and alignment are the same; the
			// game's search finds either, so a pack whose two copies differ is refused.
			if at, dup := index[rec.typ+rec.name]; dup {
				if prev := recs[at]; !bytes.Equal(prev.data, rec.data) || prev.align != rec.align {
					return nil, fmt.Errorf("%s %s is stored twice in the pack (%s and %s) with different bytes or alignment", rec.typ, rec.name, prev.source, rec.source)
				}
				duplicates++
				continue
			}
			index[rec.typ+rec.name] = len(recs)
			recs = append(recs, rec)
			added++
		}
		log.Info("pack set read", "set", m.Package, "stock_packages", m.StockPackages, "resources", len(set.Manifest.Resources), "added_or_replaced", added, "same_record_in_an_earlier_set", duplicates)
	}
	if len(recs) == 0 {
		return nil, errors.New("the pack's own packages store no record")
	}
	return recs, nil
}

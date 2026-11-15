// Command cargo-pack turns loose files into content-addressed zstd objects and a manifest in the schema of
// src/cdn/cdn_manifest.h (docs/adr/0045-cdn-texture-delivery.md), for objects that are made outside the game's packages
// (the lobby posters). cargo-extract does the same for the game's own packages; the frames and the manifest are the same
// shape, so cargo-publish sends either.
//
//	cargo-pack -spec <spec.json> -out <dir> -max-window <bytes>
//	cargo-pack -content-pack <dir> -label <label> -platform <platform> -out <dir> -max-window <bytes>
//
// The second form reads a content pack (docs/reference/content-packs.md: <dir> holds content.json, manifests/ and packages/)
// as data and makes one label of the records the pack's own packages store: every record whose chunk is in a listed package
// numbered from the manifest's stock_packages on (a pack's packages include every record it adds or replaces, and may include
// others). A record of 0 bytes goes to the manifest's empty list. The manifest and package files are checked against the sha-256 content.json lists for them; the
// pack's scripts are not records (the game loads them from files) and are left out, and no file of the pack is run.
//
// The spec is {"entries": [{"label", "platform", "type", "name", "file", "align", "stock_size"?}]}: `type` and `name` are
// "0x" hex strings, `file` a path relative to the spec, `stock_size` the stock buffer the art was made against (omit it when
// the stock buffer is not known). Each file becomes one zstd frame of its own, stored at <out>/blobs/sha256/<hex of the
// frame>, and one entry. The frame states its content size, carries no checksum and has a window of at most -max-window
// (the stock chunks' largest window; cargo-extract prints it as stock_frames.max_window). The manifest is written only as the
// zstd frame <out>/manifest.json.zst, which cargo-publish sends. A duplicate (platform, type, name), a non-power-of-two
// alignment or one above 4096, or an empty file is refused and nothing is written.
//
// Exit codes: 0 done; 1 refused; 2 usage; 74 the run log failed (internal/runlog).
package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"math/bits"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"sync"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const (
	schemaName      = "nevr-cargo-manifest"
	schemaVersion   = 1
	formatZstdFrame = "zstd-frame"
	maxAlignment    = 4096 // cdn_validate.h, rule alignment
	hexDigits       = 16   // the digits of a type or a name: a 64-bit value (cdn_format.h ParseHex64 takes 1 to 16)
	maxLabelBytes   = 48   // the longest label name a config file may name (src/quest/sentinel/quest_config.cpp IsAssetLabel)
	exitUsage       = 2    // the exit code for a bad command line
	filePerm        = 0o644
	dirPerm         = 0o755
	manifestName    = "manifest.json.zst"
)

var (
	hexName  = regexp.MustCompile(fmt.Sprintf("^0x[0-9a-f]{%d}$", hexDigits))
	labelRe  = regexp.MustCompile(fmt.Sprintf("^[a-z0-9_]{1,%d}$", maxLabelBytes))
	platform = map[string]bool{"android-arm64": true, "windows-x86_64": true}
)

type specEntry struct {
	Label     string  `json:"label"`
	Platform  string  `json:"platform"`
	Type      string  `json:"type"`
	Name      string  `json:"name"`
	File      string  `json:"file"`
	Align     uint32  `json:"align"`
	StockSize *uint64 `json:"stock_size,omitempty"`
}

type spec struct {
	Entries []specEntry `json:"entries"`
}

type entryDoc struct {
	Platform  string  `json:"platform"`
	Type      string  `json:"type"`
	Name      string  `json:"name"`
	Format    string  `json:"format"`
	Sha256    string  `json:"sha256"`
	Size      uint64  `json:"size"`
	Bytes     uint64  `json:"bytes"`
	Align     uint32  `json:"align"`
	StockSize *uint64 `json:"stock_size,omitempty"`
}

type labelDoc struct {
	Entries []entryDoc `json:"entries"`
}

type manifestDoc struct {
	Schema        string              `json:"schema"`
	SchemaVersion int                 `json:"schema_version"`
	Labels        map[string]labelDoc `json:"labels"`
	Empty         []emptyDoc          `json:"empty"`
}

// record is one buffer to publish: its key, its alignment and its bytes.
type record struct {
	label, platform, typ, name string
	align                      uint32
	data                       []byte
	stockSize                  *uint64
	source                     string // what to name in an error: a file of the spec, or the pack's set
}

type emptyDoc struct {
	Type  string `json:"type"`
	Name  string `json:"name"`
	Align uint32 `json:"align"`
}

type objectSummary struct {
	Label    string `json:"label"`
	Platform string `json:"platform"`
	Name     string `json:"name"`
	Type     string `json:"type"`
	Sha256   string `json:"sha256"`
	Size     uint64 `json:"size"`
	Bytes    uint64 `json:"bytes"`
}

type summary struct {
	Objects        []objectSummary `json:"objects"`
	EmptyRecords   int             `json:"empty_records"`
	ManifestSha256 string          `json:"manifest_sha256"`
	ManifestBytes  int             `json:"manifest_frame_bytes"`
	MaxWindow      uint64          `json:"max_window"`
}

func main() {
	os.Exit(runlog.Run("cargo-pack", os.Getenv, os.Stderr, func(log *slog.Logger) int {
		return run(os.Args[1:], os.Stdout, os.Stderr, log)
	}))
}

func run(args []string, stdout, stderr io.Writer, log *slog.Logger) int {
	fs := flag.NewFlagSet("cargo-pack", flag.ContinueOnError)
	fs.SetOutput(stderr)
	specPath := fs.String("spec", "", "the spec file")
	contentPack := fs.String("content-pack", "", "a content pack folder (content.json, manifests/, packages/)")
	label := fs.String("label", "", "the label of the records of a content pack")
	platformName := fs.String("platform", "", "the platform of the records of a content pack")
	out := fs.String("out", "", "the output directory")
	maxWindow := fs.Uint64("max-window", 0, "the largest zstd window a frame may ask for, in bytes")
	if err := fs.Parse(args); err != nil {
		return exitUsage
	}
	bySpec, byPack := *specPath != "", *contentPack != ""
	if bySpec == byPack || *out == "" || *maxWindow == 0 || fs.NArg() != 0 || (byPack && (*label == "" || *platformName == "")) {
		fmt.Fprintln(stderr, "usage: cargo-pack -spec <spec.json> -out <dir> -max-window <bytes>\n       cargo-pack -content-pack <dir> -label <label> -platform <platform> -out <dir> -max-window <bytes>")
		return exitUsage
	}
	var recs []record
	var err error
	if bySpec {
		recs, err = specRecords(*specPath)
	} else {
		recs, err = contentPackRecords(*contentPack, *label, *platformName, log)
	}
	var sum *summary
	if err == nil {
		sum, err = build(recs, *out, *maxWindow, log)
	}
	if err != nil {
		fmt.Fprintln(stderr, "cargo-pack:", err)
		log.Error("refused", "err", err.Error())
		return 1
	}
	line, err := json.Marshal(sum)
	if err != nil {
		fmt.Fprintln(stderr, "cargo-pack:", err)
		return 1
	}
	fmt.Fprintln(stdout, string(line))
	return 0
}

// encodeFrame is one zstd frame of data whose header states the content size. A file that fits the window is a
// single-segment frame (its window is its size); a larger one has the window descriptor and, being 256 bytes or more,
// states its size (TestEncodeFrameStatesTheSizeAndKeepsTheWindow reads the header). The encoder is given the window, so no
// frame asks for more.
func encodeFrame(data []byte, window uint64) ([]byte, error) {
	f, err := newFramer(window)
	if err != nil {
		return nil, err
	}
	defer f.close()
	return f.frame(data), nil
}

// framer holds the two encoders a set needs, made once: an encoder is expensive to make and cheap to reuse, and EncodeAll is
// safe to call from several goroutines.
type framer struct {
	window          uint64
	whole, windowed *zstd.Encoder
}

// maxEncodeWorkers bounds the records encoded at once and each encoder's own concurrency. Measured on the 65 MB EchoCombat pack:
// 4 workers took 2.0 s and 467 MB at peak, 32 took 2.2 s and 2.49 GB.
const maxEncodeWorkers = 4

func newFramer(window uint64) (*framer, error) {
	mk := func(single bool) (*zstd.Encoder, error) {
		return zstd.NewWriter(nil, zstd.WithEncoderLevel(zstd.SpeedBestCompression), zstd.WithWindowSize(int(window)),
			zstd.WithEncoderCRC(false), zstd.WithSingleSegment(single), zstd.WithEncoderConcurrency(maxEncodeWorkers))
	}
	whole, err := mk(true)
	if err != nil {
		return nil, err
	}
	windowed, err := mk(false)
	if err != nil {
		whole.Close()
		return nil, err
	}
	return &framer{window: window, whole: whole, windowed: windowed}, nil
}

func (f *framer) close() {
	f.whole.Close()
	f.windowed.Close()
}

func (f *framer) frame(data []byte) []byte {
	if uint64(len(data)) <= f.window {
		return f.whole.EncodeAll(data, nil)
	}
	return f.windowed.EncodeAll(data, nil)
}

// manifestWindow is the smallest power of two the encoder accepts that holds the whole manifest text.
func manifestWindow(textBytes int) uint64 {
	return max(uint64(1)<<bits.Len64(uint64(max(textBytes, 1))-1), zstd.MinWindowSize)
}

func checkEntry(e specEntry) error {
	if !labelRe.MatchString(e.Label) {
		return fmt.Errorf("label %q is not 1 to 48 of [a-z0-9_]", e.Label)
	}
	if !platform[e.Platform] {
		return fmt.Errorf("platform %q is not android-arm64 or windows-x86_64", e.Platform)
	}
	if !hexName.MatchString(e.Type) || !hexName.MatchString(e.Name) {
		return fmt.Errorf("type %q and name %q must each be 0x and 16 lowercase hex digits", e.Type, e.Name)
	}
	if e.Align == 0 || e.Align&(e.Align-1) != 0 || e.Align > maxAlignment {
		return fmt.Errorf("alignment %d is not a power of two of at most %d", e.Align, maxAlignment)
	}
	if e.File == "" || filepath.IsAbs(e.File) {
		return fmt.Errorf("file %q must be a path relative to the spec", e.File)
	}
	return nil
}

// specRecords reads the spec and the files it names.
func specRecords(specPath string) ([]record, error) {
	raw, err := os.ReadFile(specPath)
	if err != nil {
		return nil, err
	}
	var sp spec
	if err := json.Unmarshal(raw, &sp); err != nil {
		return nil, fmt.Errorf("spec: %w", err)
	}
	if len(sp.Entries) == 0 {
		return nil, errors.New("spec has no entries")
	}
	base := filepath.Dir(specPath)
	var recs []record
	for _, e := range sp.Entries {
		if err := checkEntry(e); err != nil {
			return nil, fmt.Errorf("%s %s: %w", e.Type, e.Name, err)
		}
		data, err := os.ReadFile(filepath.Join(base, e.File))
		if err != nil {
			return nil, err
		}
		if len(data) == 0 {
			return nil, fmt.Errorf("%s is empty: a record of 0 bytes belongs in the manifest's empty list", e.File)
		}
		recs = append(recs, record{label: e.Label, platform: e.Platform, typ: e.Type, name: e.Name, align: e.Align, data: data,
			stockSize: e.StockSize, source: e.File})
	}
	return recs, nil
}

// build frames every record, checks the set whole, and only then writes the objects and the manifest frame.
func build(recs []record, out string, maxWindow uint64, log *slog.Logger) (*summary, error) {
	type built struct {
		doc   entryDoc
		label string
		frame []byte
	}
	// Check the set whole first; nothing is encoded for a set that is refused.
	var todo []record
	var empties []emptyDoc
	seen := map[string]bool{}
	for _, r := range recs {
		if r.align == 0 || r.align&(r.align-1) != 0 || r.align > maxAlignment {
			return nil, fmt.Errorf("%s %s: alignment %d is not a power of two of at most %d", r.typ, r.name, r.align, maxAlignment)
		}
		key := r.platform + r.typ + r.name
		if seen[key] {
			return nil, fmt.Errorf("duplicate key %s %s %s", r.platform, r.typ, r.name)
		}
		seen[key] = true
		if len(r.data) == 0 {
			empties = append(empties, emptyDoc{Type: r.typ, Name: r.name, Align: r.align})
			continue
		}
		todo = append(todo, r)
	}
	fr, err := newFramer(maxWindow)
	if err != nil {
		return nil, err
	}
	defer fr.close()
	all := make([]built, len(todo))
	jobs := make(chan int)
	var wg sync.WaitGroup
	for w := 0; w < min(runtime.GOMAXPROCS(0), maxEncodeWorkers); w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := range jobs {
				r := todo[i]
				frame := fr.frame(r.data)
				h := sha256.Sum256(frame)
				all[i] = built{label: r.label, frame: frame, doc: entryDoc{Platform: r.platform, Type: r.typ, Name: r.name, Format: formatZstdFrame,
					Sha256: hex.EncodeToString(h[:]), Size: uint64(len(frame)), Bytes: uint64(len(r.data)), Align: r.align, StockSize: r.stockSize}}
			}
		}()
	}
	for i := range todo {
		jobs <- i
	}
	close(jobs)
	wg.Wait()
	if empties == nil {
		empties = []emptyDoc{}
	}
	man := manifestDoc{Schema: schemaName, SchemaVersion: schemaVersion, Labels: map[string]labelDoc{}, Empty: empties}
	sum := &summary{MaxWindow: maxWindow, EmptyRecords: len(empties)}
	for _, b := range all {
		l := man.Labels[b.label]
		l.Entries = append(l.Entries, b.doc)
		man.Labels[b.label] = l
		sum.Objects = append(sum.Objects, objectSummary{Label: b.label, Platform: b.doc.Platform, Name: b.doc.Name, Type: b.doc.Type,
			Sha256: b.doc.Sha256, Size: b.doc.Size, Bytes: b.doc.Bytes})
	}
	text, err := json.Marshal(man)
	if err != nil {
		return nil, err
	}
	mframe, err := encodeFrame(text, manifestWindow(len(text)))
	if err != nil {
		return nil, fmt.Errorf("manifest: %w", err)
	}
	blobs := filepath.Join(out, "blobs", "sha256")
	if err := os.MkdirAll(blobs, dirPerm); err != nil {
		return nil, err
	}
	for _, b := range all {
		if err := os.WriteFile(filepath.Join(blobs, b.doc.Sha256), b.frame, filePerm); err != nil {
			return nil, err
		}
	}
	if err := os.WriteFile(filepath.Join(out, manifestName), mframe, filePerm); err != nil {
		return nil, err
	}
	h := sha256.Sum256(mframe)
	sum.ManifestSha256 = hex.EncodeToString(h[:])
	sum.ManifestBytes = len(mframe)
	log.Info("packed", "objects", len(all), "empty_records", len(empties), "manifest_sha256", sum.ManifestSha256)
	return sum, nil
}

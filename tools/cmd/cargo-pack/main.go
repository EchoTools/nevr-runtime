// Command cargo-pack turns loose files into content-addressed zstd objects and a manifest in the schema of
// src/cdn/cdn_manifest.h (docs/adr/0045-cdn-texture-delivery.md), for objects that are made outside the game's packages
// (the lobby posters). cargo-extract does the same for the game's own packages; the frames and the manifest are the same
// shape, so cargo-publish sends either.
//
//	cargo-pack -spec <spec.json> -out <dir> -max-window <bytes>
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

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const (
	schemaName      = "nevr-cargo-manifest"
	schemaVersion   = 1
	formatZstdFrame = "zstd-frame"
	maxAlignment    = 4096 // cdn_validate.h, rule alignment
	filePerm        = 0o644
	dirPerm         = 0o755
	manifestName    = "manifest.json.zst"
)

var (
	hexName  = regexp.MustCompile(`^0x[0-9a-f]{16}$`)
	labelRe  = regexp.MustCompile(`^[a-z0-9_]{1,48}$`)
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
	Empty         []struct{}          `json:"empty"`
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
	out := fs.String("out", "", "the output directory")
	maxWindow := fs.Uint64("max-window", 0, "the largest zstd window a frame may ask for, in bytes")
	if err := fs.Parse(args); err != nil {
		return 2
	}
	if *specPath == "" || *out == "" || *maxWindow == 0 || fs.NArg() != 0 {
		fmt.Fprintln(stderr, "usage: cargo-pack -spec <spec.json> -out <dir> -max-window <bytes>")
		return 2
	}
	sum, err := pack(*specPath, *out, *maxWindow, log)
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
// states its size.
func encodeFrame(data []byte, window uint64) ([]byte, error) {
	single := uint64(len(data)) <= window
	enc, err := zstd.NewWriter(nil, zstd.WithEncoderLevel(zstd.SpeedBestCompression), zstd.WithWindowSize(int(window)),
		zstd.WithEncoderCRC(false), zstd.WithSingleSegment(single))
	if err != nil {
		return nil, err
	}
	defer enc.Close()
	frame := enc.EncodeAll(data, nil)
	var h zstd.Header
	if err := h.Decode(frame); err != nil {
		return nil, fmt.Errorf("the frame header does not decode: %w", err)
	}
	w := h.WindowSize
	if h.SingleSegment {
		w = h.FrameContentSize
	}
	if !h.HasFCS || h.FrameContentSize != uint64(len(data)) {
		return nil, fmt.Errorf("the frame header states content size %d (stated: %t), the file is %d bytes", h.FrameContentSize, h.HasFCS, len(data))
	}
	if w > window {
		return nil, fmt.Errorf("the frame window is %d bytes, larger than %d", w, window)
	}
	return frame, nil
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

func pack(specPath, out string, maxWindow uint64, log *slog.Logger) (*summary, error) {
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
	type built struct {
		doc   entryDoc
		label string
		frame []byte
	}
	var all []built
	seen := map[string]bool{}
	for _, e := range sp.Entries {
		if err := checkEntry(e); err != nil {
			return nil, fmt.Errorf("%s %s: %w", e.Type, e.Name, err)
		}
		key := e.Platform + e.Type + e.Name
		if seen[key] {
			return nil, fmt.Errorf("duplicate key %s %s %s", e.Platform, e.Type, e.Name)
		}
		seen[key] = true
		data, err := os.ReadFile(filepath.Join(base, e.File))
		if err != nil {
			return nil, err
		}
		if len(data) == 0 {
			return nil, fmt.Errorf("%s is empty: a record of 0 bytes belongs in the manifest's empty list", e.File)
		}
		frame, err := encodeFrame(data, maxWindow)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", e.File, err)
		}
		h := sha256.Sum256(frame)
		d := entryDoc{Platform: e.Platform, Type: e.Type, Name: e.Name, Format: formatZstdFrame, Sha256: hex.EncodeToString(h[:]),
			Size: uint64(len(frame)), Bytes: uint64(len(data)), Align: e.Align, StockSize: e.StockSize}
		all = append(all, built{doc: d, label: e.Label, frame: frame})
		log.Info("packed", "file", e.File, "label", e.Label, "platform", e.Platform, "bytes", d.Bytes, "frame_bytes", d.Size, "sha256", d.Sha256)
	}
	man := manifestDoc{Schema: schemaName, SchemaVersion: schemaVersion, Labels: map[string]labelDoc{}, Empty: []struct{}{}}
	sum := &summary{MaxWindow: maxWindow}
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
	return sum, nil
}

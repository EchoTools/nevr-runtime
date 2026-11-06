// Command levelpack turns a level export into a resource set (sdk/include/nevr/resource_set.h, plugin API v9,
// docs/reference/plugin-resources.md): the file a level plugin embeds and registers, so the runtime serves the levels
// from the plugin's own file and nothing is loaded into the game's packages or manifests.
//
// The input is an export written as a repacked package set: a manifest that is the game's manifest plus the export's
// records, and the package files it names. levelpack compares it with the game's stock manifests (and, for a stock
// record the repack moved, the stock bytes). A record the game does not have goes into the set. A stock record the
// repack moved but did not change is dropped. A stock record whose bytes changed is listed in the report, and goes into
// the set as an override only when -override names it. A stock record at the same place in the same chunk as in the
// stock manifest is taken as unchanged, and its bytes are not read.
//
// Before the set is kept, it is checked with the rules the runtime's parser applies, every frame is decoded to the
// length its chunk states, each repacked record and override is compared with the export's bytes, and the set is put
// through the checks the runtime's registry makes before it serves one: no script record, every texture with its
// payload, every archive readable, and every level's archives, through the set, listing only what the set or the stock
// manifests have. Pass every manifest the game loads at boot with -stock: a key of one left out counts as new.
//
//	levelpack -manifest <export manifest> -packages <dir> [-package N=<file>]... \
//	          -stock <stock manifest>... -stock-packages <dir> [-override 0x<type>:0x<name>]... \
//	          -out <set file> [-report <json>]
//
// A package file is <dir>/<set>_<n>, where <set> is the manifest file's name (16 hex digits); -package N=<file> names
// one directly. The tool reads its inputs and writes only -out and -report. Exit codes: 0 written and checked; 1 the
// input is not a level export it can turn into a set; 2 a usage error; 74 the run log failed (internal/runlog).
package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

// On-disk types (CSymbol64(<class> + "Win10")), the keys the game's manifests use (docs/engine/asset-system.md).
const (
	levelRootType = 0xe8e38d7781a338a6 // CGameLevelResourceWin10
	archiveType   = 0x2a41cf1c1d9e5d32 // CArchiveResourceWin10
)

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

func run(_ context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	return runlog.Run("levelpack", getenv, stderr, func(log *slog.Logger) int {
		return levelpack(args, stdout, stderr, log)
	})
}

type multi []string

func (m *multi) String() string     { return strings.Join(*m, ",") }
func (m *multi) Set(v string) error { *m = append(*m, v); return nil }

type options struct {
	manifest      string
	packagesDir   string
	packages      map[uint32]string
	stock         []string
	stockPackages string
	overrides     []key
	out           string
	report        string
}

var setName = regexp.MustCompile(`([0-9a-f]{16})$`)

func parseKey(s string) (key, error) {
	parts := strings.SplitN(s, ":", 2)
	if len(parts) != 2 {
		return key{}, fmt.Errorf("%q is not 0x<type>:0x<name>", s)
	}
	t, err1 := strconv.ParseUint(strings.TrimPrefix(parts[0], "0x"), 16, 64)
	n, err2 := strconv.ParseUint(strings.TrimPrefix(parts[1], "0x"), 16, 64)
	if err1 != nil || err2 != nil {
		return key{}, fmt.Errorf("%q is not 0x<type>:0x<name>", s)
	}
	return key{t, n}, nil
}

func parseOptions(args []string, stderr io.Writer) (options, error) {
	fs := flag.NewFlagSet("levelpack", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var o options
	var packages, stock, overrides multi
	fs.StringVar(&o.manifest, "manifest", "", "the export's manifest (the game's manifest plus the export's records)")
	fs.StringVar(&o.packagesDir, "packages", "", "the directory holding the export's package files, <set>_<n>")
	fs.Var(&packages, "package", "N=<file>: package N of the export's set, named directly")
	fs.Var(&stock, "stock", "a stock manifest the game loads at boot (repeat for each)")
	fs.StringVar(&o.stockPackages, "stock-packages", "", "the directory holding the stock package files, <set>_<n>")
	fs.Var(&overrides, "override", "0x<type>:0x<name>: an edited stock record to carry as an override")
	fs.StringVar(&o.out, "out", "", "the resource set to write")
	fs.StringVar(&o.report, "report", "", "the JSON report to write (default: <out>.json)")
	if err := fs.Parse(args); err != nil {
		return o, err
	}
	if o.manifest == "" || len(stock) == 0 || o.stockPackages == "" || o.out == "" {
		return o, errors.New("-manifest, -stock, -stock-packages and -out are required")
	}
	if o.report == "" {
		o.report = o.out + ".json"
	}
	o.stock = stock
	o.packages = map[uint32]string{}
	for _, p := range packages {
		n, path, ok := strings.Cut(p, "=")
		idx, err := strconv.ParseUint(n, 10, 32)
		if !ok || err != nil || path == "" {
			return o, fmt.Errorf("-package %q is not N=<file>", p)
		}
		o.packages[uint32(idx)] = path
	}
	for _, s := range overrides {
		k, err := parseKey(s)
		if err != nil {
			return o, err
		}
		o.overrides = append(o.overrides, k)
	}
	return o, nil
}

func setID(path string) (string, error) {
	m := setName.FindStringSubmatch(strings.ToLower(filepath.Base(path)))
	if m == nil {
		return "", fmt.Errorf("%s: the file's name does not end in a 16-hex-digit set id", path)
	}
	return m[1], nil
}

// stockEntry is a stock record and where its bytes are.
type stockEntry struct {
	rec      record
	chunk    chunk
	packages string // the stock set's package path prefix: <dir>/<set>_
}

type edited struct {
	Key       string `json:"key"`
	StockSize uint32 `json:"stock_size"`
	Size      uint32 `json:"size"`
	Included  bool   `json:"included"`
}

type report struct {
	Manifest             string   `json:"manifest"`
	Out                  string   `json:"out"`
	Bytes                uint64   `json:"bytes"`
	Sha256               string   `json:"sha256"`
	Records              int      `json:"records"`
	Chunks               int      `json:"chunks"`
	ChunksCopied         int      `json:"chunks_copied"`
	ChunksRepacked       int      `json:"chunks_repacked"`
	Overrides            int      `json:"overrides"`
	FramesDecoded        int      `json:"frames_decoded"`
	RecordsCompared      int      `json:"records_compared"`
	StockUnchanged       int      `json:"stock_unchanged"`
	StockCopiesDropped   int      `json:"stock_copies_dropped"`
	StockEdited          []edited `json:"stock_edited"`
	Levels               []string `json:"levels"`
	LevelsWithoutArchive []string `json:"levels_without_archive"`
	Problems             []string `json:"problems,omitempty"`
}

type reader struct {
	dec   *zstd.Decoder
	files map[string]*os.File
	// The last chunk decoded: a repack's moved stock records come chunk after chunk, several in one.
	lastPath  string
	lastChunk chunk
	lastPlain []byte
}

func (r *reader) open(path string) (*os.File, error) {
	if f, ok := r.files[path]; ok {
		return f, nil
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	r.files[path] = f
	return f, nil
}

// plain returns chunk c of the package at path, decompressed.
func (r *reader) plain(path string, c chunk) ([]byte, error) {
	if r.lastPlain != nil && r.lastPath == path && r.lastChunk == c {
		return r.lastPlain, nil
	}
	f, err := r.open(path)
	if err != nil {
		return nil, err
	}
	frame := make([]byte, c.Compressed)
	if _, err := f.ReadAt(frame, int64(c.FileOffset)); err != nil {
		return nil, fmt.Errorf("%s at %d: %w", path, c.FileOffset, err)
	}
	out, err := r.dec.DecodeAll(frame, make([]byte, 0, c.Decompressed))
	if err != nil {
		return nil, fmt.Errorf("%s at %d: the frame does not decode: %w", path, c.FileOffset, err)
	}
	if uint32(len(out)) != c.Decompressed {
		return nil, fmt.Errorf("%s at %d: the frame decodes to %d bytes, the manifest says %d", path, c.FileOffset, len(out), c.Decompressed)
	}
	r.lastPath, r.lastChunk, r.lastPlain = path, c, out
	return out, nil
}

func (r *reader) bytesOf(path string, c chunk, rec record) ([]byte, error) {
	p, err := r.plain(path, c)
	if err != nil {
		return nil, err
	}
	if uint64(rec.OffsetInChunk)+uint64(rec.Size) > uint64(len(p)) {
		return nil, fmt.Errorf("record %s runs past its chunk", rec.Key)
	}
	return p[rec.OffsetInChunk : rec.OffsetInChunk+rec.Size], nil
}

// newDecoder is the zstd decoder levelpack reads package frames and checks the set with: up to 1 GiB a frame.
func newDecoder() (*zstd.Decoder, error) {
	return zstd.NewReader(nil, zstd.WithDecoderMaxMemory(1<<30))
}

func levelpack(args []string, stdout, stderr io.Writer, log *slog.Logger) int {
	o, err := parseOptions(args, stderr)
	if err != nil {
		fmt.Fprintln(stderr, "levelpack:", err)
		log.Error("usage", "err", err.Error())
		return 2
	}
	log.Info("start", "manifest", o.manifest, "stock", o.stock, "out", o.out, "overrides", len(o.overrides))
	rep, err := pack(o, log)
	if err != nil {
		fmt.Fprintln(stderr, "levelpack:", err)
		log.Error("refused", "err", err.Error())
		return 1
	}
	line, _ := json.Marshal(rep)
	fmt.Fprintln(stdout, string(line))
	log.Info("written", "out", rep.Out, "bytes", rep.Bytes, "sha256", rep.Sha256, "records", rep.Records,
		"levels", len(rep.Levels), "stock_copies_dropped", rep.StockCopiesDropped, "stock_edited", len(rep.StockEdited),
		"overrides", rep.Overrides)
	return 0
}

func pack(o options, log *slog.Logger) (*report, error) {
	file, err := os.ReadFile(o.manifest)
	if err != nil {
		return nil, err
	}
	m, err := parseManifest(file)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", o.manifest, err)
	}
	set, err := setID(o.manifest)
	if err != nil {
		return nil, err
	}
	packagePath := func(n uint32) string {
		if p, ok := o.packages[n]; ok {
			return p
		}
		return filepath.Join(o.packagesDir, fmt.Sprintf("%s_%d", set, n))
	}
	stock := map[key]stockEntry{}
	for _, path := range o.stock {
		file, err := os.ReadFile(path)
		if err != nil {
			return nil, err
		}
		sm, err := parseManifest(file)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", path, err)
		}
		id, err := setID(path)
		if err != nil {
			return nil, err
		}
		for _, r := range sm.Records {
			stock[r.Key] = stockEntry{rec: r, chunk: sm.Chunks[r.Chunk], packages: filepath.Join(o.stockPackages, id+"_")}
		}
	}
	dec, err := newDecoder()
	if err != nil {
		return nil, err
	}
	defer dec.Close()
	rd := &reader{dec: dec, files: map[string]*os.File{}}
	defer func() {
		for _, f := range rd.files {
			f.Close()
		}
	}()

	rep := &report{Manifest: o.manifest, Out: o.out, StockEdited: []edited{}, Levels: []string{}, LevelsWithoutArchive: []string{}}
	overrides := map[key]bool{}
	for _, k := range o.overrides {
		if k.Type == archiveType {
			return nil, fmt.Errorf("-override %s: an archive is read by the archive loader, which an override does not reach", k)
		}
		overrides[k] = true
	}
	var plan setPlan
	want := map[key][]byte{} // the bytes verifySet compares: each repacked record's and each override's
	newByChunk := map[uint32][]record{}
	recordsInChunk := map[uint32]int{}
	for _, r := range m.Records {
		recordsInChunk[r.Chunk]++
	}
	for _, r := range m.Records {
		s, inStock := stock[r.Key]
		if !inStock {
			newByChunk[r.Chunk] = append(newByChunk[r.Chunk], r)
			continue
		}
		c := m.Chunks[r.Chunk]
		if c == s.chunk && r.OffsetInChunk == s.rec.OffsetInChunk && r.Size == s.rec.Size && r.Alignment == s.rec.Alignment {
			rep.StockUnchanged++
			continue
		}
		theirs, err := rd.bytesOf(packagePath(c.Package), c, r)
		if err != nil {
			return nil, err
		}
		ours, err := rd.bytesOf(fmt.Sprintf("%s%d", s.packages, s.chunk.Package), s.chunk, s.rec)
		if err != nil {
			return nil, err
		}
		if bytes.Equal(theirs, ours) {
			rep.StockCopiesDropped++
			continue
		}
		included := overrides[r.Key]
		rep.StockEdited = append(rep.StockEdited, edited{Key: r.Key.String(), StockSize: s.rec.Size, Size: r.Size, Included: included})
		if included {
			b := append([]byte(nil), theirs...)
			plan.Overrides = append(plan.Overrides, setOverride{Key: r.Key, Bytes: b})
			want[r.Key] = b
			delete(overrides, r.Key)
		}
	}
	for k := range overrides {
		return nil, fmt.Errorf("-override %s is not a stock record the export changed", k)
	}

	chunkIDs := make([]uint32, 0, len(newByChunk))
	for ci := range newByChunk {
		chunkIDs = append(chunkIDs, ci)
	}
	sort.Slice(chunkIDs, func(a, b int) bool { return chunkIDs[a] < chunkIDs[b] })
	var enc *zstd.Encoder
	for _, ci := range chunkIDs {
		recs := newByChunk[ci]
		c := m.Chunks[ci]
		if len(recs) == recordsInChunk[ci] && chunkFollowsReaderRules(c, recs) {
			idx := uint32(len(plan.Chunks))
			plan.Chunks = append(plan.Chunks, setChunk{Path: packagePath(c.Package), Offset: int64(c.FileOffset),
				Length: c.Compressed, Decompressed: c.Decompressed})
			for _, r := range recs {
				plan.Records = append(plan.Records, setRecord{Key: r.Key, Chunk: idx, OffsetInChunk: r.OffsetInChunk,
					Size: r.Size, Alignment: r.Alignment})
			}
			rep.ChunksCopied++
			continue
		}
		// A chunk that also holds stock records (or breaks a reader rule): its new records go into new frames.
		plain, err := rd.plain(packagePath(c.Package), c)
		if err != nil {
			return nil, err
		}
		if enc == nil {
			if enc, err = zstd.NewWriter(nil); err != nil {
				return nil, err
			}
			defer enc.Close()
		}
		if err := repackRecords(&plan, enc, plain, recs, want); err != nil {
			return nil, err
		}
		rep.ChunksRepacked++
	}
	if len(plan.Records) == 0 {
		return nil, errors.New("the export holds no record the stock manifests do not: there is nothing to carry")
	}
	keys := map[key]bool{}
	for _, r := range plan.Records {
		keys[r.Key] = true
	}
	for _, r := range plan.Records {
		if r.Key.Type != levelRootType {
			continue
		}
		rep.Levels = append(rep.Levels, fmt.Sprintf("0x%016x", r.Key.Name))
		if !keys[key{archiveType, r.Key.Name}] {
			rep.LevelsWithoutArchive = append(rep.LevelsWithoutArchive, fmt.Sprintf("0x%016x", r.Key.Name))
		}
	}
	sort.Strings(rep.Levels)
	if len(rep.LevelsWithoutArchive) > 0 {
		return nil, fmt.Errorf("levels without their archive in the export: %s", strings.Join(rep.LevelsWithoutArchive, ","))
	}

	tmp := o.out + ".partial"
	f, err := os.Create(tmp)
	if err != nil {
		return nil, err
	}
	h := sha256.New()
	size, werr := writeSet(io.MultiWriter(f, h), plan)
	if cerr := f.Close(); werr == nil {
		werr = cerr
	}
	if werr != nil {
		os.Remove(tmp)
		return nil, werr
	}
	check, err := os.Open(tmp)
	if err != nil {
		return nil, err
	}
	verr := validateSet(check, size)
	var frames, compared int
	var sv *served
	if verr == nil {
		frames, compared, sv, verr = verifySet(check, size, dec, want)
	}
	check.Close()
	if verr != nil {
		os.Remove(tmp)
		return nil, fmt.Errorf("the written set does not check: %w", verr)
	}
	if problems := servableProblems(sv, func(k key) bool { _, ok := stock[k]; return ok }); len(problems) > 0 {
		os.Remove(tmp)
		rep.Problems = problems
		js, _ := json.MarshalIndent(rep, "", "  ")
		if err := os.WriteFile(o.report, append(js, '\n'), 0o644); err != nil {
			return nil, err
		}
		shown := problems
		if len(shown) > 20 {
			shown = shown[:20]
		}
		return nil, fmt.Errorf("the runtime would refuse this set (%d problems, all in %s):\n  %s", len(problems), o.report,
			strings.Join(shown, "\n  "))
	}
	if err := os.Rename(tmp, o.out); err != nil {
		return nil, err
	}
	rep.Bytes = size
	rep.Sha256 = hex.EncodeToString(h.Sum(nil))
	rep.Records = len(plan.Records)
	rep.Chunks = len(plan.Chunks)
	rep.Overrides = len(plan.Overrides)
	rep.FramesDecoded = frames
	rep.RecordsCompared = compared
	js, _ := json.MarshalIndent(rep, "", "  ")
	if err := os.WriteFile(o.report, append(js, '\n'), 0o644); err != nil {
		return nil, err
	}
	log.Info("classified", "records", rep.Records, "stock_unchanged", rep.StockUnchanged,
		"stock_copies_dropped", rep.StockCopiesDropped, "chunks_copied", rep.ChunksCopied, "chunks_repacked", rep.ChunksRepacked)
	log.Info("verified", "frames_decoded", frames, "records_compared", compared)
	return rep, nil
}

// chunkFollowsReaderRules: a record of at most blockLimit bytes in a chunk of at most blockLimit decompressed bytes,
// a larger one alone in its chunk (sdk/include/nevr/resource_set.h).
func chunkFollowsReaderRules(c chunk, recs []record) bool {
	for _, r := range recs {
		if r.Size <= blockLimit {
			if c.Decompressed > blockLimit {
				return false
			}
		} else if len(recs) != 1 || r.OffsetInChunk != 0 || r.Size != c.Decompressed {
			return false
		}
	}
	return true
}

// repackRecords packs recs (their bytes in plain) into new frames: records of at most blockLimit bytes together, up
// to blockLimit decompressed bytes a frame, at their alignment; a larger record alone in its frame. Each record's
// bytes go into want, for verifySet.
func repackRecords(plan *setPlan, enc *zstd.Encoder, plain []byte, recs []record, want map[key][]byte) error {
	var buf []byte
	var pending []setRecord
	flush := func() {
		if len(pending) == 0 {
			return
		}
		if len(buf) == 0 {
			buf = []byte{0} // only records of 0 bytes: a frame still holds a byte, since a chunk of 0 bytes is refused
		}
		idx := uint32(len(plan.Chunks))
		plan.Chunks = append(plan.Chunks, setChunk{Frame: enc.EncodeAll(buf, nil), Decompressed: uint32(len(buf))})
		for _, r := range pending {
			r.Chunk = idx
			plan.Records = append(plan.Records, r)
		}
		buf, pending = nil, nil
	}
	for _, r := range recs {
		if uint64(r.OffsetInChunk)+uint64(r.Size) > uint64(len(plain)) {
			return fmt.Errorf("record %s runs past its chunk (%d bytes at %d of %d)", r.Key, r.Size, r.OffsetInChunk, len(plain))
		}
		data := plain[r.OffsetInChunk : r.OffsetInChunk+r.Size]
		want[r.Key] = data
		align := r.Alignment
		if align == 0 || align&(align-1) != 0 {
			align = 16
		}
		// A record larger than blockLimit flushes what is before it, starts its frame at 0, and is flushed by the next
		// record or the end: it is alone in its frame, as the reader needs.
		pad := (align - uint32(len(buf))%align) % align
		if uint32(len(buf))+pad+r.Size > blockLimit {
			flush()
			pad = 0
		}
		buf = append(buf, make([]byte, pad)...)
		pending = append(pending, setRecord{Key: r.Key, OffsetInChunk: uint32(len(buf)), Size: r.Size, Alignment: align})
		buf = append(buf, data...)
	}
	flush()
	return nil
}

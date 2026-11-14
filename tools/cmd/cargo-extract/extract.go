package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"sync"
	"sync/atomic"
	"time"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const (
	// defaultLevel is the encoder level when -level is not given. On 500 records of the Quest set (every 62nd, 41 MB) the
	// frames came to 0.3943 of the bytes at "default", 0.3886 at "better" and 0.3705 at "best" (the stock chunks, which
	// pack records together, are 0.3652), taking 0.23 s, 0.34 s and 1.96 s: the objects are written once and fetched by every
	// client, so the size is what counts, and "best" costs seconds on a set of this size.
	defaultLevel = zstd.SpeedBestCompression
	// withChecksum is whether a frame carries the zstd content checksum. The object is named by the SHA-256 of its
	// frame, so the 4 bytes would prove nothing the name does not.
	withChecksum = false

	schemaName      = "nevr-cargo-manifest"
	schemaVersion   = 1
	formatZstdFrame = "zstd-frame"
	labelName       = "game"
	maxWorkers      = 8
	// residentBudget bounds the decompressed chunk bytes held while records are being encoded (a chunk is released when
	// its last record is done); a single chunk larger than this is still read, alone.
	residentBudget = 1 << 30
)

// The limits of src/cdn/cdn_manifest.h's parser, which the summary's cargo_rules reports against. They are copied:
// nothing links the Go tool to the C++ header, and the report says which constant each number is held to.
const (
	cargoMaxManifestBytes = 1 << 20
	cargoMaxBlobBytes     = 128 << 20
	cargoDefaultCapBytes  = 512 << 20
	cargoMaxEntries       = 2048
)

// entryDoc is one buffer of one resource, in the schema of src/cdn/cdn_manifest.h without `stock_size`: a stock
// extraction has no art made against a stock buffer, and the parser's refusal of that is reported, not papered over.
type entryDoc struct {
	Platform string `json:"platform"`
	Type     string `json:"type"`
	Name     string `json:"name"`
	Format   string `json:"format"`
	Sha256   string `json:"sha256"`
	Size     uint64 `json:"size"`
	Bytes    uint64 `json:"bytes"`
	Align    uint32 `json:"align"`
}

// emptyDoc is a resource of 0 bytes: there is no frame to store, and the parser refuses a size of 0.
type emptyDoc struct {
	Type  string `json:"type"`
	Name  string `json:"name"`
	Align uint32 `json:"align"`
}

type labelDoc struct {
	Entries []entryDoc `json:"entries"`
}

// manifestDoc is the manifest: the cargo schema's keys, then `sets` (which package sets it was extracted from, which
// verify reads back) and `empty`. The parser counts a key it does not know and goes on.
type manifestDoc struct {
	Schema        string              `json:"schema"`
	SchemaVersion int                 `json:"schema_version"`
	Labels        map[string]labelDoc `json:"labels"`
	Sets          []setDoc            `json:"sets"`
	Empty         []emptyDoc          `json:"empty"`
}

type largest struct {
	Bytes  uint64 `json:"bytes"`
	Sha256 string `json:"sha256"`
}

type encoderInfo struct {
	Level    string `json:"level"`
	Window   int    `json:"window"`
	Checksum bool   `json:"checksum"`
}

// cargoRules counts what the full set breaks of the parser's rules (src/cdn/cdn_manifest.h and ADR 0045, decision 8),
// each against its constant. The tool writes the true entries whatever these say.
type cargoRules struct {
	ZeroByteRecords          int    `json:"zero_byte_records"`            // size or bytes of 0: kTooBig
	Entries                  int    `json:"entries"`                      // entries in the label
	MaxEntries               int    `json:"max_entries"`                  // kMaxEntries
	EntriesOverCap           int    `json:"entries_over_cap"`             // entries past kMaxEntries
	ManifestBytes            int    `json:"manifest_bytes"`               // the manifest file's size
	MaxManifestBytes         int    `json:"max_manifest_bytes"`           // kMaxManifestBytes
	EntriesWithoutStockSize  int    `json:"entries_without_stock_size"`   // stock_size is required
	EntriesOverMaxBlobBytes  int    `json:"entries_over_max_blob_bytes"`  // size or bytes over kMaxBlobBytes
	MaxBlobBytes             uint64 `json:"max_blob_bytes"`               // kMaxBlobBytes
	ObjectBytesOverCap       bool   `json:"object_bytes_over_cap"`        // the distinct objects exceed kDefaultCapBytes
	DefaultCapBytes          uint64 `json:"default_cap_bytes"`            // kDefaultCapBytes
	EntriesAlignOverCargoMax int    `json:"entries_align_over_cargo_max"` // kMaxAlignment (never, the tool's own bound is 4096)
}

type summary struct {
	Platform           string         `json:"platform"`
	Sets               []setDoc       `json:"sets"`
	RecordsByType      map[string]int `json:"records_by_type"`
	Records            int            `json:"records"`
	Entries            int            `json:"entries"`
	ZeroByteRecords    int            `json:"zero_byte_records"`
	Objects            int            `json:"objects"` // the unique objects in the output directory, counted there
	ObjectsWritten     int            `json:"objects_written"`
	ObjectsDedup       int            `json:"objects_deduplicated"` // records_with_object less objects_written: this run's count, not the data's
	RecordsWithObject  int            `json:"records_with_object"`  // the records that point at an object: the entries
	StockChunkBytes    uint64         `json:"stock_chunk_bytes"`
	StockChunkPlain    uint64         `json:"stock_chunk_decompressed_bytes"`
	RecordBytes        uint64         `json:"record_bytes"`
	ObjectBytes        uint64         `json:"object_bytes"`
	LargestObject      largest        `json:"largest_object"`
	LargestRecordBytes uint64         `json:"largest_record_bytes"`
	StockFrames        windowStats    `json:"stock_frames"`
	Encoder            encoderInfo    `json:"encoder"`
	CargoRules         cargoRules     `json:"cargo_rules"`
	ElapsedMS          int64          `json:"elapsed_ms"`
}

// planned is one record that has bytes, and where they are.
type planned struct {
	set    int
	chunk  uint32
	offset uint32
	size   uint32
	typ    uint64
	name   uint64
	align  uint32
}

func hexOf(v uint64) string { return fmt.Sprintf("0x%016x", v) }

func keyName(typ, name uint64) string { return hexOf(typ) + ":" + hexOf(name) }

// refuse names the record a rule refused.
func refuse(typ, name uint64, err error) error {
	return fmt.Errorf("record %s: %w", keyName(typ, name), err)
}

// setIDs is the sets to read: the ones asked for, or every manifest file of the platform's folder, in order.
func setIDs(data string, asked []string) ([]string, error) {
	if len(asked) > 0 {
		ids := append([]string(nil), asked...)
		sort.Strings(ids)
		for i := 1; i < len(ids); i++ {
			if ids[i] == ids[i-1] {
				return nil, fmt.Errorf("-set %s is given twice", ids[i])
			}
		}
		return ids, nil
	}
	entries, err := os.ReadDir(filepath.Join(data, "manifests"))
	if err != nil {
		return nil, err
	}
	var ids []string
	for _, e := range entries {
		if !e.IsDir() && setID.MatchString(e.Name()) {
			ids = append(ids, e.Name())
		}
	}
	if len(ids) == 0 {
		return nil, fmt.Errorf("%s holds no manifest named by a 16-digit set id", filepath.Join(data, "manifests"))
	}
	sort.Strings(ids)
	return ids, nil
}

// setDoc names a source manifest: its set id, the SHA-256 of the manifest file and the file's length in bytes.
type setDoc struct {
	ID     string `json:"id"`
	Sha256 string `json:"sha256"`
	Bytes  uint64 `json:"bytes"`
}

func openSets(data string, ids []string, log *slog.Logger) ([]*radpack.Set, []setDoc, error) {
	var sets []*radpack.Set
	var docs []setDoc
	for _, id := range ids {
		s, raw, err := radpack.OpenSet(data, id)
		if err != nil {
			return nil, nil, err
		}
		h := sha256.Sum256(raw)
		docs = append(docs, setDoc{ID: id, Sha256: hex.EncodeToString(h[:]), Bytes: uint64(len(raw))})
		var chunkBytes uint64
		for _, c := range s.Manifest.Chunks {
			if c.Decompressed != 0 { // a package's end record states no size: its other words are not a frame's
				chunkBytes += uint64(c.Compressed)
			}
		}
		log.Info("set", "set", id, "packages", s.Manifest.PackageCount(), "records", len(s.Manifest.Resources),
			"chunks", len(s.Manifest.Chunks), "chunk_bytes", chunkBytes)
		sets = append(sets, s)
	}
	return sets, docs, nil
}

// store is the object directory: <out>/blobs/sha256/<hex>. An object is claimed once per run by its hash; the claimer
// writes it, to a temporary file in the same directory that is renamed into place, so a name never holds part of a frame.
type store struct {
	dir string

	mu      sync.Mutex
	claimed map[string]bool
	written int
	bytes   uint64
	largest largest
}

func newStore(out string) (*store, error) {
	dir := filepath.Join(out, "blobs", "sha256")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return nil, err
	}
	return &store{dir: dir, claimed: map[string]bool{}}, nil
}

func (s *store) path(sum string) string { return filepath.Join(s.dir, sum) }

// put stores frame under its hash. An object already in this run, or on disk with the size of the frame, is not written
// again; one on disk with another size is an error.
func (s *store) put(sum string, frame []byte) error {
	s.mu.Lock()
	if s.claimed[sum] {
		s.mu.Unlock()
		return nil
	}
	s.claimed[sum] = true
	s.bytes += uint64(len(frame))
	if uint64(len(frame)) > s.largest.Bytes {
		s.largest = largest{Bytes: uint64(len(frame)), Sha256: sum}
	}
	s.mu.Unlock()

	path := s.path(sum)
	if fi, err := os.Stat(path); err == nil {
		if fi.Size() != int64(len(frame)) {
			return fmt.Errorf("object %s is on disk with %d bytes, the frame is %d", sum, fi.Size(), len(frame))
		}
		return nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return err
	}
	tmp, err := os.CreateTemp(s.dir, ".tmp-")
	if err != nil {
		return err
	}
	_, werr := tmp.Write(frame)
	if cerr := tmp.Close(); werr == nil {
		werr = cerr
	}
	if werr == nil {
		werr = os.Rename(tmp.Name(), path)
	}
	if werr != nil {
		os.Remove(tmp.Name())
		return werr
	}
	s.mu.Lock()
	s.written++
	s.mu.Unlock()
	return nil
}

// countObjects is the number of objects in dir: regular files named by 64 lower-case hex digits, which is what a stored frame
// is named. A temporary file or any other name is not one.
func countObjects(dir string) (int, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return 0, err
	}
	n := 0
	for _, e := range entries {
		if e.Type().IsRegular() && isObjectName(e.Name()) {
			n++
		}
	}
	return n, nil
}

func isObjectName(s string) bool {
	if len(s) != 64 {
		return false
	}
	for _, c := range s {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

// residentChunk is a decompressed chunk that records are being cut from.
type residentChunk struct {
	bytes   int64
	pending atomic.Int32 // the records not yet done, and one for the feeder until it has handed them all out
}

// budget bounds the decompressed bytes held at once. acquire waits while something is held and n more would pass the
// limit, so a chunk larger than the limit is read alone.
type budget struct {
	mu   sync.Mutex
	cond *sync.Cond
	used int64
	max  int64
}

func newBudget(max int64) *budget {
	b := &budget{max: max}
	b.cond = sync.NewCond(&b.mu)
	return b
}

func (b *budget) acquire(n int64) {
	b.mu.Lock()
	for b.used > 0 && b.used+n > b.max {
		b.cond.Wait()
	}
	b.used += n
	b.mu.Unlock()
}

func (b *budget) release(n int64) {
	b.mu.Lock()
	b.used -= n
	b.mu.Unlock()
	b.cond.Broadcast()
}

type job struct {
	n     int
	chunk *residentChunk
	data  []byte
}

func extract(ctx context.Context, o extractOptions, log *slog.Logger) (*summary, error) {
	start := time.Now()
	data := filepath.Join(o.root, "rad15", platforms[o.platform])
	ids, err := setIDs(data, o.sets)
	if err != nil {
		return nil, err
	}
	log.Info("start", "platform", o.platform, "data", data, "sets", ids, "out", o.out, "level", o.level.String())
	sets, setDocs, err := openSets(data, ids, log)
	if err != nil {
		return nil, err
	}
	chunks, err := radpack.NewChunks()
	if err != nil {
		return nil, err
	}
	defer chunks.Close()

	sum := &summary{Platform: o.platform, Sets: setDocs, RecordsByType: map[string]int{}}
	sum.StockFrames, err = measureStock(chunks, sets)
	if err != nil {
		return nil, err
	}
	window := encoderWindow(sum.StockFrames.MaxWindow)
	workers := o.workers
	if workers <= 0 {
		workers = min(runtime.GOMAXPROCS(0), maxWorkers)
	}
	sum.Encoder = encoderInfo{Level: o.level.String(), Window: window, Checksum: withChecksum}
	log.Info("stock_frames", "chunks", sum.StockFrames.Chunks, "max_window", sum.StockFrames.MaxWindow,
		"single_segment", sum.StockFrames.SingleSegment, "with_checksum", sum.StockFrames.WithChecksum,
		"encoder_window", window, "workers", workers)
	for _, s := range sets {
		for _, c := range s.Manifest.Chunks {
			if c.Decompressed != 0 {
				sum.StockChunkBytes += uint64(c.Compressed)
				sum.StockChunkPlain += uint64(c.Decompressed)
			}
		}
	}

	// Plan: every record is checked before a byte is read or written.
	var plan []planned
	var empties []emptyDoc
	seenIn := map[radpack.Key]int{}
	for si, s := range sets {
		for _, r := range s.Manifest.Resources {
			typ, name := uint64(r.Type), uint64(r.Name)
			if int(r.Chunk) >= len(s.Manifest.Chunks) {
				return nil, refuse(typ, name, fmt.Errorf("it names chunk %d of %d in set %s", r.Chunk, len(s.Manifest.Chunks), s.ID))
			}
			if err := checkAlignment(r.Alignment); err != nil {
				return nil, refuse(typ, name, err)
			}
			k := radpack.Key{Type: r.Type, Name: r.Name}
			if first, dup := seenIn[k]; dup {
				return nil, refuse(typ, name, fmt.Errorf("it appears twice for %s (sets %s and %s)", o.platform, ids[first], s.ID))
			}
			seenIn[k] = si
			sum.Records++
			sum.RecordsByType[hexOf(typ)]++
			sum.RecordBytes += uint64(r.Size)
			sum.LargestRecordBytes = max(sum.LargestRecordBytes, uint64(r.Size))
			if r.Size == 0 {
				empties = append(empties, emptyDoc{Type: hexOf(typ), Name: hexOf(name), Align: r.Alignment})
				continue
			}
			plan = append(plan, planned{set: si, chunk: r.Chunk, offset: r.Offset, size: r.Size, typ: typ, name: name, align: r.Alignment})
		}
	}
	sum.ZeroByteRecords = len(empties)
	log.Info("planned", "records", sum.Records, "with_bytes", len(plan), "zero_byte", len(empties), "types", len(sum.RecordsByType))

	encode := o.encode
	if encode == nil {
		// A frame states its content size in the header only when it is a single-segment frame, or when the size is
		// 256 or more (RFC 8878, section 3.1.1.1.4: the encoder writes no size in a small frame that has a window
		// descriptor). So a record that fits the window is one single-segment frame (its window is its size), and a larger
		// one is a frame with the window descriptor, whose size is always 256 or more.
		mk := func(single bool) (*zstd.Encoder, error) {
			return zstd.NewWriter(nil, zstd.WithEncoderLevel(o.level), zstd.WithWindowSize(window), zstd.WithEncoderCRC(withChecksum),
				zstd.WithEncoderConcurrency(workers), zstd.WithSingleSegment(single))
		}
		whole, err := mk(true)
		if err != nil {
			return nil, err
		}
		defer whole.Close()
		windowed, err := mk(false)
		if err != nil {
			return nil, err
		}
		defer windowed.Close()
		encode = func(b []byte) []byte {
			if len(b) <= window {
				return whole.EncodeAll(b, nil)
			}
			return windowed.EncodeAll(b, nil)
		}
	}
	objects, err := newStore(o.out)
	if err != nil {
		return nil, err
	}

	// Encode: the records of one chunk are cut from one decode of it, in chunk order, and workers frame, check, hash and
	// store them.
	entries := make([]entryDoc, len(plan))
	order := make([]int, len(plan))
	for i := range order {
		order[i] = i
	}
	sort.SliceStable(order, func(a, b int) bool {
		pa, pb := plan[order[a]], plan[order[b]]
		if pa.set != pb.set {
			return pa.set < pb.set
		}
		return pa.chunk < pb.chunk
	})

	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	var failure error
	var failOnce sync.Once
	fail := func(err error) {
		failOnce.Do(func() { failure = err; cancel() })
	}
	held := newBudget(residentBudget)
	jobs := make(chan job, workers)
	var wg sync.WaitGroup
	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := range jobs {
				if ctx.Err() == nil {
					if err := frameAndStore(plan[j.n], j.data, encode, sum.StockFrames.MaxWindow, o.platform, objects, &entries[j.n]); err != nil {
						fail(err)
					}
				}
				if j.chunk.pending.Add(-1) == 0 {
					held.release(j.chunk.bytes)
				}
			}
		}()
	}
	chunksDone := 0
feed:
	for at := 0; at < len(order); {
		p := plan[order[at]]
		end := at
		for end < len(order) && plan[order[end]].set == p.set && plan[order[end]].chunk == p.chunk {
			end++
		}
		held.acquire(int64(sets[p.set].Manifest.Chunks[p.chunk].Decompressed))
		plain, err := chunks.Plain(sets[p.set], int(p.chunk))
		if err != nil {
			held.release(int64(sets[p.set].Manifest.Chunks[p.chunk].Decompressed))
			fail(err)
			break feed
		}
		rc := &residentChunk{bytes: int64(len(plain))}
		rc.pending.Store(int32(end-at) + 1)
		for _, n := range order[at:end] {
			q := plan[n]
			if uint64(q.offset)+uint64(q.size) > uint64(len(plain)) {
				fail(refuse(q.typ, q.name, fmt.Errorf("it runs past its chunk (%d bytes at %d of %d)", q.size, q.offset, len(plain))))
				rc.pending.Add(-1)
				continue
			}
			select {
			case jobs <- job{n: n, chunk: rc, data: plain[q.offset : q.offset+q.size]}:
			case <-ctx.Done():
				rc.pending.Add(-1)
			}
		}
		if rc.pending.Add(-1) == 0 {
			held.release(rc.bytes)
		}
		at = end
		chunksDone++
		if chunksDone%1000 == 0 {
			log.Info("progress", "chunks_read", chunksDone, "records", at, "of", len(order))
		}
		if ctx.Err() != nil {
			break
		}
	}
	close(jobs)
	wg.Wait()
	if failure != nil {
		return nil, failure
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}

	doc := manifestDoc{Schema: schemaName, SchemaVersion: schemaVersion, Labels: map[string]labelDoc{labelName: {Entries: entries}},
		Sets: setDocs, Empty: empties}
	if doc.Empty == nil {
		doc.Empty = []emptyDoc{}
	}
	body, err := json.Marshal(doc)
	if err != nil {
		return nil, err
	}
	body = append(body, '\n')
	// The manifest is also written as one zstd frame, by the encoder and under the rules the objects have: nothing uncompressed is
	// published, so manifest.json.zst is the file to publish and manifest.json the plain copy it decompresses to.
	manifestFrame := encode(body)
	if err := checkFrame(manifestFrame, uint64(len(body)), sum.StockFrames.MaxWindow); err != nil {
		return nil, fmt.Errorf("manifest.json.zst: %w", err)
	}
	if err := writeAtomic(filepath.Join(o.out, "manifest.json.zst"), manifestFrame); err != nil {
		return nil, err
	}
	if err := writeAtomic(filepath.Join(o.out, "manifest.json"), body); err != nil {
		return nil, err
	}

	sum.Entries = len(entries)
	sum.Objects, err = countObjects(objects.dir)
	if err != nil {
		return nil, err
	}
	sum.ObjectsWritten = objects.written
	sum.RecordsWithObject = len(entries)
	sum.ObjectsDedup = sum.RecordsWithObject - sum.ObjectsWritten
	sum.ObjectBytes = objects.bytes
	sum.LargestObject = objects.largest
	sum.CargoRules = cargoRulesOf(entries, len(empties), len(body), objects.bytes)
	sum.ElapsedMS = time.Since(start).Milliseconds()
	log.Info("written", "out", o.out, "entries", sum.Entries, "zero_byte", sum.ZeroByteRecords, "objects_written", sum.ObjectsWritten,
		"records_with_object", sum.RecordsWithObject, "objects", sum.Objects, "object_bytes", sum.ObjectBytes, "stock_chunk_bytes", sum.StockChunkBytes,
		"manifest_bytes", len(body), "elapsed_ms", sum.ElapsedMS)
	return sum, nil
}

// frameAndStore frames one record's bytes, applies the frame rule, and stores the frame under its hash.
func frameAndStore(p planned, data []byte, encode func([]byte) []byte, maxWindow uint64, platform string, objects *store, entry *entryDoc) error {
	frame := encode(data)
	if err := checkFrame(frame, uint64(p.size), maxWindow); err != nil {
		return refuse(p.typ, p.name, err)
	}
	h := sha256.Sum256(frame)
	sum := hex.EncodeToString(h[:])
	if err := objects.put(sum, frame); err != nil {
		return refuse(p.typ, p.name, err)
	}
	*entry = entryDoc{Platform: platform, Type: hexOf(p.typ), Name: hexOf(p.name), Format: formatZstdFrame, Sha256: sum,
		Size: uint64(len(frame)), Bytes: uint64(p.size), Align: p.align}
	return nil
}

func cargoRulesOf(entries []entryDoc, zeroByte, manifestBytes int, objectBytes uint64) cargoRules {
	r := cargoRules{
		ZeroByteRecords:         zeroByte,
		Entries:                 len(entries),
		MaxEntries:              cargoMaxEntries,
		ManifestBytes:           manifestBytes,
		MaxManifestBytes:        cargoMaxManifestBytes,
		EntriesWithoutStockSize: len(entries),
		MaxBlobBytes:            cargoMaxBlobBytes,
		ObjectBytesOverCap:      objectBytes > cargoDefaultCapBytes,
		DefaultCapBytes:         cargoDefaultCapBytes,
	}
	r.EntriesOverCap = max(0, len(entries)-cargoMaxEntries)
	for _, e := range entries {
		if e.Size > cargoMaxBlobBytes || e.Bytes > cargoMaxBlobBytes {
			r.EntriesOverMaxBlobBytes++
		}
		if e.Align > 1<<16 {
			r.EntriesAlignOverCargoMax++
		}
	}
	return r
}

// writeAtomic writes path by a temporary file in its directory and a rename.
func writeAtomic(path string, body []byte) error {
	tmp, err := os.CreateTemp(filepath.Dir(path), ".tmp-")
	if err != nil {
		return err
	}
	_, werr := tmp.Write(body)
	if cerr := tmp.Close(); werr == nil {
		werr = cerr
	}
	if werr == nil {
		werr = os.Rename(tmp.Name(), path)
	}
	if werr != nil {
		os.Remove(tmp.Name())
	}
	return werr
}

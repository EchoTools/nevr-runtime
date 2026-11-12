package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

type verifySummary struct {
	Platform      string `json:"platform"`
	Sets          int    `json:"sets"`
	StockRecords  int    `json:"stock_records"`
	Entries       int    `json:"entries"`
	EmptyRecords  int    `json:"empty_records"`
	ObjectsRead   int    `json:"objects_read"`
	BytesCompared uint64 `json:"bytes_compared"`
	Mismatches    int    `json:"mismatches"`
	ElapsedMS     int64  `json:"elapsed_ms"`
}

// parseHexKey reads the "0x" hex string a manifest writes a type or a name as.
func parseHexKey(s string) (uint64, error) {
	if len(s) < 3 || len(s) > 18 || !strings.HasPrefix(s, "0x") {
		return 0, fmt.Errorf("%q is not 0x followed by 1 to 16 hex digits", s)
	}
	return strconv.ParseUint(s[2:], 16, 64)
}

// checkItem is one thing verify compares: a manifest entry (frame set) or an empty record (frame nil).
type checkItem struct {
	set, res int
	entry    *entryDoc
	empty    *emptyDoc
}

// verify reads every object the manifest in out lists, decompresses it, and compares it with the bytes the stock
// package of data holds for the same record. The first difference is an error that names the record.
func verify(ctx context.Context, out, root string, log *slog.Logger) (*verifySummary, error) {
	start := time.Now()
	raw, err := os.ReadFile(filepath.Join(out, "manifest.json"))
	if err != nil {
		return nil, err
	}
	var doc manifestDoc
	if err := json.Unmarshal(raw, &doc); err != nil {
		return nil, fmt.Errorf("manifest.json: %w", err)
	}
	if doc.Schema != schemaName || doc.SchemaVersion != schemaVersion {
		return nil, fmt.Errorf("manifest.json: schema %q version %d is not %q version %d", doc.Schema, doc.SchemaVersion, schemaName, schemaVersion)
	}
	label, ok := doc.Labels[labelName]
	if !ok || len(doc.Labels) != 1 {
		return nil, fmt.Errorf("manifest.json: it does not hold exactly the label %q", labelName)
	}
	if len(doc.Sets) == 0 {
		return nil, errors.New("manifest.json: it names no package set")
	}
	platform := ""
	for _, e := range label.Entries {
		if platform == "" {
			platform = e.Platform
		}
		if e.Platform != platform {
			return nil, fmt.Errorf("entry %s:%s is for %s, the manifest's first is for %s", e.Type, e.Name, e.Platform, platform)
		}
	}
	if platform == "" {
		return nil, errors.New("manifest.json: it lists no entry")
	}
	folder, ok := platforms[platform]
	if !ok {
		return nil, fmt.Errorf("manifest.json: the platform %q is not one this tool knows", platform)
	}
	data := filepath.Join(root, "rad15", folder)
	ids := make([]string, len(doc.Sets))
	for i, s := range doc.Sets {
		ids[i] = s.ID
	}
	log.Info("start", "out", out, "data", data, "platform", platform, "sets", ids, "entries", len(label.Entries), "empty", len(doc.Empty))

	sets, _, err := openSets(data, ids, log)
	if err != nil {
		return nil, err
	}
	chunks, err := radpack.NewChunks()
	if err != nil {
		return nil, err
	}
	defer chunks.Close()
	dec, err := zstd.NewReader(nil, zstd.WithDecoderMaxMemory(1<<31))
	if err != nil {
		return nil, err
	}
	defer dec.Close()

	sum := &verifySummary{Platform: platform, Sets: len(sets), Entries: len(label.Entries), EmptyRecords: len(doc.Empty)}
	where := map[uint64]map[uint64][2]int{} // type -> name -> (set, resource index)
	for si, s := range sets {
		for ri, r := range s.Manifest.Resources {
			sum.StockRecords++
			t, n := uint64(r.Type), uint64(r.Name)
			if where[t] == nil {
				where[t] = map[uint64][2]int{}
			}
			if _, dup := where[t][n]; dup {
				return nil, fmt.Errorf("record %s is in two of the sets", keyName(t, n))
			}
			where[t][n] = [2]int{si, ri}
		}
	}

	// Every manifest item must be a stock record, once; with as many items as there are stock records, none is left out.
	var items []checkItem
	covered := map[[2]uint64]bool{}
	locate := func(typeText, nameText string) (uint64, uint64, [2]int, error) {
		t, err := parseHexKey(typeText)
		if err != nil {
			return 0, 0, [2]int{}, fmt.Errorf("record %s:%s: %w", typeText, nameText, err)
		}
		n, err := parseHexKey(nameText)
		if err != nil {
			return 0, 0, [2]int{}, fmt.Errorf("record %s:%s: %w", typeText, nameText, err)
		}
		at, ok := where[t][n]
		if !ok {
			return 0, 0, [2]int{}, refuse(t, n, errors.New("the manifest lists it and the stock sets do not hold it"))
		}
		if covered[[2]uint64{t, n}] {
			return 0, 0, [2]int{}, refuse(t, n, errors.New("the manifest lists it twice"))
		}
		covered[[2]uint64{t, n}] = true
		return t, n, at, nil
	}
	for i := range label.Entries {
		e := &label.Entries[i]
		_, _, at, err := locate(e.Type, e.Name)
		if err != nil {
			return nil, err
		}
		items = append(items, checkItem{set: at[0], res: at[1], entry: e})
	}
	for i := range doc.Empty {
		e := &doc.Empty[i]
		_, _, at, err := locate(e.Type, e.Name)
		if err != nil {
			return nil, err
		}
		items = append(items, checkItem{set: at[0], res: at[1], empty: e})
	}
	if len(items) != sum.StockRecords {
		for si, s := range sets {
			for _, r := range s.Manifest.Resources {
				if !covered[[2]uint64{uint64(r.Type), uint64(r.Name)}] {
					return nil, refuse(uint64(r.Type), uint64(r.Name), fmt.Errorf("the stock set %s holds it and the manifest does not list it", sets[si].ID))
				}
			}
		}
	}

	// Compare, a chunk at a time: the stock chunk is decoded once for all its records.
	sort.SliceStable(items, func(a, b int) bool {
		ra, rb := sets[items[a].set].Manifest.Resources[items[a].res], sets[items[b].set].Manifest.Resources[items[b].res]
		if items[a].set != items[b].set {
			return items[a].set < items[b].set
		}
		return ra.Chunk < rb.Chunk
	})
	var plain []byte
	plainSet, plainChunk := -1, uint32(0)
	for n, it := range items {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		s := sets[it.set]
		r := s.Manifest.Resources[it.res]
		t, nm := uint64(r.Type), uint64(r.Name)
		if int(r.Chunk) >= len(s.Manifest.Chunks) {
			return nil, refuse(t, nm, fmt.Errorf("the stock record names chunk %d of %d in set %s", r.Chunk, len(s.Manifest.Chunks), s.ID))
		}
		if it.empty != nil {
			if r.Size != 0 || it.empty.Align != r.Alignment {
				return nil, refuse(t, nm, fmt.Errorf("it is listed as empty with alignment %d, the stock record is %d bytes with alignment %d", it.empty.Align, r.Size, r.Alignment))
			}
			continue
		}
		e := it.entry
		if uint64(r.Size) != e.Bytes || r.Alignment != e.Align {
			return nil, refuse(t, nm, fmt.Errorf("the entry says %d bytes with alignment %d, the stock record is %d bytes with alignment %d", e.Bytes, e.Align, r.Size, r.Alignment))
		}
		if plainSet != it.set || plainChunk != r.Chunk {
			if plain, err = chunks.Plain(s, int(r.Chunk)); err != nil {
				return nil, err
			}
			plainSet, plainChunk = it.set, r.Chunk
		}
		if uint64(r.Offset)+uint64(r.Size) > uint64(len(plain)) {
			return nil, refuse(t, nm, fmt.Errorf("the stock record runs past its chunk (%d bytes at %d of %d)", r.Size, r.Offset, len(plain)))
		}
		stock := plain[r.Offset : r.Offset+r.Size]
		frame, err := os.ReadFile(filepath.Join(out, "blobs", "sha256", e.Sha256))
		if err != nil {
			return nil, refuse(t, nm, err)
		}
		if h := sha256.Sum256(frame); hex.EncodeToString(h[:]) != e.Sha256 {
			return nil, refuse(t, nm, fmt.Errorf("the object %s holds bytes that hash to %x", e.Sha256, h))
		}
		if uint64(len(frame)) != e.Size {
			return nil, refuse(t, nm, fmt.Errorf("the object %s is %d bytes, the entry says %d", e.Sha256, len(frame), e.Size))
		}
		got, err := dec.DecodeAll(frame, make([]byte, 0, e.Bytes))
		if err != nil {
			return nil, refuse(t, nm, fmt.Errorf("the object %s does not decompress: %w", e.Sha256, err))
		}
		if !bytes.Equal(got, stock) {
			return nil, refuse(t, nm, fmt.Errorf("the object %s decompresses to %d bytes that differ from the %d bytes the stock package holds", e.Sha256, len(got), len(stock)))
		}
		sum.ObjectsRead++
		sum.BytesCompared += uint64(len(stock))
		if (n+1)%10000 == 0 {
			log.Info("progress", "compared", n+1, "of", len(items))
		}
	}
	sum.ElapsedMS = time.Since(start).Milliseconds()
	log.Info("verified", "entries", sum.Entries, "empty", sum.EmptyRecords, "objects_read", sum.ObjectsRead,
		"bytes_compared", sum.BytesCompared, "mismatches", 0, "elapsed_ms", sum.ElapsedMS)
	return sum, nil
}

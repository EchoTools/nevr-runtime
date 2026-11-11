package radpack

import (
	"bytes"
	"os"
	"testing"

	"github.com/klauspost/compress/zstd"
)

// The format against the shipped data, when a machine has it: NEVR_STOCK_DATA names a stock PC data folder (the one that
// holds manifests/ and packages/, `_data/5932408047/rad15/win10` of an install). Each client set must decode and encode
// to its own decompressed bytes, read its first and last resource, and take a one-resource pack through Build. Skipped
// without the variable; nothing of the game's is in the repository.
func TestTheShippedSetsDecodeEncodeAndBuild(t *testing.T) {
	data := os.Getenv("NEVR_STOCK_DATA")
	if data == "" {
		t.Skip("NEVR_STOCK_DATA is not set (a stock PC data folder: manifests/ and packages/)")
	}
	for _, id := range []string{"48037dc70b0ecab2", "2b47aab238f60515"} {
		s, raw, err := OpenSet(data, id)
		if err != nil {
			t.Fatalf("%s: %v", id, err)
		}
		dec, _ := zstd.NewReader(nil, zstd.WithDecoderMaxMemory(1<<31))
		plain, err := dec.DecodeAll(raw[headerSize:], nil)
		dec.Close()
		if err != nil {
			t.Fatal(err)
		}
		if !bytes.Equal(s.Manifest.Plain(), plain) {
			t.Errorf("%s: decode then encode is not the shipped manifest", id)
		}
		for _, i := range []int{0, len(s.Manifest.Resources) - 1} {
			r := s.Manifest.Resources[i]
			if _, ok, err := s.Read(r.Type, r.Name); !ok || err != nil {
				t.Errorf("%s: resource %d does not read: %v", id, i, err)
			}
		}
		r := s.Manifest.Resources[0]
		if _, _, err := Build(s.Manifest, map[Key][]byte{{Type: r.Type, Name: r.Name}: []byte("x")}); err != nil {
			t.Errorf("%s: Build refused the shipped set: %v", id, err)
		}
		t.Logf("%s: %d resources, %d chunks, %d packages", id, len(s.Manifest.Resources), len(s.Manifest.Chunks), s.Manifest.PackageCount())
	}
}

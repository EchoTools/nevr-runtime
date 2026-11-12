package radpack

import (
	"fmt"
	"io"
	"os"
	"path/filepath"

	"github.com/klauspost/compress/zstd"
)

// Chunks reads the chunks of package sets, the way the engine's streaming reader does: a chunk is `Compressed` bytes at
// `Offset` of package file `Package`, one zstd frame that decompresses to `Decompressed` bytes. Each package file is
// opened once and kept open until Close, and one decoder serves every read, so reading a whole set costs one open per
// package and one decode per chunk asked for. It is not safe for concurrent use.
type Chunks struct {
	dec   *zstd.Decoder
	files map[string]*os.File
}

// NewChunks returns a reader with no package file open.
func NewChunks() (*Chunks, error) {
	dec, err := zstd.NewReader(nil, zstd.WithDecoderMaxMemory(1<<31))
	if err != nil {
		return nil, err
	}
	return &Chunks{dec: dec, files: map[string]*os.File{}}, nil
}

// Close closes the package files and the decoder.
func (c *Chunks) Close() {
	for _, f := range c.files {
		f.Close()
	}
	c.files = map[string]*os.File{}
	c.dec.Close()
}

func (c *Chunks) chunk(s *Set, i int) (Chunk, *os.File, error) {
	if i < 0 || i >= len(s.Manifest.Chunks) {
		return Chunk{}, nil, fmt.Errorf("chunk %d of %s: the manifest has %d chunks", i, s.ID, len(s.Manifest.Chunks))
	}
	ch := s.Manifest.Chunks[i]
	path := filepath.Join(s.data, "packages", fmt.Sprintf("%s_%d", s.ID, ch.Package))
	if f, ok := c.files[path]; ok {
		return ch, f, nil
	}
	f, err := os.Open(path)
	if err != nil {
		return Chunk{}, nil, err
	}
	c.files[path] = f
	return ch, f, nil
}

// FrameHead is the first n bytes (fewer when the chunk is shorter) of chunk i's stored frame: enough to read its zstd
// frame header without reading the frame.
func (c *Chunks) FrameHead(s *Set, i int, n int) ([]byte, error) {
	ch, f, err := c.chunk(s, i)
	if err != nil {
		return nil, err
	}
	n = min(n, int(ch.Compressed))
	head := make([]byte, n)
	if _, err := f.ReadAt(head, int64(ch.Offset)); err != nil && err != io.EOF {
		return nil, fmt.Errorf("chunk %d of %s: %w", i, s.ID, err)
	}
	return head, nil
}

// Plain is chunk i, decompressed. It is an error when the frame does not decode, or decodes to other than the length the
// chunk's record states.
func (c *Chunks) Plain(s *Set, i int) ([]byte, error) {
	ch, f, err := c.chunk(s, i)
	if err != nil {
		return nil, err
	}
	packed := make([]byte, ch.Compressed)
	if _, err := f.ReadAt(packed, int64(ch.Offset)); err != nil && err != io.EOF {
		return nil, fmt.Errorf("chunk %d of %s: %w", i, s.ID, err)
	}
	plain, err := c.dec.DecodeAll(packed, make([]byte, 0, ch.Decompressed))
	if err != nil || uint32(len(plain)) != ch.Decompressed {
		return nil, fmt.Errorf("chunk %d of %s does not decompress to the resource it holds", i, s.ID)
	}
	return plain, nil
}

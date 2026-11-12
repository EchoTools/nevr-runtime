package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math/bits"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const (
	zstdMagic    = 0xFD2FB528 // RFC 8878, section 3.1.1: the first four bytes of a frame, little endian
	maxAlignment = 4096       // the game's manifests use 4, 16, 2048 and 4096 (docs/reference/plugin-resources.md)
)

// frameWindow is the window a frame asks its decoder for: the Window_Descriptor's, or, in a single-segment frame, the
// content size (RFC 8878, section 3.1.1.1.2).
func frameWindow(h *zstd.Header) uint64 {
	if h.SingleSegment {
		return h.FrameContentSize
	}
	return h.WindowSize
}

// checkFrame is the rule a stored frame must meet: it starts with the zstd magic (a skippable frame does not), its
// header states a content size and the size is the record's, and its window is at most maxWindow.
func checkFrame(frame []byte, bytes, maxWindow uint64) error {
	if len(frame) < 4 || binary.LittleEndian.Uint32(frame) != zstdMagic {
		return errors.New("the frame does not start with the zstd magic")
	}
	var h zstd.Header
	if err := h.Decode(frame); err != nil {
		return fmt.Errorf("the frame header does not decode: %w", err)
	}
	if !h.HasFCS || h.FrameContentSize != bytes {
		return fmt.Errorf("the frame header content size is %d (stated: %t), the record is %d bytes", h.FrameContentSize, h.HasFCS, bytes)
	}
	if w := frameWindow(&h); w > maxWindow {
		return fmt.Errorf("the frame window is %d bytes, larger than the stock chunks' largest, %d", w, maxWindow)
	}
	return nil
}

// checkAlignment is the rule for a record's alignment: a power of two of at most maxAlignment.
func checkAlignment(a uint32) error {
	if a == 0 || a&(a-1) != 0 || a > maxAlignment {
		return fmt.Errorf("the alignment %d is not a power of two of at most %d", a, maxAlignment)
	}
	return nil
}

// windowStats is what the stock chunks' frame headers say (every chunk of every set read, an end record excepted).
type windowStats struct {
	Chunks          int            `json:"chunks"`
	MaxWindow       uint64         `json:"max_window"`
	SingleSegment   int            `json:"single_segment"`
	WithContentSize int            `json:"with_content_size"`
	WithChecksum    int            `json:"with_checksum"`
	WindowLog2      map[string]int `json:"window_log2"` // chunks by ceil(log2(window)): "22" holds windows in (2^21, 2^22]
}

// measureStock reads the frame header of every chunk of the sets (a chunk of 0 decompressed bytes is a package's end
// record, not a frame).
func measureStock(chunks *radpack.Chunks, sets []*radpack.Set) (windowStats, error) {
	st := windowStats{WindowLog2: map[string]int{}}
	for _, s := range sets {
		for i, c := range s.Manifest.Chunks {
			if c.Decompressed == 0 {
				continue
			}
			head, err := chunks.FrameHead(s, i, zstd.HeaderMaxSize)
			if err != nil {
				return st, err
			}
			var h zstd.Header
			if len(head) < 4 || binary.LittleEndian.Uint32(head) != zstdMagic {
				return st, fmt.Errorf("chunk %d of %s does not start with the zstd magic", i, s.ID)
			}
			if err := h.Decode(head); err != nil {
				return st, fmt.Errorf("chunk %d of %s: the frame header does not decode: %w", i, s.ID, err)
			}
			w := frameWindow(&h)
			st.Chunks++
			st.MaxWindow = max(st.MaxWindow, w)
			if h.SingleSegment {
				st.SingleSegment++
			}
			if h.HasFCS {
				st.WithContentSize++
			}
			if h.HasCheckSum {
				st.WithChecksum++
			}
			st.WindowLog2[fmt.Sprint(bits.Len64(max(w, 1)-1))]++
		}
	}
	if st.Chunks == 0 {
		return st, errors.New("the sets hold no chunk")
	}
	return st, nil
}

// encoderWindow is the window the encoder is given: the largest power of two that is at most the stock chunks' largest
// window (the encoder takes powers of two only), inside the encoder's own bounds.
func encoderWindow(stockMax uint64) int {
	w := uint64(1) << (bits.Len64(max(stockMax, 1)) - 1)
	return int(min(max(w, zstd.MinWindowSize), zstd.MaxWindowSize))
}

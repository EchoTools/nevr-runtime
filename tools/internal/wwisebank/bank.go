// Package wwisebank reads a Wwise sound bank into its chunks and writes it back byte for byte.
//
// A bank is a run of chunks: a 4-byte tag, a u32 little-endian payload size and the payload (BKHD, DIDX, DATA, HIRC,
// and others). The payload of a chunk is kept as it was; HIRC has a second reader, ParseHIRC, that splits its payload
// into objects (a u8 type, a u32 size, a u32 id and a body) and keeps each body opaque.
//
// This is not the scanner of tools/cmd/wwise-events: that one searches a byte stream for banks and records only ids,
// so it keeps no bytes to write back. This package takes one bank that starts at its BKHD chunk.
package wwisebank

import (
	"encoding/binary"
	"errors"
	"fmt"
)

const (
	tagBytes    = 4
	sizeBytes   = 4
	headerBytes = tagBytes + sizeBytes
	// hircCountBytes is the u32 object count that opens a HIRC payload.
	hircCountBytes = 4
	// hircObjHeaderBytes is an object's u8 type and u32 size.
	hircObjHeaderBytes = 1 + sizeBytes
	// hircIDBytes is the u32 id that opens every object's sized part.
	hircIDBytes = 4
	headerTag   = "BKHD"
)

var (
	// ErrTruncated is returned when a chunk header or payload runs past the end of the input.
	ErrTruncated = errors.New("wwisebank: chunk runs past the end of the bank")
	// ErrNoHeader is returned when the first chunk is not BKHD.
	ErrNoHeader = errors.New("wwisebank: the first chunk is not BKHD")
	// ErrBadHIRC is returned when a HIRC payload does not hold exactly its counted objects.
	ErrBadHIRC = errors.New("wwisebank: malformed HIRC payload")
)

// Chunk is one tagged chunk of a bank.
type Chunk struct {
	Tag     [tagBytes]byte
	Payload []byte
}

// Bank is the chunks of a bank in file order.
type Bank struct {
	Chunks []Chunk
}

// Parse splits b, which starts at the BKHD chunk and holds nothing after the last chunk, into chunks.
func Parse(b []byte) (*Bank, error) {
	bank := &Bank{}
	for off := 0; off < len(b); {
		if len(b)-off < headerBytes {
			return nil, fmt.Errorf("%w: header at offset %d", ErrTruncated, off)
		}
		var c Chunk
		copy(c.Tag[:], b[off:off+tagBytes])
		size := uint64(binary.LittleEndian.Uint32(b[off+tagBytes : off+headerBytes]))
		off += headerBytes
		if size > uint64(len(b)-off) {
			return nil, fmt.Errorf("%w: %s declares %d bytes, %d remain", ErrTruncated, c.Tag[:], size, len(b)-off)
		}
		c.Payload = append([]byte(nil), b[off:off+int(size)]...)
		off += int(size)
		bank.Chunks = append(bank.Chunks, c)
	}
	if len(bank.Chunks) == 0 || string(bank.Chunks[0].Tag[:]) != headerTag {
		return nil, ErrNoHeader
	}
	return bank, nil
}

// Bytes writes the bank back: the input of Parse, byte for byte.
func (b *Bank) Bytes() []byte {
	var out []byte
	for _, c := range b.Chunks {
		out = append(out, c.Tag[:]...)
		out = binary.LittleEndian.AppendUint32(out, uint32(len(c.Payload)))
		out = append(out, c.Payload...)
	}
	return out
}

// Chunk returns the first chunk with the tag, or nil.
func (b *Bank) Chunk(tag string) *Chunk {
	for i := range b.Chunks {
		if string(b.Chunks[i].Tag[:]) == tag {
			return &b.Chunks[i]
		}
	}
	return nil
}

// Object is one HIRC object. Body is everything after the id, kept opaque.
type Object struct {
	Type uint8
	ID   uint32
	Body []byte
}

// ParseHIRC splits a HIRC payload into its objects. The payload must hold exactly the counted objects.
func ParseHIRC(p []byte) ([]Object, error) {
	if len(p) < hircCountBytes {
		return nil, fmt.Errorf("%w: no object count", ErrBadHIRC)
	}
	count := binary.LittleEndian.Uint32(p)
	off := hircCountBytes
	var objs []Object
	for i := uint32(0); i < count; i++ {
		if len(p)-off < hircObjHeaderBytes {
			return nil, fmt.Errorf("%w: object %d header", ErrBadHIRC, i)
		}
		typ := p[off]
		size := uint64(binary.LittleEndian.Uint32(p[off+1 : off+hircObjHeaderBytes]))
		off += hircObjHeaderBytes
		if size < hircIDBytes || size > uint64(len(p)-off) {
			return nil, fmt.Errorf("%w: object %d size %d", ErrBadHIRC, i, size)
		}
		objs = append(objs, Object{
			Type: typ,
			ID:   binary.LittleEndian.Uint32(p[off:]),
			Body: append([]byte(nil), p[off+hircIDBytes:off+int(size)]...),
		})
		off += int(size)
	}
	if off != len(p) {
		return nil, fmt.Errorf("%w: %d bytes after the last object", ErrBadHIRC, len(p)-off)
	}
	return objs, nil
}

// EncodeHIRC writes objects as a HIRC payload.
func EncodeHIRC(objs []Object) []byte {
	out := binary.LittleEndian.AppendUint32(nil, uint32(len(objs)))
	for _, o := range objs {
		out = append(out, o.Type)
		out = binary.LittleEndian.AppendUint32(out, uint32(hircIDBytes+len(o.Body)))
		out = binary.LittleEndian.AppendUint32(out, o.ID)
		out = append(out, o.Body...)
	}
	return out
}

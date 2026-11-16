package wwisebank

import (
	"encoding/binary"
	"errors"
	"fmt"
)

// ErrBadResource is returned when the bytes are not a CWWiseSoundBankResourceWin10 record: the wrapper, the bank or the
// trailer does not hold what PLAN-v5 S-SND 1.1 says.
var ErrBadResource = errors.New("wwisebank: not a sound bank resource")

const (
	resU64Bytes = 8
	// resHeaderBytes is the record's u64 bank length and u64 reserved word (PLAN-v5 S-SND 1.1).
	resHeaderBytes = 2 * resU64Bytes
	// resTrailerBytes is the record's u32 trailer flag and u32 trailer bank id.
	resTrailerBytes = 8
	// resWrapperBytes is the whole wrapper around the bank.
	resWrapperBytes = resHeaderBytes + resTrailerBytes
	resU32Bytes     = 4
	// bkhdBankIDOff is the bank id's offset in the BKHD payload (PLAN-v5 S-SND 1.1).
	bkhdBankIDOff   = 4
	bkhdBankIDBytes = 4
)

// Resource is a CWWiseSoundBankResourceWin10 record: its bank and the trailer's flag (PLAN-v5 S-SND 1.1). The trailer's
// bank id is not kept: it is the bank's own id, so Bytes writes it from the BKHD chunk.
type Resource struct {
	Bank        *Bank
	TrailerFlag uint32
}

// ParseResource reads a resource record. It refuses (ErrBadResource) a record whose u64 bank length is not the length
// between the wrapper's two words, whose reserved word is not 0, whose bank does not parse or has no BKHD bank id, or
// whose trailer bank id is not the bank's. The result shares no bytes with b.
func ParseResource(b []byte) (*Resource, error) {
	if len(b) < resWrapperBytes {
		return nil, fmt.Errorf("%w: %d bytes is shorter than the %d-byte wrapper", ErrBadResource, len(b), resWrapperBytes)
	}
	bankLen := binary.LittleEndian.Uint64(b)
	if bankLen != uint64(len(b)-resWrapperBytes) {
		return nil, fmt.Errorf("%w: bank length %d, the record holds %d", ErrBadResource, bankLen, len(b)-resWrapperBytes)
	}
	if binary.LittleEndian.Uint64(b[resU64Bytes:]) != 0 {
		return nil, fmt.Errorf("%w: reserved word is not 0", ErrBadResource)
	}
	bank, err := Parse(b[resHeaderBytes : len(b)-resTrailerBytes])
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrBadResource, err)
	}
	id, err := bank.BankID()
	if err != nil {
		return nil, err
	}
	if trailerID := binary.LittleEndian.Uint32(b[len(b)-resU32Bytes:]); trailerID != id {
		return nil, fmt.Errorf("%w: trailer bank id %#x, the bank's is %#x", ErrBadResource, trailerID, id)
	}
	return &Resource{
		Bank:        bank,
		TrailerFlag: binary.LittleEndian.Uint32(b[len(b)-resTrailerBytes:]),
	}, nil
}

// Bytes writes the record: the bank length, the reserved 0, the bank, the trailer flag and the bank id. A Resource that
// ParseResource returned has a BKHD bank id; one built by hand with a shorter BKHD writes 0 as its id.
func (r *Resource) Bytes() []byte {
	bank := r.Bank.Bytes()
	id, err := r.Bank.BankID()
	if err != nil {
		id = 0
	}
	out := make([]byte, 0, resWrapperBytes+len(bank))
	out = binary.LittleEndian.AppendUint64(out, uint64(len(bank)))
	out = binary.LittleEndian.AppendUint64(out, 0)
	out = append(out, bank...)
	out = binary.LittleEndian.AppendUint32(out, r.TrailerFlag)
	return binary.LittleEndian.AppendUint32(out, id)
}

// BankID returns the bank id the BKHD chunk holds (PLAN-v5 S-SND 1.1). It refuses (ErrBadResource) a bank with no BKHD
// chunk or a BKHD payload too short to hold it.
func (b *Bank) BankID() (uint32, error) {
	c := b.Chunk(headerTag)
	if c == nil || len(c.Payload) < bkhdBankIDOff+bkhdBankIDBytes {
		return 0, fmt.Errorf("%w: the BKHD chunk holds no bank id", ErrBadResource)
	}
	return binary.LittleEndian.Uint32(c.Payload[bkhdBankIDOff:]), nil
}

package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/klauspost/compress/zstd"
)

func chunk(tag string, body []byte) []byte {
	out := append([]byte(tag), 0, 0, 0, 0)
	binary.LittleEndian.PutUint32(out[4:], uint32(len(body)))
	return append(out, body...)
}

func u32(v uint32) []byte {
	b := make([]byte, 4)
	binary.LittleEndian.PutUint32(b, v)
	return b
}

// object is one HIRC object: type, then size, then the id and `extra` bytes.
func object(typ byte, id uint32, extra int) []byte {
	body := append(u32(id), make([]byte, extra)...)
	out := append([]byte{typ}, u32(uint32(len(body)))...)
	return append(out, body...)
}

// fixtureBank is a bank of version 134 and id `bankID` whose HIRC holds the given events and one sound (type 2) per sound id.
func fixtureBank(bankID uint32, events, sounds []uint32) []byte {
	bkhd := append(u32(134), u32(bankID)...)
	bkhd = append(bkhd, make([]byte, 12)...)
	hirc := u32(uint32(len(events) + len(sounds)))
	for _, s := range sounds {
		hirc = append(hirc, object(2, s, 7)...)
	}
	for _, e := range events {
		hirc = append(hirc, object(hircEventType, e, 9)...)
	}
	out := chunk("BKHD", bkhd)
	out = append(out, chunk("DIDX", make([]byte, 12))...)
	out = append(out, chunk("DATA", bytes.Repeat([]byte("BKHD"), 3))...) // a "BKHD" inside a chunk is not a bank
	out = append(out, chunk("HIRC", hirc)...)
	return append(out, chunk("STID", make([]byte, 8))...)
}

// stream is filler, two false "BKHD"s (one with a header size too large and a plausible version, one with a plausible size and
// version 0), two banks and more filler.
func stream() []byte {
	var b []byte
	b = append(b, bytes.Repeat([]byte{0xaa}, 1000)...)
	b = append(b, []byte("BKHD")...)
	b = append(b, u32(0x7fffffff)...)
	b = append(b, u32(134)...)
	b = append(b, bytes.Repeat([]byte{0x55}, 50)...)
	b = append(b, []byte("BKHD")...)
	b = append(b, u32(16)...)
	b = append(b, u32(0)...)
	b = append(b, bytes.Repeat([]byte{0x66}, 50)...)
	b = append(b, fixtureBank(0x11111111, []uint32{0x1406b30a, 0x22222222}, []uint32{0xc7b3d4fb})...)
	b = append(b, fixtureBank(0x33333333, []uint32{0x1406b30a}, nil)...)
	b = append(b, []byte("end of the record")...)
	return b
}

func TestScanFindsBanksAndEvents(t *testing.T) {
	banks, err := Scan(bytes.NewReader(stream()), "fixture")
	if err != nil {
		t.Fatal(err)
	}
	if len(banks) != 2 {
		t.Fatalf("banks = %d, want 2: %+v", len(banks), banks)
	}
	if banks[0].BankID != "0x11111111" || banks[0].Version != 134 || banks[0].Events != 2 || banks[0].Objects != 3 || banks[0].Malformed {
		t.Fatalf("bank 0 = %+v", banks[0])
	}
	if banks[0].Offset != 1000+(4+4+4+50)+(4+4+4+50) {
		t.Fatalf("bank 0 offset = %d", banks[0].Offset)
	}
	if banks[1].BankID != "0x33333333" || banks[1].Events != 1 {
		t.Fatalf("bank 1 = %+v", banks[1])
	}
	n, q := answer(banks, []uint32{0x1406b30a, 0x00000001, 0xc7b3d4fb})
	if n != 2 {
		t.Fatalf("distinct events = %d, want 2", n)
	}
	if !q[0].Event || strings.Join(q[0].EventBanks, ",") != "0x11111111,0x33333333" || q[0].OtherObject {
		t.Fatalf("0x1406b30a = %+v", q[0])
	}
	if q[1].Event || q[1].OtherObject {
		t.Fatalf("0x00000001 = %+v", q[1])
	}
	if q[2].Event || !q[2].OtherObject {
		t.Fatalf("0xc7b3d4fb = %+v", q[2])
	}
}

// A bank that straddles the scanner's window is read whole.
func TestScanAcrossTheWindow(t *testing.T) {
	b := append(bytes.Repeat([]byte{0}, scanWindow-10), fixtureBank(0x44444444, []uint32{7}, nil)...)
	banks, err := Scan(bytes.NewReader(b), "fixture")
	if err != nil {
		t.Fatal(err)
	}
	if len(banks) != 1 || banks[0].Events != 1 || banks[0].Offset != scanWindow-10 {
		t.Fatalf("banks = %+v", banks)
	}
}

func TestMalformedHIRCIsMarked(t *testing.T) {
	bank := fixtureBank(0x55555555, []uint32{1}, nil)
	// The HIRC count claims one more object than the chunk holds.
	i := bytes.Index(bank, []byte("HIRC"))
	binary.LittleEndian.PutUint32(bank[i+8:], 2)
	banks, err := Scan(bytes.NewReader(bank), "fixture")
	if err != nil {
		t.Fatal(err)
	}
	if len(banks) != 1 || !banks[0].Malformed {
		t.Fatalf("banks = %+v", banks)
	}
}

func env(dir string) func(string) string {
	return func(k string) string {
		if k == "XDG_STATE_HOME" {
			return filepath.Join(dir, "state")
		}
		return ""
	}
}

func TestRunOnAZstdFileWritesTheReportAndTheLog(t *testing.T) {
	dir := t.TempDir()
	enc, err := zstd.NewWriter(nil)
	if err != nil {
		t.Fatal(err)
	}
	half := len(stream()) / 2
	// Two frames back to back, split inside a bank: the package files' shape.
	data := append(enc.EncodeAll(stream()[:half], nil), enc.EncodeAll(stream()[half:], nil)...)
	in := filepath.Join(dir, "pkg_0")
	if err := os.WriteFile(in, data, 0o644); err != nil {
		t.Fatal(err)
	}
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"-id", "0x1406b30a", "-id", "0x1", "-report", filepath.Join(dir, "r.json"), "-events", filepath.Join(dir, "e.txt"), in}, env(dir), nil, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("exit %d, stderr %s", code, stderr.String())
	}
	var rep Report
	if err := json.Unmarshal(stdout.Bytes(), &rep); err != nil {
		t.Fatal(err)
	}
	if len(rep.Banks) != 2 || !rep.Queries[0].Event || rep.Queries[1].Event || rep.Queries[1].ID != "0x00000001" {
		t.Fatalf("report = %+v", rep)
	}
	if _, err := os.Stat(filepath.Join(dir, "r.json")); err != nil {
		t.Fatal(err)
	}
	events, err := os.ReadFile(filepath.Join(dir, "e.txt"))
	if err != nil {
		t.Fatal(err)
	}
	if string(events) != "0x1406b30a\n0x22222222\n" {
		t.Fatalf("events file = %q", events)
	}
	logBytes, err := os.ReadFile(filepath.Join(dir, "state", "nevr-runtime", "wwise-events.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	for _, ev := range []string{`"event":"input_scanned"`, `"event":"id_answered"`, `"event":"done"`} {
		if !bytes.Contains(logBytes, []byte(ev)) {
			t.Fatalf("log lacks %s:\n%s", ev, logBytes)
		}
	}
}

func TestRunRefusesAnInputWithNoBank(t *testing.T) {
	dir := t.TempDir()
	in := filepath.Join(dir, "plain")
	if err := os.WriteFile(in, bytes.Repeat([]byte("x"), 100), 0o644); err != nil {
		t.Fatal(err)
	}
	var stdout, stderr bytes.Buffer
	if code := run(context.Background(), []string{in}, env(dir), nil, &stdout, &stderr); code != 1 {
		t.Fatalf("exit %d, want 1", code)
	}
	if code := run(context.Background(), []string{"-id", "zz", in}, env(dir), nil, &stdout, &stderr); code != 2 {
		t.Fatalf("bad -id: exit %d, want 2", code)
	}
}

// Command wwise-events lists the Wwise sound banks found in a byte stream and the ids of their HIRC objects, and says
// whether each id asked for with -id is an event in one of them. It answers "is this CSoundSymbol an event the game's
// banks define?" for the sound-symbol guard (src/quest/combat/sound_symbol_guard_targets.h).
//
// The input files are scanned whole for the "BKHD" chunk that starts a bank; a file that begins with a zstd frame is
// decompressed first (all of its frames, back to back), so a package file of the game's data
// (_data/<build>/rad15/<platform>/packages/<set>_<n>) is read as it is, with no manifest: the banks are records inside it.
// A bank is the chunk sequence BKHD, then any of DIDX, DATA, HIRC, STID, STMG, ENVS, FXPR, INIT, PLAT, until the next
// tag is none of them. A "BKHD" whose header is not a bank's (its size not 8..65536 bytes, or its version outside 1..1000)
// is passed over. In HIRC, each object is a type byte, a u32 size and that many bytes, of which the first u32 is the id;
// type 4 is an event.
//
//	wwise-events [-id 0x<hex>]... [-report <json>] [-events <file>] <file>...
//
// Standard output: one JSON object (the report): the banks with their file, offset in the decompressed stream, version,
// bank id and event count; the number of distinct event ids; and per -id whether it is an event and in which banks, and
// whether it is any other HIRC object. -report writes the same object to a file as well; -events writes the event ids.
//
// Exit codes: 0 the report is written (whatever it says); 1 an input could not be read or held no bank; 2 a usage
// error; 74 the run log failed (internal/runlog). The run log records each input's byte count and bank count, and each
// -id's answer.
package main

import (
	"bufio"
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"sort"
	"strconv"
	"strings"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const (
	hircEventType  = 4
	maxBankHeader  = 0x10000
	maxBankVersion = 1000
	scanWindow     = 1 << 20
	maxHIRC        = 64 << 20
)

var (
	bankMagic = []byte("BKHD")
	zstdMagic = []byte{0x28, 0xb5, 0x2f, 0xfd}
	bankTags  = map[string]bool{"BKHD": true, "DIDX": true, "DATA": true, "HIRC": true, "STID": true, "STMG": true,
		"ENVS": true, "FXPR": true, "INIT": true, "PLAT": true}
)

// Bank is one bank found in an input.
type Bank struct {
	File      string   `json:"file"`
	Offset    int64    `json:"offset"`
	Version   uint32   `json:"version"`
	BankID    string   `json:"bank_id"`
	Events    int      `json:"events"`
	Objects   int      `json:"objects"`
	Malformed bool     `json:"hirc_malformed,omitempty"`
	eventIDs  []uint32 // the HIRC event ids
	objectIDs []uint32 // every HIRC object id, events included
}

// Query is the answer for one -id.
type Query struct {
	ID          string   `json:"id"`
	Event       bool     `json:"event"`
	EventBanks  []string `json:"event_banks"`
	OtherObject bool     `json:"other_hirc_object"`
}

// Report is the tool's output.
type Report struct {
	Banks        []Bank  `json:"banks"`
	DistinctEvts int     `json:"distinct_events"`
	Queries      []Query `json:"queries"`
}

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

func run(_ context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	return runlog.Run("wwise-events", getenv, stderr, func(log *slog.Logger) int {
		return wwiseEvents(args, stdout, stderr, log)
	})
}

type multi []string

func (m *multi) String() string     { return strings.Join(*m, ",") }
func (m *multi) Set(v string) error { *m = append(*m, v); return nil }

func wwiseEvents(args []string, stdout, stderr io.Writer, log *slog.Logger) int {
	fs := flag.NewFlagSet("wwise-events", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var idArgs multi
	var reportPath, eventsPath string
	fs.Var(&idArgs, "id", "0x<hex>: an id to look up (repeat)")
	fs.StringVar(&reportPath, "report", "", "also write the report to this file")
	fs.StringVar(&eventsPath, "events", "", "write every distinct event id to this file, one 0x<hex> per line, sorted")
	if err := fs.Parse(args); err != nil {
		log.Error("usage", "error", err.Error())
		return 2
	}
	ids := make([]uint32, 0, len(idArgs))
	for _, s := range idArgs {
		v, err := strconv.ParseUint(strings.TrimPrefix(s, "0x"), 16, 32)
		if err != nil {
			fmt.Fprintf(stderr, "wwise-events: -id %q is not a 32-bit hex id\n", s)
			log.Error("usage", "error", "bad -id", "id", s)
			return 2
		}
		ids = append(ids, uint32(v))
	}
	if fs.NArg() == 0 {
		fmt.Fprintln(stderr, "usage: wwise-events [-id 0x<hex>]... [-report <json>] [-events <file>] <file>...")
		log.Error("usage", "error", "no input file")
		return 2
	}
	log.Info("start", "inputs", fs.Args(), "ids", idArgs)

	var report Report
	for _, path := range fs.Args() {
		banks, n, err := scanFile(path)
		if err != nil {
			fmt.Fprintf(stderr, "wwise-events: %s: %v\n", path, err)
			log.Error("input_failed", "file", path, "bytes", n, "error", err.Error())
			return 1
		}
		log.Info("input_scanned", "file", path, "bytes", n, "banks", len(banks))
		if len(banks) == 0 {
			fmt.Fprintf(stderr, "wwise-events: %s: no Wwise bank in %d bytes\n", path, n)
			log.Error("no_bank", "file", path, "bytes", n)
			return 1
		}
		report.Banks = append(report.Banks, banks...)
	}
	report.DistinctEvts, report.Queries = answer(report.Banks, ids)
	for _, q := range report.Queries {
		log.Info("id_answered", "id", q.ID, "event", q.Event, "event_banks", q.EventBanks, "other_hirc_object", q.OtherObject)
	}
	out, err := json.MarshalIndent(report, "", "  ")
	if err != nil {
		log.Error("report_failed", "error", err.Error())
		return 1
	}
	out = append(out, '\n')
	if _, err := stdout.Write(out); err != nil {
		log.Error("report_failed", "error", err.Error())
		return 1
	}
	if reportPath != "" {
		if err := os.WriteFile(reportPath, out, 0o644); err != nil {
			fmt.Fprintf(stderr, "wwise-events: %v\n", err)
			log.Error("report_failed", "file", reportPath, "error", err.Error())
			return 1
		}
	}
	if eventsPath != "" {
		if err := os.WriteFile(eventsPath, []byte(eventList(report.Banks)), 0o644); err != nil {
			fmt.Fprintf(stderr, "wwise-events: %v\n", err)
			log.Error("events_failed", "file", eventsPath, "error", err.Error())
			return 1
		}
	}
	log.Info("done", "banks", len(report.Banks), "distinct_events", report.DistinctEvts, "report", reportPath)
	return 0
}

// answer counts the distinct event ids and answers each query.
func answer(banks []Bank, ids []uint32) (int, []Query) {
	events := map[uint32][]string{}
	objects := map[uint32]bool{}
	for _, b := range banks {
		for _, id := range b.eventIDs {
			events[id] = append(events[id], b.BankID)
		}
		for _, id := range b.objectIDs {
			objects[id] = true
		}
	}
	queries := make([]Query, 0, len(ids))
	for _, id := range ids {
		inBanks := append([]string{}, events[id]...)
		sort.Strings(inBanks)
		_, isEvent := events[id]
		queries = append(queries, Query{
			ID: fmt.Sprintf("0x%08x", id), Event: isEvent, EventBanks: inBanks, OtherObject: objects[id] && !isEvent,
		})
	}
	return len(events), queries
}

// eventList is every distinct event id of the banks, sorted, one 0x%08x per line.
func eventList(banks []Bank) string {
	seen := map[uint32]bool{}
	var ids []uint32
	for _, b := range banks {
		for _, id := range b.eventIDs {
			if !seen[id] {
				seen[id] = true
				ids = append(ids, id)
			}
		}
	}
	sort.Slice(ids, func(i, j int) bool { return ids[i] < ids[j] })
	var sb strings.Builder
	for _, id := range ids {
		fmt.Fprintf(&sb, "0x%08x\n", id)
	}
	return sb.String()
}

// countingReader counts the bytes read through it.
type countingReader struct {
	r io.Reader
	n int64
}

func (c *countingReader) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.n += int64(n)
	return n, err
}

func scanFile(path string) ([]Bank, int64, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, 0, err
	}
	defer f.Close()
	head := bufio.NewReaderSize(f, 64)
	magic, _ := head.Peek(4)
	var src io.Reader = head
	if bytes.Equal(magic, zstdMagic) {
		dec, err := zstd.NewReader(head, zstd.WithDecoderMaxWindow(1<<31), zstd.WithDecoderConcurrency(1))
		if err != nil {
			return nil, 0, err
		}
		defer dec.Close()
		src = dec
	}
	counter := &countingReader{r: src}
	banks, err := Scan(counter, path)
	return banks, counter.n, err
}

// Scan reads r to its end and returns every bank it finds. file names the input in each Bank.
func Scan(r io.Reader, file string) ([]Bank, error) {
	br := bufio.NewReaderSize(r, scanWindow)
	var off int64
	var banks []Bank
	for {
		buf, perr := br.Peek(scanWindow)
		if len(buf) < 8 {
			if perr != nil && !errors.Is(perr, io.EOF) {
				return banks, perr
			}
			return banks, nil
		}
		i := bytes.Index(buf, bankMagic)
		if i < 0 {
			skip := len(buf) - (len(bankMagic) - 1)
			if _, err := br.Discard(skip); err != nil {
				return banks, err
			}
			off += int64(skip)
			continue
		}
		if _, err := br.Discard(i); err != nil {
			return banks, err
		}
		off += int64(i)
		bank, used, err := readBank(br)
		if err != nil {
			return banks, err
		}
		if used == 0 {
			if _, err := br.Discard(1); err != nil {
				return banks, err
			}
			off++
			continue
		}
		bank.File = file
		bank.Offset = off
		banks = append(banks, bank)
		off += used
	}
}

// readBank reads the bank at the reader's position. It consumes nothing and returns used == 0 when the "BKHD" there
// does not start a bank.
func readBank(br *bufio.Reader) (Bank, int64, error) {
	hdr, _ := br.Peek(16)
	if len(hdr) < 16 {
		return Bank{}, 0, nil
	}
	size := binary.LittleEndian.Uint32(hdr[4:8])
	version := binary.LittleEndian.Uint32(hdr[8:12])
	if size < 8 || size > maxBankHeader || version == 0 || version > maxBankVersion {
		return Bank{}, 0, nil
	}
	bank := Bank{Version: version, BankID: fmt.Sprintf("0x%08x", binary.LittleEndian.Uint32(hdr[12:16]))}
	var used int64
	for {
		tagHdr, _ := br.Peek(8)
		if len(tagHdr) < 8 || !bankTags[string(tagHdr[:4])] {
			return bank, used, nil
		}
		if used > 0 && string(tagHdr[:4]) == "BKHD" {
			return bank, used, nil // the next bank starts here
		}
		chunk := int64(binary.LittleEndian.Uint32(tagHdr[4:8]))
		if _, err := br.Discard(8); err != nil {
			return bank, used, err
		}
		used += 8
		if string(tagHdr[:4]) == "HIRC" {
			if chunk > maxHIRC {
				bank.Malformed = true
				return bank, used, nil
			}
			body := make([]byte, chunk)
			n, err := io.ReadFull(br, body)
			used += int64(n)
			if err != nil {
				bank.Malformed = true
				return bank, used, nil
			}
			parseHIRC(body, &bank)
			continue
		}
		n, err := br.Discard(int(chunk))
		used += int64(n)
		if err != nil {
			return bank, used, nil // the stream ended inside the chunk
		}
	}
}

// parseHIRC records the object ids of a HIRC body.
func parseHIRC(body []byte, bank *Bank) {
	if len(body) < 4 {
		bank.Malformed = true
		return
	}
	count := binary.LittleEndian.Uint32(body[:4])
	p := 4
	for i := uint32(0); i < count; i++ {
		if p+5 > len(body) {
			bank.Malformed = true
			return
		}
		typ := body[p]
		size := int(binary.LittleEndian.Uint32(body[p+1 : p+5]))
		p += 5
		if size < 4 || p+size > len(body) {
			bank.Malformed = true
			return
		}
		id := binary.LittleEndian.Uint32(body[p : p+4])
		bank.objectIDs = append(bank.objectIDs, id)
		bank.Objects++
		if typ == hircEventType {
			bank.eventIDs = append(bank.eventIDs, id)
			bank.Events++
		}
		p += size
	}
	if p != len(body) {
		bank.Malformed = true
	}
}

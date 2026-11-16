// Package scriptgate is the script_undelivered check of the cargo manifest builders (cargo-pack, cargo-merge; PLAN-v5 piece G2).
//
// The game loads a script DLL by name from its scripts folder and has no existence check before the load. A label that names a
// script the player's game cannot have therefore ships a level that fails at load. The check refuses such a label whole: a script
// the label names must be one of the stock game's (its CScriptResource record is in the stock manifest) or one the label itself
// lists in its `scripts` list, which is the label's promise to deliver that file (docs/adr/0045-cdn-texture-delivery.md).
//
// A label names a script in two places:
//   - a CScriptResource record (on-disk type 0x991421b41de58370, whether it is an entry or a record of 0 bytes): its name is the
//     script's name (src/core/resource_set_registry.h kScriptType);
//   - a CScriptCR row: a record of the level table d99f6bbd8009c92c holds a 0x38-byte header and rows of 720 bytes, and the row's
//     u64 at +32 is the script it runs (the layout the EchoCombat pack's attach_point_script reads). The record's name is the
//     level the table is for.
//
// A row whose script is 0 names none and is not checked. A table whose bytes the caller does not have cannot be read: Check
// returns its name in Result.Unchecked so the caller says so, and never counts it as delivered.
package scriptgate

import (
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"sort"
	"strconv"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
)

const (
	// ScriptType is CScriptResourceWin10 (src/core/resource_set_registry.h kScriptType).
	ScriptType uint64 = 0x991421b41de58370
	// TableType is the level table that holds the CScriptCR rows (radpack.RowTable: scripts d99f6bbd8009c92c).
	TableType uint64 = 0xd99f6bbd8009c92c

	// RowStride is the size of one CScriptCR row, and ScriptOffset is where its script is (fix_point.py attach_point_script).
	RowStride    = 720
	ScriptOffset = 32

	headerBytes = 0x38 // the table's header: +0x08 the rows' bytes, +0x28 the capacity, +0x30 the count
	bytesAt     = 0x08
	countAt     = 0x30
	hexBase     = 16
	hexBits     = 64
	maxListed   = 8 // the scripts a refusal names before it counts the rest

	// Reason is the token the refusal carries (the runtime's cdn_mount line names the same reason).
	Reason = "script_undelivered"
)

// Stock is the script names the stock game has: the CScriptResource records of its manifests.
type Stock map[uint64]bool

// LoadStock reads each set `<data>/manifests/<id>` and collects the name of every record of ScriptType.
func LoadStock(data string, sets []string) (Stock, error) {
	stock := Stock{}
	for _, id := range sets {
		s, _, err := radpack.OpenSet(data, id)
		if err != nil {
			return nil, fmt.Errorf("stock set %s: %w", id, err)
		}
		for _, r := range s.Manifest.Resources {
			if uint64(r.Type) == ScriptType {
				stock[uint64(r.Name)] = true
			}
		}
	}
	return stock, nil
}

// Listed is one entry of a label's `scripts` list: the script it delivers, for the one platform the entry names. A list
// entry delivers a script only to a record of its own platform, so an android-arm64 entry never excuses a windows-x86_64
// record of the same name.
type Listed struct {
	Platform string
	Name     uint64
}

// Gate is what a label's scripts are checked against: the stock scripts, and the scripts each label lists as its own.
// The zero Gate has no stock script and no list, so every script a label names is undelivered.
type Gate struct {
	Stock  Stock
	Listed map[string]map[Listed]bool // label -> the (platform, script) entries its `scripts` list holds
}

// WithListed is a copy of g in which label's `scripts` list also holds entries. g's maps are not written, so a caller's gate
// is never changed by a label's list.
func (g Gate) WithListed(label string, entries []Listed) Gate {
	out := Gate{Stock: g.Stock, Listed: make(map[string]map[Listed]bool, len(g.Listed)+1)}
	for l, set := range g.Listed {
		out.Listed[l] = set
	}
	set := make(map[Listed]bool, len(g.Listed[label])+len(entries))
	for e := range g.Listed[label] {
		set[e] = true
	}
	for _, e := range entries {
		set[e] = true
	}
	out.Listed[label] = set
	return out
}

// Record is one record of a label, with its platform. Data is the record's bytes, or nil when the caller does not hold them.
type Record struct {
	Platform   string
	Type, Name uint64
	Data       []byte
}

// Undelivered is one script a label names that nothing delivers.
type Undelivered struct {
	Script uint64
	Level  uint64 // the table's level for a row; 0 for a script record, which has no level
	Row    bool
}

// Result is what Check found in one label.
type Result struct {
	Undelivered []Undelivered
	// Unchecked is the names of the level tables whose bytes were not supplied, so their rows were not read.
	Unchecked []uint64
}

// Check reads one label's records against the gate.
func (g Gate) Check(label string, recs []Record) (Result, error) {
	var res Result
	seen := map[Undelivered]bool{}
	note := func(platform string, u Undelivered) {
		if !g.Stock[u.Script] && !g.Listed[label][Listed{Platform: platform, Name: u.Script}] && !seen[u] {
			seen[u] = true
			res.Undelivered = append(res.Undelivered, u)
		}
	}
	for _, r := range recs {
		switch r.Type {
		case ScriptType:
			note(r.Platform, Undelivered{Script: r.Name})
		case TableType:
			if r.Data == nil {
				res.Unchecked = append(res.Unchecked, r.Name)
				continue
			}
			if len(r.Data) == 0 {
				continue // a record of 0 bytes holds no row
			}
			scripts, err := RowScripts(r.Data)
			if err != nil {
				return res, fmt.Errorf("label %s: level table %s: %w", label, Hex(r.Name), err)
			}
			for _, s := range scripts {
				if s != 0 {
					note(r.Platform, Undelivered{Script: s, Level: r.Name, Row: true})
				}
			}
		}
	}
	sort.Slice(res.Undelivered, func(i, j int) bool {
		a, b := res.Undelivered[i], res.Undelivered[j]
		return a.Script < b.Script || (a.Script == b.Script && a.Level < b.Level)
	})
	return res, nil
}

// RowScripts is the script of each row of a CScriptCR table.
func RowScripts(table []byte) ([]uint64, error) {
	if len(table) < headerBytes {
		return nil, errors.New("shorter than its 0x38-byte header")
	}
	le := binary.LittleEndian
	nbytes, count := le.Uint64(table[bytesAt:]), le.Uint64(table[countAt:])
	if count > uint64(len(table)) || nbytes != count*RowStride || nbytes > uint64(len(table))-headerBytes {
		return nil, fmt.Errorf("header says %d rows in %d bytes, not rows of %d bytes that fit a table of %d", count, nbytes, RowStride, len(table))
	}
	out := make([]uint64, 0, count)
	for i := uint64(0); i < count; i++ {
		out = append(out, le.Uint64(table[headerBytes+i*RowStride+ScriptOffset:]))
	}
	return out, nil
}

// Hex is a name as the manifests write it.
func Hex(v uint64) string { return fmt.Sprintf("0x%016x", v) }

// ParseHex reads a manifest's 0x and 16 hex digits.
func ParseHex(s string) (uint64, error) {
	h, ok := strings.CutPrefix(s, "0x")
	if !ok {
		return 0, fmt.Errorf("%q is not 0x and hex digits", s)
	}
	return strconv.ParseUint(h, hexBase, hexBits)
}

// Refusal is the error that refuses a label, naming the label, each script and its level; nil when nothing is undelivered.
func Refusal(label string, us []Undelivered) error {
	if len(us) == 0 {
		return nil
	}
	parts := make([]string, 0, maxListed)
	for _, u := range us[:min(len(us), maxListed)] {
		if u.Row {
			parts = append(parts, fmt.Sprintf("script=%s level=%s (a CScriptCR row)", Hex(u.Script), Hex(u.Level)))
		} else {
			parts = append(parts, fmt.Sprintf("script=%s level=none (a CScriptResource record)", Hex(u.Script)))
		}
	}
	if len(us) > maxListed {
		parts = append(parts, fmt.Sprintf("and %d more", len(us)-maxListed))
	}
	return fmt.Errorf("label %s reason=%s: %d undelivered: %s; each is neither in the stock manifest nor in the label's scripts list, and the game has no existence check before a script load, so the label is refused",
		label, Reason, len(us), strings.Join(parts, "; "))
}

// Flags is the command-line half both tools share: where the stock manifests are.
type Flags struct {
	data string
	sets setList
}

type setList []string

func (s *setList) String() string { return strings.Join(*s, ",") }

func (s *setList) Set(v string) error {
	if len(v) != hexBase {
		return fmt.Errorf("a stock set is 16 lowercase hex digits, not %q", v)
	}
	*s = append(*s, v)
	return nil
}

// Register adds -stock-data and the repeatable -stock-set to fs.
func Register(fs *flag.FlagSet) *Flags {
	f := &Flags{}
	fs.StringVar(&f.data, "stock-data", "", "the game's data folder, which holds manifests/<set>: the scripts its records name are the stock ones")
	fs.Var(&f.sets, "stock-set", "a stock package set id to read from -stock-data; repeatable")
	return f
}

// Gate reads the stock scripts the flags name. With no flag there is no stock script, so any script a label names is undelivered.
func (f *Flags) Gate() (Gate, error) {
	if (f.data == "") != (len(f.sets) == 0) {
		return Gate{}, errors.New("-stock-data and -stock-set go together")
	}
	if f.data == "" {
		return Gate{}, nil
	}
	stock, err := LoadStock(f.data, f.sets)
	return Gate{Stock: stock}, err
}

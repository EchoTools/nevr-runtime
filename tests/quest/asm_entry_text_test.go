package quest

import (
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"
)

// A reviewed assembly entry's own text (callback_thunk.h rule 7), read as a flow of states: the sensor runs the entry's
// instructions from its first one, along every branch, and at each instruction knows what sp, the frame pointer, x30 and every
// register the entry has loaded hold. It checks the entry against that, and against the unwind row in force at the instruction.

// One instruction of an entry, as llvm-objdump prints it. `target` is a direct branch's destination (0 otherwise).
type asmInsn struct {
	addr      uint64
	mnem, ops string
	target    uint64
}

// One row of an FDE's unwind table, as readelf --debug-dump=frames-interp prints it: the CFA rule ("sp+16", "x29+16") and
// the return-address column ("c-8": saved at CFA-8; "r19": kept in x19; "u" or "s": no rule, so x30 is taken to hold it).
type cfiRow struct {
	loc     uint64
	cfa, ra string
}

// An entry and what the sensor reads of it: its text, the rows of its FDE (nil: no FDE; the walk reports that too) and the
// continuation its record names.
type asmEntryText struct {
	name       string
	start, end uint64
	insns      []asmInsn
	rows       []cfiRow
	cont       uint64
	contName   string
}

var (
	insnRe      = regexp.MustCompile(`^\s+([0-9a-f]+):\s+(\S+)(?:\s+(.*?))?\s*$`)
	branchTgtRe = regexp.MustCompile(`0x([0-9a-f]+) <`)
	loadBaseRe  = regexp.MustCompile(`\[(\w+)`)
	spPreRe     = regexp.MustCompile(`\[sp, #(-?(?:0x)?[0-9a-f]+)\]!$`)
	spPostRe    = regexp.MustCompile(`\[sp\], #(-?(?:0x)?[0-9a-f]+)$`)
	spOffRe     = regexp.MustCompile(`\[sp(?:, #(-?(?:0x)?[0-9a-f]+))?\]$`)
	spArithRe   = regexp.MustCompile(`^sp, sp, #((?:0x)?[0-9a-f]+)$`)
	fpAddRe     = regexp.MustCompile(`^x29, sp, #((?:0x)?[0-9a-f]+)$`)
	gpRegRe     = regexp.MustCompile(`^[xw]([0-9]|[12][0-9]|30)$`)
)

// The loads that order a read after the release store that published the word (ldar, and the RCpc ldapr, in their sized forms).
// Every other load of a word the entry does not own is refused by its exact opcode: ldadd and ldxr begin with the same letters.
var acquireLoads = map[string]bool{"ldar": true, "ldarb": true, "ldarh": true, "ldapr": true, "ldaprb": true, "ldaprh": true}

func isDirectBranch(mnem string) bool {
	switch mnem {
	case "b", "bl", "cbz", "cbnz", "tbz", "tbnz":
		return true
	}
	return strings.HasPrefix(mnem, "b.")
}

func imm(s string) int64 {
	v, err := strconv.ParseInt(s, 0, 64)
	if err != nil {
		return 0
	}
	return v
}

// A general register's canonical name (x16 for w16), or "" for anything else (sp, xzr, an immediate, a condition).
func gpReg(s string) string {
	s = strings.TrimSpace(s)
	if m := gpRegRe.FindStringSubmatch(s); m != nil {
		return "x" + m[1]
	}
	return ""
}

func calleeSaved(reg string) bool {
	n, err := strconv.Atoi(strings.TrimPrefix(reg, "x"))
	return err == nil && reg[0] == 'x' && n >= 19 && n <= 28
}

// Whether the instruction writes its first operand: a store's and a compare's first operand is read, and a branch has no
// destination.
func writesFirstOperand(mnem string) bool {
	switch {
	case strings.HasPrefix(mnem, "st"), mnem == "cmp", mnem == "cmn", mnem == "tst", mnem == "ccmp", mnem == "ccmn",
		mnem == "bti", mnem == "br", mnem == "ret", isDirectBranch(mnem):
		return false
	}
	return true
}

// The general registers an instruction writes: its first operand (a load pair writes two), and x30 for a call.
func regsWritten(in asmInsn) []string {
	if in.mnem == "bl" || in.mnem == "blr" {
		return []string{"x30"}
	}
	if !writesFirstOperand(in.mnem) {
		return nil
	}
	ops := strings.Split(in.ops, ",")
	n := 1
	switch in.mnem {
	case "ldp", "ldnp", "ldpsw", "ldxp", "ldaxp":
		n = 2
	}
	var out []string
	for i := 0; i < n && i < len(ops); i++ {
		if r := gpReg(ops[i]); r != "" {
			out = append(out, r)
		}
	}
	return out
}

// The position of `reg` among the register operands that come before the memory operand (0 for the first of a pair), or -1.
func operandIndex(ops, reg string) int {
	for i, tok := range strings.Split(ops, ",") {
		tok = strings.TrimSpace(tok)
		if strings.Contains(tok, "[") {
			return -1
		}
		if tok == reg {
			return i
		}
	}
	return -1
}

// What the entry holds a register to be. `acq` is set when the register was last loaded by an acquire load on every path that
// reaches the instruction; `by` is the instruction that last set it on a path where it was not (empty: never set).
type regSrc struct {
	acq bool
	by  string
}

// The state before an instruction, joined over every path that reaches it.
type asmState struct {
	sp, fp           int64 // offsets from the entry's own sp, which is the CFA
	spKnown, fpKnown bool
	x30Orig          bool   // x30 holds the return address the caller gave the entry
	slot             int64  // that address is also in the stack slot CFA+slot ...
	slotSaved        bool   // ... when this is set (the slot is live while it is at or above sp)
	raReg            string // ... or in this callee-saved register (empty: in none)
	regs             map[string]regSrc
}

func (s asmState) clone() asmState {
	n := s
	n.regs = make(map[string]regSrc, len(s.regs))
	for k, v := range s.regs {
		n.regs[k] = v
	}
	return n
}

func (s asmState) key() string {
	regs := make([]string, 0, len(s.regs))
	for k, v := range s.regs {
		regs = append(regs, fmt.Sprintf("%s=%t/%s", k, v.acq, v.by))
	}
	sort.Strings(regs)
	return fmt.Sprintf("%d/%t %d/%t %t %d/%t %s %v", s.sp, s.spKnown, s.fp, s.fpKnown, s.x30Orig, s.slot, s.slotSaved, s.raReg, regs)
}

// The places the caller's return address is kept at this point, as the unwind table names them: the live stack slot ("c-8") and
// the register ("r19").
func (s asmState) savedLocs() []string {
	var out []string
	if s.slotSaved && s.spKnown && s.slot >= s.sp {
		out = append(out, fmt.Sprintf("c%+d", s.slot))
	}
	if s.raReg != "" {
		out = append(out, "r"+strings.TrimPrefix(s.raReg, "x"))
	}
	return out
}

func joinStates(a, b asmState) asmState {
	n := asmState{regs: map[string]regSrc{}}
	if a.spKnown && b.spKnown && a.sp == b.sp {
		n.sp, n.spKnown = a.sp, true
	}
	if a.fpKnown && b.fpKnown && a.fp == b.fp {
		n.fp, n.fpKnown = a.fp, true
	}
	n.x30Orig = a.x30Orig && b.x30Orig
	if a.slotSaved && b.slotSaved && a.slot == b.slot {
		n.slot, n.slotSaved = a.slot, true
	}
	if a.raReg == b.raReg {
		n.raReg = a.raReg
	}
	for r := range a.regs {
		n.regs[r] = joinReg(a.regs[r], b.regs[r])
	}
	for r := range b.regs {
		if _, ok := a.regs[r]; !ok {
			n.regs[r] = joinReg(regSrc{}, b.regs[r])
		}
	}
	return n
}

// A register is an acquire load's result only if it is on every path; the instruction named is one from a path where it is not.
func joinReg(a, b regSrc) regSrc {
	switch {
	case a.acq && b.acq:
		return a
	case a.acq:
		return b
	case b.acq:
		return a
	case a.by != "":
		return a
	}
	return b
}

// The effective address of an sp-based memory operand, as an offset from the entry's sp, and the sp the instruction leaves.
func stackOperand(ops string, s asmState) (addr, newSP int64, ok bool) {
	if !s.spKnown {
		return 0, 0, false
	}
	switch {
	case spPreRe.MatchString(ops):
		a := s.sp + imm(spPreRe.FindStringSubmatch(ops)[1])
		return a, a, true
	case spPostRe.MatchString(ops):
		return s.sp, s.sp + imm(spPostRe.FindStringSubmatch(ops)[1]), true
	case spOffRe.MatchString(ops):
		return s.sp + imm(spOffRe.FindStringSubmatch(ops)[1]), s.sp, true
	}
	return 0, 0, false
}

// The state after one instruction. sp is followed through writeback on sp, add and sub of an immediate, and mov from x29; any
// other write to sp is not followed. x30 is the caller's return address from the first instruction until a call or another
// write overwrites it, and again after a load from the slot it was stored in or a move from the register it was copied to.
func transfer(in asmInsn, s asmState) asmState {
	n := s.clone()
	m := in.mnem
	written := regsWritten(in)
	addr, newSP, onStack := stackOperand(in.ops, s)
	writes := func(reg string) bool {
		for _, w := range written {
			if w == reg {
				return true
			}
		}
		return false
	}

	// x30 stored on the stack: the slot it lands in, if it holds the caller's address.
	if strings.HasPrefix(m, "st") && onStack && s.x30Orig {
		if idx := operandIndex(in.ops, "x30"); idx >= 0 {
			n.slot, n.slotSaved = addr+int64(8*idx), true
		}
	}
	// x30 written.
	if writes("x30") {
		switch {
		case strings.HasPrefix(m, "ld"):
			idx := operandIndex(in.ops, "x30")
			n.x30Orig = onStack && idx >= 0 && s.slotSaved && s.slot >= s.sp && addr+int64(8*idx) == s.slot
		case m == "mov":
			ops := strings.Split(in.ops, ",")
			n.x30Orig = len(ops) == 2 && gpReg(ops[1]) != "" && gpReg(ops[1]) == s.raReg
		default: // bl, blr and any other write
			n.x30Orig = false
		}
	}
	// A register the entry copied x30 to is gone when it is written; `mov xN, x30` makes the copy.
	for _, w := range written {
		if w == s.raReg {
			n.raReg = ""
		}
		src := regSrc{by: m + " " + in.ops}
		if acquireLoads[m] {
			src.acq = true
		}
		n.regs[w] = src
	}
	if m == "mov" && s.x30Orig && strings.HasSuffix(in.ops, ", x30") {
		if r := gpReg(strings.Split(in.ops, ",")[0]); calleeSaved(r) {
			n.raReg = r
		}
	}

	// sp.
	switch {
	case onStack && (spPreRe.MatchString(in.ops) || spPostRe.MatchString(in.ops)):
		n.sp = newSP
	case spPreRe.MatchString(in.ops) || spPostRe.MatchString(in.ops):
		n.spKnown = false
	case writesFirstOperand(m) && strings.HasPrefix(in.ops, "sp,"):
		switch {
		case !s.spKnown:
		case m == "sub" && spArithRe.MatchString(in.ops):
			n.sp = s.sp - imm(spArithRe.FindStringSubmatch(in.ops)[1])
		case m == "add" && spArithRe.MatchString(in.ops):
			n.sp = s.sp + imm(spArithRe.FindStringSubmatch(in.ops)[1])
		case m == "mov" && in.ops == "sp, x29" && s.fpKnown:
			n.sp = s.fp
		default:
			n.spKnown = false
		}
	}
	// x29, when it is set from sp: a CFA rule may be based on it.
	if writes("x29") {
		switch {
		case m == "mov" && in.ops == "x29, sp":
			n.fp, n.fpKnown = s.sp, s.spKnown
		case m == "add" && fpAddRe.MatchString(in.ops):
			n.fp, n.fpKnown = s.sp+imm(fpAddRe.FindStringSubmatch(in.ops)[1]), s.spKnown
		default:
			n.fpKnown = false
		}
	}
	return n
}

func whereRA(s asmState) string {
	if locs := s.savedLocs(); len(locs) > 0 {
		return "saved at " + strings.Join(locs, " and ")
	}
	return "the caller's return address"
}

func unique(list []string) []string {
	var out []string
	for _, x := range list {
		if !contains(out, x) {
			out = append(out, x)
		}
	}
	return out
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}

// What is wrong with an entry's own text. The instructions are run from the first one along every branch (the entry is a
// small flow graph of forward branches; a state is joined where paths meet), and each reached instruction is checked against
// the state before it:
//   - every word the entry reads from outside its own stack is read with an acquire load (acquireLoads), and a `br` jumps
//     through a register that an acquire load set on every path that reaches it;
//   - the only function the entry branches to is its record's continuation, by a direct `bl` or `b`; a `bl` inside the entry
//     is refused (the sensor does not follow a call within an entry), and so is a `blr`;
//   - at every instruction the unwind row in force gives the CFA and the return-address column that the instructions before
//     it imply (the row at a pc describes the state before the instruction there), and at a `bl` the return address is saved
//     where the row says: x30 is overwritten by the call and the continuation's frame may overwrite anything below sp;
//   - x30 is stored at or above sp; and whatever leaves the entry for good (ret, br, a branch out) leaves with sp where the
//     caller had it and x30 the caller's return address (reloaded after a call).
func asmEntryTextErrors(e asmEntryText) []string {
	var errs []string
	fail := func(format string, args ...any) {
		errs = append(errs, e.name+": "+fmt.Sprintf(format, args...))
	}
	seen := map[string]bool{}
	failOnce := func(key, format string, args ...any) {
		if !seen[key] {
			seen[key] = true
			fail(format, args...)
		}
	}
	index := map[uint64]int{}
	for i, in := range e.insns {
		index[in.addr] = i
	}
	inside := func(a uint64) bool { return a >= e.start && a < e.end }

	// Published words are read with an acquire load, wherever the load is.
	for _, in := range e.insns {
		if !strings.HasPrefix(in.mnem, "ld") || acquireLoads[in.mnem] {
			continue
		}
		base := "pc"
		if m := loadBaseRe.FindStringSubmatch(in.ops); m != nil {
			base = m[1]
		}
		if base != "sp" {
			fail("%s %s at %#x is not an acquire load of a published word: an entry reads every such word with ldar (or ldapr)", in.mnem, in.ops, in.addr)
		}
	}

	// The states, to a fixed point.
	states := make([]*asmState, len(e.insns))
	var queue []int
	flow := func(i int, s asmState) {
		if i < 0 || i >= len(e.insns) {
			return
		}
		if states[i] == nil {
			c := s.clone()
			states[i] = &c
			queue = append(queue, i)
			return
		}
		j := joinStates(*states[i], s)
		if j.key() != states[i].key() {
			states[i] = &j
			queue = append(queue, i)
		}
	}
	// The destination of a branch inside the entry; ok is false for one that leaves it or lands between instructions.
	local := func(in asmInsn) (int, bool) {
		if in.target == 0 || !inside(in.target) {
			return 0, false
		}
		i, ok := index[in.target]
		return i, ok
	}
	if len(e.insns) > 0 {
		flow(0, asmState{spKnown: true, x30Orig: true, regs: map[string]regSrc{}})
	}
	for len(queue) > 0 {
		i := queue[0]
		queue = queue[1:]
		in, s := e.insns[i], *states[i]
		next := transfer(in, s)
		switch {
		case in.mnem == "br" || in.mnem == "ret":
		case in.mnem == "b":
			if t, ok := local(in); ok {
				flow(t, next)
			}
		case isDirectBranch(in.mnem) && in.mnem != "bl":
			if t, ok := local(in); ok {
				flow(t, next)
			}
			flow(i+1, next)
		default:
			flow(i+1, next)
		}
	}

	rowAt := func(addr uint64) *cfiRow {
		var row *cfiRow
		for k := range e.rows {
			if e.rows[k].loc <= addr {
				row = &e.rows[k]
			}
		}
		return row
	}
	leaves := func(in asmInsn, s asmState, what string) {
		switch {
		case !s.spKnown:
			fail("the sensor cannot follow sp to the %s at %#x", what, in.addr)
		case s.sp != 0:
			fail("the %s at %#x leaves the entry with sp %d bytes below the caller's: the frame it pushed is still on the stack", what, in.addr, -s.sp)
		}
		if !s.x30Orig {
			fail("the %s at %#x leaves the entry with x30 not the caller's return address: a call overwrote it and nothing reloaded it", what, in.addr)
		}
	}
	branchesToCont := 0
	for i, in := range e.insns {
		if states[i] == nil {
			continue
		}
		s := *states[i]
		next := transfer(in, s)

		// The unwind row in force describes the state before this instruction.
		if row := rowAt(in.addr); row == nil {
			if in.mnem == "bl" {
				fail("no unwind row covers the bl at %#x", in.addr)
			}
		} else if s.spKnown {
			want := fmt.Sprintf("sp+%d", -s.sp)
			if row.cfa != want && !(s.fpKnown && row.cfa == fmt.Sprintf("x29+%d", -s.fp)) {
				failOnce(fmt.Sprintf("cfa/%x/%s/%s", row.loc, row.cfa, want),
					"at %#x sp is %d bytes below the entry's, but the unwind row there gives CFA=%s (want %s): an unwinder walking out of the continuation would read the caller's frame at the wrong address", in.addr, -s.sp, row.cfa, want)
			}
			allowed := s.savedLocs()
			if s.x30Orig && in.mnem != "bl" {
				allowed = append(allowed, "u", "s")
				if s.slotSaved {
					allowed = append(allowed, fmt.Sprintf("c%+d", s.slot)) // the slot after the pop: stale, and x30 holds the value
				}
			}
			allowed = unique(allowed)
			if len(allowed) > 0 && !contains(allowed, row.ra) {
				failOnce(fmt.Sprintf("ra/%x/%s/%s", row.loc, row.ra, strings.Join(allowed, "|")),
					"at %#x x30 is %s, but the unwind row there gives ra=%s (want %s): an unwinder would take the return address from the wrong place",
					in.addr, whereRA(s), row.ra, strings.Join(allowed, " or "))
			}
		}

		switch {
		case in.mnem == "br":
			// What the entry jumps through must have come from an acquire load (its next word), on every path.
			reg := gpReg(in.ops)
			switch src := s.regs[reg]; {
			case reg == "":
				fail("the br at %#x jumps through %q, which is not a general register", in.addr, in.ops)
			case src.acq:
			case src.by == "":
				fail("the br at %#x jumps through %s, which the entry never loads", in.addr, reg)
			default:
				fail("the br at %#x jumps through %s, last set by `%s` on a path that reaches it, which is not an acquire load (the next word is read with ldar)", in.addr, reg, src.by)
			}
			leaves(in, s, "br")
		case in.mnem == "ret":
			leaves(in, s, "ret")
		case in.mnem == "blr":
			fail("the blr at %#x calls through a register: an entry calls its record's continuation directly, so the sensor can see it", in.addr)
		case isDirectBranch(in.mnem):
			_, isLocal := local(in)
			switch {
			case isLocal:
				if in.mnem == "bl" {
					fail("the bl at %#x calls %#x inside the entry: an entry calls its record's continuation directly (the sensor does not follow a call within an entry)", in.addr, in.target)
				}
			case in.target != 0 && inside(in.target):
				fail("the %s at %#x branches to %#x, which is not an instruction of the entry", in.mnem, in.addr, in.target)
			case in.target != e.cont:
				fail("the %s at %#x branches to %#x, not to its record's continuation %s (%#x)", in.mnem, in.addr, in.target, e.contName, e.cont)
			default:
				branchesToCont++
			}
			switch {
			case in.mnem == "bl":
				switch {
				case !s.spKnown:
					fail("the sensor cannot follow sp to the bl at %#x", in.addr)
				case len(s.savedLocs()) == 0:
					fail("the bl at %#x overwrites x30, which the entry has not saved (in a live stack slot at or above sp, or in a callee-saved register)", in.addr)
				}
			case !inside(in.target):
				leaves(in, s, in.mnem) // a branch out of the entry ends the entry's own frame
			}
		}

		// x30 stored below the sp it leaves: the continuation's own frame overwrites the slot.
		if addr, _, ok := stackOperand(in.ops, s); ok && strings.HasPrefix(in.mnem, "st") {
			if idx := operandIndex(in.ops, "x30"); idx >= 0 {
				slot := addr + int64(8*idx)
				switch {
				case slot < next.sp:
					fail("the %s at %#x stores x30 at CFA%+d, below the sp (CFA%+d) it leaves: the frame of any call overwrites that slot", in.mnem, in.addr, slot, next.sp)
				case slot >= 0:
					fail("the %s at %#x stores x30 at CFA%+d, in the caller's frame: the entry's own frame is below its incoming sp", in.mnem, in.addr, slot)
				}
			}
		}
		// Control falls out of the last instruction.
		if i == len(e.insns)-1 && in.mnem != "b" && in.mnem != "br" && in.mnem != "ret" {
			fail("the entry's last instruction, the %s at %#x, falls through to whatever follows it", in.mnem, in.addr)
		}
	}
	if branchesToCont == 0 {
		fail("never branches to its record's continuation %s (%#x) with a direct bl or b", e.contName, e.cont)
	}
	return errs
}

// ---- the controls: synthetic entries, one fault each -------------------------------------------------

func insns(start uint64, lines ...string) []asmInsn {
	var out []asmInsn
	for i, l := range lines {
		mnem, ops, _ := strings.Cut(l, " ")
		in := asmInsn{addr: start + uint64(4*i), mnem: mnem, ops: ops}
		if isDirectBranch(mnem) {
			if m := branchTgtRe.FindStringSubmatch(ops); m != nil {
				in.target, _ = strconv.ParseUint(m[1], 16, 64)
			}
		}
		out = append(out, in)
	}
	return out
}

// An entry at 0x1000 whose continuation is cont at 0x2000, with the unwind rows given.
func entryOf(rows []cfiRow, lines ...string) asmEntryText {
	return asmEntryText{name: "e", start: 0x1000, end: 0x1000 + uint64(4*len(lines)), cont: 0x2000, contName: "cont",
		insns: insns(0x1000, lines...), rows: rows}
}

// The frame every correct entry below is a variation of: stp and bl and ldp, 16 bytes, x30 at CFA-8.
var (
	framedLines = []string{"stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "bl 0x2000 <cont>", "ldp x29, x30, [sp], #0x10", "ret"}
	framedRows  = framedRowsPoppedAt(0x1010)
)

// The rows of that frame when `ret` is the instruction at the address given, which the pop precedes.
func framedRowsPoppedAt(ret uint64) []cfiRow {
	return []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+16", ra: "c-8"}, {loc: ret, cfa: "sp+0", ra: "u"}}
}

func errorsOf(e asmEntryText) string { return strings.Join(asmEntryTextErrors(e), "\n") }

func mustPass(t *testing.T, what string, e asmEntryText) {
	t.Helper()
	if errs := asmEntryTextErrors(e); len(errs) != 0 {
		t.Errorf("%s must pass, got:\n%s", what, strings.Join(errs, "\n"))
	}
}

func mustFail(t *testing.T, what string, e asmEntryText, want ...string) {
	t.Helper()
	got := errorsOf(e)
	if got == "" {
		t.Errorf("%s must fail", what)
	}
	for _, w := range want {
		if !strings.Contains(got, w) {
			t.Errorf("%s: want an error containing %q, got:\n%s", what, w, got)
		}
	}
}

func TestAsmEntryTextAcceptsAFramedEntry(t *testing.T) {
	mustPass(t, "stp, bl, ldp, ret with the rows of the frame", entryOf(framedRows, framedLines...))
	// A second call while the frame is still pushed is checked against the same frame.
	mustPass(t, "two calls under one frame", entryOf(framedRowsPoppedAt(0x1014), "stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "bl 0x2000 <cont>",
		"bl 0x2000 <cont>", "ldp x29, x30, [sp], #0x10", "ret"))
	// The frame popped, then a tail branch to the continuation: sp and x30 are the caller's again.
	mustPass(t, "a tail branch after the pop", entryOf(framedRows, "stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "bl 0x2000 <cont>",
		"ldp x29, x30, [sp], #0x10", "b 0x2000 <cont>"))
	// A row that keeps the dead slot after the pop is harmless: x30 holds the value.
	stale := []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+16", ra: "c-8"}, {loc: 0x1010, cfa: "sp+0", ra: "c-8"}}
	mustPass(t, "a pop whose row keeps ra=c-8", entryOf(stale, framedLines...))
}

// The headset trampoline's shape: no load, no frame, a tail branch to the continuation with sp and x30 untouched. It passes.
func TestAsmEntryTextAcceptsATailBranchEntry(t *testing.T) {
	mustPass(t, "a tail-branch entry", asmEntryText{name: "tramp", start: 0x1000, end: 0x1010, cont: 0x2000, contName: "dispatch",
		insns: insns(0x1000, "bti c", "mov x0, sp", "mov x1, x30", "b 0x2000 <dispatch>"),
		rows:  []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}}})
}

// sp is followed through a pre-index store, a frame pointer set from sp, a sub, a restore from the frame pointer and a
// post-index load, and a CFA rule based on x29 is accepted once x29 is set and while it still holds: three calls, each checked
// against its own frame.
func TestAsmEntryTextFollowsSpAndTheFramePointer(t *testing.T) {
	lines := []string{
		"stp x29, x30, [sp, #-0x20]!", // 0x1000: sp -32, x30 at CFA-24
		"mov x29, sp",                 // 0x1004: x29 = CFA-32
		"sub sp, sp, #0x10",           // 0x1008: sp -48
		"bl 0x2000 <cont>",            // 0x100c: CFA = x29+32 = sp+48
		"mov sp, x29",                 // 0x1010: sp -32
		"bl 0x2000 <cont>",            // 0x1014: CFA = x29+32 = sp+32
		"ldp x29, x30, [sp], #0x20",   // 0x1018: sp 0, x29 is the caller's again
		"stp x29, x30, [sp, #-0x10]!", // 0x101c: sp -16, x30 at CFA-8
		"bl 0x2000 <cont>",            // 0x1020: CFA = sp+16
		"ldp x29, x30, [sp], #0x10",   // 0x1024
		"ret"}                         // 0x1028
	rows := func(fpCFA string) []cfiRow {
		return []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+32", ra: "c-24"}, {loc: 0x1008, cfa: fpCFA, ra: "c-24"},
			{loc: 0x101c, cfa: "sp+0", ra: "u"}, {loc: 0x1020, cfa: "sp+16", ra: "c-8"}, {loc: 0x1028, cfa: "sp+0", ra: "u"}}
	}
	mustPass(t, "three frames under their rows", entryOf(rows("x29+32"), lines...))
	got := errorsOf(entryOf(rows("x29+16"), lines...))
	if strings.Count(got, "\n") != 1 || !strings.Contains(got, "want sp+32") || !strings.Contains(got, "want sp+48") {
		t.Errorf("a frame-pointer CFA at the wrong offset must fail once for each sp it is read at, got:\n%s", got)
	}
	// A frame-pointer rule before x29 is set describes nothing, whatever offset it gives.
	mustFail(t, "CFA=x29+0 before x29 is set", entryOf([]cfiRow{{loc: 0x1000, cfa: "x29+0", ra: "u"}}, "nop", "b 0x2000 <cont>"), "gives CFA=x29+0 (want sp+0)")
	// A frame-pointer rule before x29 is set describes nothing.
	early := rows("x29+32")
	early[1].cfa = "x29+32"
	mustFail(t, "a CFA based on x29 before x29 is set", entryOf(early, lines...), "gives CFA=x29+32 (want sp+32)")
	// x29 set by an add from sp.
	mustPass(t, "add x29, sp, #16 under a 32-byte frame: CFA=x29+16", entryOf(
		[]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+32", ra: "c-24"}, {loc: 0x1008, cfa: "x29+16", ra: "c-24"}, {loc: 0x1010, cfa: "sp+0", ra: "u"}},
		"stp x29, x30, [sp, #-0x20]!", "add x29, sp, #0x10", "bl 0x2000 <cont>", "ldp x29, x30, [sp], #0x20", "ret"))
}

func TestAsmEntryTextRejectsWhatItCannotFollow(t *testing.T) {
	base := func(lines ...string) asmEntryText {
		return entryOf([]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+16", ra: "c-8"}}, lines...)
	}
	for _, c := range []struct {
		what  string
		lines []string
		want  string
	}{
		{"a call through a register", []string{"stp x29, x30, [sp, #-0x10]!", "blr x8", "b 0x2000 <cont>"}, "calls through a register"},
		{"an sp write it cannot follow", []string{"stp x29, x30, [sp, #-0x10]!", "sub sp, sp, x9", "bl 0x2000 <cont>"}, "cannot follow sp"},
		{"a bl with x30 not saved", []string{"sub sp, sp, #0x10", "bl 0x2000 <cont>"}, "has not saved"},
		{"a br through a register it never loads", []string{"br x17"}, "never loads"},
		{"a bl to another function", []string{"stp x29, x30, [sp, #-0x10]!", "bl 0x3000 <other>"}, "not to its record's continuation"},
		{"a b to another function", []string{"b 0x3000 <other>"}, "the b at 0x1000 branches to 0x3000"},
		{"a b.ne out of the entry", []string{"b.ne 0x3000 <other>"}, "the b.ne at 0x1000 branches to 0x3000"},
		{"a cbz out of the entry", []string{"cbz x0, 0x3000 <other>"}, "the cbz at 0x1000 branches to 0x3000"},
		{"a cbnz out of the entry", []string{"cbnz x0, 0x3000 <other>"}, "the cbnz at 0x1000 branches to 0x3000"},
		{"a tbz out of the entry", []string{"tbz w0, #0x3, 0x3000 <other>"}, "the tbz at 0x1000 branches to 0x3000"},
		{"a tbnz out of the entry", []string{"tbnz w0, #0x3, 0x3000 <other>"}, "the tbnz at 0x1000 branches to 0x3000"},
		{"a plain load through the pc", []string{"ldr x16, 0x4000 <word>", "b 0x2000 <cont>"}, "ldr x16, 0x4000 <word> at 0x1000 is not an acquire load"},
		{"an entry that never reaches the continuation", []string{"ret"}, "never branches to its record's continuation cont (0x2000)"},
		{"a branch into the middle of an instruction", []string{"b 0x1002 <e+0x2>", "b 0x2000 <cont>"}, "not an instruction of the entry"},
		{"control falling out of the last instruction", []string{"bl 0x2000 <cont>"}, "falls through"},
	} {
		mustFail(t, c.what, base(c.lines...), c.want)
	}
	// The same frame written with an offset store (no writeback), a sub and an add is followed exactly; a load from the stack is
	// not a published word.
	mustPass(t, "sub sp 32, add sp 16, stp x29, x30, [sp]", entryOf(
		[]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+32", ra: "u"}, {loc: 0x1008, cfa: "sp+16", ra: "u"},
			{loc: 0x100c, cfa: "sp+16", ra: "c-8"}, {loc: 0x101c, cfa: "sp+0", ra: "u"}},
		"sub sp, sp, #0x20", "add sp, sp, #0x10", "stp x29, x30, [sp]", "bl 0x2000 <cont>", "ldp x29, x30, [sp]",
		"ldr x16, [sp, #0x8]", "add sp, sp, #0x10", "ret"))
	// A store or a compare of the jump register reads it: the register is still the one the ldar set.
	reads := entryOf([]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}},
		"adrp x16, 0x4000 <word>", "ldar x16, [x16]", "cmp x16, x0", "cmn x16, #0x1", "tst x16, #0x1",
		"ccmp x16, x0, #0x0, eq", "ccmn x16, #0x1, #0x0, ne", "str x16, [x1]", "b.eq 0x1028 <e+0x28>", "br x16", "b 0x2000 <cont>")
	mustPass(t, "a compare or a store of x16", reads)
}

// A tail b out of the entry (and a conditional branch out of it) leaves with sp where the caller had it and x30 the caller's
// return address, as a ret does.
func TestAsmEntryTextChecksATailBranch(t *testing.T) {
	rows := []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+16", ra: "c-8"}}
	mustFail(t, "a tail b with the frame pushed", entryOf(rows, "stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "b 0x2000 <cont>"),
		"the b at 0x1008 leaves the entry with sp 16 bytes below the caller's")
	mustFail(t, "a conditional tail branch with the frame pushed", entryOf(framedRows, "stp x29, x30, [sp, #-0x10]!", "mov x29, sp",
		"cbz x0, 0x2000 <cont>", "ldp x29, x30, [sp], #0x10", "ret"), "the cbz at 0x1008 leaves the entry with sp 16 bytes below")
	mustFail(t, "a tail b after a call that nothing reloaded x30 from", entryOf(rows, "stp x29, x30, [sp, #-0x10]!", "mov x29, sp",
		"bl 0x2000 <cont>", "add sp, sp, #0x10", "b 0x2000 <cont>"), "leaves the entry with x30 not the caller's return address")
	mustFail(t, "a ret after a call with x30 not reloaded", entryOf(rows, "stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "bl 0x2000 <cont>",
		"add sp, sp, #0x10", "ret"), "the ret at 0x1010 leaves the entry with x30 not the caller's return address")
	mustFail(t, "a ret with the frame pushed", entryOf(rows, "stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "ret"),
		"the ret at 0x1008 leaves the entry with sp 16 bytes below the caller's")
	mustFail(t, "a br with the frame pushed", entryOf(rows, "stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "ldar x16, [x16]", "br x16"),
		"the br at 0x100c leaves the entry with sp 16 bytes below")
}

// x30 is the caller's again after the pop that reloads it from its slot, and the slot is dead after it: a second bl then has
// nowhere that holds the caller's address.
func TestAsmEntryTextReloadsX30AfterThePop(t *testing.T) {
	rows := []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+16", ra: "c-8"}, {loc: 0x1010, cfa: "sp+0", ra: "u"}}
	mustFail(t, "a second bl after the pop", entryOf(rows, "stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "bl 0x2000 <cont>",
		"ldp x29, x30, [sp], #0x10", "bl 0x2000 <cont>", "ret"),
		"the bl at 0x1010 overwrites x30, which the entry has not saved", "the ret at 0x1014 leaves the entry with x30 not")
	mustFail(t, "a reload from the slot after the pop", entryOf(rows, "stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "bl 0x2000 <cont>",
		"ldp x29, x30, [sp], #0x10", "bl 0x2000 <cont>", "ldur x30, [sp, #-8]", "ret"), "the ret at 0x1018 leaves the entry with x30 not")
	mustFail(t, "a store of the clobbered x30 is not the save", entryOf(rows, "sub sp, sp, #0x20", "bl 0x2000 <cont>", "str x30, [sp, #0x8]",
		"bl 0x2000 <cont>", "add sp, sp, #0x20", "ret"), "the bl at 0x100c overwrites x30, which the entry has not saved")
	mustFail(t, "a copy of the clobbered x30 is not the save", entryOf(rows, "sub sp, sp, #0x10", "bl 0x2000 <cont>", "mov x19, x30",
		"bl 0x2000 <cont>", "add sp, sp, #0x10", "ret"), "the bl at 0x100c overwrites x30, which the entry has not saved")
	mustFail(t, "a pop that reloads x30 from another slot", entryOf(rows, "stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "bl 0x2000 <cont>",
		"ldp x30, x29, [sp], #0x10", "ret"), "the ret at 0x1010 leaves the entry with x30 not")
}

func TestAsmEntryTextRefusesX30StoredOutsideItsFrame(t *testing.T) {
	rows := []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+0", ra: "c-8"}}
	mustFail(t, "x30 stored below sp, sp not moved", entryOf(rows, "stur x30, [sp, #-8]", "bl 0x2000 <cont>", "ldur x30, [sp, #-8]", "ret"),
		"the stur at 0x1000 stores x30 at CFA-8, below the sp (CFA+0) it leaves")
	mustFail(t, "x30 stored below a moved sp", entryOf(rows, "sub sp, sp, #0x10", "str x30, [sp, #-8]", "bl 0x2000 <cont>",
		"ldr x30, [sp, #-8]", "add sp, sp, #0x10", "ret"), "stores x30 at CFA-24, below the sp (CFA-16) it leaves")
	mustFail(t, "x30 stored in the caller's frame", entryOf(rows, "str x30, [sp, #0x8]", "bl 0x2000 <cont>", "ldr x30, [sp, #0x8]", "ret"),
		"stores x30 at CFA+8, in the caller's frame")
	mustPass(t, "x30 stored at sp itself", entryOf(
		[]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+8", ra: "u"}, {loc: 0x1008, cfa: "sp+8", ra: "c-8"}, {loc: 0x1014, cfa: "sp+0", ra: "u"}},
		"sub sp, sp, #0x8", "str x30, [sp]", "bl 0x2000 <cont>", "ldr x30, [sp]", "add sp, sp, #0x8", "ret"))
	mustPass(t, "x30 stored at the bottom of the frame", entryOf(
		[]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+16", ra: "u"}, {loc: 0x1008, cfa: "sp+16", ra: "c-8"}, {loc: 0x1014, cfa: "sp+0", ra: "u"}},
		"sub sp, sp, #0x10", "stur x30, [sp, #8]", "bl 0x2000 <cont>", "ldur x30, [sp, #8]", "add sp, sp, #0x10", "ret"))
}

// A bl to a label inside the entry overwrites x30 as any bl does; the sensor does not follow a call within an entry.
func TestAsmEntryTextChecksALocalBl(t *testing.T) {
	rows := []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}}
	mustFail(t, "a local bl", entryOf(rows, "stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "bl 0x1014 <e+0x14>", "ldp x29, x30, [sp], #0x10",
		"ret", "b 0x2000 <cont>"), "the bl at 0x1008 calls 0x1014 inside the entry", "gives CFA=sp+0 (want sp+16)")
	mustFail(t, "a local bl with x30 not saved", entryOf(rows, "bl 0x1008 <e+0x8>", "ret", "b 0x2000 <cont>"), "overwrites x30, which the entry has not saved")
}

// The register a br jumps through must be an acquire load's result on every path to the br.
func TestAsmEntryTextFindsTheBrRegisterByPath(t *testing.T) {
	rows := []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}}
	// ldar; cbz over a mov; join at the br: the mov path reaches it.
	mustFail(t, "a mov on one path", entryOf(rows, "ldar x16, [x16]", "cbz x0, 0x100c <e+0xc>", "mov x16, x1", "br x16"),
		"the br at 0x100c jumps through x16, last set by `mov x16, x1`", "not an acquire load")
	// The same shape where the textually last writer is the ldar, which is what an address-order scan sees.
	mustFail(t, "a mov on the path that jumps over the ldar", entryOf(rows, "cbz x0, 0x100c <e+0xc>", "mov x16, x1", "b 0x1010 <e+0x10>",
		"ldar x16, [x16]", "br x16"), "last set by `mov x16, x1`")
	// An ldar on both paths is an acquire load on every path.
	got := errorsOf(entryOf(rows, "ldar x16, [x16]", "cbz x0, 0x100c <e+0xc>", "ldar x16, [x16]", "br x16"))
	if strings.Contains(got, "jumps through") {
		t.Errorf("an ldar on both paths to the br must pass the br check, got:\n%s", got)
	}
	// A path that never sets the register.
	mustFail(t, "a path on which the register is never loaded", entryOf(rows, "cbz x0, 0x1008 <e+0x8>", "ldar x16, [x16]", "br x16"), "jumps through x16")
	// A later write on the one path replaces the load.
	mustFail(t, "the loaded register overwritten before the br", entryOf(rows, "ldar x16, [x16]", "add x16, x16, #0x8", "br x16"),
		"last set by `add x16, x16, #0x8`")
}

// An acquire load is told by its exact opcode: ldar and ldapr and their byte and halfword forms. Anything else that reads a word
// from outside the stack, ldadd and the exclusive and plain loads included, is refused.
func TestAsmEntryTextMatchesAcquireLoadsByExactOpcode(t *testing.T) {
	rows := []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}}
	for _, m := range []string{"ldar", "ldarb", "ldarh", "ldapr", "ldaprb", "ldaprh"} {
		reg := "x16"
		if strings.HasSuffix(m, "b") || strings.HasSuffix(m, "h") {
			reg = "w16" // the sized forms load a w register; the br jumps through its x name
		}
		if got := errorsOf(entryOf(rows, m+" "+reg+", [x16]", "br x16")); strings.Contains(got, "acquire") || strings.Contains(got, "jumps through") {
			t.Errorf("%s is an acquire load, got:\n%s", m, got)
		}
	}
	for _, c := range []struct{ mnem, ops string }{
		{"ldadd", "xzr, x17, [x16]"}, {"ldaddal", "xzr, x17, [x16]"}, {"ldadda", "xzr, x17, [x16]"}, {"ldxr", "x16, [x16]"},
		{"ldaxr", "x16, [x16]"}, {"ldr", "x16, [x16]"}, {"ldur", "x16, [x16, #-8]"}, {"ldp", "x16, x17, [x16]"}, {"ldrb", "w16, [x16]"},
		{"ldursw", "x16, [x16]"}, {"ldapur", "x16, [x16]"},
	} {
		mustFail(t, c.mnem+" "+c.ops, entryOf(rows, c.mnem+" "+c.ops, "b 0x2000 <cont>"), c.mnem+" "+c.ops+" at 0x1000 is not an acquire load of a published word")
	}
	// Loads from the stack are the entry's own.
	for _, ops := range []string{"x16, [sp]", "x16, [sp, #0x8]", "x16, [sp], #0x10", "x16, [sp, #-0x10]!"} {
		if got := errorsOf(entryOf(rows, "ldr "+ops, "b 0x2000 <cont>")); strings.Contains(got, "acquire") {
			t.Errorf("ldr %s reads the entry's own stack, got:\n%s", ops, got)
		}
	}
}

// The unwind row is read at each instruction, against the state the instructions before it leave: a row right at the bl and
// wrong before it, or after the pop, fails.
func TestAsmEntryTextReadsTheRowAtEveryInstruction(t *testing.T) {
	mustFail(t, "CFI that begins at the bl", entryOf([]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1008, cfa: "sp+16", ra: "c-8"}, {loc: 0x1010, cfa: "sp+0", ra: "u"}}, framedLines...),
		"at 0x1004 sp is 16 bytes below the entry's, but the unwind row there gives CFA=sp+0 (want sp+16)")
	mustFail(t, "CFI that is never undone after the pop", entryOf([]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+16", ra: "c-8"}}, framedLines...),
		"at 0x1010 sp is 0 bytes below the entry's, but the unwind row there gives CFA=sp+16 (want sp+0)")
	mustFail(t, "no ra rule for the saved x30", entryOf([]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+16", ra: "u"}, {loc: 0x1010, cfa: "sp+0", ra: "u"}}, framedLines...),
		"gives ra=u (want c-8)")
	mustFail(t, "a wrong ra slot", entryOf([]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+16", ra: "c-16"}, {loc: 0x1010, cfa: "sp+0", ra: "u"}}, framedLines...),
		"gives ra=c-16")
	mustFail(t, "a ra rule that begins after the bl", entryOf([]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+16", ra: "u"},
		{loc: 0x100c, cfa: "sp+16", ra: "c-8"}, {loc: 0x1010, cfa: "sp+0", ra: "u"}}, framedLines...), "at 0x1008 x30 is saved at c-8, but the unwind row there gives ra=u (want c-8)")
	mustFail(t, "no unwind row at all", entryOf(nil, framedLines...), "no unwind row covers the bl at 0x1008")
}

// x30 kept in a callee-saved register is described with .cfi_register: the row's ra is "r19".
func TestAsmEntryTextAcceptsX30InACalleeSavedRegister(t *testing.T) {
	lines := []string{"stp x19, x20, [sp, #-0x10]!", "mov x19, x30", "bl 0x2000 <cont>", "mov x30, x19", "ldp x19, x20, [sp], #0x10", "ret"}
	rows := func(ra string) []cfiRow {
		return []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1004, cfa: "sp+16", ra: "u"}, {loc: 0x1008, cfa: "sp+16", ra: ra},
			{loc: 0x1010, cfa: "sp+16", ra: "u"}, {loc: 0x1014, cfa: "sp+0", ra: "u"}}
	}
	mustPass(t, "x30 in x19 with ra=r19", entryOf(rows("r19"), lines...))
	mustFail(t, "ra=r20", entryOf(rows("r20"), lines...), "gives ra=r20 (want r19)")
	mustFail(t, "no .cfi_register", entryOf(rows("u"), lines...), "gives ra=u (want r19)")
	mustFail(t, "x30 copied to a caller-saved register", entryOf(rows("r9"), "stp x19, x20, [sp, #-0x10]!", "mov x9, x30", "bl 0x2000 <cont>",
		"mov x30, x9", "ldp x19, x20, [sp], #0x10", "ret"), "overwrites x30, which the entry has not saved")
	mustFail(t, "the register overwritten before the call", entryOf(rows("r19"), "stp x19, x20, [sp, #-0x10]!", "mov x19, x30", "mov x19, #0x0",
		"bl 0x2000 <cont>", "mov x30, x19", "ldp x19, x20, [sp], #0x10", "ret"), "overwrites x30, which the entry has not saved")
	mustFail(t, "x30 reloaded from the wrong register", entryOf(rows("r19"), "stp x19, x20, [sp, #-0x10]!", "mov x19, x30", "bl 0x2000 <cont>",
		"mov x30, x20", "ldp x19, x20, [sp], #0x10", "ret"), "the ret at 0x1014 leaves the entry with x30 not")
}

// Where two paths meet the state is the part they agree on: a reload on one path, a different frame on each, or a different
// register holding x30 leaves nothing the sensor can rely on after the join.
func TestAsmEntryTextJoinsThePathsThatMeet(t *testing.T) {
	rows := []cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}}
	mustFail(t, "x30 reloaded on one path only", entryOf(framedRowsPoppedAt(0x1014), "stp x29, x30, [sp, #-0x10]!", "mov x29, sp", "bl 0x2000 <cont>",
		"cbz x0, 0x1014 <e+0x14>", "ldp x29, x30, [sp], #0x10", "ret"), "the ret at 0x1014 leaves the entry with x30 not")
	mustFail(t, "sp moved on one path only", entryOf(rows, "cbz x0, 0x100c <e+0xc>", "sub sp, sp, #0x10", "b 0x100c <e+0xc>", "ret"),
		"the sensor cannot follow sp to the ret at 0x100c")
	mustFail(t, "x30 saved in a different slot on each path", entryOf(rows, "cbz x0, 0x1010 <e+0x10>", "sub sp, sp, #0x10", "stur x30, [sp, #8]",
		"b 0x1018 <e+0x18>", "sub sp, sp, #0x10", "stur x30, [sp]", "bl 0x2000 <cont>"), "the bl at 0x1018 overwrites x30, which the entry has not saved")
	mustFail(t, "x30 copied to a different register on each path", entryOf(rows, "cbz x0, 0x1010 <e+0x10>", "mov x19, x30", "b 0x1014 <e+0x14>", "nop",
		"mov x20, x30", "bl 0x2000 <cont>"), "the bl at 0x1014 overwrites x30, which the entry has not saved")
}

// The return-address rule is read where x30 has not been saved too: before the store there is nothing for ra to name.
func TestAsmEntryTextRefusesARaRuleForAnUnsavedX30(t *testing.T) {
	mustFail(t, "ra=c-8 before the store", entryOf([]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "c-8"}}, "nop", "b 0x2000 <cont>"), "gives ra=c-8 (want u or s)")
}

// Only x19..x28 survive the continuation's call.
func TestAsmEntryTextKeepsX30OnlyInACalleeSavedRegister(t *testing.T) {
	for _, c := range []struct {
		reg  string
		keep bool
	}{{"x19", true}, {"x28", true}, {"x18", false}, {"x29", false}, {"x9", false}} {
		reg := c.reg[1:]
		e := entryOf([]cfiRow{{loc: 0x1000, cfa: "sp+0", ra: "u"}, {loc: 0x1008, cfa: "sp+0", ra: "r" + reg}},
			"mov "+c.reg+", x30", "mov x1, x1", "bl 0x2000 <cont>", "mov x30, "+c.reg, "ret")
		got := errorsOf(e)
		if saved := !strings.Contains(got, "has not saved"); saved != c.keep {
			t.Errorf("x30 in %s: kept=%t, want %t; errors:\n%s", c.reg, saved, c.keep, got)
		}
	}
}

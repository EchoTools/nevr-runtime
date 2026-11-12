package quest

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"
)

// The hook-frame sensor (callback_thunk.h, rule 1), shared by every artifact it reads: the sentinel
// library, the login probe and the two control probes.
//
// Rule: a frame that is live while game code runs under a hook must sit under the personality-free
// "zR" CIE. The sensor starts from every hook record's entry and handler (the nevr_hook_records
// section), follows direct bl/b edges and fails on any reachable function under a personality-bearing
// CIE. Indirect calls are not followed (callback_thunk.h, item 5).
//
// THE ONE EXEMPTION. A function that runs entirely outside any game call may carry a personality even
// though a handler calls it directly. It is marked at its definition with NEVR_OUTSIDE_GAME_CALL
// (src/quest/sentinel/outside_game_call.h), which places it in the output section
// `nevr_outside_game_call`. The sensor reads that section from the built artifact: a function whose
// address lies inside it is not checked and its callees are not followed from it, and every such
// function that the walk reaches is listed (in the log and in the verdict) so the set is reviewed, not
// silent. There is no name list in this file. A function that is live across a game call and carries a
// personality fails however it is marked in a comment or a name: only the section exempts, and the
// controls below (frames_probe_bad, frames_probe_ok, the synthetic graphs) pin that.
//
// A reviewed assembly entry (callback_thunk.h rule 7) is the one hook that is not a CallbackThunk. Its record,
// NEVR_ASM_ENTRY_RECORD, is {entry, continuation} in the same section: the sensor roots the walk at both, so the
// continuation and everything it calls directly are checked like a handler's, and the entry itself must have an FDE
// under "zR" (an entry written in a top-level asm() block with no CFI has none). An FDE under "zR" is not yet one that
// describes the frame: NDK clang gives an __attribute__((naked)) function an FDE whose one row is CFA=sp, and that row
// stays in force after the body's stp and across its bl. So the entry's own text is read too (asmEntryTextErrors, asm_entry_text_test.go):
// along every path the unwind row in force at each instruction must give the CFA that the instructions before it imply,
// and at each bl the place x30 is kept; every word it loads from outside its stack is an acquire load, so is what its
// br jumps through on every path; whatever leaves the entry leaves with the caller's sp and x30; and the only function
// it branches to is its record's continuation. A record whose entry is neither a thunk Entry nor in reviewedAsmEntries fails, so
// adding an entry is a reviewed change to the list below, and the list names an entry only while the library carries
// its record.
//
// Not exempted by the annotation, and so not entered either way: the cold, noreturn tail of libc++ and
// libc++abi (a length or range error, terminate, the exception allocator). Direct container operations
// reach them but they do not run during a normal call, and when one does run it raises a sentinel
// exception that never returns, so it is never a frame a game exception passes through.
var libcxxThrowTail = regexp.MustCompile(`^(__cxa_|_ZSt9terminatev|_ZSt11__terminatePFvvE|_ZNSt6__ndk120__throw_|_ZNSt11logic_error|_ZN10__cxxabiv1)`)

// A linker veneer for the Cortex-A53 erratum 843419 (aarch64 ld.lld replaces an adrp that sits at the end of a 4 KiB
// page by a branch to one of these): the one relocated instruction and a `b` back into the middle of the function it
// was taken from, e.g. `ldr x8, [x8, #816]; b <caller>+0x14`. It is a piece of its caller, not a frame: it does not
// touch the stack and has no unwind entry of its own, and its caller's FDE (checked, since the caller is reached) is
// what an exception passing through sees. Where the linker places one depends on code layout, so any change can
// make the walk reach one. Matched by the exact name form the linker gives it.
var linkerErratumVeneer = regexp.MustCompile(`^__CortexA53843419_[0-9A-F]+$`)

const outsideSection = "nevr_outside_game_call"

// The assembly entries the sentinel may carry, by symbol name: each is reviewed for callback_thunk.h rule 7 (the br with the
// caller's registers untouched, and a personality-free continuation; the sensor checks the ldar loads, the CFI rows at each bl
// and the branch to the continuation). The combat modules that need one add its name here, in the change that moves them.
var reviewedAsmEntries = []*regexp.Regexp{
	regexp.MustCompile(`^nevr_combat_headset_tramp$`),    // src/quest/combat/headset_gates_entry.inc: CVR::HeadsetType (bot-body and menu gates)
	regexp.MustCompile(`^nevr_combat_party_tramp$`),      // src/quest/combat/headset_gates_entry.inc: PartyMemberHeadsetType (party-permissions gate)
	regexp.MustCompile(`^nevr_combat_pb_resolve_entry$`), // src/quest/combat/powerboost_thunk.inc: CPhTarget::ResolveBody (the POWER BOOST cone)
	regexp.MustCompile(`^nevr_combat_dm_tp_entry$`),      // src/quest/combat/dronemuzzle_entry.inc: CncaGameSpace::WorldTeleport (drone muzzle, F4)
	regexp.MustCompile(`^nevr_combat_dm_fire_entry$`),    // src/quest/combat/dronemuzzle_entry.inc: CR15NetBulletCS::FireBullet (drone muzzle, F6)
}

// What is wrong with the library's hook records, as messages. `names` maps a function start to its symbol, `thunkEntries`
// holds the CallbackThunk Entry functions, `recordEntries` counts the records that name each entry.
//   - a thunk Entry has exactly one record;
//   - any other record entry is a reviewed assembly entry (matched by name), and has exactly one record;
//   - every reviewed pattern matches exactly one record entry, so a name cannot outlive its entry.
func recordEntryErrors(names map[uint64]string, thunkEntries map[uint64]bool, recordEntries map[uint64]int, reviewed []*regexp.Regexp) []string {
	var errs []string
	for a := range thunkEntries {
		if recordEntries[a] != 1 {
			errs = append(errs, fmt.Sprintf("thunk entry %s has %d hook records, want exactly 1 (define the hook with NEVR_HOOK_RECORD)", names[a], recordEntries[a]))
		}
	}
	matched := make([]int, len(reviewed))
	for a, n := range recordEntries {
		if thunkEntries[a] {
			continue
		}
		hit := -1
		for i, re := range reviewed {
			if re.MatchString(names[a]) {
				hit = i
			}
		}
		if hit < 0 {
			errs = append(errs, fmt.Sprintf("hook record names %#x (%s, x%d), which is neither a CallbackThunk Entry nor a reviewed assembly entry (add the entry to reviewedAsmEntries in the change that adds it)", a, names[a], n))
			continue
		}
		matched[hit]++
		if n != 1 {
			errs = append(errs, fmt.Sprintf("assembly entry %s has %d hook records, want exactly 1 (declare it once with NEVR_ASM_ENTRY_RECORD)", names[a], n))
		}
	}
	for i, re := range reviewed {
		if matched[i] != 1 {
			errs = append(errs, fmt.Sprintf("reviewed assembly entry %s matches %d record entries, want exactly 1: the entry or its record is gone", re, matched[i]))
		}
	}
	sort.Strings(errs)
	return errs
}

// frameGraph is everything the verdict needs, so the verdict is a pure function that the controls can
// feed synthetic graphs.
type frameGraph struct {
	names     map[uint64]string   // function start -> symbol
	aug       map[uint64]string   // function start -> CIE augmentation of its FDE (absent: no FDE)
	edges     map[uint64][]uint64 // direct call/branch edges between functions
	roots     []uint64            // hook record entries and handlers
	annotated map[uint64]bool     // functions inside the nevr_outside_game_call section
}

type frameVerdict struct {
	violations       []string
	annotatedReached []string // names, sorted
	checked          int
}

func walkHookFrames(g frameGraph) frameVerdict {
	var v frameVerdict
	reached := map[uint64]bool{}
	queue := append([]uint64(nil), g.roots...)
	for _, r := range g.roots {
		reached[r] = true
	}
	annotatedSeen := map[string]bool{}
	for len(queue) > 0 {
		a := queue[0]
		queue = queue[1:]
		name, known := g.names[a]
		if !known {
			continue
		}
		if g.annotated[a] {
			annotatedSeen[name] = true
			continue // runs outside any game call: not checked, not entered
		}
		if libcxxThrowTail.MatchString(name) || linkerErratumVeneer.MatchString(name) {
			continue
		}
		aug, hasFDE := g.aug[a]
		if !hasFDE {
			v.violations = append(v.violations, fmt.Sprintf("%s at %#x has no FDE", name, a))
			continue
		}
		v.checked++
		if aug != `"zR"` {
			v.violations = append(v.violations, fmt.Sprintf(
				"function %s (reachable from a hook entry or handler) sits under CIE augmentation %s, want \"zR\" (no personality, no LSDA)", name, aug))
		}
		for _, callee := range g.edges[a] {
			if !reached[callee] {
				reached[callee] = true
				queue = append(queue, callee)
			}
		}
	}
	for n := range annotatedSeen {
		v.annotatedReached = append(v.annotatedReached, n)
	}
	sort.Strings(v.annotatedReached)
	sort.Strings(v.violations)
	return v
}

// ---- reading a built artifact ---------------------------------------------------------------------

type builtArtifact struct {
	graph         frameGraph
	funcs         []elfFunc
	recordEntries map[uint64]int
	records       [][2]uint64 // {entry, handler or continuation}, in section order
}

func loadFrameGraph(t *testing.T, path string) builtArtifact {
	t.Helper()
	cieAug, fdeCIE := framesOf(t, path)
	funcs := funcsOf(t, path)
	byAddr := map[uint64]*elfFunc{}
	for i := range funcs {
		byAddr[funcs[i].addr] = &funcs[i]
	}
	containing := func(a uint64) *elfFunc {
		if f, ok := byAddr[a]; ok {
			return f
		}
		for i := range funcs {
			if a >= funcs[i].addr && a < funcs[i].addr+funcs[i].size {
				return &funcs[i]
			}
		}
		return nil
	}

	g := frameGraph{names: map[uint64]string{}, aug: map[uint64]string{}, edges: map[uint64][]uint64{}, annotated: map[uint64]bool{}}
	for i := range funcs {
		g.names[funcs[i].addr] = funcs[i].name
		if cie, ok := fdeCIE[funcs[i].addr]; ok {
			g.aug[funcs[i].addr] = cieAug[cie]
		}
	}

	// Direct call/branch edges, from one disassembly.
	seen := map[[2]uint64]bool{}
	var from uint64
	for _, line := range strings.Split(run(t, "llvm-objdump", "-d", "--no-show-raw-insn", path), "\n") {
		if m := hdrRe.FindStringSubmatch(line); m != nil {
			from, _ = strconv.ParseUint(m[1], 16, 64)
			continue
		}
		m := branchR.FindStringSubmatch(line)
		if m == nil || strings.HasSuffix(m[3], "@plt") {
			continue // imports are not sentinel frames
		}
		to, _ := strconv.ParseUint(m[2], 16, 64)
		callee, caller := containing(to), containing(from)
		if callee == nil || caller == nil || callee.addr == caller.addr {
			continue
		}
		if k := [2]uint64{caller.addr, callee.addr}; !seen[k] {
			seen[k] = true
			g.edges[caller.addr] = append(g.edges[caller.addr], callee.addr)
		}
	}

	// Sections: the hook records and the annotation.
	var recAddr, recSize, annAddr, annSize uint64
	for _, line := range strings.Split(run(t, "readelf", "-SW", path), "\n") {
		m := secFull.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		switch m[1] {
		case "nevr_hook_records":
			recAddr, _ = strconv.ParseUint(m[2], 16, 64)
			recSize, _ = strconv.ParseUint(m[3], 16, 64)
		case outsideSection:
			annAddr, _ = strconv.ParseUint(m[2], 16, 64)
			annSize, _ = strconv.ParseUint(m[3], 16, 64)
		}
	}
	for i := range funcs {
		if annSize != 0 && funcs[i].addr >= annAddr && funcs[i].addr < annAddr+annSize {
			g.annotated[funcs[i].addr] = true
		}
	}

	// Hook records: {entry, handler} pairs read from the section's relocation addends.
	relative := map[uint64]uint64{}
	for _, line := range strings.Split(run(t, "readelf", "-rW", path), "\n") {
		if m := relRe.FindStringSubmatch(line); m != nil {
			off, _ := strconv.ParseUint(m[1], 16, 64)
			add, _ := strconv.ParseUint(m[2], 16, 64)
			relative[off] = add
		}
	}
	if recSize == 0 || recSize%16 != 0 {
		t.Fatalf("no nevr_hook_records section in %s (size %d): the sensor has no roots", path, recSize)
	}
	entries := map[uint64]int{}
	var records [][2]uint64
	for off := recAddr; off < recAddr+recSize; off += 16 {
		entry, okE := relative[off]
		handler, okH := relative[off+8]
		if !okE || !okH {
			t.Fatalf("hook record at %#x has no relocation for its entry/handler pointer", off)
		}
		entries[entry]++
		records = append(records, [2]uint64{entry, handler})
		for _, a := range []uint64{entry, handler} {
			if byAddr[a] == nil {
				t.Errorf("hook record at %#x points at %#x, which is not a function symbol", off, a)
				continue
			}
			g.roots = append(g.roots, a)
		}
	}
	return builtArtifact{graph: g, funcs: funcs, recordEntries: entries, records: records}
}

func anyMatches(names []string, re *regexp.Regexp) bool {
	for _, n := range names {
		if re.MatchString(n) {
			return true
		}
	}
	return false
}

func probeExe(t *testing.T, name string) string {
	t.Helper()
	p, err := filepath.Abs("../../build/android-arm64/integration/" + name)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(p); err != nil {
		t.Fatalf("control probe %s not found (run `just build-android`): %v", p, err)
	}
	return p
}

// ---- a reviewed assembly entry's own text (callback_thunk.h rule 7; the check is in asm_entry_text_test.go) ------------

var (
	cieHdrRe = regexp.MustCompile(`^([0-9a-f]+) [0-9a-f]+ [0-9a-f]+ CIE`)
	fdeHdrRe = regexp.MustCompile(`^[0-9a-f]+ [0-9a-f]+ [0-9a-f]+ FDE cie=([0-9a-f]+) pc=([0-9a-f]+)\.\.([0-9a-f]+)`)
	cfiRowRe = regexp.MustCompile(`^([0-9a-f]{8,16}) +(\S+)(.*)$`)
)

// The unwind rows of the FDEs starting at `starts`, from readelf's interpreted table. An FDE with no instructions has no table
// of its own: its one row is its CIE's initial rule, at the FDE's start. An address in `starts` with no FDE is absent.
// An FDE's unwind table and the address its range ends at.
type fdeTable struct {
	rows []cfiRow
	end  uint64
}

func cfiRowsOf(t *testing.T, path string, starts map[uint64]bool) map[uint64]fdeTable {
	t.Helper()
	cieRow := map[string]cfiRow{}
	cieOf := map[uint64]string{}
	out := map[uint64]fdeTable{}
	var cols []string
	cie := ""                      // the CIE whose table is being read
	fde, inFDE := uint64(0), false // the FDE whose table is being read
	for _, line := range strings.Split(run(t, "readelf", "--debug-dump=frames-interp", path), "\n") {
		if m := cieHdrRe.FindStringSubmatch(line); m != nil {
			cie, inFDE, cols = m[1], false, nil
			continue
		}
		if m := fdeHdrRe.FindStringSubmatch(line); m != nil {
			cie, inFDE, cols = "", false, nil
			start, _ := strconv.ParseUint(m[2], 16, 64)
			if starts[start] {
				fde, inFDE = start, true
				cieOf[start] = m[1]
				tab := out[start]
				tab.end, _ = strconv.ParseUint(m[3], 16, 64)
				out[start] = tab
			}
			continue
		}
		if f := strings.Fields(line); len(f) >= 2 && f[0] == "LOC" && f[1] == "CFA" {
			cols = f
			continue
		}
		m := cfiRowRe.FindStringSubmatch(line)
		if m == nil || cols == nil {
			continue
		}
		// readelf follows a register rule with the register's name in parentheses ("r19 (x19)"): not a column.
		vals := []string{m[1], m[2]}
		for _, f := range strings.Fields(m[3]) {
			if !strings.HasPrefix(f, "(") {
				vals = append(vals, f)
			}
		}
		row := cfiRow{cfa: vals[1], ra: "u", regs: map[string]string{}}
		row.loc, _ = strconv.ParseUint(vals[0], 16, 64)
		for i, c := range cols {
			switch {
			case i >= len(vals) || c == "LOC" || c == "CFA":
			case c == "ra":
				row.ra = vals[i]
			default:
				row.regs[c] = vals[i]
			}
		}
		switch {
		case cie != "":
			if _, seen := cieRow[cie]; !seen {
				cieRow[cie] = row
			}
		case inFDE:
			tab := out[fde]
			tab.rows = append(tab.rows, row)
			out[fde] = tab
		}
	}
	for start, c := range cieOf {
		if len(out[start].rows) == 0 {
			if initial, ok := cieRow[c]; ok {
				tab := out[start]
				tab.rows = []cfiRow{{loc: start, cfa: initial.cfa, ra: initial.ra, regs: initial.regs}}
				out[start] = tab
			} else {
				t.Fatalf("FDE at %#x names CIE %s, whose initial rule was not read: the parser is blind", start, c)
			}
		}
	}
	return out
}

// asmEntryErrors reads the text of every record entry that is not a CallbackThunk Entry and checks it (asmEntryTextErrors).
func asmEntryErrors(t *testing.T, path string, a builtArtifact, thunkEntries map[uint64]bool) []string {
	t.Helper()
	byAddr := map[uint64]elfFunc{}
	for _, f := range a.funcs {
		byAddr[f.addr] = f
	}
	var entries []asmEntryText
	starts := map[uint64]bool{}
	for _, r := range a.records {
		if thunkEntries[r[0]] {
			continue
		}
		f, ok := byAddr[r[0]]
		if !ok {
			continue // the record check reports it
		}
		entries = append(entries, asmEntryText{name: f.name, start: f.addr, end: f.addr + f.size, cont: r[1], contName: a.graph.names[r[1]]})
		starts[f.addr] = true
	}
	if len(entries) == 0 {
		return nil
	}
	rows := cfiRowsOf(t, path, starts)
	var errs []string
	for i := range entries {
		e := &entries[i]
		if r, ok := rows[e.start]; ok {
			e.rows, e.fdeEnd = r.rows, r.end
		}
		dis := run(t, "llvm-objdump", "-d", "--no-show-raw-insn", fmt.Sprintf("--start-address=%#x", e.start), fmt.Sprintf("--stop-address=%#x", e.end), path)
		for _, line := range strings.Split(dis, "\n") {
			m := insnRe.FindStringSubmatch(line)
			if m == nil {
				continue
			}
			in := asmInsn{mnem: m[2], ops: m[3]}
			in.addr, _ = strconv.ParseUint(m[1], 16, 64)
			if isDirectBranch(in.mnem) {
				if tm := branchTgtRe.FindStringSubmatch(in.ops); tm != nil {
					in.target, _ = strconv.ParseUint(tm[1], 16, 64)
				}
			}
			e.insns = append(e.insns, in)
		}
		if len(e.insns) == 0 {
			errs = append(errs, fmt.Sprintf("%s: no instructions disassembled at %#x..%#x: the sensor is blind", e.name, e.start, e.end))
			continue
		}
		errs = append(errs, asmEntryTextErrors(*e)...)
	}
	sort.Strings(errs)
	return errs
}

// ---- the controls ---------------------------------------------------------------------------------

func synthetic(annotateHelper bool) frameGraph {
	const handler, helper, deep, tail = 0x100, 0x200, 0x300, 0x400
	g := frameGraph{
		names:     map[uint64]string{handler: "Handler", helper: "Helper", deep: "Deep", tail: "_ZSt9terminatev"},
		aug:       map[uint64]string{handler: `"zR"`, helper: `"zPLR"`, deep: `"zPLR"`, tail: `"zPLR"`},
		edges:     map[uint64][]uint64{handler: {helper}, helper: {deep, tail}},
		roots:     []uint64{handler},
		annotated: map[uint64]bool{},
	}
	if annotateHelper {
		g.annotated[helper] = true
	}
	return g
}

// A personality function live across the game call (reached from the handler, not annotated) fails.
func TestSensorFailsALivePersonalityFunction(t *testing.T) {
	v := walkHookFrames(synthetic(false))
	if len(v.violations) == 0 || !strings.Contains(strings.Join(v.violations, "\n"), "Helper") {
		t.Fatalf("a personality-bearing function reachable from a handler must fail, got %v", v.violations)
	}
	if len(v.annotatedReached) != 0 {
		t.Errorf("nothing is annotated, got %v", v.annotatedReached)
	}
}

// The same function in the annotation section passes, is listed, and its callees are not entered.
func TestSensorSkipsAnAnnotatedFunctionAndListsIt(t *testing.T) {
	v := walkHookFrames(synthetic(true))
	if len(v.violations) != 0 {
		t.Fatalf("an annotated function must be exempt, got %v", v.violations)
	}
	if len(v.annotatedReached) != 1 || v.annotatedReached[0] != "Helper" {
		t.Errorf("annotated functions reached = %v, want [Helper]", v.annotatedReached)
	}
}

// The annotation exempts the marked function only: a personality function that is also reached on
// another path (here straight from the handler) still fails.
func TestSensorAnnotationDoesNotLaunderOtherPaths(t *testing.T) {
	g := synthetic(true)
	g.edges[0x100] = append(g.edges[0x100], 0x300) // the handler also calls Deep directly
	v := walkHookFrames(g)
	if !strings.Contains(strings.Join(v.violations, "\n"), "Deep") {
		t.Fatalf("Deep is live across the game call by the direct edge and must fail, got %v", v.violations)
	}
}

// A handler (or a thunk entry) itself under a personality fails; an annotated root is the author's claim
// about the root and is honoured the same way as any other function.
func TestSensorChecksTheRootsThemselves(t *testing.T) {
	g := synthetic(true)
	g.aug[0x100] = `"zPLR"`
	v := walkHookFrames(g)
	if !strings.Contains(strings.Join(v.violations, "\n"), "Handler") {
		t.Fatalf("a personality-bearing handler must fail, got %v", v.violations)
	}
}

func TestSensorFailsAFunctionWithoutAnFDE(t *testing.T) {
	g := synthetic(true)
	delete(g.aug, 0x100)
	v := walkHookFrames(g)
	if !strings.Contains(strings.Join(v.violations, "\n"), "no FDE") {
		t.Fatalf("a function without an FDE must fail, got %v", v.violations)
	}
}

// A linker erratum veneer (one relocated instruction and a branch back, no FDE) reached from a handler is a piece
// of its caller and passes; a function without an FDE under any other name still fails.
func TestSensorPassesALinkerErratumVeneer(t *testing.T) {
	g := synthetic(true)
	g.names[0x500] = "__CortexA53843419_2BD004"
	g.edges[0x100] = append(g.edges[0x100], 0x500)
	v := walkHookFrames(g)
	if len(v.violations) != 0 {
		t.Fatalf("an erratum veneer must pass, got %v", v.violations)
	}
	// The exemption is the exact name form the linker gives: anything around it, or another hex alphabet, is not it.
	for _, lookAlike := range []string{
		"__CortexA53843419_helper_not_a_veneer",
		"x__CortexA53843419_ABC",
		"__CortexA53843419_ABCdef",
		"__CortexA53843419_2BD004_x",
		"__CortexA53843419_",
		"_ZN3foo__CortexA53843419_2BD004Ev",
	} {
		g.names[0x500] = lookAlike
		v = walkHookFrames(g)
		if !strings.Contains(strings.Join(v.violations, "\n"), "no FDE") {
			t.Fatalf("the look-alike %q without an FDE must fail, got %v", lookAlike, v.violations)
		}
	}
	g.names[0x500] = "AnotherFunction"
	v = walkHookFrames(g)
	if !strings.Contains(strings.Join(v.violations, "\n"), "no FDE") {
		t.Fatalf("any other function without an FDE must fail, got %v", v.violations)
	}
}

// On a real linked executable: the unannotated helper with a try/catch, called straight from the
// handler across the call into the "game", is a violation.
func TestFramesProbeBadIsRejected(t *testing.T) {
	a := loadFrameGraph(t, probeExe(t, "frames_probe_bad"))
	v := walkHookFrames(a.graph)
	if !strings.Contains(strings.Join(v.violations, "\n"), "LiveHelper") {
		t.Fatalf("frames_probe_bad: the unannotated personality helper must be reported, got %v", v.violations)
	}
	if len(v.annotatedReached) != 0 {
		t.Errorf("frames_probe_bad annotates nothing, got %v", v.annotatedReached)
	}
}

// The identical helper annotated NEVR_OUTSIDE_GAME_CALL passes and is listed; and the parser can see its
// personality (so the pass is the exemption, not blindness).
func TestFramesProbeOkIsAcceptedAndListed(t *testing.T) {
	a := loadFrameGraph(t, probeExe(t, "frames_probe_ok"))
	v := walkHookFrames(a.graph)
	if len(v.violations) != 0 {
		t.Fatalf("frames_probe_ok: %v", v.violations)
	}
	if !anyMatches(v.annotatedReached, regexp.MustCompile(`LiveHelper`)) {
		t.Fatalf("frames_probe_ok: LiveHelper must be listed as an annotated function reached, got %v", v.annotatedReached)
	}
	sawPersonality := false
	for addr, name := range a.graph.names {
		if strings.Contains(name, "LiveHelper") && strings.Contains(a.graph.aug[addr], "P") {
			sawPersonality = true
		}
	}
	if !sawPersonality {
		t.Errorf("LiveHelper does not sit under a personality-bearing CIE: the control proves nothing")
	}
}

// The cold-tail exemption must not swallow our own functions: a function in a top-level anonymous
// namespace (where hook handlers live) that carries a personality is a violation. An earlier pattern
// exempted every `_ZN12_GLOBAL__N_1` name and made frames_probe_bad pass.
func TestSensorDoesNotExemptAnonymousNamespaceFunctions(t *testing.T) {
	g := synthetic(false)
	g.names[0x200] = "_ZN12_GLOBAL__N_112ProbeHandlerEPFiiEi"
	v := walkHookFrames(g)
	if !strings.Contains(strings.Join(v.violations, "\n"), "ProbeHandler") {
		t.Fatalf("an anonymous-namespace function under a personality must fail, got %v", v.violations)
	}
}

// ---- the assembly entry's record ----------------------------------------------------------------------

func TestRecordEntryErrorsAcceptsThunksAndReviewedAsmEntries(t *testing.T) {
	names := map[uint64]string{0x100: "ThunkEntry", 0x200: "q15t_fire_thunk"}
	reviewed := []*regexp.Regexp{regexp.MustCompile(`^q15t_fire_thunk$`)}
	if errs := recordEntryErrors(names, map[uint64]bool{0x100: true}, map[uint64]int{0x100: 1, 0x200: 1}, reviewed); len(errs) != 0 {
		t.Fatalf("a thunk entry and a reviewed assembly entry, one record each, must pass, got %v", errs)
	}
}

// A record whose entry is not a thunk Entry and not reviewed is how an unreviewed assembly entry would reach a build.
func TestRecordEntryErrorsRejectsAnUnreviewedEntry(t *testing.T) {
	names := map[uint64]string{0x100: "ThunkEntry", 0x200: "some_naked_function"}
	errs := recordEntryErrors(names, map[uint64]bool{0x100: true}, map[uint64]int{0x100: 1, 0x200: 1}, nil)
	if len(errs) != 1 || !strings.Contains(errs[0], "some_naked_function") || !strings.Contains(errs[0], "reviewed assembly entry") {
		t.Fatalf("an unreviewed record entry must fail naming it, got %v", errs)
	}
}

func TestRecordEntryErrorsRejectsADoubledOrStaleEntry(t *testing.T) {
	names := map[uint64]string{0x200: "q15t_fire_thunk"}
	reviewed := []*regexp.Regexp{regexp.MustCompile(`^q15t_fire_thunk$`), regexp.MustCompile(`^q15t_gone_thunk$`)}
	errs := recordEntryErrors(names, nil, map[uint64]int{0x200: 2}, reviewed)
	joined := strings.Join(errs, "\n")
	if !strings.Contains(joined, "has 2 hook records") || !strings.Contains(joined, "q15t_gone_thunk") {
		t.Fatalf("a doubled record and a reviewed name with no record must both fail, got %v", errs)
	}
}

func TestRecordEntryErrorsStillRequiresOneRecordPerThunk(t *testing.T) {
	names := map[uint64]string{0x100: "ThunkEntry"}
	errs := recordEntryErrors(names, map[uint64]bool{0x100: true}, map[uint64]int{}, nil)
	if len(errs) != 1 || !strings.Contains(errs[0], "has 0 hook records") {
		t.Fatalf("a thunk entry without a record must fail, got %v", errs)
	}
}

var probeAsmEntryRe = regexp.MustCompile(`^probe_asm_entry$`)

func probeAsm(t *testing.T, variant string) (builtArtifact, frameVerdict) {
	t.Helper()
	a := loadFrameGraph(t, probeExe(t, "frames_asm_probe_"+variant))
	return a, walkHookFrames(a.graph)
}

func addrOf(a builtArtifact, name string) uint64 {
	for addr, n := range a.graph.names {
		if n == name {
			return addr
		}
	}
	return 0
}

// On a real linked executable: the record names the entry and its continuation, both are roots, the entry has an FDE under
// "zR", and the entry's bl to the continuation is an edge. The record passes the entry check as a reviewed entry.
func TestFramesProbeAsmEntryIsReadAsARecord(t *testing.T) {
	a, v := probeAsm(t, "ok")
	entry, cont := addrOf(a, "probe_asm_entry"), addrOf(a, "probe_asm_continuation")
	if entry == 0 || cont == 0 {
		t.Fatalf("probe_asm_entry %#x or probe_asm_continuation %#x is not a function symbol of frames_asm_probe_ok", entry, cont)
	}
	if a.recordEntries[entry] != 1 || len(a.recordEntries) != 1 {
		t.Errorf("want exactly one record, naming the entry; got %v", a.recordEntries)
	}
	roots := map[uint64]bool{}
	for _, r := range a.graph.roots {
		roots[r] = true
	}
	if !roots[entry] || !roots[cont] {
		t.Errorf("the entry %#x and its continuation %#x must both be roots, roots=%v", entry, cont, a.graph.roots)
	}
	if a.graph.aug[entry] != `"zR"` {
		t.Errorf("the entry sits under CIE augmentation %q, want \"zR\"", a.graph.aug[entry])
	}
	found := false
	for _, callee := range a.graph.edges[entry] {
		found = found || callee == cont
	}
	if !found {
		t.Errorf("no direct edge from the entry to its continuation: the entry's bl was not read (edges %v)", a.graph.edges[entry])
	}
	errs := recordEntryErrors(a.graph.names, map[uint64]bool{}, a.recordEntries, []*regexp.Regexp{probeAsmEntryRe})
	if len(errs) != 0 || len(v.violations) != 0 {
		t.Errorf("frames_asm_probe_ok: record errors %v, frame violations %v", errs, v.violations)
	}
	if !anyMatches(v.annotatedReached, regexp.MustCompile(`LiveHelper`)) {
		t.Errorf("the continuation's helper must be listed as an annotated function reached, got %v", v.annotatedReached)
	}
	if text := asmEntryErrors(t, probeExe(t, "frames_asm_probe_ok"), a, nil); len(text) != 0 {
		t.Errorf("frames_asm_probe_ok: the entry's text (ldar loads, the bl to its continuation, CFA=sp+16 and ra=c-8 at the bl) must pass, got %v", text)
	}
}

// The continuation calls an unannotated helper with a personality across the game call: the walk from the record must find it,
// and must find it from the record's continuation alone, without the entry's own edge to it (an entry whose call the
// disassembly would not show).
func TestFramesProbeAsmEntryContinuationIsWalked(t *testing.T) {
	a, v := probeAsm(t, "bad")
	if !strings.Contains(strings.Join(v.violations, "\n"), "LiveHelper") {
		t.Fatalf("frames_asm_probe_bad: the unannotated personality helper behind the continuation must be reported, got %v", v.violations)
	}
	g := a.graph
	g.edges = map[uint64][]uint64{}
	for from, to := range a.graph.edges {
		g.edges[from] = to
	}
	delete(g.edges, addrOf(a, "probe_asm_entry"))
	if v := walkHookFrames(g); !strings.Contains(strings.Join(v.violations, "\n"), "LiveHelper") {
		t.Fatalf("frames_asm_probe_bad without the entry's edges: the record's continuation must be a root of its own, got %v", v.violations)
	}
}

// An entry with no CFI has no FDE, so a game exception could not unwind through it: the sensor fails the entry itself.
func TestFramesProbeAsmEntryWithoutCFIFails(t *testing.T) {
	a, v := probeAsm(t, "nocfi")
	if !strings.Contains(strings.Join(v.violations, "\n"), "probe_asm_entry") || !strings.Contains(strings.Join(v.violations, "\n"), "no FDE") {
		t.Fatalf("frames_asm_probe_nocfi: the entry with no FDE must be reported, got %v", v.violations)
	}
	if got := strings.Join(asmEntryErrors(t, probeExe(t, "frames_asm_probe_nocfi"), a, nil), "\n"); !strings.Contains(got, "no unwind row covers the bl") {
		t.Fatalf("frames_asm_probe_nocfi: the text check must report the bl no row covers, got:\n%s", got)
	}
}

// The q15t shape: the same body as an __attribute__((naked)) function with no CFI. clang gives it an FDE under "zR", so the walk
// passes it; its one row is CFA=sp, which is wrong after the stp and at the bl, and the entry's text check fails it.
func TestFramesProbeAsmEntryNakedFails(t *testing.T) {
	a, v := probeAsm(t, "naked")
	entry := addrOf(a, "probe_asm_entry")
	if a.graph.aug[entry] != `"zR"` || len(v.violations) != 0 {
		t.Fatalf("frames_asm_probe_naked: the control expects an FDE under \"zR\" that the walk passes (aug %q, violations %v)", a.graph.aug[entry], v.violations)
	}
	got := strings.Join(asmEntryErrors(t, probeExe(t, "frames_asm_probe_naked"), a, nil), "\n")
	if !strings.Contains(got, "gives CFA=sp+0 (want sp+16)") || !strings.Contains(got, "gives ra=u (want c-8)") {
		t.Fatalf("frames_asm_probe_naked: the CFA and the return-address rule at the bl must be reported, got:\n%s", got)
	}
}

// #2216's shape: the two words read with a plain ldr. Both loads, and the br through what the second one loaded, fail.
func TestFramesProbeAsmEntryPlainLoadFails(t *testing.T) {
	a, _ := probeAsm(t, "ldr")
	errs := asmEntryErrors(t, probeExe(t, "frames_asm_probe_ldr"), a, nil)
	got := strings.Join(errs, "\n")
	if strings.Count(got, "reads a word from outside the entry's stack, and it is not ldar or ldapr") != 2 || !strings.Contains(got, "which is not an acquire load") {
		t.Fatalf("frames_asm_probe_ldr: both plain loads and the br through one must be reported, got:\n%s", got)
	}
	if strings.Contains(got, "CFA=") || strings.Contains(got, "ra=") {
		t.Errorf("frames_asm_probe_ldr has the ok probe's CFI; only its loads are wrong, got:\n%s", got)
	}
}

// The record must name what the entry calls: a record whose continuation is another function fails, on the real entry's text.
func TestFramesProbeAsmEntryMustCallItsRecordsContinuation(t *testing.T) {
	a, _ := probeAsm(t, "ok")
	helper := uint64(0)
	for addr, n := range a.graph.names {
		if strings.Contains(n, "LiveHelper") {
			helper = addr
		}
	}
	if helper == 0 || len(a.records) != 1 {
		t.Fatalf("frames_asm_probe_ok: want one record and a LiveHelper function, got records %v", a.records)
	}
	a.records[0][1] = helper
	got := strings.Join(asmEntryErrors(t, probeExe(t, "frames_asm_probe_ok"), a, nil), "\n")
	if !strings.Contains(got, "not to its record's continuation") || !strings.Contains(got, "never branches to its record's continuation") {
		t.Fatalf("a record naming another function as the continuation must fail, got:\n%s", got)
	}
}

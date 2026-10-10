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
// Not exempted by the annotation, and so not entered either way: the cold, noreturn tail of libc++ and
// libc++abi (a length or range error, terminate, the exception allocator). Direct container operations
// reach them but they do not run during a normal call, and when one does run it raises a sentinel
// exception that never returns, so it is never a frame a game exception passes through.
var libcxxThrowTail = regexp.MustCompile(`^(__cxa_|_ZSt9terminatev|_ZSt11__terminatePFvvE|_ZNSt6__ndk120__throw_|_ZNSt11logic_error|_ZN10__cxxabiv1)`)

const outsideSection = "nevr_outside_game_call"

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
		if libcxxThrowTail.MatchString(name) {
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
	for off := recAddr; off < recAddr+recSize; off += 16 {
		entry, okE := relative[off]
		handler, okH := relative[off+8]
		if !okE || !okH {
			t.Fatalf("hook record at %#x has no relocation for its entry/handler pointer", off)
		}
		entries[entry]++
		for _, a := range []uint64{entry, handler} {
			if byAddr[a] == nil {
				t.Errorf("hook record at %#x points at %#x, which is not a function symbol", off, a)
				continue
			}
			g.roots = append(g.roots, a)
		}
	}
	return builtArtifact{graph: g, funcs: funcs, recordEntries: entries}
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

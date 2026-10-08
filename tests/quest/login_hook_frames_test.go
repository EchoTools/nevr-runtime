package quest

import (
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// The login hook, on the same rule as TestHookFramesCarryNoPersonality (callback_thunk.h, rule
// 1): a frame that is live while game code runs must sit under the personality-free "zR" CIE.
// The login code is not part of the sentinel .so yet, so this test applies the same walk to a
// probe executable that links the whole nevr_quest_login archive (login_frames_probe): start
// from every hook record's entry and handler (the nevr_hook_records section), follow direct
// bl/b edges and fail on any reachable function under a personality-bearing CIE.
//
// The one allowed exception is ComposePlan, the exceptions-enabled compose phase. It calls no
// game code and has returned before the next game call, so it is never on the stack while game
// code runs; the walk does not enter it. The test requires that it IS reached, so the
// exemption cannot go stale, and that it does sit under a personality, so the parser is not
// blind to one.
const loginProbeRel = "../../build/android-arm64/login_frames_probe"

var composePlanRe = regexp.MustCompile(`^_ZN10QuestLogin11ComposePlanE`)

// The cold, noreturn tail of libc++ and libc++abi's checks (a length or range error, terminate,
// the exception allocator). They are reached by direct edges from container operations but do
// not run during a normal call, and when one does run it raises a sentinel exception that never
// returns, so it is never a frame a game exception passes through. Not entered by the walk.
var libcxxThrowTail = regexp.MustCompile(`^(__cxa_|_ZSt9terminatev|_ZSt11__terminatePFvvE|_ZNSt6__ndk120__throw_|_ZNSt11logic_error|_ZN10__cxxabiv1|_ZN12_GLOBAL__N_1)`)

func probePath(t *testing.T) string {
	t.Helper()
	p, err := filepath.Abs(loginProbeRel)
	if err != nil {
		t.Fatalf("resolve path: %v", err)
	}
	if _, err := os.Stat(p); err != nil {
		t.Fatalf("probe executable not found at %s (run `just build-android`): %v", p, err)
	}
	return p
}

func framesOf(t *testing.T, path string) (map[string]string, map[uint64]string) {
	t.Helper()
	cieAug := map[string]string{}
	fdeCIE := map[uint64]string{}
	current := ""
	for _, line := range strings.Split(run(t, "readelf", "--debug-dump=frames", path), "\n") {
		if m := cieRe.FindStringSubmatch(line); m != nil {
			current = m[1]
			continue
		}
		if m := augRe.FindStringSubmatch(line); m != nil && current != "" {
			cieAug[current] = m[1]
			current = ""
			continue
		}
		if m := fdeRe.FindStringSubmatch(line); m != nil {
			pc, err := strconv.ParseUint(m[2], 16, 64)
			if err != nil {
				t.Fatalf("bad pc %q", m[2])
			}
			fdeCIE[pc] = m[1]
		}
	}
	if len(cieAug) == 0 || len(fdeCIE) == 0 {
		t.Fatalf("parsed no CIE/FDE from %s; the parser is blind", path)
	}
	return cieAug, fdeCIE
}

func funcsOf(t *testing.T, path string) []elfFunc {
	t.Helper()
	sections := map[string]string{}
	for _, line := range strings.Split(run(t, "readelf", "-SW", path), "\n") {
		if m := secRe.FindStringSubmatch(line); m != nil {
			sections[m[1]] = m[2]
		}
	}
	var fs []elfFunc
	for _, line := range strings.Split(run(t, "readelf", "-sW", path), "\n") {
		m := symRe.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		addr, _ := strconv.ParseUint(m[1], 16, 64)
		size, _ := strconv.ParseUint(m[2], 10, 64)
		if size == 0 {
			continue
		}
		fs = append(fs, elfFunc{addr: addr, size: size, name: m[4], section: sections[m[3]]})
	}
	if len(fs) == 0 {
		t.Fatalf("no function symbols parsed from %s", path)
	}
	return fs
}

func TestLoginHookFramesCarryNoPersonality(t *testing.T) {
	path := probePath(t)
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

	edges := map[uint64]map[uint64]bool{}
	var from uint64
	for _, line := range strings.Split(run(t, "llvm-objdump", "-d", "--no-show-raw-insn", path), "\n") {
		if m := hdrRe.FindStringSubmatch(line); m != nil {
			from, _ = strconv.ParseUint(m[1], 16, 64)
			continue
		}
		m := branchR.FindStringSubmatch(line)
		if m == nil || strings.HasSuffix(m[3], "@plt") {
			continue
		}
		to, _ := strconv.ParseUint(m[2], 16, 64)
		callee, caller := containing(to), containing(from)
		if callee == nil || caller == nil || callee.addr == caller.addr {
			continue
		}
		if edges[caller.addr] == nil {
			edges[caller.addr] = map[uint64]bool{}
		}
		edges[caller.addr][callee.addr] = true
	}

	// Hook records: {entry, handler} pairs read from the section's relocation addends.
	var recAddr, recSize uint64
	for _, line := range strings.Split(run(t, "readelf", "-SW", path), "\n") {
		if m := secFull.FindStringSubmatch(line); m != nil && m[1] == "nevr_hook_records" {
			recAddr, _ = strconv.ParseUint(m[2], 16, 64)
			recSize, _ = strconv.ParseUint(m[3], 16, 64)
		}
	}
	relative := map[uint64]uint64{}
	for _, line := range strings.Split(run(t, "readelf", "-rW", path), "\n") {
		if m := relRe.FindStringSubmatch(line); m != nil {
			off, _ := strconv.ParseUint(m[1], 16, 64)
			add, _ := strconv.ParseUint(m[2], 16, 64)
			relative[off] = add
		}
	}
	if recSize == 0 || recSize%16 != 0 {
		t.Fatalf("no nevr_hook_records section (size %d): the walk has no roots", recSize)
	}
	var roots []uint64
	for off := recAddr; off < recAddr+recSize; off += 16 {
		entry, okE := relative[off]
		handler, okH := relative[off+8]
		if !okE || !okH {
			t.Fatalf("hook record at %#x has no relocation for its entry/handler pointer", off)
		}
		roots = append(roots, entry, handler)
	}
	reached := map[uint64]bool{}
	queue := append([]uint64(nil), roots...)
	for _, r := range roots {
		reached[r] = true
	}
	composeReached, composeAug := false, ""
	checked := 0
	for len(queue) > 0 {
		a := queue[0]
		queue = queue[1:]
		f := byAddr[a]
		if f == nil {
			continue
		}
		cie, ok := fdeCIE[a]
		if !ok {
			t.Errorf("%s at %#x has no FDE", f.name, a)
			continue
		}
		if composePlanRe.MatchString(f.name) {
			composeReached, composeAug = true, cieAug[cie]
			continue // runs between game calls and calls none; not entered
		}
		if libcxxThrowTail.MatchString(f.name) {
			continue
		}
		checked++
		if aug := cieAug[cie]; aug != `"zR"` {
			t.Errorf("function %s (reachable from the login handler) sits under CIE augmentation %s, want \"zR\" (no personality, no LSDA)", f.name, aug)
		}
		for callee := range edges[a] {
			if !reached[callee] {
				reached[callee] = true
				queue = append(queue, callee)
			}
		}
	}
	if !composeReached {
		t.Errorf("ComposePlan was not reached from the handler: the exemption is stale or the walk lost an edge")
	}
	if !strings.Contains(composeAug, "P") {
		t.Errorf("ComposePlan sits under CIE augmentation %s, want a personality-bearing CIE (the parser must be able to see one)", composeAug)
	}
	if checked < len(roots)+3 {
		t.Errorf("checked only %d functions for %d roots: the walk lost its edges", checked, len(roots))
	}
	t.Logf("login roots=%d, functions checked=%d", len(roots), checked)
}

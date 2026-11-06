package quest

import (
	"regexp"
	"strings"
	"testing"
)

// The assembly-entry check's controls, built by src/quest/integration/CMakeLists.txt from src/quest/tests/frames_asm_probes/*.cpp
// and src/quest/tests/frames_asm_probe_real.cpp. Each of the first seven is an entry that is wrong in one way the entry text
// check must see, behind an FDE under the personality-free "zR" CIE (so the frame walk passes it: only the text check can fail
// it). x30_in_x19 is a correct entry that keeps x30 in a callee-saved register. The real_* executables hold the q15t plugin's
// four entries (two dronemuzzle thunks, powerboost's resolve thunk, the headset trampoline) in three forms.

// asmProbeText runs the entry check on a linked probe and returns what it reports, with the frame walk's violations.
func asmProbeText(t *testing.T, variant string) (errs []string, violations []string, a builtArtifact) {
	t.Helper()
	a, v := probeAsm(t, variant)
	return asmEntryErrors(t, probeExe(t, "frames_asm_probe_"+variant), a, nil), v.violations, a
}

func TestFramesAsmProbesWrongEntriesFail(t *testing.T) {
	for _, c := range []struct {
		variant string
		why     string
		want    []string // each must appear in the report
	}{
		{"tailb_sp", "a tail b with the frame still pushed", []string{"the b at", "leaves the entry with sp 16 bytes below the caller's"}},
		{"secondbl", "a second bl after the pop: x30 is no longer saved anywhere", []string{"overwrites x30, which the entry has not saved", "leaves the entry with x30 not the caller's return address"}},
		{"below_sp", "x30 stored below sp", []string{"stores x30 at CFA-8, below the sp (CFA+0) it leaves"}},
		{"local_bl", "a bl to a label inside the entry", []string{"calls", "inside the entry"}},
		{"path_br", "a br register set by a mov on one path", []string{"jumps through x16, last set by `mov x16, x1`", "which is not an acquire load"}},
		{"ldadd_ret", "a relaxed ldadd as the call-site load", []string{"ldadd xzr, x17, [x16]", "is not an acquire load of a published word"}},
		{"cfi_only_at_bl", "unwind rows right at the bl and wrong before it", []string{"gives CFA=sp+0 (want sp+16)"}},
	} {
		t.Run(c.variant, func(t *testing.T) {
			errs, violations, _ := asmProbeText(t, c.variant)
			if len(violations) != 0 {
				t.Fatalf("frames_asm_probe_%s (%s) must reach the entry check: the frame walk reports %v", c.variant, c.why, violations)
			}
			got := strings.Join(errs, "\n")
			for _, w := range c.want {
				if !strings.Contains(got, w) {
					t.Errorf("frames_asm_probe_%s (%s): want the report to contain %q, got:\n%s", c.variant, c.why, w, got)
				}
			}
			if len(errs) == 0 {
				t.Errorf("frames_asm_probe_%s (%s) passes the entry check", c.variant, c.why)
			}
		})
	}
}

// A correct entry that keeps x30 in x19 and says so with .cfi_register passes the walk and the text check, and the record check.
func TestFramesAsmProbeX30InCalleeSavedRegisterPasses(t *testing.T) {
	errs, violations, a := asmProbeText(t, "x30_in_x19")
	if len(violations) != 0 || len(errs) != 0 {
		t.Fatalf("frames_asm_probe_x30_in_x19: walk violations %v, entry errors %v; want none", violations, errs)
	}
	if got := recordEntryErrors(a.graph.names, map[uint64]bool{}, a.recordEntries, []*regexp.Regexp{probeAsmEntryRe}); len(got) != 0 {
		t.Errorf("frames_asm_probe_x30_in_x19: record errors %v", got)
	}
}

var q15tEntryRes = []*regexp.Regexp{
	regexp.MustCompile(`^q15t_dm_tp_thunk$`), regexp.MustCompile(`^q15t_dm_fire_thunk$`),
	regexp.MustCompile(`^q15t_pb_resolve_thunk$`), regexp.MustCompile(`^q15t_headset_tramp$`),
}

// The plugin's four entries as top-level asm() with CFI and ldar loads pass the walk, the record check and the text check.
func TestFramesAsmProbeRealMovedEntriesPass(t *testing.T) {
	errs, violations, a := asmProbeText(t, "real_moved")
	if len(a.records) != 4 {
		t.Fatalf("frames_asm_probe_real_moved: want the four q15t records, got %v", a.records)
	}
	if got := recordEntryErrors(a.graph.names, map[uint64]bool{}, a.recordEntries, q15tEntryRes); len(got) != 0 {
		t.Errorf("frames_asm_probe_real_moved: record errors %v", got)
	}
	if len(violations) != 0 || len(errs) != 0 {
		t.Fatalf("frames_asm_probe_real_moved: walk violations %v, entry errors %v; want none", violations, errs)
	}
}

// #2216: powerboost's two words read with a plain ldr fail, and so does the br through one of them; the other three entries do not.
func TestFramesAsmProbeRealPowerboostPlainLoadFails(t *testing.T) {
	errs, violations, _ := asmProbeText(t, "real_moved_pbldr")
	if len(violations) != 0 {
		t.Fatalf("frames_asm_probe_real_moved_pbldr: the walk must pass it, got %v", violations)
	}
	got := strings.Join(errs, "\n")
	if strings.Count(got, "is not an acquire load of a published word") != 2 || !strings.Contains(got, "which is not an acquire load (the next word") {
		t.Fatalf("frames_asm_probe_real_moved_pbldr: both ldr and the br through one must be reported, got:\n%s", got)
	}
	for _, e := range errs {
		if !strings.HasPrefix(e, "q15t_pb_resolve_thunk: ") {
			t.Errorf("frames_asm_probe_real_moved_pbldr: only powerboost's entry is wrong, got %q", e)
		}
	}
}

// The entries as the plugin writes them: the three naked functions get an FDE whose one row is CFA=sp, which the text check
// fails; the trampoline is a top-level block with no CFI, so it has no FDE and the walk fails it.
func TestFramesAsmProbeRealNakedEntriesFail(t *testing.T) {
	errs, violations, _ := asmProbeText(t, "real_naked")
	got := strings.Join(errs, "\n")
	for _, name := range []string{"q15t_dm_tp_thunk", "q15t_dm_fire_thunk", "q15t_pb_resolve_thunk"} {
		found := false
		for _, e := range errs {
			found = found || (strings.HasPrefix(e, name+": ") && strings.Contains(e, "gives CFA=sp+0 (want sp+16)"))
		}
		if !found {
			t.Errorf("frames_asm_probe_real_naked: %s must fail the text check on its CFA, got:\n%s", name, got)
		}
	}
	if !strings.Contains(strings.Join(violations, "\n"), "q15t_headset_tramp at ") || !strings.Contains(strings.Join(violations, "\n"), "has no FDE") {
		t.Errorf("frames_asm_probe_real_naked: the trampoline with no CFI must fail the walk, got %v", violations)
	}
}

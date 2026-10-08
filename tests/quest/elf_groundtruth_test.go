// Ground-truth tests for the Quest crash-reporter .so.
//
// On-device runtime needs a headset, so the automatable ground truth is the
// ELF shape of the built artifact. Each test traces to a BAC in
// docs/2026-07-13-quest-crash-reporter-injection.md and shells to
// readelf/nm on the real output (success derives from the artifact, not a
// proxy). The test FAILS (not skips) when the .so is absent — build it first:
//
//	just build-android
package quest

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// Path to the built shim, relative to this test package (tests/quest).
const soRelPath = "../../build/android-arm64/sentinel/libovrplatformloader.so"

func soPath(t *testing.T) string {
	t.Helper()
	p, err := filepath.Abs(soRelPath)
	if err != nil {
		t.Fatalf("resolve path: %v", err)
	}
	return p
}

// run executes a tool and returns combined output, failing the test on error.
func run(t *testing.T, name string, args ...string) string {
	t.Helper()
	out, err := exec.Command(name, args...).CombinedOutput()
	if err != nil {
		t.Fatalf("%s %s: %v\n%s", name, strings.Join(args, " "), err, out)
	}
	return string(out)
}

// requireArtifact fails with an actionable message when the .so is missing,
// giving the RED->GREEN contract its RED state.
func requireArtifact(t *testing.T) string {
	t.Helper()
	p := soPath(t)
	out, err := exec.Command("readelf", "-h", p).CombinedOutput()
	if err != nil {
		t.Fatalf("crash-reporter .so not found or unreadable at %s (run `just build-android`): %v\n%s", p, err, out)
	}
	return string(out)
}

// BAC-1: the artifact is an ELF64 AArch64 shared object.
func TestBAC1_ELFShapeArm64(t *testing.T) {
	hdr := requireArtifact(t)
	for _, want := range []string{"ELF64", "AArch64", "DYN (Shared object file)"} {
		if !strings.Contains(hdr, want) {
			t.Errorf("readelf -h missing %q\n%s", want, hdr)
		}
	}
}

// BAC-2: the sentinel marker export is present (our code is in the artifact).
func TestBAC2_SentinelMarkerExported(t *testing.T) {
	requireArtifact(t)
	dyn := run(t, "nm", "-D", soPath(t))
	if !strings.Contains(dyn, "nevr_sentinel_marker") {
		t.Errorf("nm -D missing exported nevr_sentinel_marker\n%s", dyn)
	}
}

// BAC-3: forward-to-original is wired — DT_NEEDED libovrplatformloader_orig.so.
func TestBAC3_ForwardsToRenamedOriginal(t *testing.T) {
	requireArtifact(t)
	dyn := run(t, "readelf", "-d", soPath(t))
	if !strings.Contains(dyn, "libovrplatformloader_orig.so") {
		t.Errorf("readelf -d missing NEEDED libovrplatformloader_orig.so (forwarding not wired)\n%s", dyn)
	}
}

// BAC-4: the shim takes the hijacked name (SONAME libovrplatformloader.so).
func TestBAC4_SonameHijacksLoader(t *testing.T) {
	requireArtifact(t)
	dyn := run(t, "readelf", "-d", soPath(t))
	if !strings.Contains(dyn, "Library soname: [libovrplatformloader.so]") {
		t.Errorf("readelf -d SONAME is not libovrplatformloader.so\n%s", dyn)
	}
}

// BAC-5: it arms early — exports JNI_OnLoad and has a non-empty .init_array
// (the ELF constructor).
func TestBAC5_EarlyArmHooks(t *testing.T) {
	requireArtifact(t)
	dyn := run(t, "nm", "-D", soPath(t))
	if !strings.Contains(dyn, "JNI_OnLoad") {
		t.Errorf("nm -D missing exported JNI_OnLoad\n%s", dyn)
	}
	sections := run(t, "readelf", "-S", soPath(t))
	if !strings.Contains(sections, ".init_array") {
		t.Errorf("readelf -S missing .init_array (no load-time constructor)\n%s", sections)
	}
}

// Guard: breakpad's internal symbols must not leak from our export set
// (--exclude-libs,ALL). Not a numbered BAC but a quality gate.
func TestExportHygiene_NoBreakpadLeak(t *testing.T) {
	requireArtifact(t)
	dyn := run(t, "nm", "-D", soPath(t))
	for _, line := range strings.Split(dyn, "\n") {
		low := strings.ToLower(line)
		if strings.Contains(low, "breakpad") || strings.Contains(low, "minidump") {
			t.Errorf("breakpad/minidump symbol leaked into dynamic exports: %s", line)
		}
	}
}

// Only the two entry points the loader needs are exported. Everything else,
// including the hook API and its test seams, is hidden (-fvisibility=hidden).
func TestExportAllowlist(t *testing.T) {
	requireArtifact(t)
	allowed := map[string]bool{"nevr_sentinel_marker": true, "JNI_OnLoad": true}
	dyn := run(t, "nm", "-D", "--defined-only", soPath(t))
	for _, line := range strings.Split(dyn, "\n") {
		fields := strings.Fields(line)
		if len(fields) < 3 {
			continue
		}
		if name := fields[len(fields)-1]; !allowed[name] {
			t.Errorf("unexpected dynamic export %q (allowed: nevr_sentinel_marker, JNI_OnLoad)", name)
		}
	}
}

// The sentinel keeps no thread_local state of its own: with the API 26 toolchain
// thread_local is emulated (__emutls_v.*) and its first use on a thread allocates,
// which must not happen inside a hooked libc function. The only emulated-TLS
// symbol allowed is libc++abi's own exception-globals slot.
func TestNoEmulatedTLSInHookPath(t *testing.T) {
	requireArtifact(t)
	syms := run(t, "nm", soPath(t))
	seen := 0
	for _, line := range strings.Split(syms, "\n") {
		if !strings.Contains(line, "__emutls_v.") {
			continue
		}
		seen++
		if !strings.Contains(line, "__cxxabiv1") {
			t.Errorf("emulated TLS variable that is not libc++abi's: %s", line)
		}
	}
	if seen == 0 {
		t.Errorf("no __emutls_v.* symbol at all: the allowlist is not looking at what it thinks it is")
	}
}

var (
	cieRe = regexp.MustCompile(`^([0-9a-f]{8}) [0-9a-f]{16} [0-9a-f]{8} CIE$`)
	fdeRe = regexp.MustCompile(`^[0-9a-f]{8} [0-9a-f]{16} [0-9a-f]{8} FDE cie=([0-9a-f]{8}) pc=([0-9a-f]+)\.\.([0-9a-f]+)`)
	augRe = regexp.MustCompile(`^\s+Augmentation:\s+("[^"]*")`)
)

// Rule (callback_thunk.h, "What the contract is"): a frame that is live while game code
// runs under a hook must carry no personality routine and no LSDA, i.e. sit under the "zR"
// CIE. A "zPLR" CIE names the sentinel's own personality, and a game exception unwinding
// through such a frame would hand libc++_shared's _Unwind_Context to it.
//
// Live frames are the thunk's Entry, every handler, and every sentinel function a handler
// can have on the stack when it calls `original`. A static check cannot tell "before/after
// the call" from "across the call", so it is conservative: it starts from every hook and
// follows direct bl/b edges through the library, failing on any reachable function under a
// personality-bearing CIE. There is no allowlist: a hook does not log, so no logging code is
// reachable from it. Indirect calls (function pointers, virtual calls, std::function) are not
// followed: that part of the rule is a rule, not a check (callback_thunk.h, item 5).
//
// A hook is a record in the nevr_hook_records output section, emitted by NEVR_HOOK_RECORD:
// {entry, handler}, two function pointers that the dynamic linker relocates (R_AARCH64_RELATIVE),
// so they are read from the relocation addends. Every thunk Entry must have exactly one
// record, so a thunk whose handler is not recorded (and therefore not walked) fails.
type elfFunc struct {
	addr, size uint64
	name       string
	section    string
}

func parseFrames(t *testing.T) (cieAug map[string]string, fdeCIE map[uint64]string) {
	t.Helper()
	frames := run(t, "readelf", "--debug-dump=frames", soPath(t))
	cieAug = map[string]string{} // CIE offset -> augmentation
	fdeCIE = map[uint64]string{} // FDE start pc -> CIE offset
	current := ""
	for _, line := range strings.Split(frames, "\n") {
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
		t.Fatalf("parsed no CIE/FDE from readelf --debug-dump=frames; the parser is blind")
	}
	return cieAug, fdeCIE
}

var (
	secRe   = regexp.MustCompile(`^\s*\[\s*(\d+)\]\s+(\S+)`)
	secFull = regexp.MustCompile(`^\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)`)
	relRe   = regexp.MustCompile(`^([0-9a-f]+)\s+[0-9a-f]+\s+R_AARCH64_RELATIVE\s+([0-9a-f]+)\s*$`)
	symRe   = regexp.MustCompile(`^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+FUNC\s+\S+\s+\S+\s+(\d+)\s+(\S+)`)
	hdrRe   = regexp.MustCompile(`^([0-9a-f]{16}) <(.*)>:$`)
	branchR = regexp.MustCompile(`^\s+[0-9a-f]+:\s+(bl|b)\s+0x([0-9a-f]+)\s+<(.*)>$`)
)

func parseFuncs(t *testing.T) []elfFunc {
	t.Helper()
	sections := map[string]string{}
	for _, line := range strings.Split(run(t, "readelf", "-SW", soPath(t)), "\n") {
		if m := secRe.FindStringSubmatch(line); m != nil {
			sections[m[1]] = m[2]
		}
	}
	var fs []elfFunc
	for _, line := range strings.Split(run(t, "readelf", "-sW", soPath(t)), "\n") {
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
		t.Fatalf("no function symbols parsed from readelf -s")
	}
	return fs
}

func TestHookFramesCarryNoPersonality(t *testing.T) {
	requireArtifact(t)
	cieAug, fdeCIE := parseFrames(t)
	funcs := parseFuncs(t)
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

	// Direct call/branch edges, from one disassembly of the library.
	edges := map[uint64]map[uint64]bool{}
	var from uint64
	for _, line := range strings.Split(run(t, "llvm-objdump", "-d", "--no-show-raw-insn", soPath(t)), "\n") {
		if m := hdrRe.FindStringSubmatch(line); m != nil {
			from, _ = strconv.ParseUint(m[1], 16, 64)
			continue
		}
		m := branchR.FindStringSubmatch(line)
		if m == nil || strings.HasSuffix(m[3], "@plt") {
			continue // imports are not sentinel frames
		}
		to, _ := strconv.ParseUint(m[2], 16, 64)
		callee := containing(to)
		caller := containing(from)
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
	for _, line := range strings.Split(run(t, "readelf", "-SW", soPath(t)), "\n") {
		if m := secFull.FindStringSubmatch(line); m != nil && m[1] == "nevr_hook_records" {
			recAddr, _ = strconv.ParseUint(m[2], 16, 64)
			recSize, _ = strconv.ParseUint(m[3], 16, 64)
		}
	}
	relative := map[uint64]uint64{} // slot address -> function address
	for _, line := range strings.Split(run(t, "readelf", "-rW", soPath(t)), "\n") {
		if m := relRe.FindStringSubmatch(line); m != nil {
			off, _ := strconv.ParseUint(m[1], 16, 64)
			add, _ := strconv.ParseUint(m[2], 16, 64)
			relative[off] = add
		}
	}
	if recSize == 0 || recSize%16 != 0 {
		t.Fatalf("no nevr_hook_records section (size %d): no hook is recorded, the sensor is looking at nothing", recSize)
	}
	recordEntries := map[uint64]int{}
	var roots []*elfFunc
	for off := recAddr; off < recAddr+recSize; off += 16 {
		entry, okE := relative[off]
		handler, okH := relative[off+8]
		if !okE || !okH {
			t.Fatalf("hook record at %#x has no relocation for its entry/handler pointer", off)
		}
		recordEntries[entry]++
		for _, a := range []uint64{entry, handler} {
			f := byAddr[a]
			if f == nil {
				t.Errorf("hook record at %#x points at %#x, which is not a function symbol", off, a)
				continue
			}
			roots = append(roots, f)
		}
	}
	// Exactly one record per thunk Entry, and no record for anything else.
	thunkEntries := map[uint64]bool{}
	for i := range funcs {
		if strings.Contains(funcs[i].name, "CallbackThunk") && strings.Contains(funcs[i].name, "5EntryE") {
			thunkEntries[funcs[i].addr] = true
		}
	}
	if len(thunkEntries) == 0 {
		t.Fatalf("no CallbackThunk Entry in the library: the sensor is looking at nothing")
	}
	for a := range thunkEntries {
		if recordEntries[a] != 1 {
			t.Errorf("thunk entry %s has %d hook records, want exactly 1 (define the hook with NEVR_HOOK_RECORD)", byAddr[a].name, recordEntries[a])
		}
	}
	for a, n := range recordEntries {
		if !thunkEntries[a] {
			t.Errorf("hook record names %#x (x%d), which is not a CallbackThunk Entry", a, n)
		}
	}

	reached := map[uint64]bool{}
	var queue []uint64
	for _, r := range roots {
		reached[r.addr] = true
		queue = append(queue, r.addr)
	}
	for len(queue) > 0 {
		a := queue[0]
		queue = queue[1:]
		for callee := range edges[a] {
			if !reached[callee] {
				reached[callee] = true
				queue = append(queue, callee)
			}
		}
	}

	checked := 0
	for addr := range reached {
		f := byAddr[addr]
		if f == nil {
			continue
		}
		cie, ok := fdeCIE[addr]
		if !ok {
			t.Errorf("%s at %#x has no FDE", f.name, addr)
			continue
		}
		checked++
		if aug := cieAug[cie]; aug != `"zR"` {
			t.Errorf("function %s (reachable from a hook entry or handler) sits under CIE augmentation %s, want \"zR\" (no personality, no LSDA)", f.name, aug)
		}
	}
	if checked < len(roots) {
		t.Errorf("checked %d functions for %d roots: the walk lost its roots", checked, len(roots))
	}
	t.Logf("hooks=%d (roots=%d), reachable sentinel functions checked=%d", len(recordEntries), len(roots), checked)
}

// The backend library is built with -fno-exceptions, exactly like the host test build, so the
// host tests exercise the same code generation and the backend's frames carry no personality
// either.
func TestBackendBuiltWithoutExceptions(t *testing.T) {
	requireArtifact(t)
	cieAug, fdeCIE := parseFrames(t)
	want := map[string]bool{"7GotHook7Install": false, "ResolveSlot": false, "9LogFields": false, "13StartReporter": false, "ReporterMain": false}
	backend := []string{"7GotHook", "ResolveSlot", "9LogFields", "8LogEvent", "9HexString", "10SetLogSink",
		"13StartReporter", "20RegisterReportCounter", "12StopReporter", "ReporterMain"}
	for _, f := range parseFuncs(t) {
		match := false
		for _, b := range backend {
			if strings.Contains(f.name, b) {
				match = true
			}
		}
		if !match {
			continue
		}
		for k := range want {
			if strings.Contains(f.name, k) {
				want[k] = true
			}
		}
		cie, ok := fdeCIE[f.addr]
		if !ok {
			t.Errorf("%s has no FDE", f.name)
			continue
		}
		if aug := cieAug[cie]; aug != `"zR"` {
			t.Errorf("backend function %s sits under CIE augmentation %s, want \"zR\"", f.name, aug)
		}
	}
	for k, seen := range want {
		if !seen {
			t.Errorf("backend function matching %q not found: the test is looking at nothing", k)
		}
	}
}

// The raw GotHook::Install (any function pointer) is private. Its test access class may be
// named only in got_hook.h (the friend declaration) and under src/quest/tests; production code
// installs through InstallThunk, which keeps every hook a recorded, walked thunk entry. The
// check is textual and cheap: it scans every C++ source/header extension, and it also fails if
// any file outside a tests/ directory includes a header that lives in src/quest/tests (so a
// wrapper placed under tests/ cannot be pulled into production). It is not bypass-proof (macro token pasting defeats
// it); it exists to catch an honest mistake (callback_thunk.h, "Limits").
var includeRe = regexp.MustCompile(`#\s*include\s*[<"]([^>"]+)[>"]`)

func TestRawInstallOnlyInTests(t *testing.T) {
	root, err := filepath.Abs("../../src")
	if err != nil {
		t.Fatal(err)
	}
	exts := map[string]bool{".cpp": true, ".cc": true, ".cxx": true, ".h": true, ".hpp": true, ".inc": true}
	testDir := filepath.Join(root, "quest", "tests")
	testBase := map[string]bool{}
	entries, err := os.ReadDir(testDir)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		testBase[e.Name()] = true
	}
	seen := 0
	err = filepath.Walk(root, func(path string, info os.FileInfo, werr error) error {
		if werr != nil || info.IsDir() || !exts[filepath.Ext(path)] {
			return werr
		}
		data, rerr := os.ReadFile(path)
		if rerr != nil {
			return rerr
		}
		rel, _ := filepath.Rel(root, path)
		inTests := strings.HasPrefix(rel, "quest/tests/")
		if strings.Contains(string(data), "GotHookTestAccess") {
			seen++
			if rel != "quest/sentinel/got_hook.h" && !inTests {
				t.Errorf("%s names GotHookTestAccess; only got_hook.h and src/quest/tests may", rel)
			}
		}
		// Other tests (src/runtime/tests, ...) may include the shared test vectors; production may not.
		if !strings.Contains("/"+rel, "/tests/") {
			for _, m := range includeRe.FindAllStringSubmatch(string(data), -1) {
				inc := m[1]
				if strings.HasPrefix(inc, "quest/tests/") || testBase[filepath.Base(inc)] && !strings.Contains(inc, "/sentinel/") {
					t.Errorf("%s includes %s from src/quest/tests; production code must not", rel, inc)
				}
			}
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if seen < 2 {
		t.Errorf("GotHookTestAccess found in %d files, want at least got_hook.h and the host test", seen)
	}
}

// The hook backend is compiled once. Every Quest target links the one static
// library; a second compile would give that copy its own write lock and slot
// registry. Counted from the configured build graph, so a GLOB, an included
// .cmake file or a computed path cannot hide a second copy.
func TestBackendCompiledOnce(t *testing.T) {
	requireArtifact(t)
	p, err := filepath.Abs("../../build/android-arm64/build.ninja")
	if err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(p)
	if err != nil {
		t.Fatalf("cannot read the configured build graph %s (run `just build-android`): %v", p, err)
	}
	var rules []string
	for _, line := range strings.Split(string(data), "\n") {
		if !strings.HasPrefix(line, "build ") {
			continue
		}
		out := strings.TrimPrefix(line, "build ")
		if i := strings.Index(out, ": "); i >= 0 {
			out = out[:i]
		}
		if strings.HasSuffix(strings.Fields(out + " x")[0], "got_hook.cpp.o") {
			rules = append(rules, out)
		}
	}
	if len(rules) != 1 {
		t.Errorf("got_hook.cpp is compiled %d times in the Quest build, want 1 (link nevr_quest_got_hook instead): %v", len(rules), rules)
	}
}

// callback_thunk.h's exception contract depends on the sentinel having its own
// C++ runtime while libr15.so uses libc++_shared.so. If the sentinel starts
// linking libc++_shared.so, that contract (and docs/adr/0003) must be revisited.
func TestStlContract(t *testing.T) {
	requireArtifact(t)
	dyn := run(t, "readelf", "-d", soPath(t))
	if strings.Contains(dyn, "libc++_shared.so") {
		t.Errorf("sentinel NEEDs libc++_shared.so; revisit the Exceptions section of callback_thunk.h and ADR 0003\n%s", dyn)
	}
	exported := run(t, "nm", "-D", "--defined-only", soPath(t))
	for _, sym := range []string{"__cxa_throw", "__cxa_begin_catch", "__gxx_personality_v0"} {
		if strings.Contains(exported, sym) {
			t.Errorf("C++ runtime symbol %s is exported by the sentinel", sym)
		}
	}
}

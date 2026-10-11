// Ground-truth tests for the Quest crash-reporter .so.
//
// On-device runtime needs a headset, so the automatable ground truth is the
// ELF shape of the built artifact. Each test traces to one of the acceptance criteria numbered
// BAC-1..5 in docs/design/2026-07-13-quest-crash-reporter-injection.md or to the hook contract in
// docs/adr/0003-quest-networking-port.md, and shells to
// readelf/nm on the real output (success derives from the artifact, not a
// proxy). The test FAILS (not skips) when the .so is absent — build it first:
//
//	just build-android
package quest

import (
	"os"
	"os/exec"
	"path"
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
	art := loadFrameGraph(t, soPath(t))
	byAddr := map[uint64]*elfFunc{}
	for i := range art.funcs {
		byAddr[art.funcs[i].addr] = &art.funcs[i]
	}
	// Exactly one record per thunk Entry, and no record for anything else.
	thunkEntries := map[uint64]bool{}
	for i := range art.funcs {
		if strings.Contains(art.funcs[i].name, "CallbackThunk") && strings.Contains(art.funcs[i].name, "5EntryE") {
			thunkEntries[art.funcs[i].addr] = true
		}
	}
	if len(thunkEntries) == 0 {
		t.Fatalf("no CallbackThunk Entry in the library: the sensor is looking at nothing")
	}
	for a := range thunkEntries {
		if art.recordEntries[a] != 1 {
			t.Errorf("thunk entry %s has %d hook records, want exactly 1 (define the hook with NEVR_HOOK_RECORD)", byAddr[a].name, art.recordEntries[a])
		}
	}
	for a, n := range art.recordEntries {
		if !thunkEntries[a] {
			t.Errorf("hook record names %#x (x%d), which is not a CallbackThunk Entry", a, n)
		}
	}

	v := walkHookFrames(art.graph)
	for _, msg := range v.violations {
		t.Error(msg)
	}
	if v.checked < len(art.graph.roots) {
		t.Errorf("checked %d functions for %d roots: the walk lost its roots", v.checked, len(art.graph.roots))
	}
	// The annotated set the sentinel is allowed to have, pinned so that dropping an annotation fails here
	// (the function then carries a personality and is reported above) and adding one is a reviewed change.
	want := map[string]*regexp.Regexp{
		"login compose phase":         regexp.MustCompile(`^_ZN16nevr_quest_login11ComposePlanE`),
		"post-load installs (dlopen)": regexp.MustCompile(`^_ZN10nevr_quest11integration11AfterDlopenE`),
	}
	for what, re := range want {
		if !anyMatches(v.annotatedReached, re) {
			t.Errorf("annotated function for the %s was not reached from a hook: the annotation or the call edge is gone (reached: %v)", what, v.annotatedReached)
		}
	}
	for _, n := range v.annotatedReached {
		ok := false
		for _, re := range want {
			ok = ok || re.MatchString(n)
		}
		if !ok {
			t.Errorf("unreviewed NEVR_OUTSIDE_GAME_CALL function reached from a hook: %s (add it to this list in the change that annotates it)", n)
		}
	}
	// The annotation section must hold only what the sensor lists: an annotated function the walk never
	// reaches is a stale annotation or a lost edge.
	for addr := range art.graph.annotated {
		found := false
		for _, n := range v.annotatedReached {
			found = found || n == art.graph.names[addr]
		}
		if !found {
			t.Logf("annotated function not reached by a direct edge from any hook (called indirectly, or stale): %s", art.graph.names[addr])
		}
	}
	t.Logf("hooks=%d (roots=%d), reachable sentinel functions checked=%d, annotated outside-game-call functions reached=%v",
		len(art.recordEntries), len(art.graph.roots), v.checked, v.annotatedReached)
}

// The backend library is built with -fno-exceptions, exactly like the host test build, so the
// host tests exercise the same code generation and the backend's frames carry no personality
// either.
//
// The backend is the set of functions the backend archive defines (`nm` on libnevr_quest_got_hook.a,
// types T and t: the archive is what this target builds, so a function added to it is covered
// without editing a list, and const members and file-static functions are included). A name pattern
// cannot take its place: a prefix like `_ZN8sentinel` also matches the functions other targets put in
// namespace sentinel, which are built with exceptions.
const backendArchiveRelPath = "../../build/android-arm64/sentinel/libnevr_quest_got_hook.a"

var archiveSymRe = regexp.MustCompile(`^(?:[0-9a-f]+\s+)?([Tt])\s+(_Z\S+)\s*$`)

func backendFunctionNames(t *testing.T) map[string]bool {
	t.Helper()
	archive, err := filepath.Abs(backendArchiveRelPath)
	if err != nil {
		t.Fatalf("resolve path: %v", err)
	}
	names := map[string]bool{}
	for _, line := range strings.Split(run(t, "nm", "--defined-only", archive), "\n") {
		if m := archiveSymRe.FindStringSubmatch(line); m != nil {
			names[m[2]] = true
		}
	}
	if len(names) < 10 {
		t.Fatalf("found only %d function symbols in %s: the list is looking at nothing", len(names), archive)
	}
	return names
}

func TestBackendBuiltWithoutExceptions(t *testing.T) {
	requireArtifact(t)
	cieAug, fdeCIE := parseFrames(t)
	backend := backendFunctionNames(t)
	// Functions that must be in the shipped .so, by mangled prefix, or the check is looking at nothing.
	want := map[string]bool{"_ZN8sentinel7GotHook7Install": false, "_ZN8sentinel11ResolveSlot": false,
		"_ZN8sentinel9LogFields": false, "_ZN8sentinel13StartReporter": false,
		"_ZN8sentinel12_GLOBAL__N_112ReporterMain": false,
		"_ZN8sentinel21RegisterReportCounter":      false}
	checked := 0
	for _, f := range parseFuncs(t) {
		if !backend[f.name] {
			continue
		}
		checked++
		for k := range want {
			if strings.HasPrefix(f.name, k) {
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
			t.Errorf("backend function with prefix %q not found: the test is looking at nothing", k)
		}
	}
	t.Logf("backend functions in the archive=%d, present in the .so and checked=%d", len(backend), checked)
}

// The raw GotHook::Install (any function pointer) is private. Its test access class may be
// named only in got_hook.h (the friend declaration) and under src/quest/tests; production code
// installs through InstallThunk, which keeps every hook a recorded, walked thunk entry. The
// check is textual and cheap. It reads every file with a .cpp, .cc, .cxx, .h, .hpp or .inc
// extension under src/, and it fails if a file outside a tests/ directory
//   - names GotHookTestAccess, or
//   - includes a header that lives anywhere under src/quest/tests (recursively): the include is
//     resolved the ways a compiler would find it here (relative to the including file, relative
//     to src/ because of -Isrc, and relative to src/quest/sentinel because of the target's own
//     include directory), each cleaned with path.Clean, before the prefix test.
//
// It is not bypass-proof (macro token pasting defeats it); it exists to catch an honest mistake
// (callback_thunk.h, "Limits").
var includeRe = regexp.MustCompile(`#\s*include(?:_next)?\s*[<"]([^>"]+)[>"]`)

func TestRawInstallOnlyInTests(t *testing.T) {
	root, err := filepath.Abs("../../src")
	if err != nil {
		t.Fatal(err)
	}
	exts := map[string]bool{".cpp": true, ".cc": true, ".cxx": true, ".h": true, ".hpp": true, ".inc": true}
	testFiles := map[string]bool{} // slash paths relative to src/, everything under quest/tests
	err = filepath.Walk(filepath.Join(root, "quest", "tests"), func(p string, info os.FileInfo, werr error) error {
		if werr != nil || info.IsDir() {
			return werr
		}
		rel, _ := filepath.Rel(root, p)
		testFiles[filepath.ToSlash(rel)] = true
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if len(testFiles) < 5 {
		t.Fatalf("found only %d files under src/quest/tests: the walk is looking at nothing", len(testFiles))
	}
	seen := 0
	err = filepath.Walk(root, func(p string, info os.FileInfo, werr error) error {
		if werr == nil && info.Mode()&os.ModeSymlink != 0 {
			// filepath.Walk does not follow a symlink, so a link under src/ would hide the files behind it
			// from this scan. None exists; one appearing is an error to be looked at, not a gap.
			t.Errorf("%s is a symlink under src/: the scan does not follow it", p)
			return nil
		}
		if werr != nil || info.IsDir() || !exts[filepath.Ext(p)] {
			return werr
		}
		data, rerr := os.ReadFile(p)
		if rerr != nil {
			return rerr
		}
		relOS, _ := filepath.Rel(root, p)
		rel := filepath.ToSlash(relOS)
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
				for _, cand := range []string{
					path.Join(path.Dir(rel), m[1]),
					path.Clean(m[1]),
					path.Join("quest/sentinel", m[1]),
				} {
					if testFiles[cand] {
						t.Errorf("%s includes %s, which resolves to %s under src/quest/tests; production code must not", rel, m[1], cand)
						break
					}
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

// heavyLink matches what must not reach the sentinel unguarded: token auth, libcurl, OpenSSL.
var heavyLink = regexp.MustCompile(`libnevr_quest_token_auth\.a|lib(ssl|crypto|curl)\.a|(^|\s)-l(ssl|crypto|curl)(\s|$)`)

// sentinelLinkViolations reads a build.ninja and judges the sentinel's link statement: when it
// carries token auth, libcurl or OpenSSL it must also carry -Wl,--exclude-libs,ALL, because the
// game's own libraries export an older OpenSSL/libcurl and have the sentinel as DT_NEEDED.
func sentinelLinkViolations(ninja string) []string {
	lines := strings.Split(ninja, "\n")
	start := -1
	for i, l := range lines {
		if strings.HasPrefix(l, "build sentinel/libovrplatformloader.so:") {
			start = i
			break
		}
	}
	if start < 0 {
		return []string{"no link statement for sentinel/libovrplatformloader.so in build.ninja"}
	}
	block := lines[start]
	for _, l := range lines[start+1:] {
		if !strings.HasPrefix(l, "  ") {
			break
		}
		block += "\n" + l
	}
	var v []string
	if heavyLink.MatchString(block) && !strings.Contains(block, "--exclude-libs,ALL") {
		v = append(v, "the sentinel link line carries token auth/libcurl/OpenSSL without -Wl,--exclude-libs,ALL")
	}
	return v
}

// The real, configured link line of the built sentinel.
// The sentinel's DT_NEEDED list is exactly the forward-to-original plus bionic. Everything else it uses at
// run time (libvulkan, libEGL, libGLESv3, libandroid, libvrapi, libopenxr_loader for the hardware dump, #335)
// is reached with dlopen/dlsym, so a stray -l link shows up here instead of as a new load-time dependency of
// the game.
func TestSentinelNeededList(t *testing.T) {
	requireArtifact(t)
	dyn := run(t, "readelf", "-d", soPath(t))
	var got []string
	for _, line := range strings.Split(dyn, "\n") {
		if !strings.Contains(line, "(NEEDED)") {
			continue
		}
		if i, j := strings.Index(line, "["), strings.LastIndex(line, "]"); i >= 0 && j > i {
			got = append(got, line[i+1:j])
		}
	}
	want := map[string]bool{"libovrplatformloader_orig.so": true, "liblog.so": true, "libdl.so": true, "libm.so": true, "libc.so": true}
	for _, lib := range got {
		if !want[lib] {
			t.Errorf("sentinel NEEDs %s; reach it with dlopen/dlsym instead (DT_NEEDED: %v)", lib, got)
		}
	}
	if len(got) == 0 {
		t.Errorf("no DT_NEEDED entries read from readelf -d: the check is blind\n%s", dyn)
	}
}

func TestSentinelLinkLineGuard(t *testing.T) {
	requireArtifact(t)
	p, err := filepath.Abs("../../build/android-arm64/build.ninja")
	if err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(p)
	if err != nil {
		t.Fatalf("cannot read the configured build graph %s (run `just build-android`): %v", p, err)
	}
	for _, v := range sentinelLinkViolations(string(data)) {
		t.Error(v)
	}
}

// The guard itself, on synthetic link statements.
func TestSentinelLinkLineGuardJudgesLinkStatements(t *testing.T) {
	stmt := func(libs, flags string) string {
		return "build sentinel/libovrplatformloader.so: LINK a.o b.o\n  LINK_FLAGS = -shared " + flags +
			"\n  LINK_LIBRARIES = " + libs + "\n  OBJECT_DIR = x\nbuild other: PHONY\n"
	}
	const guard = "-Wl,--exclude-libs,ALL"
	cases := []struct {
		name  string
		ninja string
		bad   bool
	}{
		{"plain sentinel", stmt("sentinel/libnevr_quest_got_hook.a -llog -ldl", guard), false},
		{"plain sentinel without the flag", stmt("sentinel/libnevr_quest_got_hook.a -llog", ""), false},
		{"token auth with the flag", stmt("libnevr_quest_token_auth.a vcpkg/lib/libssl.a", guard), false},
		{"token auth without the flag", stmt("libnevr_quest_token_auth.a -llog", ""), true},
		{"libssl by path without the flag", stmt("/x/vcpkg_installed/arm64-android/lib/libssl.a -llog", ""), true},
		{"libcrypto without the flag", stmt("sentinel/libcrypto.a", ""), true},
		{"libcurl without the flag", stmt("vcpkg/lib/libcurl.a", ""), true},
		{"-lcurl without the flag", stmt("-lcurl -llog", ""), true},
		{"the flag on another target only", stmt("libnevr_quest_token_auth.a", "") +
			"build other.so: LINK c.o\n  LINK_FLAGS = " + guard + "\n", true},
		{"no sentinel statement at all", "build other.so: LINK c.o\n", true},
	}
	for _, c := range cases {
		got := sentinelLinkViolations(c.ninja)
		if (len(got) > 0) != c.bad {
			t.Errorf("%s: violations=%v, want bad=%v", c.name, got, c.bad)
		}
	}
}

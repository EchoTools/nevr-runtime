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

// The frames a game call passes through on its way into the hook (the thunk's
// Entry and its members, and the handler) must carry no personality routine and
// no LSDA: they sit under the "zR" CIE. A "zPLR" CIE names the sentinel's own
// personality, and a game exception unwinding through such a frame would hand
// libc++_shared's _Unwind_Context to it (callback_thunk.h, "Exceptions").
func TestHookFramesCarryNoPersonality(t *testing.T) {
	requireArtifact(t)
	frames := run(t, "readelf", "--debug-dump=frames", soPath(t))
	cieAug := map[string]string{} // CIE offset -> augmentation
	fdeCIE := map[uint64]string{} // FDE start pc -> CIE offset
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

	hookFrames := 0
	var sawEntry, sawHandler bool
	for _, line := range strings.Split(run(t, "nm", "-S", "--defined-only", soPath(t)), "\n") {
		f := strings.Fields(line)
		if len(f) < 4 {
			continue
		}
		name := f[3]
		if !strings.ContainsAny(f[2], "tTwW") { // code symbols only; the thunk's data members have no frame
			continue
		}
		if !strings.Contains(name, "CallbackThunk") && !strings.Contains(name, "HookedClockGettime") {
			continue
		}
		addr, err := strconv.ParseUint(f[0], 16, 64)
		if err != nil {
			continue
		}
		cie, ok := fdeCIE[addr]
		if !ok {
			t.Errorf("%s at %#x has no FDE", name, addr)
			continue
		}
		hookFrames++
		sawEntry = sawEntry || strings.Contains(name, "5EntryE")
		sawHandler = sawHandler || strings.Contains(name, "HookedClockGettime")
		if aug := cieAug[cie]; aug != `"zR"` {
			t.Errorf("hook frame %s sits under CIE augmentation %s, want \"zR\" (no personality, no LSDA)", name, aug)
		}
	}
	if hookFrames == 0 || !sawEntry || !sawHandler {
		t.Errorf("hook frames not found (frames=%d entry=%v handler=%v): the test is looking at nothing", hookFrames, sawEntry, sawHandler)
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
		t.Errorf("sentinel NEEDs libc++_shared.so; revisit callback_thunk.h 'Two C++ runtimes' and ADR 0003\n%s", dyn)
	}
	exported := run(t, "nm", "-D", "--defined-only", soPath(t))
	for _, sym := range []string{"__cxa_throw", "__cxa_begin_catch", "__gxx_personality_v0"} {
		if strings.Contains(exported, sym) {
			t.Errorf("C++ runtime symbol %s is exported by the sentinel", sym)
		}
	}
}

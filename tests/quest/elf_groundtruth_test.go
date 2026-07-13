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
	"os/exec"
	"path/filepath"
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

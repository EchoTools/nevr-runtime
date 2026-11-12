package updatewiring

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

func root(t *testing.T) string {
	t.Helper()
	wd, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	r := filepath.Join(wd, "..", "..", "..")
	if _, err := os.Stat(filepath.Join(r, "tools", "go.mod")); err != nil {
		t.Fatalf("%s is not the repository root: %v", r, err)
	}
	return r
}

func read(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(filepath.Join(root(t), path))
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

var blockComment = regexp.MustCompile(`(?s)/\*.*?\*/`)
var lineComment = regexp.MustCompile(`//[^\n]*`)

func code(t *testing.T, path string) string {
	t.Helper()
	return lineComment.ReplaceAllString(blockComment.ReplaceAllString(read(t, path), ""), "")
}

// body returns the text of the braced function whose signature starts with sig, comments stripped.
func body(t *testing.T, path, sig string) string {
	t.Helper()
	src := code(t, path)
	at := strings.Index(src, sig)
	if at < 0 {
		t.Fatalf("%s: no %q", path, sig)
	}
	open := strings.Index(src[at:], "{")
	depth := 0
	for i := at + open; i < len(src); i++ {
		switch src[i] {
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return src[at : i+1]
			}
		}
	}
	t.Fatalf("%s: unbalanced %q", path, sig)
	return ""
}

func before(t *testing.T, s, a, b, why string) {
	t.Helper()
	ia, ib := strings.Index(s, a), strings.Index(s, b)
	if ia < 0 || ib < 0 || ia >= ib {
		t.Errorf("want %q before %q (at %d and %d): %s", a, b, ia, ib, why)
	}
}

func mustMatch(t *testing.T, re, s, what string) {
	t.Helper()
	if !regexp.MustCompile(re).MatchString(s) {
		t.Errorf("%s: no match for %s", what, re)
	}
}

// ---- ADR 0024, "Tests, failing first", item 5: nothing is written outside the nevr root ----

var jobFiles = []string{
	"src/runtime/lifecycle/update_run.cpp",
	"src/runtime/lifecycle/update_worker.cpp",
	"src/runtime/lifecycle/update_policy.cpp",
}

func TestTheJobsNameNoFolderOfTheirOwn(t *testing.T) {
	for _, f := range jobFiles {
		src := code(t, f)
		for _, source := range []string{"LOCALAPPDATA", "GetModuleFileName", "GetCurrentDirectory", "SHGetKnownFolderPath", "GetEnvironmentVariable",
			"ExpandEnvironmentStrings", "GetTempPath", "GetWindowsDirectory", "echovr", "Program Files"} {
			if strings.Contains(src, source) {
				t.Errorf("%s reaches a folder that is not the root it was given (%s)", f, source)
			}
		}
	}
}

func TestEveryPathInTheJobsIsComposedFromTheRoot(t *testing.T) {
	src := code(t, "src/runtime/lifecycle/update_run.cpp")
	if !strings.Contains(src, `std::wstring UpdateDir(const std::wstring& root) { return root + L"\\" + names::kUpdateDir; }`) {
		t.Error("update_run.cpp: UpdateDir is no longer the root plus the update folder's name")
	}
	allowed := map[string]bool{`\\`: true, `\\*`: true, `.`: true, `..`: true, `.tmp-`: true}
	for _, m := range regexp.MustCompile(`L"([^"]*)"`).FindAllStringSubmatch(src, -1) {
		if !allowed[m[1]] {
			t.Errorf("update_run.cpp: a path literal that is not the root's: L%q", m[1])
		}
	}
}

func TestTheOneSourceOfTheRootIsTheNevrRoot(t *testing.T) {
	src := code(t, "src/runtime/lifecycle/update_process.cpp")
	if n := strings.Count(src, "nevr_root::RootW()"); n != 1 {
		t.Errorf("update_process.cpp reads the nevr root %d times, want 1", n)
	}
	mustMatch(t, `deps\.root\s*=\s*nevr_root::RootW\(\);`, src, "update_process.cpp")
	if !strings.Contains(src, "deps.switches.no_root = deps.root.empty();") {
		t.Error("update_process.cpp: no %LOCALAPPDATA% must stop the worker (switches.no_root)")
	}
	if !strings.Contains(code(t, "src/runtime/lifecycle/update_worker.cpp"), "if (s.deps.root.empty()) switches.no_root = true;") {
		t.Error("update_worker.cpp: an empty root must stop the worker even when the caller forgot to say so")
	}
}

func TestTheCoreIsGivenTheNevrRootAndNothingElse(t *testing.T) {
	src := code(t, "src/runtime/lifecycle/update_run.cpp")
	mustMatch(t, `json config = \{\{"platform", "pc"\}, \{"nevr_root", root_utf8\}\};`, src, "update_run.cpp")
	if regexp.MustCompile(`config\["(root|log|work|state)"\]`).MatchString(src) {
		t.Error("update_run.cpp sets one of the core's other folders; they are derived from nevr_root")
	}
}

// ---- where the worker starts and what feeds its mark ----

func TestTheWorkerStartsAtTheEndOfTheDeferredBootstrap(t *testing.T) {
	b := body(t, "src/runtime/lifecycle/boot.cpp", "void RunDeferredRuntimeBootstrap(")
	if n := strings.Count(b, "nevr_update::StartProcessWorker("); n != 1 {
		t.Fatalf("StartProcessWorker is called %d times in RunDeferredRuntimeBootstrap, want 1", n)
	}
	mustMatch(t, `nevr_update::StartProcessWorker\(\s*nevr_runtime_entry::HostCanSwap\(\)\s*,\s*g_noUpdate\s*!=\s*FALSE\s*,\s*g_isServer\s*!=\s*FALSE\s*\)\s*;`, b, "boot.cpp")
	before(t, b, `"[NEVR.BOOT] runtime bootstrap complete`, "nevr_update::StartProcessWorker(", "N146: the worker starts after the boot-state limit, not before it")
	before(t, b, "g_noUpdate = TRUE", "nevr_update::StartProcessWorker(", "the flag is read before the worker decides")
}

func TestTheWorkerNeverStartsFromDllMainInitializeOrTheHost(t *testing.T) {
	for _, f := range []string{"src/runtime/lifecycle/dllmain.cpp", "src/runtime/lifecycle/initialize.cpp", "src/runtime/lifecycle/runtime_entry.cpp",
		"src/runtime/host/host_entry.cpp", "src/runtime/host/runtime_rendezvous.cpp"} {
		if strings.Contains(code(t, f), "StartProcessWorker") {
			t.Errorf("%s starts the worker: not before the loader lock is released and the boot-state limit is passed", f)
		}
	}
}

func TestTheHostCapabilityIsReadFromTheContextTheHostPassed(t *testing.T) {
	src := code(t, "src/runtime/lifecycle/runtime_entry.cpp")
	mustMatch(t, `g_hostCanSwap\.store\(\s*nevr_runtime_abi::HostCanSwap\(host\)`, src, "runtime_entry.cpp")
	before(t, src, "ValidateHostContext(host)", "g_hostCanSwap.store(", "the capability is read from a context that validated")
}

func TestTheOneDispatcherFeedsTheMark(t *testing.T) {
	b := body(t, "src/runtime/frame/tick.cpp", "void DispatchPerFrameWork(")
	if n := strings.Count(b, "nevr_update::OnFrame(nowUs);"); n != 1 {
		t.Errorf("DispatchPerFrameWork calls OnFrame %d times, want 1", n)
	}
	before(t, b, "if (nowUs - g_lastTickUs < kTickIntervalUs) return;", "nevr_update::OnFrame(nowUs);", "the mark counts dispatches, not the clock's reads")
	if n := len(regexp.MustCompile(`nevr_update::OnFrame\(`).FindAllString(code(t, "src/runtime/frame/tick.cpp"), -1)); n != 1 {
		t.Errorf("tick.cpp has %d OnFrame call sites, want 1 (N86: one dispatcher)", n)
	}
}

func TestTheFlagIsRegisteredParsedAndDocumented(t *testing.T) {
	if !strings.Contains(read(t, "src/core/cli_flags.h"), `{L"-no-update", nullptr}`) {
		t.Error("cli_flags.h: -no-update is not in kFlags")
	}
	if !strings.Contains(read(t, "src/runtime/lifecycle/cli.cpp"), `AddArgSyntax(pArgSyntax, "-no-update", 0, 0, FALSE)`) {
		t.Error("cli.cpp: the game's parser does not accept -no-update")
	}
	mustMatch(t, `lstrcmpW\(flag, L"-no-update"\) == 0\)\s*\{\s*g_noUpdate = TRUE;`, code(t, "src/runtime/lifecycle/boot.cpp"), "boot.cpp")
	if !strings.Contains(read(t, "README.md"), "| `-no-update` |") {
		t.Error("README.md: -no-update is not in the flag table")
	}
}

func TestTheTwoConfigKeysAreReadThroughTheAccessorsAndDocumented(t *testing.T) {
	cfg := code(t, "src/runtime/lifecycle/service_config.cpp")
	for _, call := range []string{`NevrCfg().GetBool("update.enabled")`, `NevrCfg().GetString("update.channel")`} {
		if !strings.Contains(cfg, call) {
			t.Errorf("service_config.cpp: no %s", call)
		}
	}
	mustMatch(t, `(?m)^# update:\n#   enabled: true\n#   channel: beta$`, read(t, "docs/reference/example-config.yaml"), "example-config.yaml")
}

// ---- the confirmation outlives the off switches ----

func TestOnlyADevBuildAndAHostThatCannotSwapSkipTheConfirmation(t *testing.T) {
	b := body(t, "src/runtime/lifecycle/update_worker.cpp", "Disabled Worker::Start()")
	mustMatch(t, `const bool can_confirm = s\.decision != Disabled::kDevBuild && s\.decision != Disabled::kNoRoot && s\.decision != Disabled::kHostCannotSwap;`, b, "update_worker.cpp")
	for _, off := range []string{"kFlag", "kConfig"} {
		if strings.Contains(b, off) {
			t.Errorf("Worker::Start names %s: a flag or a config key must not stop the confirmation (a swapped release would be rolled back)", off)
		}
	}
}

func TestTheDecisionOrderIsDevFlagConfigHost(t *testing.T) {
	b := body(t, "src/runtime/lifecycle/update_policy.cpp", "Disabled Decide(")
	last := -1
	for _, w := range []string{"s.dev_build", "s.no_root", "s.flag_no_update", "s.config_enabled", "s.host_can_swap"} {
		at := strings.Index(b, w)
		if at < 0 || at < last {
			t.Fatalf("Decide: %s is missing or out of the logged order", w)
		}
		last = at
	}
}

func TestTheProcessWorkerIsNeverDestroyed(t *testing.T) {
	src := code(t, "src/runtime/lifecycle/update_process.cpp")
	if !strings.Contains(src, "new Worker(") {
		t.Error("update_process.cpp: the process worker is no longer leaked on purpose")
	}
	if strings.Contains(src, "unique_ptr<Worker>") {
		t.Error("update_process.cpp owns the worker with a unique_ptr: its destructor at DLL_PROCESS_DETACH would join threads under the loader lock")
	}
}

// ---- the core is linked and measured ----

func TestTheRuntimeLinksTheCoreAndTheUnitRecipeRunsTheTests(t *testing.T) {
	cmake := regexp.MustCompile(`\s+`).ReplaceAllString(read(t, "src/runtime/CMakeLists.txt"), " ")
	for _, want := range []string{
		"target_link_libraries(nevr_runtime PRIVATE nevr_install)",
		"NEVR_UPDATE_CORE_LINKED=1",
		"build --locked --release --target ${NEVR_INSTALL_TARGET} --lib",
		`set(NEVR_INSTALL_LIB "" CACHE FILEPATH`,
	} {
		if !strings.Contains(cmake, want) {
			t.Errorf("src/runtime/CMakeLists.txt: no %q", want)
		}
	}
	just := read(t, "justfile")
	for _, want := range []string{"build_targets=\"--target test_update_worker\"", "grep -qw test_update_core_link <<<\"$core_targets\"", "for name in test_update_worker test_update_core_link; do"} {
		if !strings.Contains(just, want) {
			t.Errorf("justfile: no %q", want)
		}
	}
}

// The build job has no Rust (test_build_workflow_installer_job.py); a version run links the library the core-library job built.
func TestAVersionRunLinksTheLibraryTheCoreLibraryJobBuilt(t *testing.T) {
	wf := read(t, ".github/workflows/build.yml")
	job := regexp.MustCompile(`(?s)\n  core-library:\n.*?\n  build:\n`).FindString(wf)
	if job == "" {
		t.Fatal("build.yml has no core-library job before the build job")
	}
	for _, want := range []string{"--lib --manifest-path installer/Cargo.toml", "name: core-library", "libnevr_install.a"} {
		if !strings.Contains(job, want) {
			t.Errorf("core-library job: no %q", want)
		}
	}
	build := regexp.MustCompile(`(?s)\n  build:\n.*?\n  quest:\n`).FindString(wf)
	if strings.Contains(build, "rustup-init") || strings.Contains(build, "cargo") && !strings.Contains(build, "core-library") {
		t.Error("the build job installs or calls Rust; the library must come from the core-library job")
	}
	before(t, build, "- name: Wait for the core library job", "- name: Download the core library", "download after the wait")
	before(t, build, "- name: Download the core library", "- name: Configure CMake", "the library must be there when CMake configures")
	if !strings.Contains(build, `-DNEVR_UPDATE_CORE=ON "-DNEVR_INSTALL_LIB=$GITHUB_WORKSPACE/core-library/libnevr_install.a"`) {
		t.Error("the configure step does not pass the prebuilt library with NEVR_UPDATE_CORE=ON on a version run")
	}
}

// F5 (ADR 0024): a release the verifier refused is recorded by the core, so the next launch does not fetch it again.
func TestARefusedReleaseIsRecordedByTheCore(t *testing.T) {
	run := code(t, "src/runtime/lifecycle/update_run.cpp")
	mustMatch(t, `env\.core\.discard\s*&&\s*env\.core\.discard\(root_utf8, staged\.version, runtime_sha, "verify_failed"`, run, "update_run.cpp")
	before(t, run, "env.core.discard", "run.Finish(\"unchanged\", \"verify_failed\")", "the discard is recorded while the core session is still open")
	proc := code(t, "src/runtime/lifecycle/update_process.cpp")
	mustMatch(t, `NevrPcDiscard\(\*session,`, proc, "update_process.cpp")
	mustMatch(t, `out\.refused\s*=\s*!out\.ok;`, proc, "update_process.cpp: only a verdict is a refusal")
	if !strings.Contains(read(t, "installer/include/nevr_install.h"), "int NevrPcDiscard(") {
		t.Error("nevr_install.h does not declare NevrPcDiscard")
	}
	if strings.Contains(read(t, "docs/adr/0024-pc-self-update.md"), "KNOWN"+"-WRONG") {
		t.Error("ADR 0024 marks F5 as a known-wrong state again")
	}
}

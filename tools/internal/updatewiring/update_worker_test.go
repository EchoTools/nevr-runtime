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
	"src/runtime/lifecycle/update_fs_win32.cpp",
	"src/core/update/fs_posix.cpp",
	"src/core/update/engine_check.cpp",
	"src/core/update/engine_start.cpp",
	"src/core/update/worker.cpp",
	"src/core/update/policy.cpp",
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
	if !strings.Contains(code(t, "src/core/update/engine_check.cpp"), `std::string Update() const { return Join(root, names::kUpdateDir); }`) {
		t.Error("engine_check.cpp: Paths::Update is no longer the root plus the update folder's name")
	}
	src := code(t, "src/runtime/lifecycle/update_run.cpp")
	allowed := map[string]bool{`\\`: true, `\\*`: true, `.`: true, `..`: true, `.tmp-`: true}
	for _, m := range regexp.MustCompile(`L"([^"]*)"`).FindAllStringSubmatch(src, -1) {
		if !allowed[m[1]] {
			t.Errorf("update_run.cpp: a path literal that is not the root's: L%q", m[1])
		}
	}
	// engine_check.cpp spells no path of its own: a narrow literal with a separator is a path part, and the only separators in it
	// are in #include names.
	for _, m := range regexp.MustCompile(`"([^"\n]*)"`).FindAllStringSubmatch(code(t, "src/core/update/engine_check.cpp"), -1) {
		if strings.ContainsAny(m[1], `\/`) && !strings.HasSuffix(m[1], ".h") {
			t.Errorf("engine_check.cpp: a path literal that is not the root's: %q", m[1])
		}
	}
}

// A path literal in the engine or in the PC fs is a separator, a glob, a temporary's suffix or a dot name: every other part of a
// path is the root or a name of the closed vocabulary (core/update/names.h), which the code never spells out itself.
// "failed" is left out: it is also the word of a check's action.
var vocabulary = []string{"update", "lock", "staged.json", "swap.json", "pending.json", "state.json", "crashed", "staging", "previous",
	"plugins", "nevr-runtime.dll"}

// In these files a literal with no separator is a log word, a reason or a JSON key, not a path part.
var logWordFiles = map[string]bool{
	"src/runtime/lifecycle/update_run.cpp": true,
	"src/core/update/engine_check.cpp":     true,
	"src/core/update/engine_start.cpp":     true,
}

// "crashed" is also the reason a rollback records (ADR 0024, section 6: reason=crashed), a word of a record and not the path of the
// marker, which is names.h's kCrashedFile. Only that literal of engine_start.cpp is left out of the vocabulary check.
var reasonWords = map[string]map[string]bool{
	"src/core/update/engine_start.cpp": {"crashed": true},
}

func TestNoPathPartIsSpelledOutInTheEngineOrItsPcFs(t *testing.T) {
	allowed := map[string]bool{`\\`: true, `\\*`: true, `.`: true, `..`: true, `.tmp-`: true}
	for _, f := range []string{"src/runtime/lifecycle/update_run.cpp", "src/runtime/lifecycle/update_fs_win32.cpp", "src/core/update/fs_posix.cpp",
		"src/core/update/engine_check.cpp", "src/core/update/engine_start.cpp"} {
		for _, m := range regexp.MustCompile(`L?"([^"\n]*)"`).FindAllStringSubmatch(code(t, f), -1) {
			lit := m[1]
			if !reasonWords[f][lit] {
				for _, name := range vocabulary {
					if lit == name {
						t.Errorf("%s: %q is a name of the closed vocabulary and is spelled out; use core/update/names.h", f, lit)
					}
				}
			}
			if !strings.ContainsAny(lit, `\/.`) || allowed[lit] || strings.HasSuffix(lit, ".h") { // an #include is not a path of the root
				continue
			}
			if logWordFiles[f] && !strings.ContainsAny(lit, `\/`) {
				continue // a log word, a reason or a JSON key with a dot in it is not a path
			}
			t.Errorf("%s: a literal that looks like a path part: %q", f, lit)
		}
	}
}

func TestTheOneSourceOfTheRootIsTheNevrRoot(t *testing.T) {
	src := code(t, "src/runtime/lifecycle/update_process.cpp")
	if n := strings.Count(src, "nevr_root::RootW()"); n != 1 {
		t.Errorf("update_process.cpp reads the nevr root %d times, want 1", n)
	}
	mustMatch(t, `deps\.root_unconvertible\s*=\s*!RootToUtf8\(nevr_root::RootW\(\),\s*&deps\.root\);`, src, "update_process.cpp")
	if !strings.Contains(src, "deps.switches.no_root = deps.root.empty() && !deps.root_unconvertible;") {
		t.Error("update_process.cpp: no %LOCALAPPDATA% must stop the worker (switches.no_root)")
	}
	if !strings.Contains(code(t, "src/core/update/worker.cpp"), "if (s.deps.root.empty() && !s.deps.root_unconvertible) switches.no_root = true;") {
		t.Error("update_worker.cpp: an empty root must stop the worker even when the caller forgot to say so")
	}
}

func TestTheCoreIsGivenTheNevrRootAndNothingElse(t *testing.T) {
	src := code(t, "src/core/update/engine_check.cpp")
	mustMatch(t, `json config = \{\{"platform", env\.platform\}, \{"nevr_root", env\.root\}\};`, src, "engine_check.cpp")
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

func TestTheQuestRuntimePassesTheDevSourceKeysToTheWiring(t *testing.T) {
	entry := code(t, "src/quest/runtime/runtime_entry.cpp")
	mustMatch(t, `in\.config_base = config\.update\.base;`, entry, "runtime_entry.cpp")
	mustMatch(t, `in\.config_pins = config\.update\.pins;`, entry, "runtime_entry.cpp")
	mustMatch(t, `in\.dev_build = nevr_build::kIsDev;`, entry, "runtime_entry.cpp")
}

// ---- the confirmation outlives the off switches ----

func TestOnlyADevBuildAndAHostThatCannotSwapSkipTheConfirmation(t *testing.T) {
	b := body(t, "src/core/update/worker.cpp", "Disabled Worker::Start()")
	mustMatch(t, `const bool can_confirm = s\.decision != Disabled::kDevBuild && s\.decision != Disabled::kNoRoot && s\.decision != Disabled::kHostCannotSwap;`, b, "update_worker.cpp")
	for _, off := range []string{"kFlag", "kConfig"} {
		if strings.Contains(b, off) {
			t.Errorf("Worker::Start names %s: a flag or a config key must not stop the confirmation (a swapped release would be rolled back)", off)
		}
	}
}

func TestTheDecisionOrderIsDevFlagConfigHost(t *testing.T) {
	b := body(t, "src/core/update/policy.cpp", "Disabled Decide(")
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
	for _, want := range []string{"--target test_update_worker --target test_update_fs_win32 --target test_update_core_link", "for name in test_update_worker test_update_fs_win32 test_update_core_link engine_pending_test; do"} {
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

// workflowStep returns one step of a job, from its `- name:` line to the next step.
func workflowStep(t *testing.T, job, name string) string {
	t.Helper()
	at := strings.Index(job, "    - name: "+name+"\n")
	if at < 0 {
		t.Fatalf("no step %q", name)
	}
	rest := job[at+len("    - "):]
	if end := strings.Index(rest, "\n    - "); end >= 0 {
		return rest[:end]
	}
	return rest
}

// runBlock returns the `run: |` body of a step.
func runBlock(t *testing.T, step string) string {
	t.Helper()
	at := strings.Index(step, "run: |\n")
	if at < 0 {
		t.Fatalf("step has no run block: %q", step)
	}
	return step[at+len("run: |\n"):]
}

// lineBefore fails unless a and b are each a whole line of s (indentation aside) and a comes first: a line commented out,
// or one with more commands after it (`|| true`), is not the line.
func lineBefore(t *testing.T, s, a, b, why string) {
	t.Helper()
	at := func(line string) int {
		loc := regexp.MustCompile(`(?m)^[ ]*` + regexp.QuoteMeta(line) + `$`).FindStringIndex(s)
		if loc == nil {
			return -1
		}
		return loc[0]
	}
	ia, ib := at(a), at(b)
	if ia < 0 || ib < 0 || ia >= ib {
		t.Errorf("want the line %q before the line %q (at %d and %d): %s", a, b, ia, ib, why)
	}
}

// afterName returns a step without its `name:` line: its shell, its environment and its run block.
func afterName(t *testing.T, step string) string {
	t.Helper()
	at := strings.Index(step, "\n")
	if at < 0 {
		t.Fatalf("step has one line: %q", step)
	}
	return step[at+1:]
}

const (
	pcCoreTarget    = "--target x86_64-pc-windows-gnu"
	questCoreTarget = "--target aarch64-linux-android"
	questToolchain  = "Install the Rust toolchain for the Quest core"
	questBuild      = "Build and test the Quest library"
	questAsset      = "Name and check the release asset"
	questConfigure  = "(cd src/quest && cmake --preset android-arm64 -DNEVR_UPDATE_CORE=ON)"
	questTestRecipe = `"$HOME/.local/bin/just" test-android`
)

// A tag run of build.yml publishes nevr-runtime-vX.Y.Z-quest-runtime.so with the core install method linked in, or fails (ADR 0024
// section 3; ADR 0023 section 7): the quest job installs the core-library job's pinned toolchain with the Android target, configures
// with NEVR_UPDATE_CORE=ON before test-android builds, and refuses to name the asset unless cargo wrote the core the runtime links.
func TestATagRunLinksTheCoreIntoTheQuestRuntime(t *testing.T) {
	t.Parallel()
	wf := read(t, ".github/workflows/build.yml")
	pc := regexp.MustCompile(`(?s)\n  core-library:\n.*?\n  build:\n`).FindString(wf)
	quest := regexp.MustCompile(`(?s)\n  quest:\n.*?\n  seal:\n`).FindString(wf)
	if pc == "" || quest == "" {
		t.Fatal("build.yml has no core-library job before the build job, or no quest job before the seal job")
	}

	// The same pins as the PC core: the whole step but its name is the core-library job's, with the Android target in place of
	// the Windows one, so no line of it (the shell, the homes, the digest, the toolchain, the hand-over through GITHUB_ENV) can drift.
	pcRun := afterName(t, workflowStep(t, pc, "Install the Rust toolchain for the core library"))
	if strings.Count(pcRun, pcCoreTarget) != 1 {
		t.Fatalf("the core-library job's toolchain step does not name %q exactly once", pcCoreTarget)
	}
	want := strings.Replace(pcRun, pcCoreTarget, questCoreTarget, 1)
	if got := afterName(t, workflowStep(t, quest, questToolchain)); got != want {
		t.Errorf("the quest job's toolchain step is not the core-library job's with %q:\ngot:\n%s\nwant:\n%s", questCoreTarget, got, want)
	}
	cmake := regexp.MustCompile(`\s+`).ReplaceAllString(read(t, "src/quest/sentinel/CMakeLists.txt"), " ")
	if !strings.Contains(cmake, "build --locked --release "+questCoreTarget+" --lib") {
		t.Errorf("src/quest/sentinel/CMakeLists.txt does not build the core for %q, the target the job installs", questCoreTarget)
	}
	if strings.Contains(quest, "GITHUB_PATH") {
		t.Error("the quest job edits PATH; the toolchain reaches the configure through CARGO_HOME (find_program's first HINT)")
	}
	before(t, quest, "    - name: "+questToolchain+"\n", "    - name: "+questBuild+"\n", "the toolchain must be there when the Quest build configures")

	// ON, not the default AUTO: a configure that finds no cargo stops instead of building a runtime that never updates itself.
	build := runBlock(t, workflowStep(t, quest, questBuild))
	lineBefore(t, build, questConfigure, questTestRecipe, "the ON configure must make the cache that test-android's configure keeps")
	recipe := regexp.MustCompile(`(?s)\nconfigure-android:[^\n]*\n.*?\n\n`).FindString(read(t, "justfile"))
	if !strings.Contains(recipe, "cd src/quest && ") || !strings.HasSuffix(strings.TrimSpace(recipe), "cmake --preset android-arm64") {
		t.Errorf("configure-android does not reconfigure the android-arm64 preset in src/quest, the tree the ON configure made:\n%s", recipe)
	}
	mustMatch(t, `elseif\(NEVR_UPDATE_CORE STREQUAL "ON"\) message\(FATAL_ERROR `, cmake, "ON with no cargo is a configure error")

	// The asset step refuses a runtime the core is not linked into: the core's archive, at the path CMake gives it in the
	// preset's tree, exists only when the configure linked the core.
	preset := read(t, "src/quest/CMakePresets.json")
	if !strings.Contains(preset, `"binaryDir": "${sourceDir}/../../build/android-arm64",`) {
		t.Fatal("src/quest/CMakePresets.json: the android-arm64 preset does not build in build/android-arm64")
	}
	for _, want := range []string{
		`set(NEVR_QUEST_INSTALL_DIR "${CMAKE_BINARY_DIR}/installer-lib")`,
		`set(NEVR_QUEST_INSTALL_LIB "${NEVR_QUEST_INSTALL_DIR}/aarch64-linux-android/release/libnevr_install.a")`,
	} {
		if !strings.Contains(cmake, want) {
			t.Errorf("src/quest/sentinel/CMakeLists.txt: no %q", want)
		}
	}
	asset := runBlock(t, workflowStep(t, quest, questAsset))
	check := "test -s build/android-arm64/installer-lib/aarch64-linux-android/release/libnevr_install.a"
	lineBefore(t, asset, check, `cp "$runtime" "$runtime_out"`, "the runtime is published only after the core it links is found")
}

// F5 (ADR 0024): a release the verifier refused is recorded by the core, so the next launch does not fetch it again.
func TestARefusedReleaseIsRecordedByTheCore(t *testing.T) {
	run := code(t, "src/core/update/engine_check.cpp")
	mustMatch(t, `env\.core\.discard\s*&&\s*env\.core\.discard\(env\.root, staged\.version, runtime_sha, "verify_failed"`, run, "engine_check.cpp")
	before(t, run, "env.core.discard", "run.Finish(\"unchanged\", \"verify_failed\")", "the discard is recorded while the core session is still open")
	proc := code(t, "src/core/update/core_ffi.cpp")
	mustMatch(t, `NevrPcDiscard\(\*session,`, proc, "core_ffi.cpp")
	mustMatch(t, `out\.refused\s*=\s*!out\.ok;`, code(t, "src/runtime/lifecycle/update_process.cpp"), "update_process.cpp: only a verdict is a refusal")
	if !strings.Contains(read(t, "installer/include/nevr_install.h"), "int NevrPcDiscard(") {
		t.Error("nevr_install.h does not declare NevrPcDiscard")
	}
	if strings.Contains(read(t, "docs/adr/0024-pc-self-update.md"), "KNOWN"+"-WRONG") {
		t.Error("ADR 0024 marks F5 as a known-wrong state again")
	}
}

// The Quest runtime stages its library through the same core as PC, with the quest-runtime profile (installer/src/stage.rs names the
// text; the runtime must pass the same text), and verifies each staged file against its stage entry (src/quest/runtime/update_wiring.cpp).
// Source guards: the wiring is one call in runtime_entry.cpp, so a removed line fails here, not only on a headset.
func TestTheQuestRuntimeStagesTheQuestProfileAndVerifiesTheBytes(t *testing.T) {
	profile := regexp.MustCompile(`(?m)^pub const QUEST_PROFILE: &str = r#"(.*)"#;$`).FindStringSubmatch(read(t, "installer/src/stage.rs"))
	if profile == nil {
		t.Fatal("installer/src/stage.rs: no QUEST_PROFILE constant")
	}
	mustMatch(t, regexp.QuoteMeta(`inline constexpr char kQuestStageProfile[] = R"(`+profile[1]+`)";`), read(t, "src/quest/runtime/update_wiring.h"),
		"update_wiring.h: kQuestStageProfile is not installer's QUEST_PROFILE")
	entry := read(t, "src/quest/runtime/runtime_entry.cpp")
	mustMatch(t, `in\.core = nevr_update::MakeFfiCore\(nevr_quest::update_wiring::kQuestStageProfile\);`, entry, "runtime_entry.cpp")
	mustMatch(t, `in\.verify = &nevr_quest::update_wiring::VerifyStagedQuestFile;`, entry, "runtime_entry.cpp")
	core := read(t, "src/core/update/core_ffi.cpp")
	mustMatch(t, `status = profile\.empty\(\)\s*\?\s*NevrPcStage\(`, core, "core_ffi.cpp: the PC stage is the empty profile")
	mustMatch(t, `:\s*NevrStage\(\*session, root_dir\.c_str\(\), profile\.c_str\(\)`, core, "core_ffi.cpp: a profile goes to NevrStage")
	verify := read(t, "src/quest/runtime/update_wiring.cpp")
	mustMatch(t, `O_RDONLY \| O_NOFOLLOW \| O_CLOEXEC`, verify, "update_wiring.cpp: the staged file is opened without following a link")
	mustMatch(t, `S_ISREG\(st\.st_mode\)`, verify, "update_wiring.cpp: the staged file must be a regular file")
	mustMatch(t, `size != expected\.size`, verify, "update_wiring.cpp: the size is checked against the entry")
	mustMatch(t, `sha256 != expected\.sha256`, verify, "update_wiring.cpp: the sha256 is checked against the entry")
}

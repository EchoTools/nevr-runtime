package main

import (
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/testmap"
)

// fixedRule covers each tracked file its glob matches with its tests. why says what the tests are for.
type fixedRule struct {
	glob  string
	tests []string
	why   string
}

// fixedRules are the coverage a file gets without a build tree or a reference: the build inputs of CMake, the suites that
// live under tests/ and installer/, and the files a named check or test reads.
var fixedRules = []fixedRule{
	{"CMakeLists.txt", []string{testmap.BuildTarget}, "the top-level build: every shipped artifact depends on it"},
	{"CMakePresets.json", []string{testmap.BuildTarget}, "the presets every build configures from"},
	{"vcpkg.json", []string{testmap.BuildTarget}, "the dependencies every build installs"},
	{"vcpkg-configuration.json", []string{testmap.BuildTarget}, "the registries vcpkg installs from"},
	{".vcpkg-commit", []string{testmap.BuildTarget}, "the vcpkg commit the build pins"},
	{"cmake/**", []string{testmap.BuildTarget}, "CMake modules every target includes"},
	{"extern/CMakeLists.txt", []string{testmap.BuildTarget}, "the build of the external libraries"},
	{"src/runtime/**/CMakeLists.txt", []string{testmap.BuildTarget}, "a target of the runtime's build"},
	{"src/core/**/CMakeLists.txt", []string{testmap.BuildTarget}, "a target of the core's build"},
	{"src/abi/**/CMakeLists.txt", []string{testmap.BuildTarget}, "a target of the ABI's build"},
	{"src/modules/**/CMakeLists.txt", []string{testmap.BuildTarget}, "a target of a static module's build"},
	{"src/nevr_api/**/CMakeLists.txt", []string{testmap.BuildTarget}, "a target of the protocol's build"},
	{"src/launcher/**/CMakeLists.txt", []string{testmap.BuildTarget}, "a target of the launcher's build"},
	{"sdk/**/CMakeLists.txt", []string{testmap.BuildTarget}, "a target of the plugin SDK's build"},
	{"src/libovr-stub/**", []string{testmap.BuildTarget}, "the libovr stub the top-level build adds (its exports.def reaches the link by a flag, not as a ninja input)"},
	{"src/reconstruction/**/CMakeLists.txt", []string{testmap.BuildTarget}, "a target of the reconstruction's build"},
	{"src/runtime/host/**", []string{"host-seams", "pe-contract"}, "the host's seams and the PE contract of the built host"},
	{"tests/system/**", []string{"just:test-system-short"}, "the Go system suite that test-system-short runs"},
	{"tests/plugins/**", []string{"just:test-plugins-groundtruth"}, "the Go plugin suite that test-plugins-groundtruth runs"},
	{"tests/quest/**", []string{"just:test-android"}, "the Go Quest suite that test-android runs"},
	{"tests/crash-ingest/**", []string{"crash-ingest-vet"}, "the crash-ingest module, vetted by its target"},
	{"tests/contracts/**", []string{"just:test-service-contracts"}, "the Go service-contract module that test-service-contracts runs"},
	{"tests/vectors/maps/**", []string{testmap.KindGTest + ":test_map_manifest", testmap.KindGTest + ":test_map_derive"},
		"the vectors the map manifest and recipe tests read through NEVR_MAP_VECTORS and NEVR_RECIPE_VECTORS (src/core/maps.cmake)"},
	{"installer/**", []string{"just:test-installer"}, "the installer's own tests"},
	{"src/scripting/**", []string{"just:test-scripting"}, "the scripting host, built and tested by test-scripting"},
	{"tools/quest_apk_patch_map.tsv", []string{"pytest:test_quest_apk_patch_map"}, "the patch map its pytest reads"},
	{"tools/check_quest_social_frames.sh", []string{"just:test-quest-social"}, "the check test-quest-social runs"},
	{"tools/quest_autopilot_routes/**", []string{"just:test-quest-autopilot", "pytest:test_quest_autopilot_run"}, "the routes the autopilot test and its pytest read"},
	{"release-headlines/**", []string{"pytest:test_package_release", "pytest:test_release_headlines", "pytest:test_release_train"}, "the headline fragments the release tests read"},
	{"AGENTS.md", []string{"agents-imports"}, "the instructions agents-imports checks"},
	{"tools/verify_agents_imports.py", []string{"agents-imports"}, "the check agents-imports runs"},
	{"tools/verify_log_rules.py", []string{"log-rules"}, "the check log-rules runs"},
	{"tools/test_map.json", []string{testmap.KindGo + ":internal/testmap"}, "the map, read and checked by its package"},
}

// scanners are the checks that read every file their globs match, wherever it lives.
var scanners = []testmap.Scanner{
	{Name: "go-policy", Globs: []string{"tools/**/*.go", "tools/**/*.py"}, Tests: []string{"go:internal/policy"}},
	{Name: "one-home", Globs: []string{"**/*.md"}, Tests: []string{"go:cmd/one-home"}},
	// tools/verify_doc_paths.py reads every tracked Markdown file and the C and C++ sources (its SOURCE_GLOBS).
	{Name: "doc-paths", Globs: []string{"**/*.md", "**/*.cpp", "**/*.h", "**/*.hpp"}, Tests: []string{"doc-paths"}},
}

// targets are the commands a test id names. {preset} stands for the build preset.
var targets = map[string][]string{
	testmap.BuildTarget: {"just", "build"},
	"host-seams":        {"python3", "tools/verify_host_seams.py", "build/{preset}"},
	"pe-contract": {"python3", "tools/verify_pe_contract.py", "--dll", "build/{preset}/bin/BugSplat64.dll",
		"--runtime-dll", "build/{preset}/bin/nevr-runtime.dll"},
	"crash-ingest-vet": {"go", "-C", "tests/crash-ingest", "vet", "./..."},
	"agents-imports":   {"python3", "tools/verify_agents_imports.py"},
	"log-rules":        {"python3", "tools/verify_log_rules.py"},
	"doc-paths":        {"python3", "tools/verify_doc_paths.py"},
}

// reasonRow says why a file with no test of its own has none: the first row that matches it gives the reason.
type reasonRow struct {
	match func(string) bool
	why   string
}

// reasons are the kinds of file that have no test of their own. A file matching no row fails the generator: a new kind of
// file is a decision to make here, not a silent none.
var reasons = []reasonRow{
	{func(f string) bool { return strings.HasPrefix(f, ".claude/") },
		"agent instructions: no test of its own; the Markdown scanners (one-home, doc-paths) read it"},
	{func(f string) bool { return strings.HasSuffix(f, ".md") },
		"documentation: only the scanners read it"},
	{func(f string) bool { return strings.HasPrefix(f, "docs/") },
		"documentation: no test of its own; the Markdown scanners (one-home, doc-paths) read it"},
	{func(f string) bool { return strings.HasPrefix(f, "tools/echomod/") },
		"ADR 0044 'Where Python stays': no test in tools/tests names tools/echomod"},
	{func(f string) bool { return strings.HasPrefix(f, "certs/") },
		"local signing CA scripts (just generate-certs, just sign): no test runs them"},
	{func(f string) bool { return strings.HasPrefix(f, "sdk/include/nevr/pc/") },
		"SDK header for PC plugins: no target in this tree compiles it (plugins build in nevr-runtime-plugins)"},
	{func(f string) bool { return strings.HasPrefix(f, ".github/") },
		"GitHub configuration: no test reads this file"},
	{func(f string) bool { return strings.HasPrefix(f, "third_party/") },
		"a vendored third-party notice: no test reads it"},
	{func(f string) bool {
		return f == "tools/log_call_census.py" || f == "tools/gen_hwdump_vk_fields.py" || f == "tools/symbol_candidates.txt"
	}, "a one-off tool or data file: no test runs or reads it"},
	{func(f string) bool {
		return f == ".clang-format" || f == ".gitmodules" || f == "LICENSE" || f == "NOTICE"
	}, "repository metadata: no test reads this file"},
}

// reasonFor is the reason a file with no test of its own has none.
func reasonFor(f string) (string, bool) {
	for _, r := range reasons {
		if r.match(f) {
			return r.why, true
		}
	}
	return "", false
}

// about is the prose the map carries about itself.
var about = []string{
	"The map just test-changed reads (tools/cmd/test-changed). It is written by just test-map (tools/cmd/test-map) from the",
	"build trees, the Quest presets, the Go packages and the tests' own text: never by hand. Each tracked file is in exactly",
	"one component, the one of its directory: its tests in files, or in none the reason it has no test of its own.",
	"A file's tests are its own, then those of each scanner whose globs match it. A test id is a target below, or",
	"gtest:<binary> (just test-auth-unit <binary>), just:<recipe>, go:<package under tools/> (go -C tools test -count=1 ./<package>)",
	"or pytest:<name> (tools/tests/<name>.py). {preset} in a target is the build preset, else mingw-release.",
}

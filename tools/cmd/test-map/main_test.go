package main

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testkit"
	"github.com/EchoTools/nevr-runtime/tools/internal/testmap"
)

// cannedDir is the directory, beside a fake, that holds the output each call prints.
const cannedDir = "out"

// fakeTree is the build tree the fixtures use for the release preset.
const fakeTree = testmap.DefaultPreset

func TestMain(m *testing.M) {
	testkit.Main(m, testkit.Fakes{"ninja": fakeNinja, "go": fakeGo, "just": fakeJust})
}

// cannedName is the file name a fake's output is kept under for one call: its command's arguments, with the separators
// made safe in a file name.
func cannedName(parts ...string) string {
	return strings.NewReplacer("/", "%", " ", "_").Replace(strings.Join(parts, "@"))
}

// serve prints the canned output for key, or fails the call.
func serve(c *testkit.Call, key string) int {
	data, err := os.ReadFile(filepath.Join(c.FakeDir, cannedDir, key))
	if err != nil {
		fmt.Fprintf(c.Stderr, "fake %s: no canned output %s: %v\n", c.Name, key, err)
		return 1
	}
	c.Stdout.Write(data)
	return 0
}

// The positions of the arguments a fake takes: ninja -C <dir> -t <sub...>, go -C <dir> list ..., and the exit code of a
// fake called with arguments it does not take.
const (
	argFlag     = 0
	argDir      = 1
	argSecond   = 2
	argSub      = 3
	ninjaMinLen = argSub + 1
	goListArg   = argSecond
	goMinLen    = argSecond + 1
	fakeUsage   = 2
)

// fakeNinja is `ninja -C <build dir> -t <sub...>`: its output is keyed by the build directory's name and the subcommand.
func fakeNinja(c *testkit.Call) int {
	if len(c.Args) < ninjaMinLen || c.Args[argFlag] != "-C" || c.Args[argSecond] != "-t" {
		fmt.Fprintf(c.Stderr, "fake ninja: unexpected arguments %q\n", c.Args)
		return fakeUsage
	}
	return serve(c, cannedName(filepath.Base(c.Args[argDir]), strings.Join(c.Args[argSub:], " ")))
}

// fakeGo is `go -C <tools dir> list ...`.
func fakeGo(c *testkit.Call) int {
	if len(c.Args) < goMinLen || c.Args[goListArg] != "list" {
		fmt.Fprintf(c.Stderr, "fake go: unexpected arguments %q\n", c.Args)
		return fakeUsage
	}
	return serve(c, cannedName("go", "list"))
}

// fakeJust is `just --summary`.
func fakeJust(c *testkit.Call) int {
	if len(c.Args) != 1 || c.Args[0] != "--summary" {
		fmt.Fprintf(c.Stderr, "fake just: unexpected arguments %q\n", c.Args)
		return fakeUsage
	}
	return serve(c, cannedName("just", "--summary"))
}

// questPresetsJSON is the Quest presets of the fixture: one group, quest-host-shared, with one target.
const questPresetsJSON = `{"buildPresets":[{"name":"quest-host-shared","configurePreset":"quest-host-shared","targets":["host-tests-shared"]}]}`

// fixture is a temporary checkout with fake ninja, go and just on its environment's PATH.
type fixture struct {
	t     *testing.T
	repo  string
	out   string
	env   []string
	ninja *testkit.FakeCommand
	gocmd *testkit.FakeCommand
	just  *testkit.FakeCommand
}

func newFixture(t *testing.T) *fixture {
	t.Helper()
	ninja := testkit.Fake(t, "ninja")
	gocmd := testkit.Fake(t, "go")
	just := testkit.Fake(t, "just")
	f := &fixture{t: t, repo: t.TempDir(), out: filepath.Join(t.TempDir(), "test_map.json"),
		env: testkit.WithPath(testkit.Env(t), ninja.Dir(), gocmd.Dir(), just.Dir()), ninja: ninja, gocmd: gocmd, just: just}
	f.git("init", "-q")
	f.mkdir("build/" + fakeTree)
	f.canned(f.just, cannedName("just", "--summary"), "build test-quest-shared\n")
	f.ninjaOut(fakeTree, "targets all", "")
	f.ninjaOut(fakeTree, "deps", "")
	f.ninjaOut(fakeTree, "inputs all", "")
	f.canned(f.gocmd, cannedName("go", "list"), "")
	f.write("src/quest/CMakePresets.json", questPresetsJSON)
	f.write("src/quest/host_tests/groups/shared.cmake", "")
	f.write("src/runtime/CMakeLists.txt", "")
	f.mkdir("build/quest-host-shared")
	f.ninjaOut("quest-host-shared", "inputs host-tests-shared", "")
	f.ninjaOut("quest-host-shared", "deps", "")
	for _, tree := range []string{"android-arm64", "scripting-mingw-shared", "scripting-mingw-pervm", "script-check"} {
		f.mkdir("build/" + tree)
		f.ninjaOut(tree, "inputs all", "")
		f.ninjaOut(tree, "deps", "")
	}
	return f
}

func (f *fixture) git(args ...string) {
	f.t.Helper()
	cmd := exec.Command("git", args...)
	cmd.Dir, cmd.Env = f.repo, f.env
	if out, err := cmd.CombinedOutput(); err != nil {
		f.t.Fatalf("git %s: %v: %s", strings.Join(args, " "), err, out)
	}
}

// write creates a file of the checkout (not yet tracked).
func (f *fixture) write(rel, text string) {
	f.t.Helper()
	p := filepath.Join(f.repo, filepath.FromSlash(rel))
	if err := os.MkdirAll(filepath.Dir(p), dirMode); err != nil {
		f.t.Fatal(err)
	}
	if err := os.WriteFile(p, []byte(text), fileMode); err != nil {
		f.t.Fatal(err)
	}
}

// track adds every file of the checkout to the index, so ls-files lists it.
func (f *fixture) track() { f.git("add", "-A") }

func (f *fixture) mkdir(rel string) {
	f.t.Helper()
	if err := os.MkdirAll(filepath.Join(f.repo, filepath.FromSlash(rel)), dirMode); err != nil {
		f.t.Fatal(err)
	}
}

// canned writes the output a fake prints for key.
func (f *fixture) canned(fake *testkit.FakeCommand, key, text string) {
	f.t.Helper()
	dir := filepath.Join(fake.Dir(), cannedDir)
	if err := os.MkdirAll(dir, dirMode); err != nil {
		f.t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, key), []byte(text), fileMode); err != nil {
		f.t.Fatal(err)
	}
}

// ninjaOut is the output of `ninja -C <build> -t <sub>`.
func (f *fixture) ninjaOut(build, sub, text string) {
	f.t.Helper()
	f.canned(f.ninja, cannedName(build, sub), text)
}

// abs is the path of a file of the checkout, as ninja prints an absolute source path.
func (f *fixture) abs(rel string) string { return filepath.Join(f.repo, filepath.FromSlash(rel)) }

// trees is the build directory of the fixture: the trees' parent is the checkout.
func (f *fixture) trees() string { return filepath.Join(f.repo, "build") }

// mapper is a mapper over the fixture, for one stage at a time.
func (f *fixture) mapper(t *testing.T) *mapper {
	t.Helper()
	m, err := newMapper(toolchain{ctx: context.Background(), env: f.env}, slog.New(slog.NewTextHandler(io.Discard, nil)), f.repo, []string{f.trees()})
	if err != nil {
		t.Fatal(err)
	}
	return m
}

// run is the command over the fixture: its checkout, its trees, its map file, and the given arguments.
func (f *fixture) run(args ...string) (int, string, string) {
	var out, errb bytes.Buffer
	all := append([]string{"-repo", f.repo, "-trees", f.trees(), "-out", f.out}, args...)
	code := run(context.Background(), all, f.env, &out, &errb)
	return code, out.String(), errb.String()
}

// parsed reads the map the last run wrote.
func (f *fixture) parsed(t *testing.T) *testmap.Map {
	t.Helper()
	data, err := os.ReadFile(f.out)
	if err != nil {
		t.Fatal(err)
	}
	m, err := testmap.Parse(data)
	if err != nil {
		t.Fatalf("the map the generator wrote does not parse: %v", err)
	}
	return m
}

func hasID(ids []string, want string) bool {
	for _, id := range ids {
		if id == want {
			return true
		}
	}
	return false
}

// A GTest's executable is covered by its inputs and their header dependencies.
func TestGTestsCoverTheInputsAndTheirHeaderDeps(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/runtime/CMakeLists.txt", "add_executable(test_a tests/test_a.cpp)\n")
	f.write("src/runtime/tests/test_a.cpp", "#include \"x.h\"\n")
	f.write("src/runtime/tests/helper.h", "")
	f.track()
	f.ninjaOut(fakeTree, "targets all", "bin/test_a.exe: CXX_EXECUTABLE_LINKER__test_a\n")
	f.ninjaOut(fakeTree, "inputs bin/test_a.exe", ".\n"+f.abs("src/runtime/tests/test_a.cpp")+
		"\nsrc/runtime/CMakeFiles/test_a.dir/tests/test_a.cpp.obj\n")
	f.ninjaOut(fakeTree, "deps", "src/runtime/CMakeFiles/test_a.dir/tests/test_a.cpp.obj: #deps 1, deps mtime 1 (VALID)\n"+
		"    "+f.abs("src/runtime/tests/helper.h")+"\n")
	m := f.mapper(t)
	if err := m.gtests(); err != nil {
		t.Fatal(err)
	}
	for _, file := range []string{"src/runtime/tests/test_a.cpp", "src/runtime/tests/helper.h"} {
		if !m.cov[file]["gtest:test_a"] {
			t.Errorf("%s is not covered by gtest:test_a: %v", file, m.cov.ids(file))
		}
	}
	if len(m.fallbacks) != 0 {
		t.Errorf("an executable the tree has fell back to its sources: %v", m.fallbacks)
	}
}

// An executable the tree does not have is covered by the sources its add_executable block names.
func TestAGTestTheTreeLacksIsCoveredByTheSourcesItNames(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/runtime/CMakeLists.txt", "add_executable(test_b\n  ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_b.cpp)\n")
	f.write("src/runtime/tests/test_b.cpp", "")
	f.track()
	m := f.mapper(t)
	if err := m.gtests(); err != nil {
		t.Fatal(err)
	}
	if !m.cov["src/runtime/tests/test_b.cpp"]["gtest:test_b"] {
		t.Errorf("the source of test_b is not covered by it: %v", m.cov.ids("src/runtime/tests/test_b.cpp"))
	}
	if len(m.fallbacks) != 1 || m.fallbacks[0] != "test_b" {
		t.Errorf("fallbacks = %v, want [test_b]", m.fallbacks)
	}
}

// A Quest group is covered by its own preset's targets only: a file only the whole build compiles is not its.
func TestAQuestGroupTakesOnlyItsPresetsTargetClosure(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/quest/CMakePresets.json", `{"buildPresets":[{"name":"quest-host-shared","configurePreset":"quest-host-shared","targets":["host-tests-shared"]},`+
		`{"name":"quest-host-shared-pinned","configurePreset":"quest-host-shared-pinned","targets":["host-tests-shared"]}]}`)
	f.write("src/quest/host_tests/groups/shared.cmake", "")
	f.write("src/quest/a.cpp", "")
	f.write("src/quest/only_all.cpp", "")
	f.track()
	f.mkdir("build/quest-host-shared")
	f.ninjaOut("quest-host-shared", "inputs host-tests-shared", f.abs("src/quest/a.cpp")+"\n")
	f.ninjaOut("quest-host-shared", "deps", "")
	f.ninjaOut("quest-host-shared", "inputs all", f.abs("src/quest/a.cpp")+"\n"+f.abs("src/quest/only_all.cpp")+"\n")
	f.canned(f.just, cannedName("just", "--summary"), "test-quest-shared test-quest-shared-pinned\n")
	m := f.mapper(t)
	if err := m.loadRecipes(); err != nil {
		t.Fatal(err)
	}
	if err := m.quest(); err != nil { // a pinned preset is skipped: its tree is not built here, and must not be asked for
		t.Fatal(err)
	}
	if !m.cov["src/quest/a.cpp"]["just:test-quest-shared"] {
		t.Errorf("a.cpp, in the preset's target, is not covered: %v", m.cov.ids("src/quest/a.cpp"))
	}
	if m.cov["src/quest/only_all.cpp"]["just:test-quest-shared"] {
		t.Errorf("only_all.cpp is covered by the group, though only the whole build compiles it")
	}
}

// A preset whose build tree is not there names the tree and the recipe that makes it, and is exit 2.
func TestAMissingQuestTreeNamesItAndItsRecipe(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/quest/CMakePresets.json", questPresetsJSON)
	f.write("src/quest/host_tests/groups/shared.cmake", "")
	f.track()
	if err := os.RemoveAll(filepath.Join(f.repo, "build", "quest-host-shared")); err != nil {
		t.Fatal(err)
	}
	code, _, stderr := f.run()
	if code != exitCannotMake {
		t.Fatalf("exit %d, want %d; stderr %q", code, exitCannotMake, stderr)
	}
	for _, want := range []string{"build tree quest-host-shared is in none of " + f.trees(), "just test-quest-shared"} {
		if !strings.Contains(stderr, want) {
			t.Errorf("stderr does not name %q: %q", want, stderr)
		}
	}
}

// -trees is a search path: a tree comes from the first directory that holds it, and one in none names them all.
func TestTreesAreLookedUpInOrder(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	empty, other := t.TempDir(), t.TempDir()
	if err := os.MkdirAll(filepath.Join(other, "quest-host-shared"), dirMode); err != nil {
		t.Fatal(err)
	}
	m := f.mapper(t)
	m.trees = []string{empty, f.trees()}
	if got, err := m.tree("quest-host-shared", "just test-quest-shared"); err != nil || got != filepath.Join(f.trees(), "quest-host-shared") {
		t.Errorf("tree from the second directory = %q, %v; want the fixture's", got, err)
	}
	m.trees = []string{other, f.trees()}
	if got, err := m.tree("quest-host-shared", "just test-quest-shared"); err != nil || got != filepath.Join(other, "quest-host-shared") {
		t.Errorf("tree in both = %q, %v; want the first directory's", got, err)
	}
	m.trees = []string{empty, other}
	if _, err := m.tree("android-arm64", "just build-android"); err == nil || !strings.Contains(err.Error(), "in none of "+empty+", "+other) {
		t.Errorf("tree in neither: err = %v, want one naming both directories", err)
	}
}

// go list: a package's own files and testdata are covered by it, and by every package that depends on it, transitively.
func TestGoReverseDepsAndTestdata(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("tools/go.mod", "module x\n")
	f.write("tools/internal/b/b.go", "")
	f.write("tools/internal/b/testdata/x.txt", "")
	f.write("tools/internal/a/a.go", "")
	f.write("tools/cmd/c/c.go", "")
	f.track()
	bPkg := goModule + "internal/b"
	aPkg := goModule + "internal/a"
	f.canned(f.gocmd, cannedName("go", "list"),
		goListLine("internal/b")+
			goListLine("internal/a", bPkg)+
			goListLine("cmd/c", aPkg, bPkg)+
			goListLine("internal/b [internal/b.test]", bPkg)+
			goListLine("internal/b_test", bPkg))
	m := f.mapper(t)
	if err := m.goPackages(); err != nil {
		t.Fatal(err)
	}
	wantPkgs := []string{"internal/a", "internal/b", "cmd/c"}
	if !hasID(m.goPkgs, "internal/b") || hasID(m.goPkgs, "internal/b_test") || len(m.goPkgs) != len(wantPkgs) {
		t.Errorf("packages = %v, want internal/a, internal/b and cmd/c only", m.goPkgs)
	}
	for _, c := range []struct{ file, id string }{
		{"tools/internal/b/b.go", "go:internal/b"},
		{"tools/internal/b/b.go", "go:internal/a"},
		{"tools/internal/b/b.go", "go:cmd/c"},
		{"tools/internal/b/testdata/x.txt", "go:cmd/c"},
		{"tools/internal/a/a.go", "go:cmd/c"},
		{"tools/go.mod", "go:cmd/c"},
		{"tools/go.mod", "go:internal/b"},
	} {
		if !m.cov[c.file][c.id] {
			t.Errorf("%s is not covered by %s: %v", c.file, c.id, m.cov.ids(c.file))
		}
	}
	if m.cov["tools/cmd/c/c.go"]["go:internal/b"] {
		t.Errorf("c.go is covered by internal/b, which c does not change")
	}
}

// goListLine is one line of the go list template: the import path, its directory and its dependencies.
func goListLine(rel string, deps ...string) string {
	ip := goModule + rel
	if strings.Contains(rel, " ") {
		ip = rel // a test variant, "internal/b [internal/b.test]", which the generator skips
	}
	return ip + "\t/dir\t" + strings.Join(deps, " ") + "\n"
}

// A text names a tracked file by its path; by a unique basename of at least minUniqueBasename, quoted or after a slash; by
// a quoted directory; or by a chain of quoted parts that spells its path. A short basename or a bare one names nothing.
func TestReferencesByPathBasenameAndChain(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("docs/standards/a.md", "") // a short basename: only a path chain can name it
	f.write("tools/fixtures/fixture_data.txt", "")
	f.write("tools/fixtures/common_name.txt", "") // the same basename twice: a bare basename names neither
	f.write("tools/other/common_name.txt", "")
	f.write("tools/fixtures/a.txt", "")
	f.write("tools/fixtures/plainname_x.txt", "")
	f.track()
	m := f.mapper(t)
	if err := m.loadRecipes(); err != nil {
		t.Fatal(err)
	}
	text := `open("fixture_data.txt"); x = ["docs", "standards", "a.md"]; open("common_name.txt"); open("a.txt"); y = plainname_x.txt`
	got := m.refs(text)
	for _, want := range []string{"docs/standards/a.md", "tools/fixtures/fixture_data.txt"} {
		if !got[want] {
			t.Errorf("refs does not name %s", want)
		}
	}
	for _, not := range []string{"tools/fixtures/common_name.txt", "tools/other/common_name.txt", "tools/fixtures/a.txt", "tools/fixtures/plainname_x.txt"} {
		if got[not] {
			t.Errorf("refs names %s, which the text names by a short or an unquoted name", not)
		}
	}
}

// A pytest that imports a tools module with a unique stem covers the module.
func TestAModuleImportedByATestIsCovered(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("tools/refmod.py", "")
	f.write("tools/dup.py", "")
	f.write("tools/sub/dup.py", "") // the same stem twice: neither is a module the rule covers
	f.write("tools/tests/test_uses.py", "import refmod\nimport dup\n\ndef test_uses():\n    pass\n")
	f.track()
	m := f.mapper(t)
	if err := m.references(); err != nil {
		t.Fatal(err)
	}
	if !m.cov["tools/refmod.py"]["pytest:test_uses"] {
		t.Errorf("refmod.py is not covered by test_uses: %v", m.cov.ids("tools/refmod.py"))
	}
	if m.cov["tools/dup.py"]["pytest:test_uses"] {
		t.Errorf("dup.py, whose stem is not unique in the tree, is covered by an import of it")
	}
}

// A tools/tests/test_*.py that defines no test is a fixture, not a target (pytest collects nothing from it and exits 5): the
// fixture, and the module it imports, are covered by the test that imports it.
func TestAFixtureWithNoTestIsNoTargetAndItsImporterCoversIt(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("tools/fixmod.py", "")
	f.write("tools/tests/test_fixture_only.py", "import fixmod\n\ndef helper():\n    return 1\n")
	f.write("tools/tests/test_real.py", "import unittest\nimport test_fixture_only\n\nclass Real(unittest.TestCase):\n    def test_x(self):\n        pass\n")
	f.track()
	m := f.mapper(t)
	if err := m.references(); err != nil {
		t.Fatal(err)
	}
	for _, file := range []string{"tools/fixmod.py", "tools/tests/test_fixture_only.py", "tools/tests/test_real.py"} {
		if m.cov[file]["pytest:test_fixture_only"] {
			t.Errorf("%s is covered by the fixture test_fixture_only, which defines no test: %v", file, m.cov.ids(file))
		}
		if !m.cov[file]["pytest:test_real"] {
			t.Errorf("%s is not covered by test_real, which imports the fixture: %v", file, m.cov.ids(file))
		}
	}
}

// Each form of a collected test counts; a helper named test_ that is not at a def, or prose, does not.
func TestDefinesPytestMatchesTheFormsPytestCollects(t *testing.T) {
	t.Parallel()
	for _, text := range []string{"def test_a():\n", "class X(unittest.TestCase):\n    def test_b(self):\n", "class TestThing:\n",
		"async def test_c():\n", "class Y(TestCase):\n"} {
		if !definesPytest.MatchString(text) {
			t.Errorf("not a test: %q", text)
		}
	}
	for _, text := range []string{"def helper():\n", "x = test_a\n", "# def test_a\n", "import test_quest_apk_rename_package as t\n"} {
		if definesPytest.MatchString(text) {
			t.Errorf("a test: %q", text)
		}
	}
}

// The include closure gives a header included by a covered file the file's C++ ids, and what that header includes, to a
// fixpoint. The resolution goes through src/ and the including file's directory.
func TestIncludeClosureReachesTheHeadersOfACoveredFile(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	// The covered file sorts after the headers, so hdr.h is visited before it is covered and deep.h is reached only on a
	// second pass: a closure that stops after one pass leaves deep.h uncovered.
	f.write("src/runtime/z.cpp", "#include \"runtime/hdr.h\"\n")
	f.write("src/runtime/hdr.h", "#include \"deep.h\"\n")
	f.write("src/runtime/deep.h", "")
	f.track()
	m := f.mapper(t)
	m.cov.add("src/runtime/z.cpp", "gtest:test_a", "pytest:not_c")
	if err := m.closure(); err != nil {
		t.Fatal(err)
	}
	if m.cov["src/runtime/hdr.h"]["pytest:not_c"] {
		t.Errorf("a pytest id, which is not a C++ id, travelled to a header by the include closure")
	}
	for _, h := range []string{"src/runtime/hdr.h", "src/runtime/deep.h"} {
		if !m.cov[h]["gtest:test_a"] {
			t.Errorf("%s is not covered by the includer's gtest:test_a: %v", h, m.cov.ids(h))
		}
	}
}

// build covers every file `just build` compiles (the mingw tree's "all"), not only the shipped DLLs' inputs: the
// reconstruction and the test fixtures are built by it too.
func TestBuildCoversEveryInputOfJustBuild(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/reconstruction/r15/r.cpp", "")
	f.write("src/runtime/a.cpp", "")
	f.track()
	f.ninjaOut(fakeTree, "inputs all", f.abs("src/reconstruction/r15/r.cpp")+"\n")
	m := f.mapper(t)
	if err := m.build(); err != nil {
		t.Fatal(err)
	}
	if !m.cov["src/reconstruction/r15/r.cpp"][testmap.BuildTarget] {
		t.Errorf("a file only all compiles is not covered by build: %v", m.cov.ids("src/reconstruction/r15/r.cpp"))
	}
	if m.cov["src/runtime/a.cpp"][testmap.BuildTarget] {
		t.Errorf("a file the build does not compile is covered by build")
	}
}

// The fixed rules cover their files with their tests, and a file no rule names gets none.
func TestFixedRulesCoverTheirFiles(t *testing.T) {
	t.Parallel()
	cases := []struct{ file, id string }{
		{"tests/system/x.go", "just:test-system-short"},
		{"tools/test_map.json", "go:internal/testmap"},
		{"CMakeLists.txt", testmap.BuildTarget},
		{"src/runtime/host/exports.def", "pe-contract"},
		{"release-headlines/1.md", "pytest:test_release_train"},
	}
	for _, c := range cases {
		m := &mapper{cov: cover{}}
		m.tracked = []string{c.file}
		m.known = map[string]bool{c.file: true}
		if err := m.fixed(); err != nil {
			t.Fatal(err)
		}
		if !m.cov[c.file][c.id] {
			t.Errorf("%s is not covered by %s: %v", c.file, c.id, m.cov.ids(c.file))
		}
	}
}

// The reasons table: a file of a kind gets the first row's reason, and a file of no kind gets none.
func TestReasonsAreFirstMatchAndUnknownKindsHaveNone(t *testing.T) {
	t.Parallel()
	if why, ok := reasonFor(".claude/skills/x/SKILL.md"); !ok || !strings.HasPrefix(why, "agent instructions") {
		t.Errorf(".claude file reason = %q, %v", why, ok)
	}
	if why, ok := reasonFor("docs/a.txt"); !ok || !strings.HasPrefix(why, "documentation") {
		t.Errorf("docs file reason = %q, %v", why, ok)
	}
	if _, ok := reasonFor("src/zz/unknown.weird"); ok {
		t.Errorf("a file of no known kind has a reason")
	}
}

// norm maps a path ninja printed to a tracked repository path, and nothing outside the checkout or untracked.
func TestNormMapsOnlyTrackedPathsOfTheCheckout(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/a.cpp", "")
	f.track()
	m := f.mapper(t)
	build := filepath.Join(f.trees(), fakeTree)
	cases := map[string]string{
		f.abs("src/a.cpp"):                        "src/a.cpp",
		"../../src/a.cpp":                         "src/a.cpp",
		filepath.Join(f.repo, "src", "missing.h"): "",
		"/elsewhere/src/a.cpp":                    "",
		"":                                        "",
	}
	for in, want := range cases {
		if got := m.norm(build, in); got != want {
			t.Errorf("norm(%q) = %q, want %q", in, got, want)
		}
	}
}

// The parsers read ninja's header dump and the targets, and keep only the executables under bin/.
func TestParsers(t *testing.T) {
	t.Parallel()
	deps := parseDeps("a.obj: #deps 2, deps mtime 1 (VALID)\n    /x/h1.h\n    /x/h2.h\nb.obj: #deps 0\n")
	wantDeps := []string{"/x/h1.h", "/x/h2.h"}
	if len(deps["a.obj"]) != len(wantDeps) || deps["a.obj"][0] != wantDeps[0] {
		t.Errorf("deps of a.obj = %q", deps["a.obj"])
	}
	if _, ok := deps["b.obj"]; !ok {
		t.Errorf("b.obj with no dependency is missing from the dump")
	}
	exes := parseTargets("bin/test_a.exe: CXX\nbin/sub/test_b.exe: CXX\nsrc/x.o: CXX\nbin/plugin.dll: CXX\n")
	wantExes := map[string]string{"test_a.exe": "bin/test_a.exe", "test_b.exe": "bin/sub/test_b.exe"}
	if len(exes) != len(wantExes) || exes["test_a.exe"] != wantExes["test_a.exe"] || exes["test_b.exe"] != wantExes["test_b.exe"] {
		t.Errorf("executables = %v", exes)
	}
}

// -trees defaults to the build checkout's build/ ($NEVR_BUILD_CHECKOUT, else the work root's), then the main checkout's,
// then the repository's own; a repository that is its own main checkout is listed once.
func TestDefaultTreesAreTheBuildCheckoutTheMainCheckoutAndTheRepo(t *testing.T) {
	t.Parallel()
	notRepo := t.TempDir()
	cases := []struct {
		env  []string
		want []string
	}{
		{[]string{"NEVR_BUILD_CHECKOUT=/co"}, []string{filepath.Join("/co", "build"), filepath.Join(notRepo, "build")}},
		{[]string{"NEVR_WORK_ROOT=/w"}, []string{filepath.Join("/w", "build-checkout", "nevr-runtime", "build"), filepath.Join(notRepo, "build")}},
		{[]string{"HOME=/h"}, []string{filepath.Join("/h", "src", "nevr-runtime-work", "build-checkout", "nevr-runtime", "build"), filepath.Join(notRepo, "build")}},
	}
	for _, c := range cases {
		// Only PATH from the hermetic environment: testkit.Env sets NEVR_WORK_ROOT, which the HOME case must not have.
		env := append([]string{"PATH=" + lookup(testkit.Env(t))("PATH")}, c.env...)
		g := &generator{getenv: lookup(env)}
		got, err := g.defaultTrees(toolchain{ctx: context.Background(), env: env}, notRepo)
		if err != nil {
			t.Fatalf("defaultTrees with %v: %v", c.env, err)
		}
		if want := strings.Join(c.want, string(filepath.ListSeparator)); got != want {
			t.Errorf("defaultTrees with %v = %q, want %q", c.env, got, want)
		}
	}
	// A linked worktree: the main checkout's build/ comes before the worktree's own.
	f := newFixture(t)
	f.write("README.md", "x\n")
	f.track()
	f.git("commit", "-q", "-m", "base")
	linked := filepath.Join(t.TempDir(), "linked")
	f.git("worktree", "add", "-q", linked)
	env := append(f.env, "NEVR_BUILD_CHECKOUT=/co")
	g := &generator{getenv: lookup(env)}
	got, err := g.defaultTrees(toolchain{ctx: context.Background(), env: env}, linked)
	if err != nil {
		t.Fatal(err)
	}
	main, err := filepath.EvalSymlinks(f.repo)
	if err != nil {
		t.Fatal(err)
	}
	want := strings.Join([]string{filepath.Join("/co", "build"), filepath.Join(main, "build"), filepath.Join(linked, "build")}, string(filepath.ListSeparator))
	if got != want {
		t.Errorf("defaultTrees in a linked worktree = %q, want %q", got, want)
	}
}

// A file no test covers and no reason names is exit 1, naming it, and nothing is written.
func TestAFileWithNoTestAndNoReasonIsExit1NamingIt(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/zz/unknown.weird", "")
	f.track()
	code, _, stderr := f.run()
	if code != exitUncovered {
		t.Fatalf("exit %d, want %d; stderr %q", code, exitUncovered, stderr)
	}
	if !strings.Contains(stderr, "src/zz/unknown.weird") {
		t.Errorf("stderr does not name the file: %q", stderr)
	}
	if _, err := os.Stat(f.out); !errors.Is(err, os.ErrNotExist) {
		t.Errorf("a refused run wrote the map: %v", err)
	}
}

// A build tree the run needs and cannot find is exit 2, naming the tree and the recipe that makes it.
func TestAMissingBuildTreeIsExit2NamingTheTreeAndRecipe(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/runtime/CMakeLists.txt", "add_executable(test_a tests/test_a.cpp)\n")
	f.track()
	var out, errb bytes.Buffer
	code := run(context.Background(), []string{"-repo", f.repo, "-trees", filepath.Join(f.repo, "nowhere"), "-out", f.out}, f.env, &out, &errb)
	if code != exitCannotMake {
		t.Fatalf("exit %d, want %d", code, exitCannotMake)
	}
	for _, want := range []string{"build tree " + fakeTree + " is in none of " + filepath.Join(f.repo, "nowhere"), "just build"} {
		if !strings.Contains(errb.String(), want) {
			t.Errorf("stderr does not name %q: %q", want, errb.String())
		}
	}
}

// -check is exit 0 on the map the generator writes, names the first differing component, and is exit 1 after a change,
// and exit 1 when there is no map.
func TestCheckExitsZeroOnTheSameMapAndOneAfterAChange(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("docs/x.md", "")
	f.write("CMakeLists.txt", "")
	f.track()
	if code, _, stderr := f.run(); code != 0 {
		t.Fatalf("write: exit %d: %s", code, stderr)
	}
	if code, _, stderr := f.run("-check"); code != 0 {
		t.Fatalf("check on the same map: exit %d: %s", code, stderr)
	}
	f.write("docs/new.md", "")
	f.track()
	code, _, stderr := f.run("-check")
	if code != exitUncovered {
		t.Fatalf("check after a change: exit %d, want %d: %s", code, exitUncovered, stderr)
	}
	if !strings.Contains(stderr, "first differing component: docs") {
		t.Errorf("check does not name the component that changed: %q", stderr)
	}
	if err := os.Remove(f.out); err != nil {
		t.Fatal(err)
	}
	if code, _, _ := f.run("-check"); code != exitUncovered {
		t.Errorf("check with no map: exit %d, want %d", code, exitUncovered)
	}
}

// The map the generator writes parses with testmap.Parse, and encodes back to the same bytes.
func TestOutputParsesAndEncodesStably(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("src/runtime/CMakeLists.txt", "add_executable(test_a tests/test_a.cpp)\n")
	f.write("src/runtime/tests/test_a.cpp", "")
	f.write("docs/x.md", "")
	f.track()
	f.ninjaOut(fakeTree, "targets all", "bin/test_a.exe: CXX\n")
	f.ninjaOut(fakeTree, "inputs bin/test_a.exe", f.abs("src/runtime/tests/test_a.cpp")+"\n")
	if code, _, stderr := f.run(); code != 0 {
		t.Fatalf("exit %d: %s", code, stderr)
	}
	m := f.parsed(t)
	if len(m.Scanners) != len(scanners) || m.Scanners[0].Name != scanners[0].Name {
		t.Errorf("the parsed map carries scanners %v, want the generator's", m.Scanners)
	}
	if lk, ok := m.Lookup("src/runtime/tests/test_a.cpp"); !ok || !hasID(lk.Tests, "gtest:test_a") {
		t.Errorf("the parsed map does not give test_a.cpp its gtest: %v %v", lk.Tests, ok)
	}
	var buf bytes.Buffer
	if err := testmap.Encode(&buf, m); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(f.out)
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(buf.Bytes(), data) {
		t.Errorf("Encode of the parsed map is not the file the generator wrote")
	}
}

// The first difference names the component its line belongs to, or the line when the line is not a component.
func TestFirstDifferenceNamesTheComponent(t *testing.T) {
	t.Parallel()
	have := "head\n {\"name\":\"docs\",\"dir\":\"docs\"},\n"
	want := "head\n {\"name\":\"docs\",\"dir\":\"docs\",\"none\":{}},\n"
	if got := firstDifference(have, want); got != "first differing component: docs" {
		t.Errorf("firstDifference = %q", got)
	}
	if got := firstDifference("a\nb\n", "a\nc\n"); !strings.Contains(got, "outside the components, at line 2") {
		t.Errorf("firstDifference outside the components = %q", got)
	}
}

// The usage errors are exit 2: a stray argument.
func TestAStrayArgumentIsExit2(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	var out, errb bytes.Buffer
	if code := run(context.Background(), []string{"stray"}, f.env, &out, &errb); code != exitCannotMake {
		t.Errorf("exit %d, want %d; stderr %q", code, exitCannotMake, errb.String())
	}
}

// The output of a command is only whatever the fakes print: a fake that is not set up is exit 2, not a silent empty map.
func TestAFakeWithNoCannedOutputFailsTheRun(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("docs/x.md", "")
	f.track()
	if err := os.Remove(filepath.Join(f.just.Dir(), cannedDir, cannedName("just", "--summary"))); err != nil {
		t.Fatal(err)
	}
	if code, _, _ := f.run(); code != exitCannotMake {
		t.Errorf("exit %d, want %d", code, exitCannotMake)
	}
}

// One component per directory that holds tracked files, each file in files (with its ids) or in none (with its reason);
// a file with neither is exit 1, naming it.
func TestBuildMapOneComponentPerDirectoryAndNoTestNoReasonIsExit1(t *testing.T) {
	t.Parallel()
	cov := cover{}
	cov.add("a.txt", "x")
	cov.add("d/x.txt", "y")
	cov.add("d/y.txt", "z")
	mp, err := buildMap([]string{"a.txt", "d/x.txt", "d/y.txt"}, cov)
	if err != nil {
		t.Fatal(err)
	}
	byDir := map[string]testmap.Component{}
	for _, c := range mp.Components {
		byDir[c.Dir] = c
	}
	wantDirs := []string{"", "d"}
	wantFiles := []string{"d/x.txt", "d/y.txt"}
	if len(mp.Components) != len(wantDirs) || byDir[""].Name != rootName || len(byDir["d"].Files) != len(wantFiles) {
		t.Errorf("components = %+v", mp.Components)
	}
	_, err = buildMap([]string{"src/zz/unknown.weird"}, cover{})
	var fl failure
	if !errors.As(err, &fl) || fl.code != exitUncovered {
		t.Errorf("a file with no test and no reason: %v, want a failure with exit %d", err, exitUncovered)
	}
}

// A map whose heavy measurements name an id it cannot run is not written: the generator stops with the cause, so a test
// that was deleted or renamed cannot leave a stale heavy entry behind. This test swaps the package's list, so it does
// not run in parallel with the others.
func TestAStaleHeavyMeasurementStopsTheGenerator(t *testing.T) {
	saved := testmap.Measured
	t.Cleanup(func() { testmap.Measured = saved })
	testmap.Measured = append(append([]testmap.Measurement(nil), saved...), testmap.Measurement{ID: "pytest:", Millis: testmap.HeavyBound.Milliseconds() + 1})
	f := newFixture(t)
	f.write("docs/x.md", "")
	f.track()
	code, _, stderr := f.run()
	if code != exitCannotMake {
		t.Fatalf("exit %d, want %d: %s", code, exitCannotMake, stderr)
	}
	if !strings.Contains(stderr, `heavy measurement "pytest:"`) {
		t.Errorf("stderr does not name the entry: %q", stderr)
	}
	if _, err := os.Stat(f.out); err == nil {
		t.Errorf("the generator wrote %s although it refused", f.out)
	}
}

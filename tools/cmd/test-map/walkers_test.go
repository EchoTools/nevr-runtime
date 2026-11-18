package main

import (
	"errors"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	"github.com/EchoTools/nevr-runtime/tools/internal/testmap"
)

// A test's Python header and walk, as test_spectatorstream_order.py has them (commit 95335ce62's red test).
const spectatorWalk = `from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def runtime_sources():
    for path in sorted((ROOT / "src").rglob("*.cpp")):
        yield path


def test_only_boot_cpp_names_the_flags():
    pass
`

// matches reports whether any of globs selects file.
func matches(globs []string, file string) bool {
	for _, g := range globs {
		if testmap.MatchGlob(g, file) {
			return true
		}
	}
	return false
}

// scannerOf is the scanner the generator derived for the test stem, or nil.
func scannerOf(scs []testmap.Scanner, stem string) *testmap.Scanner {
	for i := range scs {
		if scs[i].Name == walkScannerPrefix+stem {
			return &scs[i]
		}
	}
	return nil
}

// A test that walks src/ for *.cpp is selected for a source file its text never names: the case of 95335ce62, where
// dinput_enumerate.cpp changed and test_spectatorstream_order was not run.
func TestATestThatWalksSrcIsSelectedForAFileItNeverNames(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("tools/tests/test_spectatorstream_order.py", spectatorWalk)
	f.write("src/runtime/patch/dinput_enumerate.cpp", "")
	f.write("src/runtime/patch/dinput_enumerate.h", "")
	f.write("docs/a.md", "")
	f.track()
	m := f.mapper(t)
	scs, err := m.walkScanners(goWalks)
	if err != nil {
		t.Fatal(err)
	}
	sc := scannerOf(scs, "test_spectatorstream_order")
	if sc == nil || !reflect.DeepEqual(sc.Tests, []string{"pytest:test_spectatorstream_order"}) {
		t.Fatalf("no scanner for the test: %+v", scs)
	}
	cov := cover{}
	cov.add("src/runtime/patch/dinput_enumerate.cpp", testmap.BuildTarget)
	cov.add("src/runtime/patch/dinput_enumerate.h", testmap.BuildTarget)
	cov.add("tools/tests/test_spectatorstream_order.py", "pytest:test_spectatorstream_order")
	cov.add("docs/a.md", "doc-paths")
	mp, err := buildMap([]string{"src/runtime/patch/dinput_enumerate.cpp", "src/runtime/patch/dinput_enumerate.h",
		"tools/tests/test_spectatorstream_order.py", "docs/a.md"}, cov, scs)
	if err != nil {
		t.Fatal(err)
	}
	lk, ok := mp.Lookup("src/runtime/patch/dinput_enumerate.cpp")
	if !ok || !hasID(lk.Tests, "pytest:test_spectatorstream_order") {
		t.Errorf("a change to dinput_enumerate.cpp does not select the test that scans every src .cpp: %v", lk.Tests)
	}
	if lk, _ := mp.Lookup("src/runtime/patch/dinput_enumerate.h"); hasID(lk.Tests, "pytest:test_spectatorstream_order") {
		t.Errorf("a header is selected for a test that scans only .cpp: %v", lk.Tests)
	}
	if lk, _ := mp.Lookup("docs/a.md"); hasID(lk.Tests, "pytest:test_spectatorstream_order") {
		t.Errorf("a document is selected for a test that scans src: %v", lk.Tests)
	}
}

// The walk forms the tests use, and the ones that read no repository file.
func TestPyWalksReadsEachWalkForm(t *testing.T) {
	t.Parallel()
	const head = "ROOT = Path(__file__).resolve().parents[2]\n"
	cases := []struct {
		name string
		text string
		want []string
	}{
		{"parenthesised receiver", head + `for p in sorted((ROOT / "src").rglob("*.cpp")): pass`, []string{"src/**/*.cpp"}},
		{"derived name", head + "QUEST = ROOT / \"src/quest\"\nfor p in QUEST.rglob(\"*\"): pass", []string{"src/quest/**/*"}},
		{"chained literals", head + `x = (ROOT / "src" / "runtime").rglob("*.h")`, []string{"src/runtime/**/*.h"}},
		{"glob", head + `x = (ROOT / ".github/workflows").glob("*.yml")`, []string{".github/workflows/*.yml"}},
		{"iterdir", head + `x = (ROOT / "src" / "host").iterdir()`, []string{"src/host/**"}},
		{"a name of the file's own path", "D = Path(__file__).resolve().parents[2] / \"docs\" / \"adr\"\nx = D.glob(\"*.md\")", []string{"docs/adr/*.md"}},
		{"a default parameter", head + "Q = ROOT / \"src/quest\"\ndef sources(root: Path = Q):\n    return root.rglob(\"*\")", []string{"src/quest/**/*"}},
		{"a loop over directories", head + "for tree in (\"src/quest\", \"src/runtime\"):\n    x = (ROOT / tree).rglob(\"*\")",
			[]string{"src/quest/**/*", "src/runtime/**/*"}},
		{"a loop over patterns", head + "for suffix in (\"*.h\", \"*.hpp\"):\n    x = (ROOT / \"src\").rglob(suffix)",
			[]string{"src/**/*.h", "src/**/*.hpp"}},
		{"a part that is not a literal", head + "x = (ROOT / name(a)).glob(\"*.py\")", []string{"**/*.py"}},
		{"a literal after a part that is not a literal", head + "x = (ROOT / \"src\" / sub(a) / \"tests\").glob(\"*.cpp\")", []string{"src/**/*.cpp"}},
		{"a loop name after a part that is not a literal", head + "for t in (\"a\", \"b\"):\n    x = (ROOT / \"src\" / sub(a) / t).glob(\"*.cpp\")", []string{"src/**/*.cpp"}},
		{"a glob pattern that is not a literal", head + "def f(pat):\n    return (ROOT / \"src\").glob(pat)", []string{"src/**/*"}},
		{"a pattern that holds a literal and more", head + "x = (ROOT / \"src\").rglob(\"*.\" + ext)", []string{"src/**/*"}},
		{"a pattern with a conditional", head + "x = (ROOT / \"src\").glob(pat or \"*.cpp\")", []string{"src/**/*"}},
		{"parents[N] then .parent", "R = Path(__file__).resolve().parents[1].parent\nx = (R / \"src\").rglob(\"*.cpp\")", []string{"src/**/*.cpp"}},
		{"parents[N] then too many .parent", "R = Path(__file__).resolve().parents[1].parent.parent\nx = (R / \"src\").rglob(\"*.cpp\")", nil},
		{"a name rebound to a non-literal", head + "P = \"*.h\"\nP = compute()\nx = (ROOT / \"src\").glob(P)", []string{"src/**/*"}},
		{"a part name rebound to a non-literal", head + "d = \"quest\"\nd = compute()\nx = (ROOT / \"src\" / d).glob(\"*.h\")", []string{"src/**/*.h"}},
		{"glob of every name", head + "x = (ROOT / \"src\").glob(\"*\")", []string{"src/**"}},
		{"a loop with a non-literal element", head + "for d in (\"a\", other):\n    x = (ROOT / \"src\" / d).rglob(\"*.cpp\")", []string{"src/**/*.cpp"}},
		{"a pattern loop with a non-literal element", head + "for s in (\"*.h\", extra):\n    x = (ROOT / \"src\").rglob(s)", []string{"src/**/*"}},
		{"a loop over a name", head + "s = \"*.h\"\nfor s in SUFFIXES:\n    x = (ROOT / \"src\").rglob(s)", []string{"src/**/*"}},
		{"a def parameter named like a literal", head + "s = \"*.h\"\ndef f(s):\n    return (ROOT / \"src\").rglob(s)", []string{"src/**/*"}},
		{"an augmented assignment", head + "s = \"*.h\"\ns += \"pp\"\nx = (ROOT / \"src\").rglob(s)", []string{"src/**/*"}},
		{"a glob that ends in a * segment", head + "x = (ROOT / \"src\").glob(\"quest/*\")", []string{"src/quest/**"}},
		{"git ls-files with a list extended past its end", head + `subprocess.run(["git", "ls-files", "src"] + extra, cwd=ROOT)`, []string{"**"}},
		{"a repository root over several lines", "ROOT = (\n    Path(__file__).resolve().parents[2]\n)\nx = (ROOT / \"src\").rglob(\"*.cpp\")", []string{"src/**/*.cpp"}},
		{"git ls-files as a tuple", head + `subprocess.run(("git", "ls-files", "src"), cwd=ROOT)`, []string{"src", "src/**"}},
		{"git ls-files in a directory of the repository", head + "SRC = ROOT / \"src\"\nsubprocess.run([\"git\", \"ls-files\"], cwd=SRC)", []string{"src/**"}},
		{"an escaped quote does not close a string holding a bracket", "note = \"say \\\" (\"\nROOT2 = ROOT\n" + head + `x = (ROOT / "src").rglob("*.cpp")`, []string{"src/**/*.cpp"}},
		{"a comment with an unclosed bracket before the walk", "# see (the note\n" + head + `x = (ROOT / "src").rglob("*.cpp")`, []string{"src/**/*.cpp"}},
		{"a raw string with an escaped quote and a bracket", `y = re.compile(r"a\"(")` + "\n" + head + `x = (ROOT / "src").rglob("*.cpp")`, []string{"src/**/*.cpp"}},
		{"a docstring with an apostrophe and a bracket", "\"\"\"it's (a\n\"\"\"\n" + head + `x = (ROOT / "src").rglob("*.cpp")`, []string{"src/**/*.cpp"}},
		{"os.walk with a keyword", head + `for r, d, fs in os.walk(ROOT / "docs", topdown=True): pass`, []string{"docs/**"}},
		{"os.walk", head + `for r, d, fs in os.walk(ROOT / "docs"): pass`, []string{"docs/**"}},
		{"git ls-files of the repository", head + `subprocess.run(["git", "ls-files", "-z"], cwd=ROOT)`, []string{"**"}},
		{"git ls-files of a directory", head + `subprocess.run(["git", "-C", str(ROOT), "ls-files", "--", "src/runtime"])`,
			[]string{"src/runtime", "src/runtime/**"}},
		{"git ls-files with an excluding magic pathspec", head + `subprocess.run(["git", "ls-files", ":(exclude)docs"], cwd=ROOT)`, []string{"**"}},
		{"git ls-files with a top-relative magic pathspec", head + `subprocess.run(["git", "ls-files", ":/src"], cwd=ROOT)`, []string{"**"}},
		{"git ls-files with a magic pathspec from a subdirectory", head + "TOOLS = ROOT / \"tools\"\nsubprocess.run([\"git\", \"ls-files\", \":/src\"], cwd=TOOLS)", []string{"**"}},
		{"git ls-files with an option that takes a value", head + `subprocess.run(["git", "ls-files", "--format", "%(path)"], cwd=ROOT)`, []string{"**"}},
		{"git ls-files with a pathspec after --", head + `subprocess.run(["git", "ls-files", "-z", "--", "src"], cwd=ROOT)`, []string{"src", "src/**"}},
		{"git ls-files with a wildcard that ends inside a name", head + `subprocess.run(["git", "ls-files", "src/run*"], cwd=ROOT)`, []string{"src/**"}},
		{"git ls-files in a directory with a part that is not a literal", head + "D = ROOT / \"src\" / mod\nsubprocess.run([\"git\", \"ls-files\", \"foo.h\"], cwd=D)", []string{"src/**"}},
		{"rglob of a name that may be a directory", head + `x = ROOT.rglob("tests")`, []string{"**/tests", "**/tests/**"}},
		{"git ls-files with a wildcard at both ends of a name", head + `subprocess.run(["git", "ls-files", "src/*run*"], cwd=ROOT)`, []string{"src/**"}},
		{"git ls-files with a wildcard pathspec", head + `subprocess.run(["git", "ls-files", "tools/*.py"], cwd=ROOT)`, []string{"tools/**/*.py"}},
		{"a nested parenthesised receiver", head + `x = ((ROOT / "src") / "quest").rglob("*.h")`, []string{"src/quest/**/*.h"}},
		{"two groups in one expression", head + `x = (ROOT / "src").rglob("*.h"); y = (self.a) / (self.b)`, []string{"src/**/*.h"}},
		{"a parent index too large to be a number", "Q = Path(__file__).resolve().parents[99999999999999999999]\nx = Q.glob(\"*\")", nil},
		{"a temporary directory", head + "x = self.out.glob(\"*.so\")\ny = list(tmp.iterdir())", nil},
		{"git ls-files in a temporary repository", head + `subprocess.run(["git", "ls-files"], cwd=tmp)`, nil},
	}
	for _, c := range cases {
		if got := pyWalks(c.text); !reflect.DeepEqual(got, c.want) {
			t.Errorf("%s: pyWalks = %v, want %v", c.name, got, c.want)
		}
	}
}

// A Go package whose code walks a tree and is not in goWalks stops the generator, naming it; a package declared with
// globs gets a scanner; a declaration for a package that walks nothing is refused.
func TestAGoPackageThatWalksATreeMustBeDeclared(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("tools/go.mod", "module x\n")
	f.write("tools/internal/walker/w.go", "package walker\n\nfunc W() { filepath.WalkDir(root, nil) }\n")
	f.write("tools/internal/lister/l.go", "package lister\n\nvar args = []string{\"ls-files\", \"-z\"}\n")
	f.write("tools/internal/quiet/q.go", "package quiet\n")
	f.track()
	f.canned(f.gocmd, cannedName("go", "list"),
		goListLine("internal/walker")+goListLine("internal/lister")+goListLine("internal/quiet"))
	m := f.mapper(t)
	if err := m.goPackages(); err != nil {
		t.Fatal(err)
	}
	var fl failure
	_, err := m.walkScanners(map[string]goWalk{})
	if !errors.As(err, &fl) || fl.code != exitUncovered ||
		!strings.Contains(fl.msg, "tools/internal/walker") || !strings.Contains(fl.msg, "tools/internal/lister") ||
		strings.Contains(fl.msg, "tools/internal/quiet") {
		t.Errorf("undeclared walkers: %v, want a failure with exit %d naming walker and lister only", err, exitUncovered)
	}
	scs, err := m.walkScanners(map[string]goWalk{
		"internal/walker": {globs: []string{"src/**"}, why: "reads src"},
		"internal/lister": {why: "reads no repository file"},
	})
	if err != nil {
		t.Fatal(err)
	}
	sc := scannerOf(scs, "internal-walker")
	if sc == nil || !reflect.DeepEqual(sc.Tests, []string{"go:internal/walker"}) || !matches(sc.Globs, "src/a/b.cpp") {
		t.Errorf("the declared walker has no scanner for src: %+v", scs)
	}
	if scannerOf(scs, "internal-lister") != nil {
		t.Errorf("a package declared with no globs has a scanner: %+v", scs)
	}
	_, err = m.walkScanners(map[string]goWalk{
		"internal/walker": {why: "x"}, "internal/lister": {why: "x"}, "internal/quiet": {why: "declared but walks nothing"},
	})
	if !errors.As(err, &fl) || !strings.Contains(fl.msg, "tools/internal/quiet") {
		t.Errorf("a declaration for a package that walks nothing: %v", err)
	}
}

// The command exits 1 for an undeclared walker, so `just test-map -check` fails on it.
func TestCheckFailsOnAnUndeclaredGoWalker(t *testing.T) {
	t.Parallel()
	f := newFixture(t)
	f.write("tools/go.mod", "module x\n")
	f.write("tools/internal/walker/w.go", "package walker\n\nfunc W() { filepath.WalkDir(root, nil) }\n")
	f.track()
	f.canned(f.gocmd, cannedName("go", "list"), goListLine("internal/walker"))
	code, _, stderr := f.run("-check")
	if code != exitUncovered || !strings.Contains(stderr, "tools/internal/walker") {
		t.Errorf("-check: exit %d, stderr %q; want exit %d naming tools/internal/walker", code, stderr, exitUncovered)
	}
}

// Every goWalks entry that leans on a rules.go scanner names one that exists, and every package of the table is real.
func TestGoWalksNameRealScannersAndPackages(t *testing.T) {
	t.Parallel()
	byName := map[string]bool{}
	for _, s := range scanners {
		byName[s.Name] = true
	}
	for pkg, w := range goWalks {
		if w.why == "" {
			t.Errorf("%s: no reason", pkg)
		}
		if w.scanner != "" && !byName[w.scanner] {
			t.Errorf("%s leans on scanner %q, which rules.go does not have", pkg, w.scanner)
		}
		if matches, _ := filepath.Glob(filepath.Join("..", "..", filepath.FromSlash(pkg), "*.go")); len(matches) == 0 {
			t.Errorf("%s is not a package under tools/", pkg)
		}
	}
}

// The repository's own map selects test_spectatorstream_order for the file whose change broke main in 95335ce62.
func TestTheRepositoryMapSelectsTheSpectatorTestForDinputEnumerate(t *testing.T) {
	t.Parallel()
	m, err := testmap.Load(filepath.Join("..", "..", "test_map"))
	if err != nil {
		t.Fatal(err)
	}
	lk, ok := m.Lookup("src/runtime/patch/dinput_enumerate.cpp")
	if !ok || !hasID(lk.Tests, "pytest:test_spectatorstream_order") {
		t.Errorf("the repository map does not select test_spectatorstream_order for dinput_enumerate.cpp: ok=%v tests=%v", ok, lk.Tests)
	}
	names := map[string]bool{}
	for _, sc := range m.Scanners {
		names[sc.Name] = true
	}
	for pkg, w := range goWalks {
		if want := walkScannerPrefix + strings.ReplaceAll(pkg, "/", "-"); len(w.globs) > 0 && !names[want] {
			t.Errorf("the repository map has no scanner %s for %s, which goWalks says reads %v", want, pkg, w.globs)
		}
	}
}

package main

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/testmap"
)

// cover is what each tracked file is covered by: its set of test ids.
type cover map[string]map[string]bool

func (c cover) add(file string, ids ...string) {
	for _, id := range ids {
		if c[file] == nil {
			c[file] = map[string]bool{}
		}
		c[file][id] = true
	}
}

// ids returns the test ids of file, sorted.
func (c cover) ids(file string) []string {
	var res []string
	for id := range c[file] {
		res = append(res, id)
	}
	sort.Strings(res)
	return res
}

// The C++ ids: a header a file covered by one of these includes is covered by the same one (the include closure).
func isCXXID(id string) bool {
	return id == testmap.BuildTarget || strings.HasPrefix(id, testmap.KindGTest+":") ||
		strings.HasPrefix(id, "just:"+questRecipePrefix) || id == "just:test-android" || id == "just:test-scripting"
}

// The Quest host groups: a build preset quest-host-<group>, not pinned, whose recipe is test-quest-<group>.
const (
	questPrefix       = "quest-host-"
	questPinnedSuffix = "-pinned"
	questRecipePrefix = "test-quest-"
)

// minUniqueBasename is the shortest file name a reference may name by its basename alone: a shorter one (README.md, a.py)
// appears in text by chance.
const minUniqueBasename = 7

// goListFields is the number of fields of one line of the go list template: the import path, the directory and the
// transitive dependencies.
const goListFields = 3

// goModule is the import path prefix of the packages under tools/.
const goModule = "github.com/EchoTools/nevr-runtime/tools/"

// goListFormat is the template of `go list -test`: the import path, the directory, and the dependencies.
const goListFormat = "{{.ImportPath}}\t{{.Dir}}\t{{join .Deps \" \"}}"

// srcRuntimeCMake is the CMake file that names the GTest executables, and srcQuest the tree of the Quest host.
const (
	srcRuntimeCMake = "src/runtime/CMakeLists.txt"
	// srcCoreMapsCMake is the second file that names GTest executables: the map channel's tests, which src/runtime/CMakeLists.txt includes.
	srcCoreMapsCMake = "src/core/maps.cmake"
	srcQuest         = "src/quest/"
	questPresets     = "src/quest/CMakePresets.json"
	questGroupsDir   = "src/quest/host_tests/groups"
)

// buildNinjaTarget is the ninja target `just build` builds in the mingw tree: everything, so every file it compiles is the
// build target's (the shipped DLLs, the reconstruction, the libovr stub, the test fixtures alike).
const buildNinjaTarget = "all"

// wholeTrees are the trees whose whole build ("all") a test id covers: a file the tree compiles is covered by the id.
var wholeTreeRows = []struct{ tree, recipe, id string }{
	{"android-arm64", "just build-android", "just:test-android"},
	{"scripting-mingw-shared", "just test-scripting", "just:test-scripting"},
	{"scripting-mingw-pervm", "just test-scripting", "just:test-scripting"},
	{"script-check", "just test-scripting", "just:test-scripting"},
}

// The submatch indices of addExecutableRe and the field index of a go list line's dependencies.
const (
	executableNameGroup = 1
	executableBodyGroup = 2
	goListDepsField     = 2
)

// The GTest executables are the add_executable blocks of src/runtime/CMakeLists.txt that name a test_ binary.
var (
	addExecutableRe = regexp.MustCompile(`add_executable\((test_[A-Za-z0-9_]+)\b([^)]*)\)`)
	sourceTokenRe   = regexp.MustCompile(`[A-Za-z0-9_./${}-]+\.(?:cpp|h|c)`)
	includeRe       = regexp.MustCompile(`#\s*include\s+"([^"]+)"`)
	// chainRe is a run of quoted parts joined by / or , that may spell a repository path: "docs", "standards", "x.md".
	chainRe      = regexp.MustCompile(`(?:["'][A-Za-z0-9_.-]+["']\s*[/,]\s*)+["'][A-Za-z0-9_.-]+["']`)
	quotedPartRe = regexp.MustCompile(`["']([A-Za-z0-9_.-]+)["']`)
)

// cSourceSuffixes are the files whose #include lines the closure follows.
var cSourceSuffixes = []string{".cpp", ".h", ".c", ".hpp", ".inc"}

// mapper computes what each tracked file is covered by. Each stage adds to cov; the stages are in cover.
type mapper struct {
	tc        toolchain
	log       *slog.Logger
	repo      string
	trees     []string // the directories searched for each build tree, in order
	tracked   []string
	known     map[string]bool
	recipes   map[string]bool
	deps      map[string]map[string][]string // by build directory: ninja -t deps
	exes      map[string]map[string]string   // by build directory: bin/*.exe file name to target
	goPkgs    []string
	rdeps     map[string]map[string]bool // package to the packages that depend on it (transitively)
	moduleRes map[string]*regexp.Regexp  // by tools/*.py stem that is unique in the tree
	incCache  map[string][]string
	cov       cover
	fallbacks []string
}

// newMapper reads the tracked files of repo.
func newMapper(tc toolchain, log *slog.Logger, repo string, trees []string) (*mapper, error) {
	out, err := tc.output(repo, "git", "ls-files", "-z")
	if err != nil {
		return nil, err
	}
	m := &mapper{tc: tc, log: log, repo: repo, trees: trees, known: map[string]bool{},
		deps: map[string]map[string][]string{}, exes: map[string]map[string]string{}, cov: cover{},
		incCache: map[string][]string{}}
	for _, p := range strings.Split(out, "\x00") {
		if p != "" {
			m.tracked = append(m.tracked, p)
			m.known[p] = true
		}
	}
	sort.Strings(m.tracked)
	return m, nil
}

// cover runs every stage, in the order a file's coverage is read.
func (m *mapper) cover() error {
	stages := []struct {
		name string
		run  func() error
	}{
		{"recipes", m.loadRecipes},
		{"gtest", m.gtests},
		{"build", m.build},
		{"quest", m.quest},
		{"whole-trees", m.wholeTrees},
		{"go", m.goPackages},
		{"references", m.references},
		{"fixed", m.fixed},
		{"include-closure", m.closure},
	}
	for _, s := range stages {
		if err := s.run(); err != nil {
			return err
		}
		m.log.Debug("stage", "name", s.name)
	}
	return nil
}

// tree returns the build tree named name from the first trees directory that holds it, or the failure that names the
// directories searched and the recipe that makes it.
func (m *mapper) tree(name, recipe string) (string, error) {
	for _, trees := range m.trees {
		dir := filepath.Join(trees, name)
		if st, err := os.Stat(dir); err == nil && st.IsDir() {
			return dir, nil
		}
	}
	return "", failure{exitCannotMake, fmt.Sprintf("build tree %s is in none of %s: run `%s` to make it", name, strings.Join(m.trees, ", "), recipe)}
}

// norm is the repository path of a path ninja printed: relative to the build directory when it is, made absolute, and
// mapped onto the root. "" when it is not a tracked file of the repository.
func (m *mapper) norm(builddir, p string) string {
	if p == "" {
		return ""
	}
	ap := p
	if !filepath.IsAbs(p) {
		ap = filepath.Join(builddir, p)
	}
	// A tree is <checkout>/build/<name>: the checkout it was built from is its parent's parent, whichever trees
	// directory it was found in, so a tree of another checkout maps onto this one's paths.
	rel, err := filepath.Rel(filepath.Dir(filepath.Dir(builddir)), filepath.Clean(ap))
	if err != nil || strings.HasPrefix(rel, "..") {
		return ""
	}
	rel = filepath.ToSlash(rel)
	if m.known[rel] {
		return rel
	}
	return ""
}

// depsOf is `ninja -t deps` of builddir, read once.
func (m *mapper) depsOf(builddir string) (map[string][]string, error) {
	if d, ok := m.deps[builddir]; ok {
		return d, nil
	}
	out, err := m.tc.ninja(builddir, "deps")
	if err != nil {
		return nil, err
	}
	m.deps[builddir] = parseDeps(out)
	return m.deps[builddir], nil
}

// exeTarget is the bin/ target of the executable named name in builddir, if the tree has one.
func (m *mapper) exeTarget(builddir, name string) (string, bool, error) {
	if e, ok := m.exes[builddir]; ok {
		t, found := e[name]
		return t, found, nil
	}
	out, err := m.tc.ninja(builddir, "targets", "all")
	if err != nil {
		return "", false, err
	}
	m.exes[builddir] = parseTargets(out)
	t, found := m.exes[builddir][name]
	return t, found, nil
}

// addFilesOf gives ids to every tracked file the target in builddir is built from: its inputs, and for each input its
// header dependencies from `ninja -t deps`.
func (m *mapper) addFilesOf(builddir, target string, ids ...string) error {
	out, err := m.tc.ninja(builddir, "inputs", target)
	if err != nil {
		return err
	}
	deps, err := m.depsOf(builddir)
	if err != nil {
		return err
	}
	for _, in := range lines(out) {
		m.addNorm(builddir, in, ids)
		for _, h := range deps[in] {
			m.addNorm(builddir, h, ids)
		}
	}
	return nil
}

// addNorm gives ids to the tracked file p names, when p names one.
func (m *mapper) addNorm(builddir, p string, ids []string) {
	if f := m.norm(builddir, p); f != "" {
		m.cov.add(f, ids...)
	}
}

func (m *mapper) loadRecipes() error {
	out, err := m.tc.output(m.repo, "just", "--summary")
	if err != nil {
		return err
	}
	m.recipes = map[string]bool{}
	for _, r := range strings.Fields(out) {
		m.recipes[r] = true
	}
	return nil
}

func (m *mapper) readRepo(rel string) (string, error) {
	data, err := os.ReadFile(filepath.Join(m.repo, filepath.FromSlash(rel)))
	if err != nil {
		return "", failure{exitCannotMake, err.Error()}
	}
	return string(data), nil
}

// gtests covers each GTest executable's files: its ninja inputs and their header dependencies. An executable the tree does
// not have (added since the last build) is covered by the sources its add_executable block names.
func (m *mapper) gtests() error {
	dir, err := m.tree(testmap.DefaultPreset, "just build")
	if err != nil {
		return err
	}
	var cmake string
	for _, file := range []string{srcRuntimeCMake, srcCoreMapsCMake} {
		if file != srcRuntimeCMake && !m.known[file] { // the map channel's file is optional: a checkout without it has no such tests
			continue
		}
		text, err := m.readRepo(file)
		if err != nil {
			return err
		}
		cmake += text + "\n"
	}
	for _, sub := range addExecutableRe.FindAllStringSubmatch(cmake, -1) {
		name, body := sub[executableNameGroup], sub[executableBodyGroup]
		id := testmap.KindGTest + ":" + name
		target, ok, err := m.exeTarget(dir, name+exeSuffix)
		if err != nil {
			return err
		}
		if ok {
			if err := m.addFilesOf(dir, target, id); err != nil {
				return err
			}
			continue
		}
		m.fallbacks = append(m.fallbacks, name)
		body = strings.ReplaceAll(body, "${CMAKE_CURRENT_SOURCE_DIR}/", "")
		body = strings.ReplaceAll(body, "${CMAKE_SOURCE_DIR}/", "")
		for _, tok := range sourceTokenRe.FindAllString(body, -1) {
			for _, cand := range []string{path.Join("src/runtime", tok), path.Join("src/core", tok), tok} {
				if m.known[cand] {
					m.cov.add(cand, id)
				}
			}
		}
	}
	return nil
}

// build covers every input of `just build`'s ninja target with the build target.
func (m *mapper) build() error {
	dir, err := m.tree(testmap.DefaultPreset, "just build")
	if err != nil {
		return err
	}
	return m.addFilesOf(dir, buildNinjaTarget, testmap.BuildTarget)
}

type questBuildPreset struct {
	Name            string   `json:"name"`
	ConfigurePreset string   `json:"configurePreset"`
	Targets         []string `json:"targets"`
}

// quest covers each Quest host group by the closure of its preset's targets, and each file a group's .cmake names.
func (m *mapper) quest() error {
	raw, err := m.readRepo(questPresets)
	if err != nil {
		return err
	}
	var presets struct {
		BuildPresets []questBuildPreset `json:"buildPresets"`
	}
	if err := json.Unmarshal([]byte(raw), &presets); err != nil {
		return failure{exitCannotMake, questPresets + ": " + err.Error()}
	}
	for _, bp := range presets.BuildPresets {
		if !strings.HasPrefix(bp.Name, questPrefix) || strings.HasSuffix(bp.Name, questPinnedSuffix) {
			continue
		}
		group := strings.TrimPrefix(bp.Name, questPrefix)
		recipe := questRecipePrefix + group
		if !m.recipes[recipe] {
			m.log.Debug("quest preset without a recipe", "preset", bp.Name)
			continue
		}
		dir, err := m.tree(bp.ConfigurePreset, "just "+recipe)
		if err != nil {
			return err
		}
		m.cov.add(questPresets, "just:"+recipe) // the presets configure every group's tree
		for _, t := range bp.Targets {
			if err := m.addFilesOf(dir, t, "just:"+recipe); err != nil {
				return err
			}
		}
	}
	groups, err := os.ReadDir(filepath.Join(m.repo, filepath.FromSlash(questGroupsDir)))
	if err != nil {
		return failure{exitCannotMake, err.Error()}
	}
	for _, e := range groups {
		if !strings.HasSuffix(e.Name(), ".cmake") {
			continue
		}
		text, err := m.readRepo(questGroupsDir + "/" + e.Name())
		if err != nil {
			return err
		}
		recipe := questRecipePrefix + strings.TrimSuffix(e.Name(), ".cmake")
		if !m.recipes[recipe] {
			m.log.Debug("quest group without a recipe", "group", e.Name())
			continue
		}
		id := "just:" + recipe
		m.cov.add(questGroupsDir+"/"+e.Name(), id) // the group's own file: changing it runs the group
		for _, f := range m.tracked {
			if rel, ok := strings.CutPrefix(f, srcQuest); ok && strings.Contains(text, rel) {
				m.cov.add(f, id)
			}
		}
	}
	return nil
}

// wholeTrees covers the files each whole tree compiles (its "all" target).
func (m *mapper) wholeTrees() error {
	for _, w := range wholeTreeRows {
		dir, err := m.tree(w.tree, w.recipe)
		if err != nil {
			return err
		}
		if err := m.addFilesOf(dir, "all", w.id); err != nil {
			return err
		}
	}
	return nil
}

// goPackages covers each package's own files and its testdata with go:<package>, and with each package that depends on it
// (transitively: go list's Deps), and the module's go.mod and go.sum with every package.
func (m *mapper) goPackages() error {
	out, err := m.tc.output(m.repo, "go", "-C", filepath.Join(m.repo, "tools"), "list", "-test", "-f", goListFormat, "./...")
	if err != nil {
		return err
	}
	pkgs := map[string]bool{}
	m.rdeps = map[string]map[string]bool{}
	for _, line := range lines(out) {
		fields := strings.SplitN(line, "\t", goListFields)
		if len(fields) != goListFields {
			return failure{exitCannotMake, "go list: an unexpected line: " + line}
		}
		ip := fields[0]
		if strings.Contains(ip, " ") || strings.HasSuffix(ip, ".test") || strings.HasSuffix(ip, "_test") || !strings.HasPrefix(ip, goModule) {
			continue
		}
		rel := strings.TrimPrefix(ip, goModule)
		pkgs[rel] = true
		for _, d := range strings.Fields(fields[goListDepsField]) {
			if dep, ok := strings.CutPrefix(d, goModule); ok {
				if m.rdeps[dep] == nil {
					m.rdeps[dep] = map[string]bool{}
				}
				m.rdeps[dep][rel] = true
			}
		}
	}
	for rel := range pkgs {
		m.goPkgs = append(m.goPkgs, rel)
	}
	sort.Strings(m.goPkgs)
	for _, rel := range m.goPkgs {
		reld := "tools/" + rel
		for _, f := range m.tracked {
			if path.Dir(f) == reld || strings.HasPrefix(f, reld+"/testdata/") {
				m.cov.add(f, "go:"+rel)
				for r := range m.rdeps[rel] {
					m.cov.add(f, "go:"+r)
				}
			}
		}
	}
	for _, f := range []string{"tools/go.mod", "tools/go.sum"} {
		for _, rel := range m.goPkgs {
			m.cov.add(f, "go:"+rel)
		}
	}
	return nil
}

// refs is the set of tracked files a text names: by its repository path; by its basename, when the basename is unique in
// the tree, is at least minUniqueBasename long, and appears quoted or after a slash; by a tracked directory with a slash
// in its path, quoted; or by a chain of quoted parts that spells a tracked path or directory.
func (m *mapper) refs(text string) map[string]bool {
	hit := map[string]bool{}
	baseCount := map[string]int{}
	for _, f := range m.tracked {
		baseCount[path.Base(f)]++
	}
	dirs := m.trackedDirs()
	for _, f := range m.tracked {
		if strings.Contains(text, f) {
			hit[f] = true
			continue
		}
		b := path.Base(f)
		if baseCount[b] == 1 && len(b) >= minUniqueBasename &&
			(strings.Contains(text, `"`+b+`"`) || strings.Contains(text, "'"+b+"'") || strings.Contains(text, "/"+b)) {
			hit[f] = true
		}
	}
	for d := range dirs {
		if strings.Contains(text, `"`+d+`"`) || strings.Contains(text, "'"+d+"'") {
			m.underDir(d, hit)
		}
	}
	for _, chain := range chainRe.FindAllString(text, -1) {
		var parts []string
		for _, p := range quotedPartRe.FindAllStringSubmatch(chain, -1) {
			parts = append(parts, p[1])
		}
		for i := range parts {
			p := strings.Join(parts[i:], "/")
			if m.known[p] {
				hit[p] = true
			} else if dirs[p] {
				m.underDir(p, hit)
			}
		}
	}
	return hit
}

// underDir adds every tracked file below dir to hit.
func (m *mapper) underDir(dir string, hit map[string]bool) {
	for _, q := range m.tracked {
		if strings.HasPrefix(q, dir+"/") {
			hit[q] = true
		}
	}
}

// trackedDirs is every directory that holds a tracked file or a directory of one, with a slash in its path.
func (m *mapper) trackedDirs() map[string]bool {
	dirs := map[string]bool{}
	for _, f := range m.tracked {
		for d := path.Dir(f); strings.Contains(d, "/"); d = path.Dir(d) {
			dirs[d] = true
		}
	}
	return dirs
}

// references covers each test and package by the files its text names, each tools/tests/test_*.py, each Go package
// under tools/, and the tools/*.py module a test imports when its stem is unique in the tree.
func (m *mapper) references() error {
	stems := map[string]int{}
	for _, f := range m.tracked {
		if strings.HasSuffix(f, ".py") {
			stems[strings.TrimSuffix(path.Base(f), ".py")]++
		}
	}
	m.moduleRes = map[string]*regexp.Regexp{}
	modules := map[string]string{}
	for _, f := range m.tracked {
		if path.Dir(f) == "tools" && strings.HasSuffix(f, ".py") && !strings.HasPrefix(path.Base(f), "test_") {
			stem := strings.TrimSuffix(path.Base(f), ".py")
			if stems[stem] == 1 {
				modules[stem] = f
				q := regexp.QuoteMeta(stem)
				m.moduleRes[stem] = regexp.MustCompile(`(?m)(^|\n)\s*(import|from)\s+(\w+\.)*` + q + `\b|import_module\(['"]` + q + `['"]|\b` + q + `\.py\b`)
			}
		}
	}
	for _, f := range m.tracked {
		if path.Dir(f) != "tools/tests" || !strings.HasPrefix(path.Base(f), "test_") || !strings.HasSuffix(f, ".py") {
			continue
		}
		text, err := m.readRepo(f)
		if err != nil {
			return err
		}
		id := testmap.KindPytest + ":" + strings.TrimSuffix(path.Base(f), ".py")
		m.cov.add(f, id)
		for h := range m.refs(text) {
			m.cov.add(h, id)
		}
		for stem, re := range m.moduleRes {
			if re.MatchString(text) {
				m.cov.add(modules[stem], id)
			}
		}
	}
	for _, rel := range m.goPkgs {
		reld := "tools/" + rel
		var text strings.Builder
		for _, f := range m.tracked {
			if path.Dir(f) == reld && strings.HasSuffix(f, ".go") {
				t, err := m.readRepo(f)
				if err != nil {
					return err
				}
				text.WriteString(t)
			}
		}
		for h := range m.refs(text.String()) {
			m.cov.add(h, testmap.KindGo+":"+rel)
		}
	}
	if m.known["tools/verify_log_rules.py"] {
		text, err := m.readRepo("tools/verify_log_rules.py")
		if err != nil {
			return err
		}
		for h := range m.refs(text) {
			m.cov.add(h, "log-rules")
		}
	}
	return nil
}

// fixed covers each file the fixed rules name: the rows of fixedRules.
func (m *mapper) fixed() error {
	for _, f := range m.tracked {
		for _, r := range fixedRules {
			if testmap.MatchGlob(r.glob, f) {
				m.cov.add(f, r.tests...)
			}
		}
	}
	return nil
}

// closure gives a tracked header the C++ ids of each covered file that includes it, to a fixpoint.
func (m *mapper) closure() error {
	for {
		changed := false
		for _, f := range m.tracked {
			var ids []string
			for _, id := range m.cov.ids(f) {
				if isCXXID(id) {
					ids = append(ids, id)
				}
			}
			if len(ids) == 0 {
				continue
			}
			for _, h := range m.includesOf(f) {
				for _, id := range ids {
					if !m.cov[h][id] {
						m.cov.add(h, id)
						changed = true
					}
				}
			}
		}
		if !changed {
			return nil
		}
	}
}

// includesOf is the tracked headers f includes with a quoted #include, resolved against src/, sdk/include/, src/quest/ and
// f's own directory (the first that is tracked wins).
func (m *mapper) includesOf(f string) []string {
	if got, ok := m.incCache[f]; ok {
		return got
	}
	var res []string
	if hasAnySuffix(f, cSourceSuffixes) {
		if text, err := os.ReadFile(filepath.Join(m.repo, filepath.FromSlash(f))); err == nil {
			for _, sub := range includeRe.FindAllStringSubmatch(string(text), -1) {
				inc := sub[1]
				for _, cand := range []string{"src/" + inc, "sdk/include/" + inc, srcQuest + inc, path.Join(path.Dir(f), inc)} {
					if m.known[cand] {
						res = append(res, cand)
						break
					}
				}
			}
		}
	}
	m.incCache[f] = res
	return res
}

func hasAnySuffix(s string, suffixes []string) bool {
	for _, x := range suffixes {
		if strings.HasSuffix(s, x) {
			return true
		}
	}
	return false
}

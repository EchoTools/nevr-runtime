// Command content-pack-build builds a content pack (docs/reference/content-packs.md) from a game install's own stock
// files: each step reads stock resources and scripts and says what the pack replaces or adds, and the build writes
// `<out>` (`<root>/content/<id>`) as content.json format 1, the format the runtime serves (src/runtime/content):
//
//	content-pack-build -game-dir <install>/echovr -out <root>/content/<id> -id <id> -version <x.y.z>
//
// The pack's game data is built from the player's own stock files on the machine that runs the build; nothing of the
// game's is in this repository (ADR 0028). A changed set is written as the stock manifest with the pack's resources
// added or replaced and one new package holding them (tools/internal/radpack); a script is written as
// scripts/<16 hex>.dll; a script copy a step names with Pack.Rekey is only listed, as `rekeys` in content.json (name, platform,
// stock_source, stock_sha256 of the stock file read), and carries no byte. The steps are the build's list (steps below); the EchoCombat pack's steps are added to it one
// slice at a time (#600). With no step that changes anything the build refuses: a pack that serves nothing is not made.
// A refusal exits 1 and names what failed, and -out is not written: the pack is written into a new folder beside -out
// and renamed to -out only when every file is in it, so a failed write leaves no -out. -out may not be inside -game-dir:
// nothing of ours is written into a game folder. An outside file a step reads is given as -input name=<file> (repeatable) and
// checked against the sha-256 the step pins (recipe_input.go). Each run is a record in the tool's run log.
package main

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"io/fs"
	"log/slog"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/owned"
	"github.com/EchoTools/nevr-runtime/tools/internal/radpack"
	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

// DataDir is the PC data folder the packs are built over, relative to the game folder.
const DataDir = "_data/5932408047/rad15/win10"

// Sets are the package sets a step may read and change: the game client's two (docs/engine/asset-system.md).
var Sets = []string{"48037dc70b0ecab2", "2b47aab238f60515"}

var (
	packID     = regexp.MustCompile(`^[a-z0-9][a-z0-9._-]{0,63}$`) // runtime/content/pack_rules.h, IsPackId
	version    = regexp.MustCompile(`^[\x21-\x7e]{1,64}$`)         // shown in the runtime's log; no quote or backslash
	scriptName = regexp.MustCompile(`^[0-9a-f]{16}$`)
)

// Pack is what a step sees and changes: the stock sets and scripts, and the pack's changes so far.
type Pack struct {
	gameDir string
	sets    map[string]*radpack.Set
	changes map[string]map[radpack.Key][]byte // set -> resource -> bytes
	scripts map[string][]byte                 // 16 hex -> DLL
	rekeys  []RekeyEntry                      // the copies a step named: listed, never carried (Rekey)

	gearIconsDir string            // -gear-icons-dir: he_is_the_cat's ICONS/hd, the gear icons' art (steps_gear_icons.go)
	inputFiles   map[string]string // -input name=path: the recipe inputs a step reads by name (recipe_input.go)
	step         string            // the step whose Apply is running: the step a pinned input names
	inputs       []PinnedInput     // the outside files a step read, each with the sha-256 it matched (ADR 0035 rule 4)
}

// Inputs are the outside inputs a build is given by flag. Each is read by one step, and a step whose input is absent refuses.
// GearIconsDir is -gear-icons-dir; Files is -input name=path, keyed by the name a step reads it under.
type Inputs struct {
	GearIconsDir string
	Files        map[string]string
}

// PinnedInput is one outside file a step read, with the sha-256 it matched. The build logs one `input_pinned` record for each
// and lists them in the summary line; content.json is unchanged.
type PinnedInput struct {
	Step   string `json:"step"`
	File   string `json:"file"`
	Sha256 string `json:"sha256"`
}

// RekeyPlatform is the platform a rekey entry names: Rekey reads the PC's stock scripts.
const RekeyPlatform = "windows-x86_64"

// RekeyEntry is one script copy a label asks the runtime to make: `name` is the copy, `stock_source` the stock script it is
// made from, and `stock_sha256` the sha-256 of that stock file as the build read it (docs/adr/0045, the label's `scripts` list;
// the entry shape is the manifest parser's, src/cdn/cdn_manifest.h). No script byte is part of it.
type RekeyEntry struct {
	Name        string `json:"name"`
	Platform    string `json:"platform"`
	StockSource string `json:"stock_source"`
	StockSha256 string `json:"stock_sha256"`
}

// Read is the pack's current bytes for a resource: the pack's own when a step changed it, else the stock one.
func (p *Pack) Read(set string, typ, name int64) ([]byte, error) {
	if b, ok := p.changes[set][radpack.Key{Type: typ, Name: name}]; ok {
		return b, nil
	}
	s, ok := p.sets[set]
	if !ok {
		return nil, fmt.Errorf("set %s is not one of the client's sets", set)
	}
	b, found, err := s.Read(typ, name)
	if err != nil {
		return nil, err
	}
	if !found {
		return nil, fmt.Errorf("set %s lists no resource %016x.%016x", set, uint64(typ), uint64(name))
	}
	return b, nil
}

// Put makes the pack serve `body` for a resource of `set`: a replacement when the stock set lists it, else an addition.
func (p *Pack) Put(set string, typ, name int64, body []byte) error {
	if _, ok := p.sets[set]; !ok {
		return fmt.Errorf("set %s is not one of the client's sets", set)
	}
	if len(body) == 0 {
		return fmt.Errorf("resource %016x.%016x of %s has no bytes", uint64(typ), uint64(name), set)
	}
	if p.changes[set] == nil {
		p.changes[set] = map[radpack.Key][]byte{}
	}
	p.changes[set][radpack.Key{Type: typ, Name: name}] = append([]byte(nil), body...)
	return nil
}

// ReadScript is the pack's current bytes for a script DLL: its own when a step changed it, else the stock one.
func (p *Pack) ReadScript(name string) ([]byte, error) {
	if b, ok := p.scripts[name]; ok {
		return b, nil
	}
	if !scriptName.MatchString(name) {
		return nil, fmt.Errorf("%q is not a script name (16 lowercase hex)", name)
	}
	return os.ReadFile(filepath.Join(p.gameDir, "bin", "win10", "scripts", name+".dll"))
}

// PutScript makes the pack serve `dll` as scripts/<name>.dll.
func (p *Pack) PutScript(name string, dll []byte) error {
	if !scriptName.MatchString(name) {
		return fmt.Errorf("%q is not a script name (16 lowercase hex)", name)
	}
	if len(dll) == 0 {
		return fmt.Errorf("script %s has no bytes", name)
	}
	p.scripts[name] = append([]byte(nil), dll...)
	return nil
}

// Rekey lists a copy of the stock script `stockSource` under the name `name`: it reads
// `<game>\bin\win10\scripts\<stockSource>.dll`, hashes it (sha-256) and records the entry content.json carries as `rekeys`.
// The copy is made on the player's machine from that stock file; no script byte goes into the pack, and the build writes no
// file for it.
func (p *Pack) Rekey(name, stockSource string) error {
	if !scriptName.MatchString(name) {
		return fmt.Errorf("%q is not a script name (16 lowercase hex)", name)
	}
	if !scriptName.MatchString(stockSource) {
		return fmt.Errorf("%q is not a stock script name (16 lowercase hex)", stockSource)
	}
	if name == stockSource {
		return fmt.Errorf("rekey %s names the stock script itself; a copy needs its own name", name)
	}
	if _, ok := p.scripts[name]; ok {
		return fmt.Errorf("rekey %s: the pack already carries a script of that name", name)
	}
	for _, r := range p.rekeys {
		if r.Name == name {
			return fmt.Errorf("rekey %s is listed twice", name)
		}
	}
	stock, err := os.ReadFile(filepath.Join(p.gameDir, "bin", "win10", "scripts", stockSource+".dll"))
	if err != nil {
		return fmt.Errorf("rekey %s: the stock script: %w", name, err)
	}
	if len(stock) == 0 {
		return fmt.Errorf("rekey %s: stock script %s has no bytes", name, stockSource)
	}
	p.rekeys = append(p.rekeys, RekeyEntry{Name: name, Platform: RekeyPlatform, StockSource: stockSource, StockSha256: digest(stock)})
	return nil
}

// Step is one change the build makes, from the stock files.
type Step struct {
	Name  string
	Apply func(p *Pack) error
	// Writes are the fields of the records the step changes, as the oracle test (oracle_test.go) compares them with
	// another build's output of the same records. A step compares only what it declares: a byte it does not declare
	// is not its claim.
	Writes []Field
	// PastGunPatch: the step changes a resource past the combat gun patch's files (stock_steps_test.go compares the gun
	// patch's sha-256 values with the pack as the steps that are not PastGunPatch leave it).
	PastGunPatch bool
	// After and Before name steps (by Step.Name) that run before and after this one. OrderFree names steps that write a
	// record this step also writes and whose relative order changes no output byte (the declaring step's own test shows
	// both orders). A step that writes a record another step writes declares one of the three (steps_order_test.go
	// enforces it).
	After, Before, OrderFree []string
}

// Field is one part of a record a step writes: the same part of each named record of Set and Type is read out by Read
// and compared. Read is the rule's own view of the record (a row, a byte range, a list's entries), so a field survives
// rows that move when the step adds one.
type Field struct {
	Name    string
	Set     string
	Type    uint64
	Records []uint64
	Read    func(record []byte) ([]byte, error)
	// Optional: the step changes this record only when the stock data holds what it works on (steps_mapterm.go: a
	// component table only when it owns a row of the REMATCH subtree). A record no step names as required may be absent
	// from the changes; a record that is changed is still named.
	Optional bool
}

// steps is the build's list, in order. The EchoCombat pack's steps join it one slice at a time (#600).
var steps []Step

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

func run(_ context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	list, err := productionSteps()
	if err != nil {
		fmt.Fprintf(stderr, "content-pack-build: %v\n", err)
		return 1
	}
	return runWith(list, args, getenv, stdout, stderr)
}

func runWith(list []Step, args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	return runlog.Run("content-pack-build", getenv, stderr, func(log *slog.Logger) int {
		flags := flag.NewFlagSet("content-pack-build", flag.ContinueOnError)
		flags.SetOutput(stderr)
		gameDir := flags.String("game-dir", "", "the game folder (holds _data and bin)")
		out := flags.String("out", "", "the pack's new folder, <root>/content/<id>")
		id := flags.String("id", "", "the pack's id")
		ver := flags.String("version", "", "the pack's version, shown in the runtime's log")
		gearIcons := flags.String("gear-icons-dir", "", "he_is_the_cat's ICONS/hd: the gear icons' art, read by name and checked against the pinned sha-256")
		inputs := inputFlag{}
		flags.Var(inputs, recipeInputFlag, "a recipe input, name=path, repeatable: a step reads the file by its name and checks its pinned sha-256")
		if err := flags.Parse(args); err != nil {
			return 2
		}
		if *gameDir == "" || *out == "" || *id == "" || *ver == "" {
			fmt.Fprintln(stderr, "content-pack-build: -game-dir, -out, -id and -version are required")
			return 2
		}
		result, err := Build(list, *gameDir, *out, *id, *ver, Inputs{GearIconsDir: *gearIcons, Files: inputs}, log)
		if err != nil {
			log.Warn("refused", "game_dir", *gameDir, "out", *out, "id", *id, "error", err.Error())
			fmt.Fprintf(stderr, "content-pack-build: %v\n", err)
			return 1
		}
		log.Info("pack_made", "out", *out, "id", *id, "version", *ver, "manifests", result.Manifests, "resources", result.Resources,
			"scripts", result.Scripts, "files", result.Files)
		line, _ := json.Marshal(result)
		fmt.Fprintln(stdout, string(line))
		return 0
	})
}

// Result is the build's summary line.
type Result struct {
	Event     string        `json:"event"`
	ID        string        `json:"id"`
	Manifests int           `json:"manifests"`
	Resources int           `json:"resources"`
	Scripts   int           `json:"scripts"`
	Rekeys    int           `json:"rekeys,omitempty"`
	Files     int           `json:"files"`
	Inputs    []PinnedInput `json:"inputs,omitempty"`
}

type manifestEntry struct {
	Package       string   `json:"package"`
	StockSha256   string   `json:"stock_sha256"`
	StockPackages uint32   `json:"stock_packages"`
	File          string   `json:"file"`
	Packages      []string `json:"packages"`
}

type contentJSON struct {
	Format    int               `json:"format"`
	ID        string            `json:"id"`
	Version   string            `json:"version"`
	DataDir   string            `json:"data_dir"`
	Manifests []manifestEntry   `json:"manifests"`
	Scripts   []string          `json:"scripts,omitempty"`
	Rekeys    []RekeyEntry      `json:"rekeys,omitempty"`
	Files     map[string]string `json:"files"`
}

func digest(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

// Build runs the steps over the stock install at gameDir and writes the pack to out, which must not exist. Nothing is
// written when a step fails or no step changes anything.
func Build(list []Step, gameDir, out, id, ver string, in Inputs, log *slog.Logger) (Result, error) {
	if !packID.MatchString(id) {
		return Result{}, fmt.Errorf("%q is not a pack id", id)
	}
	if !version.MatchString(ver) || strings.ContainsAny(ver, `"\`) {
		return Result{}, fmt.Errorf("%q is not a version the runtime can show", ver)
	}
	if filepath.Base(out) != id {
		return Result{}, fmt.Errorf("-out must be the pack's folder, named %s", id)
	}
	if inside, err := within(gameDir, out); err != nil {
		return Result{}, err
	} else if inside {
		return Result{}, fmt.Errorf("-out %s is inside -game-dir %s; nothing of ours is written into a game folder", out, gameDir)
	}
	if _, err := os.Lstat(out); !errors.Is(err, fs.ErrNotExist) {
		return Result{}, fmt.Errorf("%s exists; a pack is made into a new folder", out)
	}
	data := filepath.Join(gameDir, filepath.FromSlash(DataDir))
	p := &Pack{gameDir: gameDir, sets: map[string]*radpack.Set{}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}, gearIconsDir: in.GearIconsDir, inputFiles: in.Files}
	stockRaw := map[string][]byte{}
	for _, setID := range Sets {
		s, raw, err := radpack.OpenSet(data, setID)
		if err != nil {
			return Result{}, fmt.Errorf("stock set %s: %w", setID, err)
		}
		p.sets[setID], stockRaw[setID] = s, raw
	}
	for _, st := range list {
		p.step = st.Name
		if err := st.Apply(p); err != nil {
			return Result{}, fmt.Errorf("step %s: %w", st.Name, err)
		}
		log.Info("step_applied", "step", st.Name)
	}
	for _, pin := range p.inputs {
		log.Info("input_pinned", "step", pin.Step, "file", pin.File, "sha256", pin.Sha256)
	}
	res := Result{Event: "pack_made", ID: id, Inputs: p.inputs}
	if len(p.changes) == 0 && len(p.scripts) == 0 && len(p.rekeys) == 0 {
		return Result{}, errors.New("no step changes the stock data, so there is nothing to pack")
	}

	files := map[string][]byte{}
	cj := contentJSON{Format: 1, ID: id, Version: ver, DataDir: DataDir, Files: map[string]string{}}
	sets := make([]string, 0, len(p.changes))
	for s := range p.changes {
		sets = append(sets, s)
	}
	sort.Strings(sets)
	for _, set := range sets {
		stock := p.sets[set].Manifest
		manifest, pkg, err := radpack.Build(stock, p.changes[set])
		if err != nil {
			return Result{}, fmt.Errorf("set %s: %w", set, err)
		}
		mRel := "manifests/" + set
		pRel := fmt.Sprintf("packages/%s_%d", set, stock.PackageCount())
		files[mRel], files[pRel] = manifest, pkg
		cj.Manifests = append(cj.Manifests, manifestEntry{Package: set, StockSha256: digest(stockRaw[set]),
			StockPackages: stock.PackageCount(), File: mRel, Packages: []string{pRel}})
		res.Manifests++
		res.Resources += len(p.changes[set])
	}
	if len(cj.Manifests) == 0 {
		return Result{}, errors.New("the pack changes scripts only; content.json format 1 needs a manifest")
	}
	names := make([]string, 0, len(p.scripts))
	for n := range p.scripts {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, n := range names {
		rel := "scripts/" + n + ".dll"
		files[rel] = p.scripts[n]
		cj.Scripts = append(cj.Scripts, rel)
	}
	res.Scripts = len(names)
	cj.Rekeys = append(cj.Rekeys, p.rekeys...)
	sort.Slice(cj.Rekeys, func(i, j int) bool { return cj.Rekeys[i].Name < cj.Rekeys[j].Name })
	res.Rekeys = len(cj.Rekeys)
	for rel, b := range files {
		cj.Files[rel] = digest(b)
	}
	res.Files = len(files)
	text, err := json.MarshalIndent(cj, "", " ")
	if err != nil {
		return Result{}, err
	}
	files["content.json"] = append(text, '\n')
	// Written last, after every check, into a new folder beside -out that is renamed to -out once every file is in it: a
	// refusal or a failed write leaves no -out.
	if err := writeStaged(out, files); err != nil {
		return Result{}, err
	}
	return res, nil
}

// writeFile writes one file of the pack; a test replaces it to fail a write.
var writeFile = os.WriteFile

func writeStaged(out string, files map[string][]byte) (err error) {
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		return err
	}
	stage, err := os.MkdirTemp(filepath.Dir(out), "."+filepath.Base(out)+".partial-")
	if err != nil {
		return err
	}
	// The names this function makes under stage, recorded before each is made: a failure removes exactly these.
	var madeFiles, madeDirs []string
	defer func() {
		if err != nil {
			if rmErr := removeStaged(stage, madeFiles, madeDirs); rmErr != nil {
				err = fmt.Errorf("%w; the staging folder %s was not removed: %v", err, stage, rmErr)
			}
		}
	}()
	if err := os.Chmod(stage, 0o755); err != nil {
		return err
	}
	rels := make([]string, 0, len(files))
	for rel := range files {
		rels = append(rels, rel)
	}
	sort.Strings(rels)
	seen := map[string]bool{}
	for _, rel := range rels {
		parts := strings.Split(rel, "/")
		for i := 1; i < len(parts); i++ {
			if d := strings.Join(parts[:i], "/"); !seen[d] {
				seen[d] = true
				madeDirs = append(madeDirs, d)
			}
		}
	}
	for _, rel := range rels {
		path := filepath.Join(stage, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return err
		}
		madeFiles = append(madeFiles, rel)
		if err := writeFile(path, files[rel], 0o644); err != nil {
			return fmt.Errorf("write %s: %w", rel, err)
		}
	}
	// os.Rename refuses an existing directory at -out, so a folder made there while the pack was written is kept.
	return os.Rename(stage, out)
}

// removeStaged removes what writeStaged recorded making under stage, by name: the files, then the folders deepest
// first, then the staging folder itself when it is empty. A folder that holds anything else is kept and reported.
func removeStaged(stage string, files, dirs []string) error {
	sort.SliceStable(dirs, func(i, j int) bool { return strings.Count(dirs[i], "/") > strings.Count(dirs[j], "/") })
	entries := append(append([]string{}, files...), dirs...)
	var problems []string
	for _, r := range owned.RemoveOwned(stage, entries) {
		if r.Outcome != owned.Removed && r.Outcome != owned.Absent {
			problems = append(problems, fmt.Sprintf("%s: %s", r.Entry, r.Outcome))
		}
	}
	for _, r := range owned.RemoveOwned(filepath.Dir(stage), []string{filepath.Base(stage)}) {
		if r.Outcome != owned.Removed && r.Outcome != owned.Absent {
			problems = append(problems, fmt.Sprintf("%s: %s", filepath.Base(stage), r.Outcome))
		}
	}
	if len(problems) > 0 {
		return fmt.Errorf("kept: %s", strings.Join(problems, "; "))
	}
	return nil
}

// within says whether path is gameDir or inside it, links resolved: gameDir whole, and path's nearest existing
// ancestor, with the rest of path joined to it.
func within(gameDir, path string) (bool, error) {
	g, err := filepath.EvalSymlinks(gameDir)
	if err != nil {
		return false, fmt.Errorf("-game-dir %s: %w", gameDir, err)
	}
	if g, err = filepath.Abs(g); err != nil {
		return false, err
	}
	p, err := filepath.Abs(path)
	if err != nil {
		return false, err
	}
	rest := ""
	for {
		if r, err := filepath.EvalSymlinks(p); err == nil {
			p = filepath.Join(r, rest)
			break
		}
		parent := filepath.Dir(p)
		if parent == p {
			break
		}
		rest = filepath.Join(filepath.Base(p), rest)
		p = parent
	}
	rel, err := filepath.Rel(g, p)
	if err != nil {
		return false, nil
	}
	return filepath.IsLocal(rel), nil
}

// Command content-pack-build builds a content pack (docs/reference/content-packs.md) from a game install's own stock
// files: each step reads stock resources and scripts and says what the pack replaces or adds, and the build writes
// `<out>` (`<root>/content/<id>`) as content.json format 1, the format the runtime serves (src/runtime/content):
//
//	content-pack-build -game-dir <install>/echovr -out <root>/content/<id> -id <id> -version <x.y.z>
//
// The pack's game data is built from the player's own stock files on the machine that runs the build; nothing of the
// game's is in this repository (ADR 0028). A changed set is written as the stock manifest with the pack's resources
// added or replaced and one new package holding them (tools/internal/radpack); a script is written as
// scripts/<16 hex>.dll. The steps are the build's list (steps below); the EchoCombat pack's steps are added to it one
// slice at a time (#600). With no step that changes anything the build refuses: a pack that serves nothing is not made.
// A refusal exits 1 and names what failed, and -out is not written; each run is a record in the tool's run log.
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

// Step is one change the build makes, from the stock files.
type Step struct {
	Name  string
	Apply func(p *Pack) error
}

// steps is the build's list, in order. The EchoCombat pack's steps join it one slice at a time (#600).
var steps []Step

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

func run(_ context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	return runWith(steps, args, getenv, stdout, stderr)
}

func runWith(list []Step, args []string, getenv func(string) string, stdout, stderr io.Writer) int {
	return runlog.Run("content-pack-build", getenv, stderr, func(log *slog.Logger) int {
		flags := flag.NewFlagSet("content-pack-build", flag.ContinueOnError)
		flags.SetOutput(stderr)
		gameDir := flags.String("game-dir", "", "the game folder (holds _data and bin)")
		out := flags.String("out", "", "the pack's new folder, <root>/content/<id>")
		id := flags.String("id", "", "the pack's id")
		ver := flags.String("version", "", "the pack's version, shown in the runtime's log")
		if err := flags.Parse(args); err != nil {
			return 2
		}
		if *gameDir == "" || *out == "" || *id == "" || *ver == "" {
			fmt.Fprintln(stderr, "content-pack-build: -game-dir, -out, -id and -version are required")
			return 2
		}
		result, err := Build(list, *gameDir, *out, *id, *ver, log)
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
	Event     string `json:"event"`
	ID        string `json:"id"`
	Manifests int    `json:"manifests"`
	Resources int    `json:"resources"`
	Scripts   int    `json:"scripts"`
	Files     int    `json:"files"`
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
	Files     map[string]string `json:"files"`
}

func digest(b []byte) string {
	s := sha256.Sum256(b)
	return hex.EncodeToString(s[:])
}

// Build runs the steps over the stock install at gameDir and writes the pack to out, which must not exist. Nothing is
// written when a step fails or no step changes anything.
func Build(list []Step, gameDir, out, id, ver string, log *slog.Logger) (Result, error) {
	if !packID.MatchString(id) {
		return Result{}, fmt.Errorf("%q is not a pack id", id)
	}
	if !version.MatchString(ver) || strings.ContainsAny(ver, `"\`) {
		return Result{}, fmt.Errorf("%q is not a version the runtime can show", ver)
	}
	if filepath.Base(out) != id {
		return Result{}, fmt.Errorf("-out must be the pack's folder, named %s", id)
	}
	if _, err := os.Lstat(out); !errors.Is(err, fs.ErrNotExist) {
		return Result{}, fmt.Errorf("%s exists; a pack is made into a new folder", out)
	}
	data := filepath.Join(gameDir, filepath.FromSlash(DataDir))
	p := &Pack{gameDir: gameDir, sets: map[string]*radpack.Set{}, changes: map[string]map[radpack.Key][]byte{}, scripts: map[string][]byte{}}
	stockRaw := map[string][]byte{}
	for _, setID := range Sets {
		s, raw, err := radpack.OpenSet(data, setID)
		if err != nil {
			return Result{}, fmt.Errorf("stock set %s: %w", setID, err)
		}
		p.sets[setID], stockRaw[setID] = s, raw
	}
	for _, st := range list {
		if err := st.Apply(p); err != nil {
			return Result{}, fmt.Errorf("step %s: %w", st.Name, err)
		}
		log.Info("step_applied", "step", st.Name)
	}
	if len(p.changes) == 0 && len(p.scripts) == 0 {
		return Result{}, errors.New("no step changes the stock data, so there is nothing to pack")
	}

	files := map[string][]byte{}
	cj := contentJSON{Format: 1, ID: id, Version: ver, DataDir: DataDir, Files: map[string]string{}}
	res := Result{Event: "pack_made", ID: id}
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
	for rel, b := range files {
		cj.Files[rel] = digest(b)
	}
	res.Files = len(files)
	text, err := json.MarshalIndent(cj, "", " ")
	if err != nil {
		return Result{}, err
	}
	// Written last, after every check: a refusal leaves no folder.
	for rel, b := range files {
		path := filepath.Join(out, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return Result{}, err
		}
		if err := os.WriteFile(path, b, 0o644); err != nil {
			return Result{}, err
		}
	}
	if err := os.WriteFile(filepath.Join(out, "content.json"), append(text, '\n'), 0o644); err != nil {
		return Result{}, err
	}
	return res, nil
}

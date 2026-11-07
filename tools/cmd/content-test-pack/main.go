// Command content-test-pack makes a content pack (docs/reference/content-packs.md) from a game install's own stock files,
// for a test of the runtime's serving (#1424):
//
//	content-test-pack -game-dir <copy>/echovr -out <root>/content/<id> [-id testpack] [-set 48037dc70b0ecab2]
//
// The pack changes nothing the game does, and shows that each kind of file is served:
//
//   - the manifest of package set -set, rewritten with one more package: the package count is raised by one and one
//     zero-size chunk record, the end of the new package's chunk list (offset = the package's size), is appended to the
//     chunk table, so every resource record and every stock chunk is the stock one;
//   - that new package, <set>_<stock count>, 16 zero bytes no chunk reads;
//   - every stock script DLL under bin/win10/scripts, copied byte for byte.
//
// content.json lists each file with its sha-256 and the stock manifest's sha-256. Nothing of the game's is written anywhere
// but -out, which must not exist yet. A refusal exits 1 and names what failed; success prints one JSON line. Each run is a
// record in the tool's run log (internal/runlog).
package main

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/binary"
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

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

const (
	dataDir = "_data/5932408047/rad15/win10"
	// The manifest file: "ZSTD", u32 0x10, u64 decompressed length, u64 compressed length, then one zstd frame.
	headerSize = 24
	// Decompressed, three table descriptors of 0x40 bytes from 0x08 (resources, files, chunks; +0x08 the table's byte size,
	// +0x28 the element count, +0x30 the capacity), then the tables from 0xc0. Chunks are the last table.
	tablesStart     = 0xc0
	resourceDesc    = 0x08
	fileDesc        = 0x48
	chunkDesc       = 0x88
	chunkRecordSize = 0x10
	addedSize       = 16
)

var (
	packID     = regexp.MustCompile(`^[a-z0-9][a-z0-9._-]{0,63}$`) // runtime/content/pack_rules.h, IsPackId
	packageSet = regexp.MustCompile(`^[0-9a-f]{16}$`)
	scriptName = regexp.MustCompile(`^[0-9a-f]{16}\.dll$`)
)

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

func run(_ context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	return runlog.Run("content-test-pack", getenv, stderr, func(log *slog.Logger) int {
		flags := flag.NewFlagSet("content-test-pack", flag.ContinueOnError)
		flags.SetOutput(stderr)
		gameDir := flags.String("game-dir", "", "the game folder (holds _data and bin)")
		out := flags.String("out", "", "the pack's new folder, <root>/content/<id>")
		id := flags.String("id", "testpack", "the pack's id")
		set := flags.String("set", "48037dc70b0ecab2", "the package set whose manifest the pack replaces")
		if err := flags.Parse(args); err != nil {
			return 2
		}
		if *gameDir == "" || *out == "" {
			fmt.Fprintln(stderr, "content-test-pack: -game-dir and -out are required")
			return 2
		}
		result, err := build(*gameDir, *out, *id, *set)
		if err != nil {
			log.Warn("refused", "game_dir", *gameDir, "out", *out, "id", *id, "set", *set, "error", err.Error())
			fmt.Fprintf(stderr, "content-test-pack: %v\n", err)
			return 1
		}
		log.Info("pack_made", "out", result.Out, "id", result.ID, "stock_manifest_sha256", result.StockManifestSha256,
			"stock_packages", result.StockPackages, "scripts", result.Scripts, "files", result.Files)
		line, _ := json.Marshal(result)
		fmt.Fprintln(stdout, string(line))
		return 0
	})
}

type made struct {
	Event               string `json:"event"`
	ID                  string `json:"id"`
	Out                 string `json:"out"`
	StockManifestSha256 string `json:"stock_manifest_sha256"`
	StockPackages       uint32 `json:"stock_packages"`
	Scripts             int    `json:"scripts"`
	Files               int    `json:"files"`
}

func digest(b []byte) string {
	sum := sha256.Sum256(b)
	return hex.EncodeToString(sum[:])
}

// addOnePackage returns the stock manifest with one more package, and the stock package count.
func addOnePackage(manifest []byte) ([]byte, uint32, error) {
	if len(manifest) < headerSize || !bytes.Equal(manifest[:4], []byte("ZSTD")) ||
		binary.LittleEndian.Uint32(manifest[4:]) != 0x10 ||
		binary.LittleEndian.Uint64(manifest[16:]) != uint64(len(manifest)-headerSize) {
		return nil, 0, errors.New("the manifest's header is not ZSTD, 0x10 and its own length")
	}
	plainLength := binary.LittleEndian.Uint64(manifest[8:])
	dec, err := zstd.NewReader(nil, zstd.WithDecoderMaxMemory(1<<31))
	if err != nil {
		return nil, 0, err
	}
	defer dec.Close()
	plain, err := dec.DecodeAll(manifest[headerSize:], nil)
	if err != nil || uint64(len(plain)) != plainLength || len(plain) < tablesStart {
		return nil, 0, errors.New("the manifest does not decompress to the length its header names")
	}
	le := binary.LittleEndian
	packages := le.Uint32(plain[0:])
	chunkBytes := le.Uint64(plain[chunkDesc+0x08:])
	chunkCount, chunkCap := le.Uint64(plain[chunkDesc+0x28:]), le.Uint64(plain[chunkDesc+0x30:])
	if chunkBytes != chunkCount*chunkRecordSize || chunkCap != chunkCount {
		return nil, 0, errors.New("the chunk table's descriptor is not count * 16 bytes with capacity = count")
	}
	if tablesStart+le.Uint64(plain[resourceDesc+0x08:])+le.Uint64(plain[fileDesc+0x08:])+chunkBytes != uint64(len(plain)) {
		return nil, 0, errors.New("the three tables do not end where the manifest ends")
	}
	le.PutUint32(plain[0:], packages+1)
	le.PutUint64(plain[chunkDesc+0x08:], chunkBytes+chunkRecordSize)
	le.PutUint64(plain[chunkDesc+0x28:], chunkCount+1)
	le.PutUint64(plain[chunkDesc+0x30:], chunkCap+1)
	record := make([]byte, chunkRecordSize)
	le.PutUint32(record[0:], packages)  // the new package's index
	le.PutUint32(record[4:], addedSize) // its end: the package's size
	plain = append(plain, record...)
	enc, err := zstd.NewWriter(nil)
	if err != nil {
		return nil, 0, err
	}
	packed := enc.EncodeAll(plain, nil)
	enc.Close()
	header := make([]byte, headerSize)
	copy(header, "ZSTD")
	le.PutUint32(header[4:], 0x10)
	le.PutUint64(header[8:], uint64(len(plain)))
	le.PutUint64(header[16:], uint64(len(packed)))
	return append(header, packed...), packages, nil
}

func build(gameDir, out, id, set string) (made, error) {
	if !packID.MatchString(id) {
		return made{}, fmt.Errorf("%q is not a pack id", id)
	}
	if !packageSet.MatchString(set) {
		return made{}, fmt.Errorf("%q is not a package set (16 lowercase hex)", set)
	}
	if filepath.Base(out) != id {
		return made{}, fmt.Errorf("-out must be the pack's folder, named %s", id)
	}
	if _, err := os.Lstat(out); !errors.Is(err, fs.ErrNotExist) {
		return made{}, fmt.Errorf("%s exists; a pack is made into a new folder", out)
	}
	stockPath := filepath.Join(gameDir, filepath.FromSlash(dataDir), "manifests", set)
	stock, err := os.ReadFile(stockPath)
	if err != nil {
		return made{}, fmt.Errorf("no stock manifest at %s: %w", stockPath, err)
	}
	manifest, stockPackages, err := addOnePackage(stock)
	if err != nil {
		return made{}, err
	}
	scriptsDir := filepath.Join(gameDir, "bin", "win10", "scripts")
	entries, err := os.ReadDir(scriptsDir)
	if err != nil {
		return made{}, fmt.Errorf("no stock scripts at %s: %w", scriptsDir, err)
	}
	var scripts []string
	for _, e := range entries {
		if e.Type().IsRegular() && scriptName.MatchString(e.Name()) {
			scripts = append(scripts, e.Name())
		}
	}
	sort.Strings(scripts)
	if len(scripts) == 0 {
		return made{}, errors.New("no stock script under bin/win10/scripts")
	}

	files := map[string]string{}
	write := func(rel string, data []byte) error {
		path := filepath.Join(out, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return err
		}
		if err := os.WriteFile(path, data, 0o644); err != nil {
			return err
		}
		files[rel] = digest(data)
		return nil
	}
	manifestRel := "manifests/" + set
	packageRel := fmt.Sprintf("packages/%s_%d", set, stockPackages)
	if err := write(manifestRel, manifest); err != nil {
		return made{}, err
	}
	if err := write(packageRel, make([]byte, addedSize)); err != nil {
		return made{}, err
	}
	scriptRels := make([]string, 0, len(scripts))
	for _, name := range scripts {
		data, err := os.ReadFile(filepath.Join(scriptsDir, name))
		if err != nil {
			return made{}, err
		}
		rel := "scripts/" + name
		if err := write(rel, data); err != nil {
			return made{}, err
		}
		scriptRels = append(scriptRels, rel)
	}
	content := map[string]any{
		"format":   1,
		"id":       id,
		"name":     "serving test pack",
		"version":  "0.0.1",
		"data_dir": dataDir,
		"manifests": []map[string]any{{
			"package":        set,
			"stock_sha256":   digest(stock),
			"stock_packages": stockPackages,
			"file":           manifestRel,
			"packages":       []string{packageRel},
		}},
		"scripts": scriptRels,
		"files":   files,
	}
	text, err := json.MarshalIndent(content, "", " ")
	if err != nil {
		return made{}, err
	}
	if err := os.WriteFile(filepath.Join(out, "content.json"), append(text, '\n'), 0o644); err != nil {
		return made{}, err
	}
	return made{Event: "pack_made", ID: id, Out: out, StockManifestSha256: digest(stock), StockPackages: stockPackages,
		Scripts: len(scripts), Files: len(files)}, nil
}

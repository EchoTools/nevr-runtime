// Command cargo-extract turns a game data set into content-addressed zstd objects and a manifest in the schema of
// src/cdn/cdn_manifest.h (docs/adr/0045-cdn-texture-delivery.md): every resource of every package manifest of the
// set becomes one zstd frame of its own, named by the SHA-256 of the stored frame, and one manifest entry that says what
// the frame holds. The input is the game's own layout, read as the engine reads it (radpack: a resource is a record in a
// chunk, and a chunk is one zstd frame of a package file):
//
//	<root>/rad15/<platform dir>/manifests/<set>
//	<root>/rad15/<platform dir>/packages/<set>_<n>
//
//	cargo-extract extract -root <data root> -platform <android-arm64|windows-x86_64> -out <dir> [-level <name>] [-set <id>]...
//	cargo-extract verify <out> <data root>
//
// extract writes <out>/blobs/sha256/<hex> (an object already present is not rewritten) and <out>/manifest.json (one label,
// "game"), and prints a JSON summary on standard output. A record of 0 bytes is not an object: it is listed in the
// manifest's own `empty` list. The manifest records what cargo_manifest.h's parser would refuse and the caps it
// enforces are broken in the summary's `cargo_rules`, and never bends the data to fit them: the entries are the true
// ones, with no `stock_size`.
//
// Each frame is checked before it is written: it starts with the zstd magic, its header states the content size and it
// equals the record's size, its window is no larger than the largest window in the stock chunks' frame headers, the
// record's alignment is a power of two of at most 4096, and no (platform, type, name) appears twice. A record that fails
// is named on standard error and nothing more is written (no manifest).
//
// verify decompresses every object the manifest lists and compares it with the bytes the stock package holds for the
// same record; it stops at the first difference, names the record, and exits 1.
//
// Exit codes: 0 done; 1 refused (a rule, a difference, an unreadable input); 2 a usage error; 74 the run log failed
// (internal/runlog).
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"regexp"
	"strings"

	"github.com/klauspost/compress/zstd"

	"github.com/EchoTools/nevr-runtime/tools/internal/runlog"
)

func main() {
	os.Exit(run(context.Background(), os.Args[1:], os.Getenv, os.Stdin, os.Stdout, os.Stderr))
}

func run(ctx context.Context, args []string, getenv func(string) string, _ io.Reader, stdout, stderr io.Writer) int {
	return runlog.Run("cargo-extract", getenv, stderr, func(log *slog.Logger) int {
		return dispatch(ctx, args, stdout, stderr, log)
	})
}

const usage = `usage:
  cargo-extract extract -root <data root> -platform <android-arm64|windows-x86_64> -out <dir> [-level <name>] [-set <id>]...
  cargo-extract verify <out> <data root>`

func dispatch(ctx context.Context, args []string, stdout, stderr io.Writer, log *slog.Logger) int {
	if len(args) == 0 {
		fmt.Fprintln(stderr, usage)
		log.Error("usage", "err", "no command")
		return 2
	}
	switch args[0] {
	case "extract":
		o, err := parseExtract(args[1:], stderr)
		if err != nil {
			return usageError(err, stderr, log)
		}
		return finish(extractCommand(ctx, o, stdout, log), stderr, log)
	case "verify":
		if len(args) != 3 {
			return usageError(errors.New("verify takes <out> <data root>"), stderr, log)
		}
		return finish(verifyCommand(ctx, args[1], args[2], stdout, log), stderr, log)
	}
	return usageError(fmt.Errorf("%q is not a command", args[0]), stderr, log)
}

func usageError(err error, stderr io.Writer, log *slog.Logger) int {
	if !errors.Is(err, flag.ErrHelp) {
		fmt.Fprintln(stderr, "cargo-extract:", err)
	}
	fmt.Fprintln(stderr, usage)
	log.Error("usage", "err", err.Error())
	return 2
}

func finish(err error, stderr io.Writer, log *slog.Logger) int {
	if err == nil {
		return 0
	}
	fmt.Fprintln(stderr, "cargo-extract:", err)
	log.Error("refused", "err", err.Error())
	return 1
}

// platforms maps a cargo platform name (src/cdn/cdn_manifest.h) to the folder the game keeps that platform's
// manifests and packages in (rad15/<folder>): the PC build's is "win10" (docs/engine/asset-system.md, "Where assets
// live"), the Quest build's "android".
var platforms = map[string]string{
	"android-arm64":  "android",
	"windows-x86_64": "win10",
}

type multi []string

func (m *multi) String() string     { return strings.Join(*m, ",") }
func (m *multi) Set(v string) error { *m = append(*m, v); return nil }

var setID = regexp.MustCompile(`^[0-9a-f]{16}$`)

type extractOptions struct {
	root     string
	platform string
	out      string
	level    zstd.EncoderLevel
	sets     []string // empty: every manifest of the platform's folder
	// encode replaces the zstd encoder, for a test that needs a frame the encoder would never write.
	encode func(data []byte) []byte
	// workers is the number of records encoded at once; 0 is the default.
	workers int
}

func parseExtract(args []string, stderr io.Writer) (extractOptions, error) {
	fs := flag.NewFlagSet("cargo-extract extract", flag.ContinueOnError)
	fs.SetOutput(stderr)
	var o extractOptions
	var level string
	var sets multi
	fs.StringVar(&o.root, "root", "", "the data root: the folder that holds rad15/")
	fs.StringVar(&o.platform, "platform", "", "android-arm64 or windows-x86_64")
	fs.StringVar(&o.out, "out", "", "the output directory (blobs/sha256/ and manifest.json)")
	fs.StringVar(&level, "level", defaultLevel.String(), "zstd level: fastest, default, better or best")
	fs.Var(&sets, "set", "a package set id to extract (repeat for each; default: every manifest of the platform)")
	if err := fs.Parse(args); err != nil {
		return o, err
	}
	if fs.NArg() != 0 {
		return o, fmt.Errorf("unexpected argument %q", fs.Arg(0))
	}
	if o.root == "" || o.platform == "" || o.out == "" {
		return o, errors.New("-root, -platform and -out are required")
	}
	if _, ok := platforms[o.platform]; !ok {
		return o, fmt.Errorf("-platform %q is not android-arm64 or windows-x86_64", o.platform)
	}
	ok, l := zstd.EncoderLevelFromString(level)
	if !ok {
		return o, fmt.Errorf("-level %q is not fastest, default, better or best", level)
	}
	o.level = l
	for _, s := range sets {
		if !setID.MatchString(s) {
			return o, fmt.Errorf("-set %q is not 16 lower-case hex digits", s)
		}
		o.sets = append(o.sets, s)
	}
	return o, nil
}

func extractCommand(ctx context.Context, o extractOptions, stdout io.Writer, log *slog.Logger) error {
	sum, err := extract(ctx, o, log)
	if err != nil {
		return err
	}
	line, err := json.Marshal(sum)
	if err != nil {
		return err
	}
	fmt.Fprintln(stdout, string(line))
	return nil
}

func verifyCommand(ctx context.Context, out, root string, stdout io.Writer, log *slog.Logger) error {
	sum, err := verify(ctx, out, root, log)
	if err != nil {
		return err
	}
	line, err := json.Marshal(sum)
	if err != nil {
		return err
	}
	fmt.Fprintln(stdout, string(line))
	return nil
}

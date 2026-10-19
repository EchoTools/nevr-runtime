// Command symcache generates the embedded Echo VR symbol table from the pinned
// Dualgame/echo-symbols PC subset.
//
//	symcache regenerate --pin-file tools/symbols.pin --out src/runtime/log/symcache_data.cpp
//	symcache validate --input echo_vr-pc.json [--previous N]
//	symcache diff --old old.json --new new.json
//	symcache hash NAME...
//	symcache resolve 0x<16 hex>... [--data src/runtime/log/symcache_data.cpp]
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"
)

func main() {
	if err := run(os.Args[1:], os.Stdout, os.Stderr); err != nil {
		fmt.Fprintf(os.Stderr, "symcache: %v\n", err)
		os.Exit(1)
	}
}

func run(args []string, stdout, stderr io.Writer) error {
	if len(args) == 0 {
		usage(stderr)
		return errors.New("a command is required")
	}
	switch args[0] {
	case "regenerate":
		return runRegenerate(args[1:], stderr)
	case "validate":
		return runValidate(args[1:], stdout)
	case "diff":
		return runDiff(args[1:], stdout)
	case "hash":
		return runHash(args[1:], stdout)
	case "resolve":
		return runResolve(args[1:], stdout)
	case "pr-decision":
		return runPRDecision(args[1:], stdout)
	case "pr-cleanup":
		return runPRCleanup(args[1:], stdout)
	default:
		usage(stderr)
		return fmt.Errorf("unknown command %q", args[0])
	}
}

func usage(w io.Writer) {
	fmt.Fprintln(w, `usage: symcache <command> [flags]

commands:
  regenerate  fetch the corpus at the pinned commit, validate it, write the C++ table
  validate    validate a corpus JSON (parse, keys, strings, name hashes, count delta)
  diff        compare two corpus JSON files and print the added/removed/changed markdown
  hash        print CSymbol64 for each name
  resolve     resolve hashes through the generated C++ table
  pr-decision decide create/skip/update for the automation PR
  pr-cleanup  list automation branches whose PRs are merged or closed`)
}

func runRegenerate(args []string, stderr io.Writer) error {
	flags := flag.NewFlagSet("regenerate", flag.ContinueOnError)
	flags.SetOutput(stderr)
	pin := flags.String("pin", "", "echo-symbols commit sha (overrides --pin-file)")
	pinFile := flags.String("pin-file", "", "file holding the commit sha")
	input := flags.String("input", "", "local corpus JSON instead of fetching")
	baseURL := flags.String("base-url", "", "base URL for the pinned fetch")
	source := flags.String("source", "", "path within echo-symbols (default subsets/echo_vr-pc.json)")
	out := flags.String("out", "src/runtime/log/symcache_data.cpp", "generated file")
	previous := flags.Int("previous", 0, "entry count at the previous pin (0 = no check)")
	previousFrom := flags.String("previous-from", "", "read the previous count from a generated file")
	allowCountChange := flags.Bool("allow-count-change", false, "permit more than a 20% count change (baseline reset)")
	ifPinChanged := flags.Bool("if-pin-changed", false, "skip when the output already records this pin")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *previousFrom != "" {
		count, err := ReadGeneratedCount(*previousFrom)
		if err != nil {
			if !errors.Is(err, os.ErrNotExist) {
				return err
			}
			fmt.Fprintf(stderr, "symcache: previous count unavailable at %s (first pin)\n", *previousFrom)
		} else {
			*previous = count
		}
	}
	result, err := Regenerate(context.Background(), RegenerateOptions{
		Pin: *pin, PinFile: *pinFile, InputPath: *input, BaseURL: *baseURL,
		SourceName: *source, OutPath: *out, PreviousCount: *previous,
		AllowCountChange: *allowCountChange, IfPinChanged: *ifPinChanged,
	})
	if err != nil {
		return err
	}
	if result.Skipped {
		fmt.Fprintf(stderr, "symcache: %s already holds pin %s (%d entries)\n", result.OutPath, result.Pin, result.Count)
		return nil
	}
	fmt.Fprintf(stderr, "symcache: wrote %d entries to %s (previous %d, delta %+d)\n", result.Count, result.OutPath, result.Previous, result.Delta)
	return nil
}

func runValidate(args []string, stdout io.Writer) error {
	flags := flag.NewFlagSet("validate", flag.ContinueOnError)
	flags.SetOutput(stdout)
	input := flags.String("input", "", "corpus JSON")
	previous := flags.Int("previous", 0, "entry count at the previous pin (0 = no check)")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *input == "" {
		return errors.New("validate: --input is required")
	}
	file, err := os.Open(*input)
	if err != nil {
		return err
	}
	defer file.Close()
	corpus, err := LoadCorpus(file)
	if err != nil {
		return err
	}
	if err := ValidateCorpus(corpus, ValidateOptions{PreviousCount: *previous}); err != nil {
		return err
	}
	fmt.Fprintf(stdout, "symcache: %s: %d entries, all names hash to their keys\n", *input, len(corpus))
	return nil
}

func runDiff(args []string, stdout io.Writer) error {
	flags := flag.NewFlagSet("diff", flag.ContinueOnError)
	flags.SetOutput(stdout)
	oldPath := flags.String("old", "", "corpus JSON at the previous pin")
	newPath := flags.String("new", "", "corpus JSON at the new pin")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *oldPath == "" || *newPath == "" {
		return errors.New("diff: --old and --new are required")
	}
	oldCorpus, err := loadCorpusFile(*oldPath)
	if err != nil {
		return err
	}
	newCorpus, err := loadCorpusFile(*newPath)
	if err != nil {
		return err
	}
	fmt.Fprint(stdout, Diff(oldCorpus, newCorpus).Markdown())
	return nil
}

func runHash(args []string, stdout io.Writer) error {
	if len(args) == 0 {
		return errors.New("hash: at least one name is required")
	}
	for _, name := range args {
		fmt.Fprintf(stdout, "0x%016x\t%s\n", csymbol64(name), name)
	}
	return nil
}

func runResolve(args []string, stdout io.Writer) error {
	flags := flag.NewFlagSet("resolve", flag.ContinueOnError)
	flags.SetOutput(stdout)
	dataPath := flags.String("data", "src/runtime/log/symcache_data.cpp", "generated table")
	if err := flags.Parse(args); err != nil {
		return err
	}
	hashes := flags.Args()
	if len(hashes) == 0 {
		return errors.New("resolve: at least one hash is required")
	}
	file, err := os.Open(*dataPath)
	if err != nil {
		return err
	}
	defer file.Close()
	table, err := ParseGenerated(file)
	if err != nil {
		return err
	}
	for _, text := range hashes {
		raw := strings.TrimPrefix(strings.ToLower(text), "0x")
		hash, err := strconv.ParseUint(raw, 16, 64)
		if err != nil {
			return fmt.Errorf("resolve %q: %w", text, err)
		}
		name, found := table[hash]
		if !found {
			fmt.Fprintf(stdout, "0x%016x\t<unknown>\n", hash)
			continue
		}
		fmt.Fprintf(stdout, "0x%016x\t%s\n", hash, name)
	}
	return nil
}

func loadCorpusFile(path string) (Corpus, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	return LoadCorpus(file)
}

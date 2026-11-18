package main

// Recipe inputs: the outside files a step reads, each named by the -input name=path flag and pinned by sha-256 in the step
// (PLAN-v7 10.5: "recipe inputs by flag, sha-256 pinned"; ADR 0035 rule 4). The gear-icon art is the first precedent: its
// flag is a directory (-gear-icons-dir), because its eleven files share one folder and one pin table. Recipe inputs are
// a flag per file instead: the drone model, the sound files and the icons are in different folders of the author's tree, and
// a file-per-flag form names each one with the step's own name for it and no folder can reach a file the flag did not give.
//
// A step calls Pack.ReadInput(name, sha256). The name is a key the step owns, never a path: no name is joined onto a
// directory, so a step cannot read a file the flag did not name. The build refuses, naming the flag, when the flag is absent,
// when the file is not a regular file or is over maxRecipeInputBytes, and when its sha-256 is not the step's pin. A read
// input is recorded as a PinnedInput under the step that read it, and Build logs one input_pinned record for each.

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"regexp"
	"strings"
)

// maxRecipeInputBytes bounds one recipe input: the whole file is read into memory before its digest is compared. The package
// has no record limit of its own (the game's record sizes are the stock sets' and do not bound an outside file), so the bound
// is the tool's: 64 MiB, far above the largest input the plan names (Drone4.glb, 4435328 bytes, the gltf test's drone4Bytes)
// and far below what a mistaken path, such as a disk image, would load.
const maxRecipeInputBytes = 64 << 20

// recipeInputFlag is the flag's name; Build's usage text and docs/reference/content-packs.md name it the same way.
const recipeInputFlag = "input"

var (
	inputName = regexp.MustCompile(`^[a-z0-9][a-z0-9._-]{0,63}$`) // a step's name for its input, the flag's name part
	sha256Hex = regexp.MustCompile(`^[0-9a-f]{64}$`)              // a step's pin: the digest as digest() writes it
)

// inputFlag is the repeatable -input name=path flag: name is the step's name for the input, path the file it reads.
type inputFlag map[string]string

func (f inputFlag) String() string { return fmt.Sprint(map[string]string(f)) }

// Set takes one name=path. The name is checked here, so a malformed name exits 2 before any step runs; the path is checked
// when a step reads it, as a missing gear-icons file is.
func (f inputFlag) Set(v string) error {
	name, path, ok := strings.Cut(v, "=")
	if !ok || path == "" {
		return fmt.Errorf("-%s %q: want name=path", recipeInputFlag, v)
	}
	if !inputName.MatchString(name) {
		return fmt.Errorf("-%s %q: %q is not an input name (lowercase letters, digits, '.', '_' or '-')", recipeInputFlag, v, name)
	}
	if _, dup := f[name]; dup {
		return fmt.Errorf("-%s %s is given twice", recipeInputFlag, name)
	}
	f[name] = path
	return nil
}

// ReadInput returns the bytes of the file the -input flag named `name` gave, once its sha-256 is `sha` (64 lowercase hex
// digits, the step's pin). It refuses, writing nothing and recording nothing, when the flag is absent, the file is not a
// regular file or is over maxRecipeInputBytes, or the digest differs; the error names the flag and both digests.
func (p *Pack) ReadInput(name, sha string) ([]byte, error) {
	if !inputName.MatchString(name) {
		return nil, fmt.Errorf("%q is not an input name", name)
	}
	if !sha256Hex.MatchString(sha) {
		return nil, fmt.Errorf("input %s: the pinned sha-256 %q is not 64 lowercase hex digits", name, sha)
	}
	path, ok := p.inputFiles[name]
	if !ok {
		return nil, fmt.Errorf("-%s %s=<file> is not set: the recipe input %s is required, and a pack is not built without it", recipeInputFlag, name, name)
	}
	f, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("input %s: %w", name, err)
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return nil, fmt.Errorf("input %s: %w", name, err)
	}
	if !st.Mode().IsRegular() {
		return nil, fmt.Errorf("input %s: %s is not a regular file", name, path)
	}
	b, err := io.ReadAll(io.LimitReader(f, maxRecipeInputBytes+1))
	if err != nil {
		return nil, fmt.Errorf("input %s: %w", name, err)
	}
	if len(b) > maxRecipeInputBytes {
		return nil, fmt.Errorf("input %s: %s is over %d bytes", name, path, maxRecipeInputBytes)
	}
	got := digest(b)
	if got != sha {
		return nil, fmt.Errorf("input %s: %s sha256 is %s, the pinned value is %s", name, path, got, sha)
	}
	p.inputs = append(p.inputs, PinnedInput{Step: p.step, File: filepath.Base(path), Sha256: got})
	return b, nil
}

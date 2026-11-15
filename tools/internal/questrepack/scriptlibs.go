package questrepack

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// The script libraries (docs/plans/2026-10-31-combat-beta-local.md, M5): lib<hash16>.so files a combat build adds.

// isRedirected is true for lib<hash16>.so where hash16 is one of the nine redirected stock libraries.
func isRedirected(name string) bool {
	if !reLibName.MatchString(name) {
		return false
	}
	return contains(combatRedirectHashes, name[libPrefixLen:len(name)-soSuffixLen])
}

// stagedName is the name a script library is staged under: lib<hash16>_nevr.so for a redirected library, else its own.
func stagedName(name string) string {
	if isRedirected(name) {
		return name[:len(name)-len(".so")] + redirectSuffix + ".so"
	}
	return name
}

// isAArch64SharedObject is a 64-bit little-endian ELF whose machine is AArch64.
func isAArch64SharedObject(data []byte) bool {
	return len(data) >= elfMinHeaderLen && bytes.HasPrefix(data, []byte("\x7fELF")) && data[4] == elfClass64 &&
		data[5] == elfDataLE && binary.LittleEndian.Uint16(data[elfMachineOffset:]) == emAArch64
}

// StageLibsOptions are the inputs of stage-libs.
type StageLibsOptions struct {
	Libs     string // the libraries directory: lib<hash16>.so files, SHA256SUMS, and optionally REPLACE
	Stage    string
	StoreAPK string
	Out      io.Writer
}

// StageLibs stages the script libraries of opts.Libs under opts.Stage and returns the problems; it writes nothing when
// there are any. Every file is lib<hash16>.so, AArch64, and listed in SHA256SUMS with its sha-256; every listed name is
// there. A name the store APK also holds is staged only when REPLACE lists it.
func StageLibs(opts StageLibsOptions) ([]string, error) {
	info, err := os.Stat(opts.Libs)
	if err != nil || !info.IsDir() {
		return []string{fmt.Sprintf("%s: not a directory", opts.Libs)}, nil
	}
	sumsPath := filepath.Join(opts.Libs, libsSums)
	if si, err := os.Stat(sumsPath); err != nil || !si.Mode().IsRegular() {
		return []string{fmt.Sprintf("%s: missing (every script library is listed in it with its sha-256)", sumsPath)}, nil
	}
	var problems []string
	listed, err := readSums(sumsPath, &problems)
	if err != nil {
		return nil, err
	}
	names, dirProblems, err := libraryNames(opts.Libs)
	if err != nil {
		return nil, err
	}
	problems = append(problems, dirProblems...)
	replace, err := readReplaceList(filepath.Join(opts.Libs, libsReplace))
	if err != nil {
		return nil, err
	}
	store, err := openZip(opts.StoreAPK)
	if err != nil {
		return append(problems, fmt.Sprintf("%s: cannot be read as the store APK: %v", opts.StoreAPK, err)), nil
	}
	defer store.Close()
	storeHas := func(name string) bool { return store.has(name) }
	for _, name := range names {
		if !reLibName.MatchString(name) {
			problems = append(problems, fmt.Sprintf("%s: not named lib<hash16>.so; the recipe stages nothing else from here", name))
			continue
		}
		data, err := os.ReadFile(filepath.Join(opts.Libs, name))
		if err != nil {
			return nil, err
		}
		sum, isListed := listed[name]
		if !isListed {
			problems = append(problems, fmt.Sprintf("%s: not listed in %s", name, libsSums))
		} else if sha256Hex(data) != sum {
			problems = append(problems, fmt.Sprintf("%s: its sha-256 differs from the one %s lists", name, libsSums))
		}
		if !isAArch64SharedObject(data) {
			problems = append(problems, fmt.Sprintf("%s: not a 64-bit AArch64 shared object", name))
		}
		if isRedirected(name) {
			if !storeHas(abiDir + "/" + name) {
				problems = append(problems, fmt.Sprintf("%s: a redirected library, but the store APK holds no %s/%s for it to redirect",
					name, abiDir, name))
			}
			if contains(replace, name) {
				problems = append(problems, fmt.Sprintf("%s: a redirected library never replaces the store library: %s must not list it",
					name, libsReplace))
			}
			if storeHas(abiDir + "/" + stagedName(name)) {
				problems = append(problems, fmt.Sprintf("%s: the store APK already holds that name", stagedName(name)))
			}
		} else if storeHas(abiDir+"/"+name) && !contains(replace, name) {
			problems = append(problems, fmt.Sprintf("%s: the store APK holds %s/%s; it is staged only when %s lists it",
				name, abiDir, name, libsReplace))
		}
	}
	// The redirect is registered whole or not at all, so a set that holds some of the nine must hold every one.
	anyRedirected := false
	for _, name := range names {
		if isRedirected(name) {
			anyRedirected = true
		}
	}
	if anyRedirected {
		var absent []string
		for _, h := range combatRedirectHashes {
			if !contains(names, "lib"+h+".so") {
				absent = append(absent, "lib"+h+".so")
			}
		}
		if len(absent) > 0 {
			problems = append(problems, fmt.Sprintf("the %d redirected script libraries are staged all nine or none, and the "+
				"libraries directory lacks %s", redirectedCount, strings.Join(absent, ", ")))
		}
	}
	for _, name := range sortedKeys(listed) {
		if !contains(names, name) {
			problems = append(problems, fmt.Sprintf("%s: listed in %s and missing from the libraries directory", name, libsSums))
		}
	}
	for _, name := range replace {
		if !contains(names, name) || !storeHas(abiDir+"/"+name) {
			problems = append(problems, fmt.Sprintf("%s: %s lists it, but it is not a staged library that replaces a store library",
				name, libsReplace))
		}
	}
	if len(problems) > 0 {
		return problems, nil
	}
	target := filepath.Join(opts.Stage, abiDir)
	if err := os.MkdirAll(target, 0o755); err != nil {
		return nil, err
	}
	for _, name := range names {
		data, err := os.ReadFile(filepath.Join(opts.Libs, name))
		if err != nil {
			return nil, err
		}
		if err := os.WriteFile(filepath.Join(target, stagedName(name)), data, 0o644); err != nil {
			return nil, err
		}
		fmt.Fprintf(opts.Out, "quest_repack_libs: script library %s sha256 %s\n", stagedName(name), listed[name])
	}
	if err := writeLines(filepath.Join(opts.Stage, stagedReplace), sortedCopy(replace)); err != nil {
		return nil, err
	}
	staged := make([]string, 0, len(names))
	for _, n := range names {
		staged = append(staged, stagedName(n))
	}
	if err := writeLines(filepath.Join(opts.Stage, stagedScript), sortedCopy(staged)); err != nil {
		return nil, err
	}
	return nil, nil
}

// readSums parses SHA256SUMS: one `<sha-256>  <name>` line each. A line that is not one is a problem.
func readSums(path string, problems *[]string) (map[string]string, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	listed := map[string]string{}
	lines := strings.Split(string(raw), "\n")
	if n := len(lines); n > 0 && lines[n-1] == "" {
		lines = lines[:n-1] // a final newline ends the last line; it does not start another (Python's splitlines)
	}
	for _, line := range lines {
		line = strings.TrimSuffix(line, "\r")
		m := reSumsLine.FindStringSubmatch(line)
		if m == nil {
			*problems = append(*problems, fmt.Sprintf("%s: not a sha256sum line: %s", libsSums, pyRepr(line)))
			continue
		}
		listed[m[2]] = m[1]
	}
	return listed, nil
}

// libraryNames lists the libraries directory: every file but SHA256SUMS and REPLACE, sorted. A directory, a link or
// another special file is a problem.
func libraryNames(dir string) ([]string, []string, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, nil, err
	}
	var names, problems []string
	for _, e := range entries {
		if e.Name() == libsSums || e.Name() == libsReplace {
			continue
		}
		if !e.Type().IsRegular() {
			problems = append(problems, fmt.Sprintf("%s: not a regular file; the libraries directory holds files only", e.Name()))
			continue
		}
		names = append(names, e.Name())
	}
	sort.Strings(names)
	return names, problems, nil
}

// readReplaceList is the REPLACE file's names (empty when there is none).
func readReplaceList(path string) ([]string, error) {
	names, err := readLines(path)
	if os.IsNotExist(err) {
		return nil, nil
	}
	return names, err
}

// writeLines writes one line per value, each ended by a newline.
func writeLines(path string, values []string) error {
	var b strings.Builder
	for _, v := range values {
		b.WriteString(v)
		b.WriteString("\n")
	}
	return os.WriteFile(path, []byte(b.String()), 0o644)
}

func sortedCopy(values []string) []string {
	out := append([]string(nil), values...)
	sort.Strings(out)
	return out
}

func sortedKeys(m map[string]string) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	sort.Strings(out)
	return out
}

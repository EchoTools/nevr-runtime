package questrepack

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/EchoTools/nevr-runtime/tools/internal/questruntimepin"
)

// VerifyOptions are the inputs of verify-signed (docs/adr/0021 section 9, 0035, #1737).
type VerifyOptions struct {
	Source         string        // the store APK
	Signed         string        // the APK the recipe signed
	Stage          string        // the stage directory the recipe wrote
	Patcher        string        // the libr15 patcher, run on a copy of the store file
	ExtraRows      string        // the extra rows file, "" when none
	Scratch        string        // where the patcher's copy goes, "" for the system temp directory
	PlanHeader     string        // the data step's header (the pack's pins)
	PatcherTimeout time.Duration // how long the patcher may run; a hang is refused after it
	Out            io.Writer     // the digest lines
}

// VerifySigned returns the problems found in the signed APK; empty when every original library survived, libr15.so is
// the patcher's output for the store APK's, the reference runtime (when the source APK carries one) is pinned to the
// signed libr15.so and every staged file is there. It prints the three digests of libr15.so to Out.
func VerifySigned(opts VerifyOptions) ([]string, error) {
	original, err := openZip(opts.Source)
	if err != nil {
		return nil, err
	}
	defer original.Close()
	result, err := openZip(opts.Signed)
	if err != nil {
		return nil, err
	}
	defer result.Close()
	var problems []string
	replaced, err := replacedNames(opts.Stage)
	if err != nil {
		return nil, err
	}
	runtime := sourceIsReferenceRuntime(original)
	for _, entry := range libEntries(original) {
		if entry == replacedOnPurpose || entry == patchedLibrary || contains(replacedAbs(replaced), entry) ||
			(runtime && entry == pinnedLibrary) {
			continue
		}
		if !result.has(entry) {
			problems = append(problems, fmt.Sprintf("%s: in the store APK, missing from the signed APK", entry))
			continue
		}
		a, errA := original.entryDigest(entry)
		b, errB := result.entryDigest(entry)
		if errA != nil || errB != nil || a != b {
			if errA == nil && errB == nil {
				problems = append(problems, fmt.Sprintf("%s: the signed APK holds different bytes than the store APK", entry))
			} else {
				problems = append(problems, fmt.Sprintf("%s: cannot be compared with the store APK: %v %v", entry, errA, errB))
			}
		}
	}
	digests := map[string]string{}
	if !original.has(patchedLibrary) {
		problems = append(problems, fmt.Sprintf("%s: missing from the store APK, so there is nothing to patch", patchedLibrary))
	} else {
		patched, patchProblems := patchedDigest(original, opts, digests)
		problems = append(problems, patchProblems...)
		if !result.has(patchedLibrary) {
			problems = append(problems, fmt.Sprintf("%s: the patcher's output belongs here, missing from the signed APK", patchedLibrary))
		} else {
			signed, err := result.entryDigest(patchedLibrary)
			if err != nil {
				return nil, err
			}
			digests["signed"] = signed
			if patched != "" && signed != patched {
				problems = append(problems, fmt.Sprintf("%s: the signed APK does not hold the patcher's output for the store "+
					"APK's libr15.so (signed sha-256 %s, patcher output %s)", patchedLibrary, signed, patched))
			}
		}
		if patched != "" && patched == digests["store"] {
			problems = append(problems, fmt.Sprintf("%s: the patcher's output is the store APK's libr15.so, byte for byte: "+
				"nothing was patched", patchedLibrary))
		}
	}
	for _, label := range []string{"store", "patched", "signed"} {
		value, ok := digests[label]
		if !ok {
			value = "(not available)"
		}
		fmt.Fprintf(opts.Out, "quest_repack_libs: libr15.so sha256 %-7s %s\n", label, value)
	}
	if runtime {
		problems = append(problems, pinnedRuntimeProblems(original, result, opts.Out)...)
	}
	stagedFiles, err := stagedAbiFiles(opts.Stage)
	if err != nil {
		return nil, err
	}
	for _, name := range stagedFiles {
		entry := abiDir + "/" + name
		if !result.has(entry) {
			problems = append(problems, fmt.Sprintf("%s: staged, missing from the signed APK", entry))
			continue
		}
		signedDigest, err := result.entryDigest(entry)
		if err != nil {
			return nil, err
		}
		stageDigest, err := fileDigest(filepath.Join(opts.Stage, abiDir, name))
		if err != nil {
			return nil, err
		}
		if signedDigest != stageDigest {
			problems = append(problems, fmt.Sprintf("%s: the signed APK holds different bytes than the staged file", entry))
		}
	}
	problems = append(problems, pluginManifestProblems(opts.Stage)...)
	problems = append(problems, packProblems(opts.Stage, opts.PlanHeader)...)
	problems = append(problems, packSignedProblems(result, opts.Stage)...)
	info, err := os.Stat(opts.Signed)
	if err != nil {
		return nil, err
	}
	if info.Size() > apkLimitBytes {
		problems = append(problems, fmt.Sprintf("%s: %d bytes, over the 4 GiB a zip without zip64 holds (the sentinel's zip "+
			"reader refuses zip64)", filepath.Base(opts.Signed), info.Size()))
	}
	return problems, nil
}

// replacedAbs puts the stage's replaced names under the library directory, as the zip names them.
func replacedAbs(names []string) []string {
	out := make([]string, len(names))
	for i, n := range names {
		out[i] = abiDir + "/" + n
	}
	return out
}

// stagedAbiFiles are the regular files under the stage's library directory, sorted.
func stagedAbiFiles(stage string) ([]string, error) {
	entries, err := os.ReadDir(filepath.Join(stage, abiDir))
	if err != nil {
		return nil, err
	}
	var out []string
	for _, e := range entries {
		if e.Type().IsRegular() {
			out = append(out, e.Name())
		}
	}
	sort.Strings(out)
	return out, nil
}

// sourceIsReferenceRuntime reports whether the source APK's libvrapi.so carries the reference runtime's checksum list.
func sourceIsReferenceRuntime(original *zipArchive) bool {
	if !original.has(pinnedLibrary) {
		return false
	}
	data, err := original.readAll(pinnedLibrary)
	return err == nil && questruntimepin.CarriesTable(data)
}

// patchedDigest runs the patcher on a copy of the store APK's libr15.so, in a temporary directory that is removed, and
// returns the sha-256 of its output. digests receives the store file's. An empty result is a problem appended to the
// returned slice.
func patchedDigest(original *zipArchive, opts VerifyOptions, digests map[string]string) (string, []string) {
	tmp, err := os.MkdirTemp(opts.Scratch, "quest-repack-libr15-")
	if err != nil {
		return "", []string{fmt.Sprintf("%s: %v", patchedLibrary, err)}
	}
	defer os.RemoveAll(tmp)
	stock := filepath.Join(tmp, "stock-libr15.so")
	out := filepath.Join(tmp, "patched-libr15.so")
	report := filepath.Join(tmp, "report.json")
	source, err := original.readAll(patchedLibrary)
	if err != nil {
		return "", []string{fmt.Sprintf("%s: %v", patchedLibrary, err)}
	}
	if err := os.WriteFile(stock, source, 0o600); err != nil {
		return "", []string{fmt.Sprintf("%s: %v", patchedLibrary, err)}
	}
	storeDigest, err := fileDigest(stock)
	if err != nil {
		return "", []string{fmt.Sprintf("%s: %v", patchedLibrary, err)}
	}
	digests["store"] = storeDigest
	patcher, err := filepath.Abs(opts.Patcher)
	if err != nil {
		return "", []string{fmt.Sprintf("%s: the patcher %s did not run to its end: %v", patchedLibrary, opts.Patcher, err)}
	}
	argv := []string{"--stock", stock, "--out", out, "--report", report}
	if opts.ExtraRows != "" {
		argv = append(argv, "--extra-rows", opts.ExtraRows)
	}
	ctx, cancel := context.WithTimeout(context.Background(), opts.PatcherTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, patcher, argv...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	runErr := cmd.Run()
	if ctx.Err() != nil {
		return "", []string{fmt.Sprintf("%s: the patcher %s did not run to its end: %v", patchedLibrary, opts.Patcher, ctx.Err())}
	}
	var exitErr *exec.ExitError
	if errors.As(runErr, &exitErr) {
		return "", []string{fmt.Sprintf("%s: the patcher %s exited %d on the store APK's libr15.so: %s", patchedLibrary,
			opts.Patcher, exitErr.ExitCode(), tail(strings.TrimSpace(stderr.String()), probTail))}
	}
	if runErr != nil {
		return "", []string{fmt.Sprintf("%s: the patcher %s did not run to its end: %v", patchedLibrary, opts.Patcher, runErr)}
	}
	info, err := os.Stat(out)
	if err != nil || !info.Mode().IsRegular() || info.Size() == 0 {
		return "", []string{fmt.Sprintf("%s: the patcher %s exited 0 and wrote no file", patchedLibrary, opts.Patcher)}
	}
	patchedSum, err := fileDigest(out)
	if err != nil {
		return "", []string{fmt.Sprintf("%s: %v", patchedLibrary, err)}
	}
	digests["patched"] = patchedSum
	return patchedSum, nil
}

// tail is the last n bytes of s, as Python's s[-n:] gives them.
func tail(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[len(s)-n:]
}

// pinnedRuntimeProblems checks the signed APK's libvrapi.so: it must be the pin of the source APK's libvrapi.so to the
// SIGNED libr15.so, byte for byte. Prints the three digests to out.
func pinnedRuntimeProblems(original, result *zipArchive, out io.Writer) []string {
	if !result.has(pinnedLibrary) {
		return []string{fmt.Sprintf("%s: the pinned runtime belongs here, missing from the signed APK", pinnedLibrary)}
	}
	if !result.has(patchedLibrary) {
		return []string{fmt.Sprintf("%s: cannot be checked, the signed APK has no %s to pin it to", pinnedLibrary, patchedLibrary)}
	}
	signedMD5, err := result.entryMD5(patchedLibrary)
	if err != nil {
		return []string{fmt.Sprintf("%s: %v", patchedLibrary, err)}
	}
	source, err := original.readAll(pinnedLibrary)
	if err != nil {
		return []string{fmt.Sprintf("%s: %v", pinnedLibrary, err)}
	}
	expected, _, err := questruntimepin.PinData(source, signedMD5, 0, questruntimepin.StockEntryMD5)
	if err != nil {
		return []string{fmt.Sprintf("%s: the source APK's runtime cannot be pinned to the signed libr15.so (md5 %s): %v",
			pinnedLibrary, signedMD5, err)}
	}
	signed, err := result.readAll(pinnedLibrary)
	if err != nil {
		return []string{fmt.Sprintf("%s: %v", pinnedLibrary, err)}
	}
	fmt.Fprintf(out, "quest_repack_libs: libvrapi.so sha256 source  %s\n", questruntimepin.Sha256Hex(source))
	fmt.Fprintf(out, "quest_repack_libs: libvrapi.so sha256 pinned  %s (libr15.so md5 %s)\n",
		questruntimepin.Sha256Hex(expected), signedMD5)
	fmt.Fprintf(out, "quest_repack_libs: libvrapi.so sha256 signed  %s\n", questruntimepin.Sha256Hex(signed))
	if bytes.Equal(signed, expected) {
		return nil
	}
	if bytes.Equal(signed, source) {
		return []string{fmt.Sprintf("%s: the signed APK holds the source APK's runtime unpinned: its modules would refuse "+
			"the signed libr15.so (md5 %s)", pinnedLibrary, signedMD5)}
	}
	return []string{fmt.Sprintf("%s: the signed APK does not hold the source APK's runtime pinned to the signed libr15.so "+
		"(md5 %s): another libr15 or another edit", pinnedLibrary, signedMD5)}
}

// replacedNames are the store libraries the staged script libraries replace, as the stage recorded them (empty when none).
func replacedNames(stage string) ([]string, error) {
	names, err := readLines(filepath.Join(stage, stagedReplace))
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	return names, err
}

// stagedScriptLibs are the file names stage-libs staged under lib/arm64-v8a, as the stage recorded them (empty when none).
func stagedScriptLibs(stage string) []string {
	names, err := readLines(filepath.Join(stage, stagedScript))
	if err != nil {
		return nil
	}
	return names
}

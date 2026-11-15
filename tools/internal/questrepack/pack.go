package questrepack

import (
	"archive/zip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
)

// The rules of the runtime's pack (src/quest/data/pack.cpp ParsePackJson, src/quest/data/zip.cpp SafeEntryName). Python's
// `$` also matches before a final newline, so each pattern ends `\n?\z` to keep that.
var (
	reHex64        = regexp.MustCompile(`^[0-9a-f]{64}\n?\z`)
	rePackID       = regexp.MustCompile(`^[a-z0-9][a-z0-9._-]{0,63}\n?\z`)
	rePackVersion  = regexp.MustCompile(`^[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}\n?\z`)
	rePackFileName = regexp.MustCompile(`^[A-Za-z0-9._-]{1,64}\n?\z`)
	reZipPrefix    = regexp.MustCompile(`^_data/([A-Za-z0-9._-]{1,32})/rad15/android/\n?\z`)
	reManifestPin  = regexp.MustCompile(`kManifestPath\s*=\s*"_data/([A-Za-z0-9._-]+)/rad15/android/manifests/([A-Za-z0-9._-]+)"`)
	reLibName      = regexp.MustCompile(`^lib[0-9a-f]{16}\.so\n?\z`)
	reSumsLine     = regexp.MustCompile(`^([0-9a-f]{64})  (\S+)$`)
)

// reservedSuffixes are names the staging step writes for itself.
var reservedSuffixes = []string{".nevr-pack.tmp", ".stock", ".nevr-owned"}

// packMeta is the description of a pack (its meta file), as the JSON object the pack step reads.
type packMeta map[string]any

// StagePackOptions are the inputs of stage-pack.
type StagePackOptions struct {
	PackZip    string
	Meta       string
	Stage      string
	StoreAPK   string
	PlanHeader string
	Out        io.Writer
}

// pinnedManifest is (version folder, manifest id) of the data step's kManifestPath, read from planHeader; ok is false
// when the header names none (or cannot be read).
func pinnedManifest(planHeader string) (version, manifest string, ok bool) {
	raw, err := os.ReadFile(planHeader)
	if err != nil {
		return "", "", false
	}
	m := reManifestPin.FindSubmatch(raw)
	if m == nil {
		return "", "", false
	}
	return string(m[1]), string(m[2]), true
}

// safePackEntryName is the sentinel's zip reader's rule: a relative path of plain components.
func safePackEntryName(name string) bool {
	if name == "" || len(name) > maxEntryNameLen || strings.HasPrefix(name, "/") {
		return false
	}
	body := strings.TrimSuffix(name, "/")
	if body == "" {
		return false
	}
	parts := strings.Split(body, "/")
	if len(parts) > maxEntryParts {
		return false
	}
	for _, part := range parts {
		if part == "" || part == "." || part == ".." || len(part) > maxComponentLen {
			return false
		}
		for _, r := range part {
			if r == '\\' || r < asciiControlMax || r == ascii7DEL {
				return false
			}
		}
		if runes := []rune(part); len(runes) >= 2 && runes[1] == ':' {
			return false
		}
	}
	return true
}

// isPackPath is "" when path is a file the runtime places (manifests/<the game's manifest> or packages/<name>), else the
// reason it is not.
func isPackPath(path, manifestID string) string {
	directory, name, found := strings.Cut(path, "/")
	if (directory != "manifests" && directory != "packages") || !found || name == "" {
		return fmt.Sprintf("%s: not a file directly in manifests/ or packages/", path)
	}
	if !rePackFileName.MatchString(name) || strings.HasPrefix(name, ".") {
		return fmt.Sprintf("%s: not a plain file name", path)
	}
	for _, suffix := range reservedSuffixes {
		if strings.HasSuffix(name, suffix) {
			return fmt.Sprintf("%s: a name the step writes for itself", path)
		}
	}
	if directory == "manifests" && name != manifestID {
		return fmt.Sprintf("%s: the only manifest a pack may carry is manifests/%s", path, manifestID)
	}
	return ""
}

// packMetaProblems are the problems with the pack's description, and its sorted requires list.
func packMetaProblems(meta packMeta, versionFolder string) ([]string, []string) {
	var problems []string
	if id, ok := meta["id"].(string); !ok || !rePackID.MatchString(id) {
		problems = append(problems, "meta: id must match [a-z0-9][a-z0-9._-]{0,63}")
	}
	if version, ok := meta["version"].(string); !ok || !rePackVersion.MatchString(version) {
		problems = append(problems, "meta: version must be X.Y.Z")
	}
	requires, ok := stringList(meta["requires_stock_manifest_sha256"])
	if !ok || len(requires) < 1 || len(requires) > maxRequiresSums || !allMatch(requires, reHex64) {
		problems = append(problems, "meta: requires_stock_manifest_sha256 must be one or two lowercase sha-256 hex digests")
		requires = nil
	} else if len(uniqueStrings(requires)) != len(requires) {
		problems = append(problems, "meta: requires_stock_manifest_sha256 names a digest twice")
	}
	prefix, isStr := meta["zip_prefix"].(string)
	var match []string
	if isStr {
		if m := reZipPrefix.FindStringSubmatch(prefix); m != nil {
			match = m
		}
	}
	if match == nil {
		problems = append(problems, "meta: zip_prefix must be _data/<version>/rad15/android/")
	} else if match[1] != versionFolder {
		problems = append(problems, fmt.Sprintf("meta: zip_prefix names data version %s, the data step's kManifestPath says %s",
			match[1], versionFolder))
	}
	sorted := append([]string(nil), requires...)
	sort.Strings(sorted)
	return problems, sorted
}

// stringList is the value as a list of strings; ok is false when it is not a list of strings.
func stringList(v any) ([]string, bool) {
	raw, ok := v.([]any)
	if !ok {
		return nil, false
	}
	out := make([]string, 0, len(raw))
	for _, item := range raw {
		s, ok := item.(string)
		if !ok {
			return nil, false
		}
		out = append(out, s)
	}
	return out, true
}

func allMatch(values []string, re *regexp.Regexp) bool {
	for _, v := range values {
		if !re.MatchString(v) {
			return false
		}
	}
	return true
}

func uniqueStrings(values []string) map[string]bool {
	set := map[string]bool{}
	for _, v := range values {
		set[v] = true
	}
	return set
}

// packFile is what the zip scan records for one file of the pack: the digest of the bytes read and their size.
type packFile struct {
	sha256 string
	size   uint64
}

// packZipScan reads every entry of packZip: the problems, and the files below prefix by their path.
func packZipScan(packZip, prefix, manifestID string) ([]string, map[string]packFile) {
	found := map[string]packFile{}
	rc, err := zip.OpenReader(packZip)
	if err != nil {
		return []string{fmt.Sprintf("%s: not a readable zip archive: %v", packZip, err)}, found
	}
	defer rc.Close()
	var problems []string
	folded := map[string]string{}
	for _, info := range rc.File {
		name := info.Name
		if !safePackEntryName(name) {
			problems = append(problems, fmt.Sprintf("%s: an entry name the sentinel's zip reader refuses", pyRepr(name)))
			continue
		}
		key := strings.ToLower(strings.TrimSuffix(name, "/"))
		if other, dup := folded[key]; dup {
			problems = append(problems, fmt.Sprintf("%s: the same name as %s (names that differ only in case are one name)", name, other))
		}
		folded[key] = name
		if info.Flags&zipEncryptedBit != 0 {
			problems = append(problems, fmt.Sprintf("%s: encrypted", name))
		}
		if info.CreatorVersion>>8 == zipCreatorUnix {
			mode := info.ExternalAttrs >> zipModeShift // a mode with no file type bits is a plain file
			if t := mode & unixModeTypeMask; t != 0 && t != unixModeRegular && t != unixModeDirectory {
				problems = append(problems, fmt.Sprintf("%s: a link or another special file", name))
			}
		}
		if strings.HasSuffix(name, "/") {
			if !(strings.HasPrefix(prefix, name) || strings.HasPrefix(name, prefix)) {
				problems = append(problems, fmt.Sprintf("%s: a directory outside %s", name, prefix))
			}
			continue
		}
		if !strings.HasPrefix(name, prefix) {
			problems = append(problems, fmt.Sprintf("%s: outside %s", name, prefix))
			continue
		}
		relative := name[len(prefix):]
		if reason := isPackPath(relative, manifestID); reason != "" {
			problems = append(problems, reason)
			continue
		}
		digest, size, err := readDigest(info)
		if err != nil {
			problems = append(problems, fmt.Sprintf("%s: cannot be read: %v", name, err))
			continue
		}
		if size != info.UncompressedSize64 {
			problems = append(problems, fmt.Sprintf("%s: the zip records %d bytes, %d were read", name, info.UncompressedSize64, size))
		}
		if size == 0 || size > maxPackFileBytes {
			problems = append(problems, fmt.Sprintf("%s: %d bytes (a pack file is 1 byte to %d)", name, size, maxPackFileBytes))
		}
		found[relative] = packFile{sha256: digest, size: size}
	}
	if len(found) > maxPackFiles {
		problems = append(problems, fmt.Sprintf("%d files: a pack carries at most %d", len(found), maxPackFiles))
	}
	if _, ok := found["manifests/"+manifestID]; !ok {
		problems = append(problems, fmt.Sprintf("the zip has no %smanifests/%s", prefix, manifestID))
	}
	return problems, found
}

// readDigest reads one zip entry: the sha-256 and the size of the bytes read.
func readDigest(info *zip.File) (string, uint64, error) {
	rc, err := info.Open()
	if err != nil {
		return "", 0, err
	}
	defer rc.Close()
	h := sha256.New()
	n, err := io.Copy(h, rc)
	if err != nil {
		return "", 0, err
	}
	return hex.EncodeToString(h.Sum(nil)), uint64(n), nil
}

// packJSON is pack.json: schema 1, sorted keys, no spaces, one trailing newline.
func packJSON(meta packMeta, requires []string, versionFolder string, files map[string]packFile, zipSHA string, zipSize int64) ([]byte, error) {
	paths := make([]string, 0, len(files))
	for p := range files {
		paths = append(paths, p)
	}
	sort.Strings(paths)
	entries := make([]map[string]any, 0, len(paths))
	for _, p := range paths {
		entries = append(entries, map[string]any{"path": p, "sha256": files[p].sha256, "size": files[p].size})
	}
	return marshalSorted(map[string]any{
		"schema":                         packSchema,
		"id":                             meta["id"],
		"version":                        meta["version"],
		"stock_data_version":             versionFolder,
		"requires_stock_manifest_sha256": requires,
		"files":                          entries,
		"source_zip_sha256":              zipSHA,
		"source_zip_size":                zipSize,
	})
}

// StagePack stages the pack under opts.Stage as APK assets and returns the problems; it writes nothing when there are any.
func StagePack(opts StagePackOptions) ([]string, error) {
	version, manifestID, ok := pinnedManifest(opts.PlanHeader)
	if !ok {
		return []string{fmt.Sprintf("%s: names no kManifestPath, so the pack cannot be checked against the data step's pins",
			opts.PlanHeader)}, nil
	}
	raw, err := os.ReadFile(opts.Meta)
	if err != nil {
		return []string{fmt.Sprintf("%s: cannot be read as a JSON object: %v", opts.Meta, err)}, nil
	}
	var meta packMeta
	if err := json.Unmarshal(raw, &meta); err != nil || meta == nil {
		reason := "not an object"
		if err != nil {
			reason = err.Error()
		}
		return []string{fmt.Sprintf("%s: cannot be read as a JSON object: %s", opts.Meta, reason)}, nil
	}
	problems, requires := packMetaProblems(meta, version)
	if len(problems) > 0 {
		return problems, nil
	}
	prefix := meta["zip_prefix"].(string)
	scanProblems, files := packZipScan(opts.PackZip, prefix, manifestID)
	problems = append(problems, scanProblems...)
	store, err := openZip(opts.StoreAPK)
	if err != nil {
		return append(problems, fmt.Sprintf("%s: cannot be read as the store APK: %v", opts.StoreAPK, err)), nil
	}
	defer store.Close()
	storeInfo, err := os.Stat(opts.StoreAPK)
	if err != nil {
		return append(problems, fmt.Sprintf("%s: cannot be read as the store APK: %v", opts.StoreAPK, err)), nil
	}
	for _, name := range store.names() {
		if name == packDir || strings.HasPrefix(name, packDir+"/") {
			problems = append(problems, fmt.Sprintf("%s already has the entry %s: a staged pack would replace it",
				filepath.Base(opts.StoreAPK), name))
		}
	}
	var total uint64
	for _, f := range files {
		total += f.size
	}
	if uint64(storeInfo.Size())+total+apkSlackBytes > apkLimitBytes {
		problems = append(problems, fmt.Sprintf("the APK would be about %d bytes: a zip without zip64 holds %d (the sentinel's zip "+
			"reader refuses zip64)", uint64(storeInfo.Size())+total, apkLimitBytes))
	}
	if len(problems) > 0 {
		return problems, nil
	}
	zipSHA, err := fileDigest(opts.PackZip)
	if err != nil {
		return nil, err
	}
	zipInfo, err := os.Stat(opts.PackZip)
	if err != nil {
		return nil, err
	}
	if err := writePackFiles(opts, prefix, files); err != nil {
		return []string{err.Error()}, nil
	}
	document, err := packJSON(meta, requires, version, files, zipSHA, zipInfo.Size())
	if err != nil {
		return nil, err
	}
	if err := os.WriteFile(filepath.Join(opts.Stage, packJSONEntry), document, 0o644); err != nil {
		return nil, err
	}
	relatives := make([]string, 0, len(files))
	for r := range files {
		relatives = append(relatives, r)
	}
	sort.Strings(relatives)
	for _, r := range relatives {
		fmt.Fprintf(opts.Out, "quest_repack_libs: pack file %s sha256 %s size %d\n", r, files[r].sha256, files[r].size)
	}
	return nil, nil
}

// writePackFiles copies each scanned file of the pack into the stage. It reports a file whose bytes read twice differ.
func writePackFiles(opts StagePackOptions, prefix string, files map[string]packFile) error {
	rc, err := zip.OpenReader(opts.PackZip)
	if err != nil {
		return err
	}
	defer rc.Close()
	byName := map[string]*zip.File{}
	for _, f := range rc.File {
		byName[f.Name] = f
	}
	relatives := make([]string, 0, len(files))
	for r := range files {
		relatives = append(relatives, r)
	}
	sort.Strings(relatives)
	root := filepath.Join(opts.Stage, packFilesDir)
	for _, relative := range relatives {
		target := filepath.Join(root, filepath.FromSlash(relative))
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		if err := copyEntry(byName[prefix+relative], target, files[relative], prefix+relative); err != nil {
			return err
		}
	}
	return nil
}

// copyEntry writes one zip entry to target and checks the bytes written against the scan's digest and size.
func copyEntry(entry *zip.File, target string, want packFile, name string) error {
	src, err := entry.Open()
	if err != nil {
		return err
	}
	defer src.Close()
	dst, err := os.Create(target)
	if err != nil {
		return err
	}
	defer dst.Close()
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(dst, h), src)
	if err != nil {
		return err
	}
	if hex.EncodeToString(h.Sum(nil)) != want.sha256 || uint64(n) != want.size {
		return fmt.Errorf("%s: the bytes read twice differ", name)
	}
	return nil
}

// packDocumentProblems are the rules of the runtime's ParsePackJson over a parsed pack.json.
func packDocumentProblems(document any, versionFolder, manifestID string) []string {
	obj, ok := document.(map[string]any)
	if !ok {
		return []string{"pack.json: not a JSON object"}
	}
	var problems []string
	if schema, ok := obj["schema"].(float64); !ok || schema != packSchema {
		problems = append(problems, "pack.json: schema must be 1")
	}
	if id, ok := obj["id"].(string); !ok || !rePackID.MatchString(id) {
		problems = append(problems, "pack.json: bad id")
	}
	if version, ok := obj["version"].(string); !ok || !rePackVersion.MatchString(version) {
		problems = append(problems, "pack.json: bad version")
	}
	if folder, ok := obj["stock_data_version"].(string); !ok || folder != versionFolder {
		problems = append(problems, fmt.Sprintf("pack.json: stock_data_version is not %s, the data step's", versionFolder))
	}
	requires, listOK := stringList(obj["requires_stock_manifest_sha256"])
	if !listOK || len(requires) < 1 || len(requires) > maxRequiresSums || !allMatch(requires, reHex64) || !strictlySorted(requires) {
		problems = append(problems, "pack.json: requires_stock_manifest_sha256 must be one or two sorted, distinct sha-256 digests")
	}
	rawFiles, isList := obj["files"].([]any)
	if !isList || len(rawFiles) < 1 || len(rawFiles) > maxPackFiles {
		problems = append(problems, fmt.Sprintf("pack.json: files must hold 1 to %d entries", maxPackFiles))
		rawFiles = nil
	}
	manifests := 0
	seen := map[string]bool{}
	for _, item := range rawFiles {
		entry, ok := item.(map[string]any)
		path, pathOK := "", false
		if ok {
			path, pathOK = entry["path"].(string)
		}
		if !ok || !pathOK {
			problems = append(problems, "pack.json: a file entry has no path")
			continue
		}
		if reason := isPackPath(path, manifestID); reason != "" {
			problems = append(problems, "pack.json: "+reason)
		}
		lower := strings.ToLower(path)
		if seen[lower] {
			problems = append(problems, fmt.Sprintf("pack.json: %s twice", path))
		}
		seen[lower] = true
		if path == "manifests/"+manifestID {
			manifests++
		}
		if sum, ok := entry["sha256"].(string); !ok || !reHex64.MatchString(sum) {
			problems = append(problems, fmt.Sprintf("pack.json: %s: bad sha256", path))
		}
		if size, ok := wholeNumber(entry["size"]); !ok || size < 1 || size > maxPackFileBytes {
			problems = append(problems, fmt.Sprintf("pack.json: %s: bad size", path))
		}
	}
	if manifests != 1 {
		problems = append(problems, fmt.Sprintf("pack.json: files must name manifests/%s once", manifestID))
	}
	sourceSize, sizeOK := wholeNumber(obj["source_zip_size"])
	sourceSum, sumOK := obj["source_zip_sha256"].(string)
	if !sumOK || !reHex64.MatchString(sourceSum) || !sizeOK || sourceSize < 1 {
		problems = append(problems, "pack.json: bad source_zip_sha256 or source_zip_size")
	}
	return problems
}

// wholeNumber is a JSON number that is a whole integer, as an int64.
func wholeNumber(v any) (int64, bool) {
	f, ok := v.(float64)
	if !ok || f != math.Trunc(f) || math.IsInf(f, 0) {
		return 0, false
	}
	return int64(f), true
}

// strictlySorted reports whether values are in strictly increasing order (sorted and distinct).
func strictlySorted(values []string) bool {
	for i := 1; i < len(values); i++ {
		if values[i-1] >= values[i] {
			return false
		}
	}
	return true
}

// packProblems are the problems with the pack staged under stage; empty when none is staged, or it is whole and the
// runtime takes it.
func packProblems(stage, planHeader string) []string {
	packPath := filepath.Join(stage, packDir)
	if _, err := os.Lstat(packPath); err != nil {
		return nil
	}
	version, manifestID, ok := pinnedManifest(planHeader)
	if !ok {
		return []string{fmt.Sprintf("%s: names no kManifestPath, so the staged pack cannot be checked", planHeader)}
	}
	jsonPath := filepath.Join(stage, packJSONEntry)
	info, err := os.Stat(jsonPath)
	if err != nil || !info.Mode().IsRegular() {
		return []string{fmt.Sprintf("%s: staged files without a pack.json", packJSONEntry)}
	}
	raw, err := os.ReadFile(jsonPath)
	if err != nil {
		return []string{fmt.Sprintf("%s: cannot be read as JSON: %v", packJSONEntry, err)}
	}
	var document any
	if err := json.Unmarshal(raw, &document); err != nil {
		return []string{fmt.Sprintf("%s: cannot be read as JSON: %v", packJSONEntry, err)}
	}
	if problems := packDocumentProblems(document, version, manifestID); len(problems) > 0 {
		return problems
	}
	obj := document.(map[string]any)
	var problems []string
	listed := map[string]bool{}
	entries, _ := obj["files"].([]any)
	for _, item := range entries {
		entry := item.(map[string]any)
		path := entry["path"].(string)
		listed[path] = true
		staged := filepath.Join(stage, packFilesDir, filepath.FromSlash(path))
		st, err := os.Lstat(staged)
		if err != nil || !st.Mode().IsRegular() {
			problems = append(problems, fmt.Sprintf("%s/%s: listed in pack.json, not staged as a file", packFilesDir, path))
			continue
		}
		size, _ := wholeNumber(entry["size"])
		if st.Size() != size {
			problems = append(problems, fmt.Sprintf("%s/%s: %d bytes staged, pack.json says %d", packFilesDir, path, st.Size(), size))
		} else if digest, err := fileDigest(staged); err != nil || digest != entry["sha256"] {
			problems = append(problems, fmt.Sprintf("%s/%s: the sha256 pack.json lists is not the staged file's", packFilesDir, path))
		}
	}
	found, err := stagedFilesBelow(filepath.Join(stage, packFilesDir))
	if err != nil {
		return append(problems, err.Error())
	}
	for _, rel := range found {
		if !listed[rel] {
			problems = append(problems, fmt.Sprintf("%s/%s: staged, not listed in pack.json", packFilesDir, rel))
		}
	}
	top, err := os.ReadDir(packPath)
	if err != nil {
		return append(problems, err.Error())
	}
	var extra []string
	for _, e := range top {
		if e.Name() != "pack.json" && e.Name() != "files" {
			extra = append(extra, e.Name())
		}
	}
	sort.Strings(extra)
	for _, name := range extra {
		problems = append(problems, fmt.Sprintf("%s/%s: not part of a pack", packDir, name))
	}
	return problems
}

// stagedFilesBelow are the regular files and symbolic links below root (a dangling link included), slash-separated and
// sorted; a missing root has none.
func stagedFilesBelow(root string) ([]string, error) {
	var out []string
	err := filepath.WalkDir(root, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.Type().IsRegular() || d.Type()&fs.ModeSymlink != 0 {
			rel, err := filepath.Rel(root, path)
			if err != nil {
				return err
			}
			out = append(out, filepath.ToSlash(rel))
		}
		return nil
	})
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	sort.Strings(out)
	return out, err
}

// packSignedProblems: every staged pack file is in the signed APK with the staged bytes, STORED, and the signed APK has
// no pack entry that was not staged (a build with no pack carries none).
func packSignedProblems(result *zipArchive, stage string) []string {
	var problems []string
	staged, _ := stagedFilesBelow(filepath.Join(stage, packDir))
	expected := map[string]bool{}
	for _, rel := range staged {
		expected[packDir+"/"+rel] = true
	}
	present := map[string]*zip.File{}
	for _, f := range result.rc.File {
		present[f.Name] = f
	}
	var unexpected []string
	for name, f := range present {
		if (name == packDir || strings.HasPrefix(name, packDir+"/")) && !expected[name] && !strings.HasSuffix(f.Name, "/") {
			unexpected = append(unexpected, name)
		}
	}
	sort.Strings(unexpected)
	for _, name := range unexpected {
		problems = append(problems, fmt.Sprintf("%s: in the signed APK, not staged", name))
	}
	names := make([]string, 0, len(expected))
	for n := range expected {
		names = append(names, n)
	}
	sort.Strings(names)
	for _, name := range names {
		info, ok := present[name]
		if !ok {
			problems = append(problems, fmt.Sprintf("%s: staged, missing from the signed APK", name))
			continue
		}
		if info.Method != zip.Store {
			problems = append(problems, fmt.Sprintf("%s: in the signed APK but not STORED (compression %d)", name, info.Method))
			continue
		}
		signedSum, err := streamDigest(info)
		stagedSum, errStaged := fileDigest(filepath.Join(stage, filepath.FromSlash(name)))
		if err != nil || errStaged != nil || signedSum != stagedSum {
			problems = append(problems, fmt.Sprintf("%s: the signed APK holds different bytes than the staged file", name))
		}
	}
	return problems
}

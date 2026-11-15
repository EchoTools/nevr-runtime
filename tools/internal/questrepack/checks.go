package questrepack

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// pluginNameCollisions is what plugin collides with: the lib/ entries of the store APK (in any ABI directory) and the
// libraries the recipe stages itself.
func pluginNameCollisions(storeEntries []string, plugin string) []string {
	var found []string
	for _, entry := range storeEntries {
		if baseName(entry) == plugin {
			found = append(found, entry)
		}
	}
	for _, name := range stagedByRecipe {
		if name == plugin {
			found = append(found, fmt.Sprintf("%s/%s (staged by the recipe)", abiDir, name))
		}
	}
	return found
}

func baseName(path string) string {
	if i := strings.LastIndex(path, "/"); i >= 0 {
		return path[i+1:]
	}
	return path
}

// checkPluginName reports the store entries a plugin named like plugin would replace.
func checkPluginName(apk, plugin string) ([]string, error) {
	z, err := openZip(apk)
	if err != nil {
		return nil, err
	}
	defer z.Close()
	return pluginNameCollisions(libEntries(z), baseName(plugin)), nil
}

// shimRowsProblems is why shim was not built for the rows the library is patched with (ADR 0021 section 11); empty
// when it was. extraRows "" means the library is patched without rows.
func shimRowsProblems(shim, extraRows string) []string {
	data, err := os.ReadFile(shim)
	if err != nil {
		return []string{fmt.Sprintf("%s: cannot be read: %v", shim, err)}
	}
	if extraRows == "" {
		if bytes.Contains(data, []byte(variantMarker)) {
			return []string{fmt.Sprintf("%s is a variant build (it carries extra rows) but no extra_rows file was given, so the "+
				"library would be patched without them and the shim would refuse it (build_id_mismatch)", baseName(shim))}
		}
		return nil
	}
	digest, err := fileDigest(extraRows)
	if err != nil {
		return []string{fmt.Sprintf("%s: cannot be read: %v", extraRows, err)}
	}
	if !bytes.Contains(data, []byte(digest+"\x00")) {
		return []string{fmt.Sprintf("%s was not built with %s (sha-256 %s): rebuild it with "+
			"-DNEVR_LIBR15_EXTRA_ROWS=<the same file>, else it refuses the patched library (build_id_mismatch)",
			baseName(shim), baseName(extraRows), digest)}
	}
	return nil
}

// pluginIsNamedForTheLoader is the file name the loader takes a plugin by: lib<name>.so, with a non-empty name.
func pluginIsNamedForTheLoader(name string) bool {
	return strings.HasPrefix(name, "lib") && strings.HasSuffix(name, ".so") && len(name) > len("lib.so")
}

// manifestDocument is the manifest's JSON, with the plugins in the order given.
type manifestPlugin struct {
	Early   bool   `json:"early"`
	Enabled bool   `json:"enabled"`
	File    string `json:"file"`
	Name    string `json:"name"`
	SHA256  string `json:"sha256"`
}

type manifestDocument struct {
	Plugins []manifestPlugin `json:"plugins"`
	Schema  int              `json:"schema"`
}

// manifestBytes are the manifest's bytes: the same plugins give the same bytes (sorted keys, no spaces, one newline).
func manifestBytes(plugins []manifestPlugin) ([]byte, error) {
	return marshalSorted(manifestDocument{Plugins: plugins, Schema: manifestSchema})
}

// writePluginManifest writes the manifest of the packaged plugins under stage and returns the problems; it writes
// nothing when there are any. The plugins are the files as the recipe was given them (only their names matter); each
// hash is of the staged copy, the bytes the APK will carry.
func writePluginManifest(stage string, plugins []string, early, off []string) []string {
	names := make([]string, len(plugins))
	for i, p := range plugins {
		names[i] = baseName(p)
	}
	var problems []string
	for _, name := range names {
		if !pluginIsNamedForTheLoader(name) {
			problems = append(problems, fmt.Sprintf("plugin %s must be named lib<name>.so", name))
		}
		if count(names, name) > 1 {
			problems = append(problems, fmt.Sprintf("two plugins are named %s", name))
		}
	}
	for _, group := range []struct {
		label string
		names []string
	}{{"plugin_early", early}, {"plugin_off", off}} {
		for _, name := range group.names {
			if !contains(names, name) {
				problems = append(problems, fmt.Sprintf("%s names %s, which is not one of the packaged plugins", group.label, name))
			}
		}
	}
	if len(problems) > 0 {
		return problems
	}
	entries := make([]manifestPlugin, 0, len(names))
	for _, name := range names {
		staged := filepath.Join(stage, abiDir, name)
		info, err := os.Stat(staged)
		if err != nil || !info.Mode().IsRegular() {
			return []string{fmt.Sprintf("%s/%s: not staged, so its hash cannot be written", abiDir, name)}
		}
		digest, err := fileDigest(staged)
		if err != nil {
			return []string{fmt.Sprintf("%s/%s: %v", abiDir, name, err)}
		}
		entries = append(entries, manifestPlugin{
			Name:    name[len("lib") : len(name)-len(".so")],
			File:    name,
			SHA256:  digest,
			Enabled: !contains(off, name),
			Early:   contains(early, name),
		})
	}
	data, err := manifestBytes(entries)
	if err != nil {
		return []string{fmt.Sprintf("%s: %v", manifestEntry, err)}
	}
	if err := os.WriteFile(filepath.Join(stage, abiDir, manifestName), data, 0o644); err != nil {
		return []string{fmt.Sprintf("%s: %v", manifestEntry, err)}
	}
	return nil
}

// buildVariantProblems are the problems with a variant word: empty for a plain release or one of variantWords, compared
// exactly (no case folding, no trimming).
func buildVariantProblems(word string) []string {
	if word == "" || contains(variantWords, word) {
		return nil
	}
	return []string{fmt.Sprintf("variant %s is not a variant word (the words are %s; empty is a plain release)",
		pyRepr(word), strings.Join(variantWords, ", "))}
}

// writeBuildVariant writes the variant stamp under stage; it returns the problems and writes nothing when there are any.
func writeBuildVariant(stage, word string) []string {
	if problems := buildVariantProblems(word); len(problems) > 0 {
		return problems
	}
	target := filepath.Join(stage, abiDir)
	if err := os.MkdirAll(target, 0o755); err != nil {
		return []string{fmt.Sprintf("%s: %v", target, err)}
	}
	if err := os.WriteFile(filepath.Join(target, variantStamp), []byte(word), 0o644); err != nil {
		return []string{fmt.Sprintf("%s: %v", variantEntry, err)}
	}
	return nil
}

// pluginManifestProblems are the problems with the staged manifest; empty when there is none or it holds every staged
// plugin with its hash, and every staged file is either the recipe's own, a script library, or listed.
func pluginManifestProblems(stage string) []string {
	path := filepath.Join(stage, abiDir, manifestName)
	raw, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return []string{fmt.Sprintf("%s: cannot be read as JSON: %v", manifestEntry, err)}
	}
	var document map[string]json.RawMessage
	if err := json.Unmarshal(raw, &document); err != nil {
		return []string{fmt.Sprintf("%s: cannot be read as JSON: %v", manifestEntry, err)}
	}
	var listedEntries []struct {
		File   string `json:"file"`
		SHA256 string `json:"sha256"`
	}
	if rawPlugins, ok := document["plugins"]; ok {
		if err := json.Unmarshal(rawPlugins, &listedEntries); err != nil {
			return []string{fmt.Sprintf("%s: cannot be read as JSON: %v", manifestEntry, err)}
		}
	}
	listed := map[string]bool{}
	var problems []string
	for _, entry := range listedEntries {
		listed[entry.File] = true
		staged := filepath.Join(stage, abiDir, entry.File)
		info, err := os.Stat(staged)
		if err != nil || !info.Mode().IsRegular() {
			problems = append(problems, fmt.Sprintf("%s: lists %s, which is not staged", manifestEntry, entry.File))
			continue
		}
		actual, err := fileDigest(staged)
		if err != nil {
			problems = append(problems, fmt.Sprintf("%s: %v", manifestEntry, err))
			continue
		}
		if actual != entry.SHA256 {
			problems = append(problems, fmt.Sprintf("%s: the sha256 it lists for %s is %s, the staged file's is %s",
				manifestEntry, entry.File, entry.SHA256, actual))
		}
	}
	// The recipe's own files, and the script libraries stage-libs staged (recorded in the stage, #2092): neither is a plugin.
	own := map[string]bool{}
	for _, n := range stagedByRecipe {
		own[n] = true
	}
	own[filepath.Base(patchedLibrary)] = true
	own[filepath.Base(pinnedLibrary)] = true
	for _, n := range stagedScriptLibs(stage) {
		own[n] = true
	}
	entries, err := os.ReadDir(filepath.Join(stage, abiDir))
	if err != nil {
		return append(problems, fmt.Sprintf("%s: %v", abiDir, err))
	}
	var files []string
	for _, e := range entries {
		if e.Type().IsRegular() {
			files = append(files, e.Name())
		}
	}
	sort.Strings(files)
	for _, name := range files {
		if own[name] || listed[name] {
			continue
		}
		problems = append(problems, fmt.Sprintf("%s/%s: staged, not listed in %s", abiDir, name, manifestName))
	}
	return problems
}

func count(list []string, s string) int {
	n := 0
	for _, v := range list {
		if v == s {
			n++
		}
	}
	return n
}

func contains(list []string, s string) bool {
	return count(list, s) > 0
}

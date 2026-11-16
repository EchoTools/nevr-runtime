package testmap

import (
	"encoding/json"
	"sort"
	"strings"
	"testing"
)

// fixtureFiles turns a map written as one JSON object (the header's keys and a "components" array of {name, dir, files,
// none}) into the files of the directory form, without checking any of it: the tests below hand each refusal to
// ParseFiles. A text that is not a JSON object is the header file as it is. A file in both files and none is two
// lines; two components of one name get two files (the second is not the one that is named for it).
func fixtureFiles(text string) map[string][]byte {
	files := map[string][]byte{}
	var raw map[string]json.RawMessage
	if err := json.Unmarshal([]byte(text), &raw); err != nil {
		files[HeaderFile] = []byte(text)
		return files
	}
	comps := raw["components"]
	delete(raw, "components")
	header, _ := json.Marshal(raw)
	files[HeaderFile] = header
	var list []Component
	if len(comps) > 0 {
		_ = json.Unmarshal(comps, &list)
	}
	for _, c := range list {
		var b strings.Builder
		head, _ := marshal(componentHead{Component: c.Name, Dir: c.Dir}, "", "")
		b.Write(head)
		b.WriteByte('\n')
		var paths []string
		for p := range c.Files {
			paths = append(paths, p)
		}
		sort.Strings(paths)
		for _, p := range paths {
			line, _ := marshal(map[string]any{"file": p, "tests": c.Files[p]}, "", "")
			b.Write(line)
			b.WriteByte('\n')
		}
		paths = nil
		for p := range c.None {
			paths = append(paths, p)
		}
		sort.Strings(paths)
		for _, p := range paths {
			line, _ := marshal(map[string]any{"file": p, "none": c.None[p]}, "", "")
			b.Write(line)
			b.WriteByte('\n')
		}
		name := ComponentFile(c.Name)
		for {
			if _, taken := files[name]; !taken {
				break
			}
			name = strings.TrimSuffix(name, ComponentExt) + ".2" + ComponentExt
		}
		files[name] = []byte(b.String())
	}
	return files
}

// parseFixture is ParseFiles of fixtureFiles(text).
func parseFixture(text string) (*Map, error) { return ParseFiles(fixtureFiles(text)) }

// mustParseFixture is parseFixture that fails the test on an error.
func mustParseFixture(t *testing.T, text string) *Map {
	t.Helper()
	m, err := parseFixture(text)
	if err != nil {
		t.Fatal(err)
	}
	return m
}

package main

import (
	"fmt"
	"path"
	"sort"
	"strings"

	"github.com/EchoTools/nevr-runtime/tools/internal/testmap"
)

// rootName is the name of the component of the repository's root directory.
const rootName = "(root)"

// buildMap makes the map from the coverage: one component per directory that holds tracked files, each file in its files
// (with its test ids) or in none (with its reason). A file with neither is an exitUncovered failure naming every such file.
func buildMap(tracked []string, cov cover) (*testmap.Map, error) {
	byDir := map[string][]string{}
	for _, f := range tracked {
		d := path.Dir(f)
		if d == "." {
			d = ""
		}
		byDir[d] = append(byDir[d], f)
	}
	dirs := make([]string, 0, len(byDir))
	for d := range byDir {
		dirs = append(dirs, d)
	}
	sort.Strings(dirs)
	var comps []testmap.Component
	var uncovered []string
	for _, d := range dirs {
		c := testmap.Component{Name: d, Dir: d}
		if d == "" {
			c.Name = rootName
		}
		for _, f := range byDir[d] {
			if ids := cov.ids(f); len(ids) > 0 {
				if c.Files == nil {
					c.Files = map[string][]string{}
				}
				c.Files[f] = ids
				continue
			}
			why, ok := reasonFor(f)
			if !ok {
				uncovered = append(uncovered, f)
				continue
			}
			if c.None == nil {
				c.None = map[string]string{}
			}
			c.None[f] = why
		}
		comps = append(comps, c)
	}
	if len(uncovered) > 0 {
		return nil, failure{exitUncovered, fmt.Sprintf(
			"%d tracked file(s) have no test and no reason: %s. Give each a test in the generator's coverage stages, "+
				"or a row in reasons (tools/cmd/test-map/rules.go) if no test reads it",
			len(uncovered), strings.Join(uncovered, ", "))}
	}
	return &testmap.Map{
		Schema:     testmap.Schema,
		About:      about,
		Targets:    targets,
		Scanners:   scanners,
		Components: comps,
	}, nil
}

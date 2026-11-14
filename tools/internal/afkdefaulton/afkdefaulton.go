// Package afkdefaulton is the list of switch files a fresh Quest install turns on by itself (the APK's
// assets/afk_default_on.txt), and the rule that no switch the reference runtime marks TEST in its source is in it.
//
// The reference combat build's own list is its QA trial's (`# qa-1104-aj4-w2z22`); on a fresh install it turned on
// pfxwarm.on, which alone hangs the GPU in the social lobby (kernel `kgsl-3d0: CP opcode error`, then the runtime's
// hang watchdog ends the process). The repack writes the list kept in this repository instead (tools/quest-afk-default-on.txt),
// after Problems finds nothing wrong with it.
package afkdefaulton

import (
	"fmt"
	"regexp"
	"sort"
	"strings"
)

// Entry is the APK entry the list is written to.
const Entry = "assets/afk_default_on.txt"

// MaxBytes is the largest list the runtime reads: a longer one is ignored whole (kDefaultOnMax, afk_persistlog.cpp:485).
const MaxBytes = 4096

// UpstreamCommit is the commit of the reference runtime's source (kotorvr/echoquest-combat) that the citations in TestOnly were read at.
const UpstreamCommit = "c5a67e8"

// TestOnly names every switch the reference runtime's source marks TEST (or QA only) on the line that defines or reads
// it, with that line: runtime/native/afkxr/<file>:<line> at UpstreamCommit. A switch in this table is never in the shipped list.
// fxchassisforklobby.on carries no mark of its own; it does nothing without fxchassisfork.on (afk_fx.cpp:7706), which is marked.
// pfxtcomet.on is named by the tier log's "(TEST ONLY)" string (afk_pfx.cpp:32346, its name on 32348), which lists it.
// pfxribspan.on is a mode bit of the afk_pfx module, whose header reads "pfx.on (TEST ONLY" (afk_pfx.cpp:1).
// The ec* switches configure the afk_ec module, whose header reads "TEST ONLY: own gs only (C5)" (afk_ec.cpp:2); depthrt.on is
// the header of afk_depthrt.cpp ("RE-47 depthrt (TEST ONLY)").
var TestOnly = map[string]string{
	"afklog.on":             "afk_persistlog.cpp:22",
	"afkqa.on":              "afk_persistlog.cpp:22",
	"depthrt.on":            "afk_depthrt.cpp:1",
	"droneguard.on":         "afk_droneguard.cpp:1",
	"dronekill.on":          "afk_dronekill.cpp:1",
	"dronekill2.on":         "afk_dronekill.cpp:49",
	"dronefx.on":            "afk_dronekill.cpp:63",
	"dronemuzzle.on":        "afk_dronemuzzle.cpp:1",
	"ec.on":                 "afk_ec.cpp:2",
	"ecplay.on":             "afk_ec.cpp:2",
	"ecplayo.on":            "afk_ec.cpp:2",
	"ecplayp.on":            "afk_ec.cpp:2",
	"fxchassisfork.on":      "afk_fx.cpp:7529",
	"fxchassisforklobby.on": "afk_fx.cpp:7706",
	"fxchassisforksoc.on":   "afk_fx.cpp:7587",
	"neutronfx.on":          "afk_ecfx.cpp:1",
	"olgate.on":             "afk_olgate.cpp:1",
	"p18add.on":             "afk_gpuprobe.cpp:292",
	"peerclamp.on":          "afk_peerclamp.cpp:1",
	"pfx.on":                "afk_pfx.cpp:1",
	"pfxargsclear.on":       "afk_pfx.cpp:2646",
	"pfxgpubar.on":          "afk_pfx.cpp:32083",
	"pfxmesh.on":            "afk_pfx.cpp:1721",
	"pfxmeshwarm.on":        "afk_pfx.cpp:1768",
	"pfxmeteorflow.on":      "afk_pfx.cpp:4811",
	"pfxnoflow.on":          "afk_pfx.cpp:4740",
	"pfxribspan.on":         "afk_pfx.cpp:1",
	"pfxtburst.on":          "afk_pfx.cpp:31944",
	"pfxtcomet.on":          "afk_pfx.cpp:32346",
	"pfxtev.on":             "afk_pfx.cpp:31933",
	"pfxtkid.on":            "afk_pfx.cpp:31935",
	"pfxtmesh.on":           "afk_pfx.cpp:31934",
	"pfxtovc.on":            "afk_pfx.cpp:31943",
	"pfxtpar.on":            "afk_pfx.cpp:31936",
	"pfxtphase.on":          "afk_pfx.cpp:31937",
	"pfxwarm.on":            "afk_pfx.cpp:31929",
	"powerboost.on":         "afk_powerboost.cpp:1",
	"volpfx.on":             "afk_fx.cpp:48",
}

// a switch file name: lower case letters, digits and underscores, then .on or .off
var nameShape = regexp.MustCompile(`^[a-z0-9_]+\.(on|off)$`)

// Parse reads the list: one switch file name a line, blank lines and lines that start with # skipped. A name of another
// shape, or a name twice, is an error naming the line.
func Parse(text string) ([]string, error) {
	var names []string
	seen := map[string]int{}
	for i, line := range strings.Split(text, "\n") {
		line = strings.TrimSuffix(line, "\r") // the runtime strips a trailing CR and nothing else (afk_persistlog.cpp:645)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if !nameShape.MatchString(line) {
			return nil, fmt.Errorf("line %d: %q is not a switch file name", i+1, line)
		}
		if first, dup := seen[line]; dup {
			return nil, fmt.Errorf("line %d: %q is already on line %d", i+1, line, first)
		}
		seen[line] = i + 1
		names = append(names, line)
	}
	return names, nil
}

// Problems returns one line for each reason the names are not fit to ship: an empty list, and each name that is in TestOnly,
// with the source line that marks it.
func Problems(names []string) []string {
	if len(names) == 0 {
		return []string{"the list is empty"}
	}
	var out []string
	for _, n := range names {
		if cite, bad := TestOnly[n]; bad {
			out = append(out, fmt.Sprintf("%s is TEST only (runtime/native/afkxr/%s at %s)", n, cite, UpstreamCommit))
		}
	}
	sort.Strings(out)
	return out
}

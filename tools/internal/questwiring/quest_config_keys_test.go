package questwiring

import (
	"regexp"
	"sort"
	"strings"
	"testing"
)

// #2382 (G7 review of 1630dda61, B1): docs/reference/example-config.yaml says "The Quest reads exactly these keys: ...", and that
// sentence drifted from src/quest/sentinel/quest_config.cpp (assets.* and network.invite_codes were missing). Nothing pinned it. This
// test derives the keys the Quest reader looks up from its source, derives the keys the sentence claims, and requires equality. The
// sentence uses shorthand ("the three combat.refresh_rate keys", "the combat.gate_* keys", "the six crash-guard keys (fxtrail, ...)"),
// so the expansion below understands exactly those forms and fails on any other one.

const (
	questConfigSrc     = "src/quest/sentinel/quest_config.cpp"
	exampleConfigDoc   = "docs/reference/example-config.yaml"
	questSentenceStart = "The Quest reads exactly these keys:"
	questSentenceEnd   = "Every other key below"
)

// A dotted key literal: section.name, with any number of dotted parts. Every literal the test finds must be in a section the
// reader names; the sentence check fails on any other key anyway.
var dottedKeyLiteral = regexp.MustCompile(`"([a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+)"`)

var numberWords = map[string]int{"one": 1, "two": 2, "three": 3, "four": 4, "five": 5, "six": 6, "seven": 7, "eight": 8, "nine": 9, "ten": 10}

var (
	fullItem     = regexp.MustCompile(`^(?:the )?([a-z][a-z0-9_]*\.[a-z0-9_]+)$`)
	wildItem     = regexp.MustCompile(`^the ([a-z][a-z0-9_]*\.[a-z0-9_]*)\* keys$`)
	groupItem    = regexp.MustCompile(`^the (?:(one|two|three|four|five|six|seven|eight|nine|ten) )?(?:([a-z][a-z0-9_]*\.[a-z0-9_]+)|(crash-guard)) keys(?: \(([a-z0-9_, ]+)\))?$`)
	danglingItem = regexp.MustCompile(`[ ,]$`)
)

// readerKeys returns the set of keys quest_config.cpp looks up: every dotted string literal outside its comments.
func readerKeys(t *testing.T) map[string]bool {
	t.Helper()
	keys := map[string]bool{}
	for _, m := range dottedKeyLiteral.FindAllStringSubmatch(stripComments(read(t, questConfigSrc)), -1) {
		keys[m[1]] = true
	}
	if len(keys) < 50 {
		t.Fatalf("%s: found %d key literals, want at least 50: the extraction has drifted from the reader's spelling", questConfigSrc, len(keys))
	}
	for _, anchor := range []string{"services.socket_uri", "auth.server_key", "combat.force", "network.invite_codes", "assets.labels"} {
		if !keys[anchor] {
			t.Fatalf("%s: the key %q is not found as a literal: the extraction has drifted", questConfigSrc, anchor)
		}
	}
	return keys
}

// sentenceText returns the Quest sentence of the example config, its "# " comment marks removed and its final period dropped.
func sentenceText(t *testing.T) string {
	t.Helper()
	doc := read(t, exampleConfigDoc)
	start := strings.Index(doc, questSentenceStart)
	if start < 0 {
		t.Fatalf("%s: no sentence starting %q", exampleConfigDoc, questSentenceStart)
	}
	end := strings.Index(doc[start:], questSentenceEnd)
	if end < 0 {
		t.Fatalf("%s: the sentence has no %q after it", exampleConfigDoc, questSentenceEnd)
	}
	s := doc[start+len(questSentenceStart) : start+end]
	s = strings.ReplaceAll(s, "\n#", " ")
	s = strings.Join(strings.Fields(s), " ")
	return strings.TrimSuffix(strings.TrimSpace(s), ".")
}

// splitTopLevel splits s at the commas that are not inside parentheses.
func splitTopLevel(s string) []string {
	var parts []string
	depth, from := 0, 0
	for i, c := range s {
		switch c {
		case '(':
			depth++
		case ')':
			depth--
		case ',':
			if depth == 0 {
				parts = append(parts, s[from:i])
				from = i + 1
			}
		}
	}
	return append(parts, s[from:])
}

// keysUnder returns the reader keys that are prefix itself or start with prefix_. A prefix that ends in "_" (a wildcard
// written "combat.gate_*") matches every key that starts with it.
func keysUnder(reader map[string]bool, prefix string) []string {
	var out []string
	for k := range reader {
		if k == prefix || strings.HasPrefix(k, prefix+"_") || (strings.HasSuffix(prefix, "_") && strings.HasPrefix(k, prefix)) {
			out = append(out, k)
		}
	}
	sort.Strings(out)
	return out
}

// expandSentence returns the set of keys the sentence claims. Each item is one of three forms, or the test fails naming it: a
// full key; "the N P keys" (every reader key under the dotted prefix P, N of them, or the names in parentheses after P's section,
// N of them); "the P* keys" (every reader key under P).
func expandSentence(t *testing.T, sentence string, reader map[string]bool) map[string]bool {
	t.Helper()
	claimed := map[string]bool{}
	s := strings.ReplaceAll(sentence, " and the ", ", the ")
	s = strings.ReplaceAll(s, " and ", ", ")
	for _, raw := range splitTopLevel(s) {
		item := strings.TrimSpace(raw)
		if item == "" || danglingItem.MatchString(item) {
			t.Fatalf("the sentence has an empty item near %q", sentence)
		}
		if m := fullItem.FindStringSubmatch(item); m != nil {
			claimed[m[1]] = true
			continue
		}
		if m := wildItem.FindStringSubmatch(item); m != nil {
			got := keysUnder(reader, m[1])
			if len(got) == 0 {
				t.Fatalf("the sentence item %q matches no reader key", item)
			}
			for _, k := range got {
				claimed[k] = true
			}
			continue
		}
		if m := groupItem.FindStringSubmatch(item); m != nil {
			var got []string
			switch {
			case m[4] != "":
				section := "combat."
				if m[2] != "" {
					section = m[2][:strings.Index(m[2], ".")+1]
				}
				for _, name := range strings.Split(m[4], ",") {
					got = append(got, section+strings.TrimSpace(name))
				}
			default:
				got = keysUnder(reader, m[2])
			}
			if m[1] != "" && numberWords[m[1]] != len(got) {
				t.Fatalf("the sentence item %q says %d keys, the reader has %d (%s)", item, numberWords[m[1]], len(got), strings.Join(got, ", "))
			}
			for _, k := range got {
				claimed[k] = true
			}
			continue
		}
		t.Fatalf("the sentence item %q is in no form this test understands (a full key, \"the N P keys\", \"the P* keys\"): extend the test or write the item plainly", item)
	}
	if len(claimed) == 0 {
		t.Fatalf("the sentence claims no keys: %q", sentence)
	}
	return claimed
}

func TestTheExampleConfigSentenceListsExactlyTheKeysTheQuestReads(t *testing.T) {
	t.Parallel()
	reader := readerKeys(t)
	claimed := expandSentence(t, sentenceText(t), reader)

	var missing, extra []string
	for k := range reader {
		if !claimed[k] {
			missing = append(missing, k)
		}
	}
	for k := range claimed {
		if !reader[k] {
			extra = append(extra, k)
		}
	}
	sort.Strings(missing)
	sort.Strings(extra)
	if len(missing) > 0 {
		t.Errorf("%s does not list %d key(s) that %s reads: %s", exampleConfigDoc, len(missing), questConfigSrc, strings.Join(missing, ", "))
	}
	if len(extra) > 0 {
		t.Errorf("%s lists %d key(s) that %s does not read: %s", exampleConfigDoc, len(extra), questConfigSrc, strings.Join(extra, ", "))
	}
}

// Package evidence checks a commit message for what "What a landed commit carries" (AGENTS.md) lists. It reads the
// form only; whether a claim is true is the review's.
package evidence

import (
	"regexp"
	"strings"
)

var (
	issueRef  = regexp.MustCompile(`(?mi)\b(closes|fixes|resolves|refs|part of|completes) #\d+`)
	fixType   = regexp.MustCompile(`^fix(\([^)]*\))?!?:`)
	exitWord  = regexp.MustCompile(`(?i)\bexit[ :=]*\d+|\brc[ :=]*\d+`)
	trailerRe = regexp.MustCompile(`(?m)^Co-Authored-By: .+`)
)

// section returns the body of the "## name" section (heading matched by prefix) and whether it exists.
func section(msg, prefix string) (string, bool) {
	lines := strings.Split(msg, "\n")
	for i, l := range lines {
		if !strings.HasPrefix(l, "## "+prefix) {
			continue
		}
		var body []string
		for _, n := range lines[i+1:] {
			if strings.HasPrefix(n, "## ") {
				break
			}
			body = append(body, n)
		}
		return strings.TrimSpace(strings.Join(body, "\n")), true
	}
	return "", false
}

// Missing returns what the message lacks, one item per string; empty means it carries everything.
func Missing(msg string) []string {
	var missing []string
	need := func(prefix string) string {
		body, ok := section(msg, prefix)
		if !ok {
			missing = append(missing, "section \"## "+prefix+"\"")
		} else if body == "" {
			missing = append(missing, "section \"## "+prefix+"\" is empty")
		}
		return body
	}
	need("Search statement")
	need("Symbols")
	need("Mutants")
	need("Platform parity")
	ev, ok := section(msg, "Commands and results")
	if !ok {
		ev, ok = section(msg, "Evidence")
	}
	switch {
	case !ok:
		missing = append(missing, "section \"## Commands and results\" (or \"## Evidence\")")
	case !exitWord.MatchString(ev):
		missing = append(missing, "an exit code in the commands section (\"exit 0\")")
	}
	subject, _, _ := strings.Cut(msg, "\n")
	if fixType.MatchString(subject) {
		need("Siblings")
	}
	if !issueRef.MatchString(msg) {
		missing = append(missing, "an issue reference (Closes #N or Refs #N)")
	}
	if !trailerRe.MatchString(msg) {
		missing = append(missing, "a Co-Authored-By trailer")
	}
	return missing
}

// Package evidence checks a commit message for what "What a landed commit carries" (AGENTS.md) lists. It reads the
// form only; whether a claim is true is the review's. Every commit names an issue (Closes, Fixes, Resolves, Refs, Part of
// or Completes #N) except one whose Conventional Commit type is docs: a one-shot documentation change has no issue, and
// its message is its record. A docs commit that names one still passes.
package evidence

import (
	"regexp"
	"strings"
)

const (
	typeFix  = "fix"
	typeDocs = "docs"
)

var (
	issueRef = regexp.MustCompile(`(?mi)\b(closes|fixes|resolves|refs|part of|completes) #\d+`)
	// subjectType is the Conventional Commits 1.0.0 prefix of a subject: type, optional scope, optional "!", colon.
	subjectType = regexp.MustCompile(`^([A-Za-z][A-Za-z0-9-]*)(\([^)]*\))?!?:`)
	exitWord    = regexp.MustCompile(`(?i)\bexit[ :=]*\d+|\brc[ :=]*\d+`)
	trailerRe   = regexp.MustCompile(`(?m)^Co-Authored-By: .+`)
)

// commitType returns the lower-cased Conventional Commit type of a subject line, or "" when the subject does not parse.
func commitType(subject string) string {
	m := subjectType.FindStringSubmatch(subject)
	if m == nil {
		return ""
	}
	return strings.ToLower(m[1])
}

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
	kind := commitType(subject)
	if kind == typeFix {
		need("Siblings")
	}
	if kind != typeDocs && !issueRef.MatchString(msg) {
		missing = append(missing, "an issue reference (Closes #N or Refs #N)")
	}
	if !trailerRe.MatchString(msg) {
		missing = append(missing, "a Co-Authored-By trailer")
	}
	return missing
}

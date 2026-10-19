package main

import (
	"fmt"
	"sort"
	"strings"
)

// DiffEntry is one hash/name in an added or removed list.
type DiffEntry struct {
	Hash uint64
	Name string
}

// ChangedEntry is a hash whose name changed between pins.
type ChangedEntry struct {
	Hash    uint64
	OldName string
	NewName string
}

// DiffResult is the pin-to-pin symbol delta.
type DiffResult struct {
	Added   []DiffEntry
	Removed []DiffEntry
	Changed []ChangedEntry
}

// Diff compares two corpora by hash. A hash present in both with a different
// name is Changed; ordering is by hash so the same pins always produce the same
// body.
func Diff(oldCorpus, newCorpus Corpus) DiffResult {
	result := DiffResult{}
	for hash, name := range newCorpus {
		oldName, existed := oldCorpus[hash]
		if !existed {
			result.Added = append(result.Added, DiffEntry{Hash: hash, Name: name})
			continue
		}
		if oldName != name {
			result.Changed = append(result.Changed, ChangedEntry{Hash: hash, OldName: oldName, NewName: name})
		}
	}
	for hash, name := range oldCorpus {
		if _, exists := newCorpus[hash]; !exists {
			result.Removed = append(result.Removed, DiffEntry{Hash: hash, Name: name})
		}
	}
	sort.Slice(result.Added, func(i, j int) bool { return result.Added[i].Hash < result.Added[j].Hash })
	sort.Slice(result.Removed, func(i, j int) bool { return result.Removed[i].Hash < result.Removed[j].Hash })
	sort.Slice(result.Changed, func(i, j int) bool { return result.Changed[i].Hash < result.Changed[j].Hash })
	return result
}

const diffListLimit = 1000

// Markdown renders the delta as the PR body sections. Long lists are truncated
// with a count so the body stays inside GitHub's size limit.
func (result DiffResult) Markdown() string {
	var builder strings.Builder
	builder.WriteString("## echo-symbols update\n\n")
	fmt.Fprintf(&builder, "- added: %d\n- removed: %d\n- changed: %d\n\n", len(result.Added), len(result.Removed), len(result.Changed))

	writeEntries := func(title string, entries []DiffEntry) {
		fmt.Fprintf(&builder, "### %s (%d)\n\n", title, len(entries))
		for i, entry := range entries {
			if i == diffListLimit {
				fmt.Fprintf(&builder, "- ... and %d more\n", len(entries)-diffListLimit)
				break
			}
			fmt.Fprintf(&builder, "- `%#016x` %s\n", entry.Hash, entry.Name)
		}
		if len(entries) == 0 {
			builder.WriteString("_none_\n")
		}
		builder.WriteString("\n")
	}
	writeEntries("Added", result.Added)
	writeEntries("Removed", result.Removed)

	fmt.Fprintf(&builder, "### Changed (%d)\n\n", len(result.Changed))
	for i, entry := range result.Changed {
		if i == diffListLimit {
			fmt.Fprintf(&builder, "- ... and %d more\n", len(result.Changed)-diffListLimit)
			break
		}
		fmt.Fprintf(&builder, "- `%#016x` %s -> %s\n", entry.Hash, entry.OldName, entry.NewName)
	}
	if len(result.Changed) == 0 {
		builder.WriteString("_none_\n")
	}
	return builder.String()
}

package main

import (
	"strings"
	"testing"
)

func TestMergeCorporaUnion(t *testing.T) {
	pc := Corpus{1: "pc-only", 3: "shared"}
	quest := Corpus{2: "quest-only", 3: "shared"}
	merged, conflicts, err := MergeCorpora(pc, quest)
	if err != nil {
		t.Fatalf("MergeCorpora: %v", err)
	}
	if len(merged) != 3 {
		t.Fatalf("merged has %d entries, want 3: %v", len(merged), merged)
	}
	if merged[1] != "pc-only" || merged[2] != "quest-only" || merged[3] != "shared" {
		t.Fatalf("merged = %v", merged)
	}
	if conflicts != 0 {
		t.Fatalf("conflicts = %d, want 0", conflicts)
	}
}

func TestMergeCorporaCaseOnlyConflictKeepsPCName(t *testing.T) {
	pc := Corpus{1: "Thing"}
	quest := Corpus{1: "thing"}
	merged, conflicts, err := MergeCorpora(pc, quest)
	if err != nil {
		t.Fatalf("MergeCorpora: %v", err)
	}
	if merged[1] != "Thing" {
		t.Fatalf("merged name = %q, want the PC name %q", merged[1], "Thing")
	}
	if conflicts != 1 {
		t.Fatalf("conflicts = %d, want 1", conflicts)
	}
}

func TestMergeCorporaNonCaseConflictFails(t *testing.T) {
	pc := Corpus{1: "alpha"}
	quest := Corpus{1: "beta"}
	_, _, err := MergeCorpora(pc, quest)
	if err == nil {
		t.Fatal("conflicting names that differ beyond case were accepted")
	}
	if !strings.Contains(err.Error(), "conflict") {
		t.Fatalf("error = %v, want a conflict error", err)
	}
}

func TestMergeCorporaEmptySide(t *testing.T) {
	merged, conflicts, err := MergeCorpora(Corpus{1: "a"}, nil)
	if err != nil || len(merged) != 1 || conflicts != 0 {
		t.Fatalf("MergeCorpora(pc, nil) = %v, %d, %v", merged, conflicts, err)
	}
	merged, _, err = MergeCorpora(nil, Corpus{2: "b"})
	if err != nil || len(merged) != 1 || merged[2] != "b" {
		t.Fatalf("MergeCorpora(nil, quest) = %v, %v", merged, err)
	}
}

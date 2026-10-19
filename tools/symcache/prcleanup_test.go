package main

import (
	"reflect"
	"testing"
)

func TestBranchesToDelete(t *testing.T) {
	prs := []PREntry{
		{HeadRefName: "automation/echo-symbols-aaaaaaaaaaaa", State: "MERGED"},
		{HeadRefName: "automation/echo-symbols-bbbbbbbbbbbb", State: "CLOSED"},
		{HeadRefName: "automation/echo-symbols-cccccccccccc", State: "OPEN"},
		{HeadRefName: "automation/echo-symbols-dddddddddddd", State: "CLOSED"},
		{HeadRefName: "automation/echo-symbols-dddddddddddd", State: "OPEN"},
		{HeadRefName: "ds-harness/symcache-sync", State: "MERGED"},
		{HeadRefName: "automation/echo-symbols-eeeeeeeeeeee", State: "OPEN"},
	}
	got := BranchesToDelete(prs)
	want := []string{
		"automation/echo-symbols-aaaaaaaaaaaa",
		"automation/echo-symbols-bbbbbbbbbbbb",
	}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("BranchesToDelete = %v, want %v", got, want)
	}
}

func TestBranchesToDeleteEmpty(t *testing.T) {
	if got := BranchesToDelete(nil); len(got) != 0 {
		t.Fatalf("BranchesToDelete(nil) = %v, want none", got)
	}
}

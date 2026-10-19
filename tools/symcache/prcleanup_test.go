package main

import (
	"errors"
	"reflect"
	"testing"
)

var errTestDelete = errors.New("delete failed")

func TestBranchesToDelete(t *testing.T) {
	prs := []PRInfo{
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

func TestDeleteBranchesStopsOnError(t *testing.T) {
	var seen []string
	err := DeleteBranches([]string{
		"automation/echo-symbols-aaaaaaaaaaaa",
		"automation/echo-symbols-bbbbbbbbbbbb",
	}, func(branch string) error {
		seen = append(seen, branch)
		if branch == "automation/echo-symbols-bbbbbbbbbbbb" {
			return errTestDelete
		}
		return nil
	})
	if err == nil || !errors.Is(err, errTestDelete) {
		t.Fatalf("DeleteBranches = %v, want the delete error", err)
	}
	if len(seen) != 2 {
		t.Fatalf("delete calls = %v, want both branches attempted in order", seen)
	}
}

func TestDeleteBranchesRejectsForeignName(t *testing.T) {
	called := false
	err := DeleteBranches([]string{"ds-harness/symcache-sync"}, func(string) error {
		called = true
		return nil
	})
	if err == nil {
		t.Fatal("a branch outside the automation namespace must be refused")
	}
	if called {
		t.Fatal("the delete function must not be called for a foreign branch")
	}
}

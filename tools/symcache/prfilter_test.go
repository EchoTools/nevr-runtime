package main

import "testing"

func TestFilterPRsRejectsWindowLimit(t *testing.T) {
	prs := []PRInfo{
		{Number: 1, HeadRefName: "automation/echo-symbols-a"},
		{Number: 2, HeadRefName: "automation/echo-symbols-b"},
	}
	if _, err := FilterPRs(prs, 2); err == nil {
		t.Fatal("a listing that reaches the limit must fail, not silently truncate")
	}
	if _, err := FilterPRs(prs, 3); err != nil {
		t.Fatalf("under the limit: %v", err)
	}
}

func TestFilterPRsDropsForksAndForeignBranches(t *testing.T) {
	prs := []PRInfo{
		{Number: 1, HeadRefName: "automation/echo-symbols-real", IsCrossRepository: false},
		{Number: 2, HeadRefName: "automation/echo-symbols-fork", IsCrossRepository: true},
		{Number: 3, HeadRefName: "someone/other", IsCrossRepository: false},
	}
	got, err := FilterPRs(prs, 100)
	if err != nil {
		t.Fatalf("FilterPRs: %v", err)
	}
	if len(got) != 1 || got[0].Number != 1 {
		t.Fatalf("FilterPRs = %+v, want only the non-fork automation PR", got)
	}
}

func TestPinCheck(t *testing.T) {
	if !ValidSHA("27fd4ce81e1fc64315dcb4cf935086b3f80c2d81") {
		t.Fatal("a 40-character lowercase sha must be valid")
	}
	for _, bad := range []string{"", "27FD4CE81E1FC64315DCB4CF935086B3F80C2D81", "short", "g7fd4ce81e1fc64315dcb4cf935086b3f80c2d81"} {
		if ValidSHA(bad) {
			t.Fatalf("ValidSHA(%q) = true, want false", bad)
		}
	}
}

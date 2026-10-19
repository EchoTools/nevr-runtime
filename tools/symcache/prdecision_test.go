package main

import (
	"strings"
	"testing"
)

const (
	testPinSHA   = "27fd4ce81e1fc64315dcb4cf935086b3f80c2d81"
	testOlderSHA = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
)

func TestDecidePR(t *testing.T) {
	cases := []struct {
		name       string
		pr         int
		prHead     string
		head       string
		wantAction string
		wantNote   string
	}{
		{
			name:       "no open PR",
			pr:         0,
			prHead:     "",
			head:       testPinSHA,
			wantAction: "create",
			wantNote:   "no open PR",
		},
		{
			name:       "open PR at the same head sha",
			pr:         17,
			prHead:     testPinSHA,
			head:       testPinSHA,
			wantAction: "skip",
			wantNote:   "#17",
		},
		{
			name:       "open PR at an older head sha",
			pr:         17,
			prHead:     testOlderSHA,
			head:       testPinSHA,
			wantAction: "update",
			wantNote:   "#17",
		},
	}
	for _, testCase := range cases {
		t.Run(testCase.name, func(t *testing.T) {
			action, note, err := DecidePR(testCase.pr, testCase.prHead, testCase.head)
			if err != nil {
				t.Fatalf("DecidePR: %v", err)
			}
			if action != testCase.wantAction {
				t.Fatalf("action = %q, want %q", action, testCase.wantAction)
			}
			if !strings.Contains(note, testCase.wantNote) {
				t.Fatalf("note = %q, want it to mention %q", note, testCase.wantNote)
			}
		})
	}
}

func TestDecidePRRejectsBadInput(t *testing.T) {
	if _, _, err := DecidePR(0, "", "not-a-sha"); err == nil {
		t.Fatal("a non-sha head must fail")
	}
	if _, _, err := DecidePR(0, testPinSHA, testPinSHA); err == nil {
		t.Fatal("a PR head sha without a PR number must fail")
	}
	if _, _, err := DecidePR(7, "short", testPinSHA); err == nil {
		t.Fatal("a non-sha PR head must fail")
	}
	if _, _, err := DecidePR(-1, "", testPinSHA); err == nil {
		t.Fatal("a negative PR number must fail")
	}
}

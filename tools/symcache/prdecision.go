package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
)

const (
	prActionCreate = "create"
	prActionSkip   = "skip"
	prActionUpdate = "update"
)

// DecidePR decides what a sync run does when the automation branch may already
// have an open pull request. It never creates a second PR: a PR that already
// tracks the target sha is left alone, and one that lags is updated in place.
func DecidePR(pr int, prHead, head string) (action, note string, err error) {
	if !shaPattern.MatchString(head) {
		return "", "", fmt.Errorf("head %q is not a 40-character lowercase sha", head)
	}
	if pr < 0 {
		return "", "", fmt.Errorf("PR number %d is negative", pr)
	}
	if pr == 0 {
		if prHead != "" {
			return "", "", errors.New("a PR head sha was given without a PR number")
		}
		return prActionCreate, fmt.Sprintf("no open PR for %s; will open one", head), nil
	}
	if !shaPattern.MatchString(prHead) {
		return "", "", fmt.Errorf("PR head %q is not a 40-character lowercase sha", prHead)
	}
	if prHead == head {
		return prActionSkip, fmt.Sprintf("PR #%d already tracks %s; nothing to do", pr, head), nil
	}
	return prActionUpdate, fmt.Sprintf("PR #%d tracks %s; updating it to %s", pr, prHead, head), nil
}

func runPRDecision(args []string, stdout io.Writer) error {
	flags := flag.NewFlagSet("pr-decision", flag.ContinueOnError)
	flags.SetOutput(stdout)
	pr := flags.Int("pr", 0, "open PR number; 0 means none")
	prHead := flags.String("pr-head", "", "head sha of the open PR")
	head := flags.String("head", "", "echo-symbols head sha")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *head == "" {
		return errors.New("pr-decision: --head is required")
	}
	action, note, err := DecidePR(*pr, *prHead, *head)
	if err != nil {
		return err
	}
	fmt.Fprintf(stdout, "action=%s\n", action)
	fmt.Fprintf(stdout, "note=%s\n", note)
	return nil
}

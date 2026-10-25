package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"sort"
	"strings"
)

// automationBranchPrefix is the only branch namespace this workflow deletes.
const automationBranchPrefix = "automation/echo-symbols-"

// BranchesToDelete returns the automation branches whose PRs are all merged or
// closed. A branch that still has an open PR, or that is outside the automation
// namespace, is never returned.
func BranchesToDelete(prs []PRInfo) []string {
	open := map[string]bool{}
	for _, pr := range prs {
		if pr.State == "OPEN" && strings.HasPrefix(pr.HeadRefName, automationBranchPrefix) {
			open[pr.HeadRefName] = true
		}
	}
	seen := map[string]bool{}
	branches := make([]string, 0)
	for _, pr := range prs {
		branch := pr.HeadRefName
		if !strings.HasPrefix(branch, automationBranchPrefix) {
			continue
		}
		if pr.State != "MERGED" && pr.State != "CLOSED" {
			continue
		}
		if open[branch] || seen[branch] {
			continue
		}
		seen[branch] = true
		branches = append(branches, branch)
	}
	sort.Strings(branches)
	return branches
}

// DeleteBranches deletes each automation branch through remove and stops at the
// first error. It refuses names outside the automation namespace.
func DeleteBranches(branches []string, remove func(string) error) error {
	for _, branch := range branches {
		if !strings.HasPrefix(branch, automationBranchPrefix) {
			return fmt.Errorf("refusing to delete non-automation branch %q", branch)
		}
		if err := remove(branch); err != nil {
			return fmt.Errorf("delete %s: %w", branch, err)
		}
	}
	return nil
}

func runPRCleanup(args []string, stdout io.Writer) error {
	flags := flag.NewFlagSet("pr-cleanup", flag.ContinueOnError)
	flags.SetOutput(stdout)
	input := flags.String("input", "", "file holding `gh pr list --state all --json ...`")
	execute := flags.Bool("execute", false, "delete the branches (git push <remote> --delete)")
	remote := flags.String("remote", "origin", "git remote used by --execute")
	limit := flags.Int("limit", 1000, "fail when the listing reaches this many PRs")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *input == "" {
		return errors.New("pr-cleanup: --input is required")
	}
	data, err := os.ReadFile(*input)
	if err != nil {
		return err
	}
	var prs []PRInfo
	if err := json.Unmarshal(data, &prs); err != nil {
		return fmt.Errorf("pr-cleanup: %w", err)
	}
	prs, err = FilterPRs(prs, *limit)
	if err != nil {
		return fmt.Errorf("pr-cleanup: %w", err)
	}
	branches := BranchesToDelete(prs)
	if !*execute {
		for _, branch := range branches {
			fmt.Fprintln(stdout, branch)
		}
		return nil
	}
	return DeleteBranches(branches, func(branch string) error {
		fmt.Fprintf(stdout, "deleting %s\n", branch)
		cmd := exec.Command("git", "push", *remote, "--delete", branch)
		cmd.Stdout = stdout
		cmd.Stderr = os.Stderr
		return cmd.Run()
	})
}

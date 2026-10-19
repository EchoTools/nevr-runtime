package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"sort"
	"strings"
)

// automationBranchPrefix is the only branch namespace this workflow deletes.
const automationBranchPrefix = "automation/echo-symbols-"

// PREntry is the subset of `gh pr list --json headRefName,state` this tool reads.
type PREntry struct {
	HeadRefName string `json:"headRefName"`
	State       string `json:"state"`
}

// BranchesToDelete returns the automation branches whose PRs are all merged or
// closed. A branch that still has an open PR, or that is outside the automation
// namespace, is never returned.
func BranchesToDelete(prs []PREntry) []string {
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

func runPRCleanup(args []string, stdout io.Writer) error {
	flags := flag.NewFlagSet("pr-cleanup", flag.ContinueOnError)
	flags.SetOutput(stdout)
	input := flags.String("input", "", "file holding `gh pr list --state all --json headRefName,state`")
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
	var prs []PREntry
	if err := json.Unmarshal(data, &prs); err != nil {
		return fmt.Errorf("pr-cleanup: %w", err)
	}
	for _, branch := range BranchesToDelete(prs) {
		fmt.Fprintln(stdout, branch)
	}
	return nil
}

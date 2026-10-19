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

const (
	prActionCreate = "create"
	prActionSkip   = "skip"
	prActionUpdate = "update"
)

// PRInfo is one automation PR as the workflow gathers it: the PR itself plus
// the echo-symbols sha its branch's tools/symbols.pin records. HeadRefOid is a
// nevr-runtime commit and is never compared with the echo-symbols head.
type PRInfo struct {
	Number      int    `json:"number"`
	HeadRefName string `json:"headRefName"`
	HeadRefOid  string `json:"headRefOid"`
	State       string `json:"state"`
	Pin         string `json:"pin"`
}

// PRPlan is what one sync run should do with the automation PR.
type PRPlan struct {
	Action string
	PR     int
	Branch string
	Tip    string
	Note   string
}

// PlanPR never opens a second PR: an open automation PR whose pin is the head
// is left alone; one whose pin is older is updated in place; a head rejected by
// a closed-unmerged PR is skipped; otherwise one PR is created.
func PlanPR(head string, open, closed []PRInfo) (PRPlan, error) {
	if !shaPattern.MatchString(head) {
		return PRPlan{}, fmt.Errorf("head %q is not a 40-character lowercase sha", head)
	}
	openAuto := make([]PRInfo, 0)
	for _, pr := range open {
		if !strings.HasPrefix(pr.HeadRefName, automationBranchPrefix) {
			continue
		}
		if !shaPattern.MatchString(pr.Pin) {
			return PRPlan{}, fmt.Errorf("PR #%d pin %q is not a 40-character lowercase sha", pr.Number, pr.Pin)
		}
		if !shaPattern.MatchString(pr.HeadRefOid) {
			return PRPlan{}, fmt.Errorf("PR #%d headRefOid %q is not a 40-character lowercase sha", pr.Number, pr.HeadRefOid)
		}
		openAuto = append(openAuto, pr)
	}
	sort.Slice(openAuto, func(i, j int) bool { return openAuto[i].Number < openAuto[j].Number })
	for _, pr := range openAuto {
		if pr.Pin == head {
			return PRPlan{Action: prActionSkip, PR: pr.Number, Branch: pr.HeadRefName,
				Tip: pr.HeadRefOid, Note: fmt.Sprintf("PR #%d already pins %s; nothing to do", pr.Number, head)}, nil
		}
	}
	if len(openAuto) > 0 {
		pr := openAuto[0]
		return PRPlan{Action: prActionUpdate, PR: pr.Number, Branch: pr.HeadRefName,
			Tip: pr.HeadRefOid, Note: fmt.Sprintf("PR #%d pins %s; moving it to %s", pr.Number, pr.Pin, head)}, nil
	}

	closedAuto := make([]PRInfo, 0)
	for _, pr := range closed {
		if !strings.HasPrefix(pr.HeadRefName, automationBranchPrefix) || pr.State != "CLOSED" {
			continue
		}
		if !shaPattern.MatchString(pr.Pin) {
			return PRPlan{}, fmt.Errorf("closed PR #%d pin %q is not a 40-character lowercase sha", pr.Number, pr.Pin)
		}
		closedAuto = append(closedAuto, pr)
	}
	sort.Slice(closedAuto, func(i, j int) bool { return closedAuto[i].Number < closedAuto[j].Number })
	for _, pr := range closedAuto {
		if pr.Pin == head {
			return PRPlan{Action: prActionSkip, PR: pr.Number, Note: fmt.Sprintf(
				"PR #%d was closed unmerged while pinning %s; not reopening it until echo-symbols moves", pr.Number, head)}, nil
		}
	}

	return PRPlan{Action: prActionCreate, Branch: automationBranchPrefix + head[:12],
		Note: fmt.Sprintf("no automation PR pins %s; opening one", head)}, nil
}

func runPRPlan(args []string, stdout io.Writer) error {
	flags := flag.NewFlagSet("pr-plan", flag.ContinueOnError)
	flags.SetOutput(stdout)
	head := flags.String("head", "", "echo-symbols head sha")
	openPath := flags.String("open", "", "JSON array of open automation PRs with pins")
	closedPath := flags.String("closed", "", "JSON array of closed automation PRs with pins")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *head == "" {
		return errors.New("pr-plan: --head is required")
	}
	read := func(path string) ([]PRInfo, error) {
		if path == "" {
			return nil, nil
		}
		data, err := os.ReadFile(path)
		if err != nil {
			return nil, err
		}
		var prs []PRInfo
		if err := json.Unmarshal(data, &prs); err != nil {
			return nil, fmt.Errorf("%s: %w", path, err)
		}
		return prs, nil
	}
	open, err := read(*openPath)
	if err != nil {
		return err
	}
	closed, err := read(*closedPath)
	if err != nil {
		return err
	}
	plan, err := PlanPR(*head, open, closed)
	if err != nil {
		return err
	}
	fmt.Fprintf(stdout, "action=%s\npr=%d\nbranch=%s\ntip=%s\nnote=%s\n",
		plan.Action, plan.PR, plan.Branch, plan.Tip, plan.Note)
	return nil
}

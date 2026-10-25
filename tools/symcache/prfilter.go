package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
)

// ValidSHA reports whether s is a 40-character lowercase hex commit sha.
func ValidSHA(s string) bool {
	return shaPattern.MatchString(s)
}

// FilterPRs enforces the listing window and keeps only non-fork PRs on the
// automation branch namespace. A listing that reaches the limit is an error,
// because a PR outside the window could otherwise let a second PR open.
func FilterPRs(prs []PRInfo, limit int) ([]PRInfo, error) {
	if limit > 0 && len(prs) >= limit {
		return nil, fmt.Errorf("PR listing reached the %d limit; a matching PR may be outside the window", limit)
	}
	filtered := make([]PRInfo, 0, len(prs))
	for _, pr := range prs {
		if pr.IsCrossRepository {
			continue
		}
		if !hasAutomationPrefix(pr.HeadRefName) {
			continue
		}
		filtered = append(filtered, pr)
	}
	return filtered, nil
}

func hasAutomationPrefix(branch string) bool {
	return len(branch) >= len(automationBranchPrefix) && branch[:len(automationBranchPrefix)] == automationBranchPrefix
}

func runPRFilter(args []string, stdout io.Writer) error {
	flags := flag.NewFlagSet("pr-filter", flag.ContinueOnError)
	flags.SetOutput(stdout)
	input := flags.String("input", "", "raw `gh pr list --json ...` output")
	limit := flags.Int("limit", 1000, "fail when the listing reaches this many PRs")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *input == "" {
		return errors.New("pr-filter: --input is required")
	}
	data, err := os.ReadFile(*input)
	if err != nil {
		return err
	}
	var prs []PRInfo
	if err := json.Unmarshal(data, &prs); err != nil {
		return fmt.Errorf("pr-filter: %w", err)
	}
	filtered, err := FilterPRs(prs, *limit)
	if err != nil {
		return err
	}
	out, err := json.MarshalIndent(filtered, "", " ")
	if err != nil {
		return err
	}
	fmt.Fprintln(stdout, string(out))
	return nil
}

func runPinCheck(args []string, stdout io.Writer) error {
	flags := flag.NewFlagSet("pin-check", flag.ContinueOnError)
	flags.SetOutput(stdout)
	sha := flags.String("sha", "", "sha to validate")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if !ValidSHA(*sha) {
		return fmt.Errorf("pin-check: %q is not a 40-character lowercase sha", *sha)
	}
	return nil
}

package main

import (
	"strings"
	"testing"
)

// Real shapes: HeadRefOid is a nevr-runtime commit; Pin is the echo-symbols sha
// that commit's tools/symbols.pin records. They are different namespaces.
const (
	echoHead  = "27fd4ce81e1fc64315dcb4cf935086b3f80c2d81"
	echoOlder = "e6a4f0d2b7c9a8135f0d4e6b8c2a1d3f5e7b9c0d"
	nevrTip   = "4c5a12727e92534330fc3c73158b0e42294564c4"
)

func TestPlanPRSkipsWhenOpenPRPinsTheEchoHead(t *testing.T) {
	open := []PRInfo{{
		Number: 17, HeadRefName: "automation/echo-symbols-old", HeadRefOid: nevrTip,
		State: "OPEN", Pin: echoHead,
	}}
	plan, err := PlanPR(echoHead, open, nil)
	if err != nil {
		t.Fatalf("PlanPR: %v", err)
	}
	if plan.Action != prActionSkip || plan.PR != 17 {
		t.Fatalf("plan = %+v, want skip #17 (the nevr headRefOid is not the pin)", plan)
	}
}

func TestPlanPRUpdatesTheOpenPRWhenEchoMoves(t *testing.T) {
	open := []PRInfo{{
		Number: 17, HeadRefName: "automation/echo-symbols-old", HeadRefOid: nevrTip,
		State: "OPEN", Pin: echoOlder,
	}}
	plan, err := PlanPR(echoHead, open, nil)
	if err != nil {
		t.Fatalf("PlanPR: %v", err)
	}
	if plan.Action != prActionUpdate || plan.PR != 17 || plan.Branch != "automation/echo-symbols-old" || plan.Tip != nevrTip {
		t.Fatalf("plan = %+v, want update #17 on its existing branch and tip", plan)
	}
}

func TestPlanPRSkipsHeadRejectedByClosedUnmergedPR(t *testing.T) {
	closed := []PRInfo{{
		Number: 9, HeadRefName: "automation/echo-symbols-rejected", HeadRefOid: nevrTip,
		State: "CLOSED", Pin: echoHead,
	}}
	plan, err := PlanPR(echoHead, nil, closed)
	if err != nil {
		t.Fatalf("PlanPR: %v", err)
	}
	if plan.Action != prActionSkip || plan.PR != 9 {
		t.Fatalf("plan = %+v, want skip #9 (the head was rejected)", plan)
	}
}

func TestPlanPRCreatesWhenNoPRAutomationPRExists(t *testing.T) {
	plan, err := PlanPR(echoHead, nil, nil)
	if err != nil {
		t.Fatalf("PlanPR: %v", err)
	}
	if plan.Action != prActionCreate || !strings.HasPrefix(plan.Branch, "automation/echo-symbols-") {
		t.Fatalf("plan = %+v, want create on a new automation branch", plan)
	}
}

func TestPlanPRRejectsBadInput(t *testing.T) {
	if _, err := PlanPR("nope", nil, nil); err == nil {
		t.Fatal("a non-sha head must fail")
	}
	if _, err := PlanPR(echoHead, []PRInfo{{Number: 1, HeadRefName: "automation/echo-symbols-x", HeadRefOid: nevrTip, State: "OPEN", Pin: "bad"}}, nil); err == nil {
		t.Fatal("an unreadable pin must fail, not guess")
	}
}

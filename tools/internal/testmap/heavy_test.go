package testmap

import (
	"strings"
	"testing"
)

func TestClassifyMakesTheAndroidAndQuestRecipesHeavyByTheirName(t *testing.T) {
	t.Parallel()
	for _, id := range []string{"just:test-android", "just:test-quest-hooks", "just:test-quest-data-net", "just:test-quest-anything-new"} {
		c := Classify(id)
		if !c.Heavy || c.Rule != RuleToolchain || c.Reason == "" {
			t.Errorf("Classify(%q) = %+v, want heavy by the toolchain rule with a reason", id, c)
		}
	}
	// A look-alike is a unit test: the rule is the recipe's whole name or its prefix, not a substring.
	for _, id := range []string{"just:test-installer", "go:cmd/test-quest-x", "pytest:test_quest_store_install", "gtest:test_android", "just:test-android-x"} {
		if c := Classify(id); c.Heavy {
			t.Errorf("Classify(%q) = %+v, want a unit test", id, c)
		}
	}
}

func TestClassifyMakesAnIdOverTheBoundHeavyByMeasurement(t *testing.T) {
	t.Parallel()
	over := []Measurement{{"pytest:slow", HeavyBound.Milliseconds() + 1}, {"pytest:edge", HeavyBound.Milliseconds()}}
	if c := classify("pytest:slow", over); !c.Heavy || c.Rule != RuleMeasurement || c.Millis != HeavyBound.Milliseconds()+1 {
		t.Errorf("slow = %+v, want heavy by measurement", c)
	}
	if c := classify("pytest:edge", over); c.Heavy {
		t.Errorf("an id exactly at the bound = %+v, want a unit test", c)
	}
	if c := classify("pytest:unmeasured", over); c.Heavy {
		t.Errorf("an id with no measurement = %+v, want a unit test", c)
	}
}

func TestEveryMeasuredIdIsOverTheBoundAndClassifiedHeavy(t *testing.T) {
	t.Parallel()
	if err := Validate(nil, Measured); err != nil {
		t.Fatal(err)
	}
	for _, e := range Measured {
		if c := Classify(e.ID); !c.Heavy {
			t.Errorf("Classify(%q) = %+v, want heavy by measurement", e.ID, c)
		}
	}
	// The two ids the shelf names are not classified here.
	for _, id := range []string{"pytest:test_run_tool_tests", "pytest:test_package_release"} {
		if Classify(id).Heavy {
			t.Errorf("%s is classified heavy: a heavy class must not hide a failure", id)
		}
	}
}

func TestValidateRefusesADuplicateAnUnrunnableIdAnUnderBoundEntryAndARuleIdListedAgain(t *testing.T) {
	t.Parallel()
	over := HeavyBound.Milliseconds() + 1
	m, err := parseFixture(`{"schema": 2, "about": [], "targets": {"build": ["just", "build"]}, "scanners": [], "components": []}`)
	if err != nil {
		t.Fatal(err)
	}
	cases := []struct {
		name string
		list []Measurement
		want string
	}{
		{"duplicate", []Measurement{{"pytest:a", over}, {"pytest:a", over}}, "listed twice"},
		{"not an id the map can run", []Measurement{{"nokind", over}}, "neither a target"},
		{"under the bound", []Measurement{{"pytest:a", HeavyBound.Milliseconds()}}, "not over"},
		{"not positive", []Measurement{{"just:test-quest-x", 0}}, "positive"},
	}
	for _, c := range cases {
		err := Validate(m, c.list)
		if err == nil || !strings.Contains(err.Error(), c.want) {
			t.Errorf("%s: error = %v, want it to contain %q", c.name, err, c.want)
		}
	}
	if err := Validate(m, []Measurement{{"pytest:a", over}, {"just:test-quest-x", 1}}); err != nil {
		t.Errorf("a good list: %v", err)
	}
}

func TestSplitHeavyKeepsOrderAndRefusesAnIdTwice(t *testing.T) {
	t.Parallel()
	unit, heavy, err := SplitHeavy([]string{"build", "gtest:a", "just:test-android", "pytest:test_launch_client", "go:cmd/x"})
	if err != nil {
		t.Fatal(err)
	}
	if strings.Join(unit, " ") != "build gtest:a go:cmd/x" {
		t.Errorf("unit = %v", unit)
	}
	if len(heavy) != 2 || heavy[0].ID != "just:test-android" || heavy[1].ID != "pytest:test_launch_client" {
		t.Errorf("heavy = %+v", heavy)
	}
	if _, _, err := SplitHeavy([]string{"gtest:a", "gtest:a"}); err == nil || !strings.Contains(err.Error(), "twice") {
		t.Errorf("a duplicate id: error = %v", err)
	}
}

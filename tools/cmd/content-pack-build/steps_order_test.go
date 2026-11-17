package main

import (
	"bytes"
	"context"
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"io/fs"
	"os"
	"reflect"
	"slices"
	"strings"
	"testing"
)

// The synthetic steps of the order tests: three registered as X, Y, Z, and M, which only a chain through it relates.
const (
	orderX     = "X step"
	orderY     = "Y step"
	orderZ     = "Z step"
	orderM     = "M step"
	noSuchStep = "no such step"
	// The record the synthetic steps of TestRunUsesTheOrderedList write, as Put takes it.
	synthType   int64 = 0x41
	synthRecord int64 = 0x42
)

// lowestFirst and highestFirst are the two tie orders the tests use: ascending and descending registration index.
func lowestFirst(a, b int) bool  { return a < b }
func highestFirst(a, b int) bool { return a > b }

// joined is the names of a list in order, as one comparable string.
func joined(names ...string) string { return strings.Join(names, ",") }

// namesOf is the step names of a list, in order.
func namesOf(list []Step) string {
	names := make([]string, 0, len(list))
	for _, st := range list {
		names = append(names, st.Name)
	}
	return joined(names...)
}

// mustProductionSteps is productionSteps, failing the test when the registry does not order.
func mustProductionSteps(t *testing.T) []Step {
	t.Helper()
	list, err := productionSteps()
	if err != nil {
		t.Fatal(err)
	}
	return list
}

func TestOrderWithoutDeclarationsIsRegistrationOrder(t *testing.T) {
	list := []Step{{Name: orderX}, {Name: orderY}, {Name: orderZ}}
	got, err := ordered(list)
	if err != nil {
		t.Fatal(err)
	}
	if want := joined(orderX, orderY, orderZ); namesOf(got) != want {
		t.Errorf("ordered gives %s, want %s", namesOf(got), want)
	}
	got, err = orderedWith(list, highestFirst)
	if err != nil {
		t.Fatal(err)
	}
	if want := joined(orderZ, orderY, orderX); namesOf(got) != want {
		t.Errorf("orderedWith descending gives %s, want %s", namesOf(got), want)
	}
}

func TestOrderFollowsAfterAndBefore(t *testing.T) {
	// X runs after Z (X.After) in one list; in the other Z runs before X (Z.Before). Both must give the same order.
	after := []Step{{Name: orderX, After: []string{orderZ}}, {Name: orderY}, {Name: orderZ}}
	before := []Step{{Name: orderX}, {Name: orderY}, {Name: orderZ, Before: []string{orderX}}}
	cases := []struct {
		name string
		list []Step
		tie  func(a, b int) bool
		want string
	}{
		{"After, lowest first", after, lowestFirst, joined(orderY, orderZ, orderX)},
		{"After, highest first", after, highestFirst, joined(orderZ, orderY, orderX)},
		{"Before, lowest first", before, lowestFirst, joined(orderY, orderZ, orderX)},
		{"Before, highest first", before, highestFirst, joined(orderZ, orderY, orderX)},
	}
	for _, c := range cases {
		got, err := orderedWith(c.list, c.tie)
		if err != nil {
			t.Errorf("%s: %v", c.name, err)
			continue
		}
		if namesOf(got) != c.want {
			t.Errorf("%s: gives %s, want %s", c.name, namesOf(got), c.want)
		}
	}
}

// TestOrderRefusesWhatCannotBeOrdered is the refusals: a cycle, a declared name no step has (in each of the three
// declarations), a step that names itself, and a name registered twice. Each error names the steps it is about.
func TestOrderRefusesWhatCannotBeOrdered(t *testing.T) {
	cases := []struct {
		name  string
		list  []Step
		wants []string
	}{
		{"a cycle", []Step{{Name: orderX, After: []string{orderY}}, {Name: orderY, After: []string{orderX}}, {Name: orderZ}},
			[]string{"cycle", orderX, orderY}},
		{"an After naming no step", []Step{{Name: orderX, After: []string{noSuchStep}}}, []string{orderX, "names no registered step"}},
		{"a Before naming no step", []Step{{Name: orderX, Before: []string{noSuchStep}}}, []string{orderX, "names no registered step"}},
		{"an OrderFree naming no step", []Step{{Name: orderX, OrderFree: []string{noSuchStep}}}, []string{orderX, "names no registered step"}},
		{"a step naming itself", []Step{{Name: orderX, After: []string{orderX}}}, []string{orderX, "names itself"}},
		{"a name registered twice", []Step{{Name: orderX}, {Name: orderX}}, []string{"registered twice"}},
	}
	for _, c := range cases {
		_, err := ordered(c.list)
		if err == nil {
			t.Errorf("%s: no error", c.name)
			continue
		}
		for _, w := range c.wants {
			if !strings.Contains(err.Error(), w) {
				t.Errorf("%s: error %q does not name %q", c.name, err, w)
			}
		}
	}
}

// TestSharedRecordPairs is the pairs of steps that write one record: a pair is named unless an After/Before chain (in
// either direction, through any number of steps) or an OrderFree declaration relates it.
func TestSharedRecordPairs(t *testing.T) {
	write := func() []Field {
		return []Field{{Name: "w", Set: Sets[0], Type: tA, Records: []uint64{rOne}}}
	}
	chainWrites := []Field{{Name: "other", Set: Sets[0], Type: tB, Records: []uint64{rTwo}}}
	cases := []struct {
		name string
		list []Step
		want []string
	}{
		{"two writers, no declaration", []Step{{Name: orderX, Writes: write()}, {Name: orderY, Writes: write()}},
			[]string{orderX + " | " + orderY}},
		{"the later declares After", []Step{{Name: orderX, Writes: write()}, {Name: orderY, After: []string{orderX}, Writes: write()}}, nil},
		{"the earlier declares Before", []Step{{Name: orderX, Before: []string{orderY}, Writes: write()}, {Name: orderY, Writes: write()}}, nil},
		{"the first declares OrderFree", []Step{{Name: orderX, OrderFree: []string{orderY}, Writes: write()}, {Name: orderY, Writes: write()}}, nil},
		{"the second declares OrderFree", []Step{{Name: orderX, Writes: write()}, {Name: orderY, OrderFree: []string{orderX}, Writes: write()}}, nil},
		{"related through a chain", []Step{
			{Name: orderX, Writes: write()},
			{Name: orderY, After: []string{orderM}, Writes: write()},
			{Name: orderM, After: []string{orderX}, Writes: chainWrites},
		}, nil},
	}
	for _, c := range cases {
		got := unorderedPairs(c.list)
		if strings.Join(got, ";") != strings.Join(c.want, ";") {
			t.Errorf("%s: unordered pairs %v, want %v", c.name, got, c.want)
		}
	}
}

// orderViolations is each way the list's order breaks its declarations, or repeats a step, as a message.
func orderViolations(list []Step) []string {
	var out []string
	pos := map[string]int{}
	for i, st := range list {
		if _, dup := pos[st.Name]; dup {
			out = append(out, fmt.Sprintf("%q is in the order twice", st.Name))
		}
		pos[st.Name] = i
	}
	for i, st := range list {
		for _, a := range st.After {
			if p, ok := pos[a]; !ok || p >= i {
				out = append(out, fmt.Sprintf("%q declares After %q, which does not run before it", st.Name, a))
			}
		}
		for _, b := range st.Before {
			if p, ok := pos[b]; !ok || p <= i {
				out = append(out, fmt.Sprintf("%q declares Before %q, which does not run after it", st.Name, b))
			}
		}
	}
	return out
}

// TestTheRegisteredStepsAreOrderedAndEachOnce checks the production order: each step once, and each declaration holds in
// the order the list gives. The check itself is run on a list that breaks a declaration, so it cannot pass by accident.
func TestTheRegisteredStepsAreOrderedAndEachOnce(t *testing.T) {
	list := mustProductionSteps(t)
	if len(list) != len(steps) {
		t.Fatalf("the production order has %d steps, the registry %d", len(list), len(steps))
	}
	if v := orderViolations(list); len(v) != 0 {
		t.Errorf("the production order breaks its declarations: %v", v)
	}
	broken := []Step{{Name: orderX, After: []string{orderY}}, {Name: orderY}}
	if v := orderViolations(broken); len(v) != 1 {
		t.Errorf("a list that runs %s before its After %s gives %d violations, want 1: %v", orderX, orderY, len(v), v)
	}
}

// TestEverySharedRecordIsOrdered is the registry's own check: a step that writes a record another step writes declares
// the order or OrderFree, and the message names the pairs that do not.
func TestEverySharedRecordIsOrdered(t *testing.T) {
	if pairs := unorderedPairs(steps); len(pairs) != 0 {
		t.Errorf("%d pairs of steps write one record with no order between them (name the step that runs first in After, "+
			"the later one in Before, or both in OrderFree): %s", len(pairs), strings.Join(pairs, "; "))
	}
}

// TestRunUsesTheOrderedList runs the build with two synthetic steps in registration order Y, X where X declares Before Y:
// the run must apply X first. It replaces the registry for the test and restores it after.
func TestRunUsesTheOrderedList(t *testing.T) {
	saved := steps
	t.Cleanup(func() { steps = saved })
	f := newFixture(t)
	var applied []string
	apply := func(name string) func(p *Pack) error {
		return func(p *Pack) error {
			applied = append(applied, name)
			return p.Put(Sets[0], synthType, synthRecord, []byte(name))
		}
	}
	steps = []Step{
		{Name: orderY, Apply: apply(orderY)},
		{Name: orderX, Apply: apply(orderX), Before: []string{orderY}},
	}
	var stdout, stderr bytes.Buffer
	code := run(context.Background(), []string{"-game-dir", f.game, "-out", f.out("order"), "-id", "order", "-version", "1"},
		func(k string) string { return f.env[k] }, nil, &stdout, &stderr)
	if code != 0 {
		t.Fatalf("exit %d: %s", code, stderr.String())
	}
	if want := joined(orderX, orderY); joined(applied...) != want {
		t.Errorf("the steps ran as %v, want %s", applied, want)
	}
}

// TestTheRegistrationsAreTheLiteralsOfTheSteps counts the Step literals of the package's non-test files: each registered
// step is one literal, so a step whose init no longer registers it, or a literal left out of the registry, is a difference.
func TestTheRegistrationsAreTheLiteralsOfTheSteps(t *testing.T) {
	fset := token.NewFileSet()
	pkgs, err := parser.ParseDir(fset, ".", func(fi fs.FileInfo) bool { return !strings.HasSuffix(fi.Name(), "_test.go") }, 0)
	if err != nil {
		t.Fatal(err)
	}
	literals := 0
	for _, pkg := range pkgs {
		for _, file := range pkg.Files {
			ast.Inspect(file, func(n ast.Node) bool {
				lit, ok := n.(*ast.CompositeLit)
				if !ok {
					return true
				}
				if id, ok := lit.Type.(*ast.Ident); ok && id.Name == "Step" {
					literals++
				}
				return true
			})
		}
	}
	if literals != len(steps) {
		t.Errorf("the package holds %d Step literals and the registry %d steps: a step is registered by a literal in an init, "+
			"or its literal is not appended to steps", literals, len(steps))
	}
}

// withoutUnsetGearIcons is list without the gear icons step when gearIconsEnv is not set, as stockSteps leaves it out.
func withoutUnsetGearIcons(list []Step) []Step {
	if os.Getenv(gearIconsEnv) != "" {
		return list
	}
	return slices.DeleteFunc(slices.Clone(list), func(st Step) bool { return st.Name == gearIconsStepName })
}

// TestTheTieOrderOfUnrelatedStepsChangesNoByte is the independent measurement of "the declared order is enough": applying the
// steps in the production order and in the reverse tie order must give the same changes, scripts and rekeys. Gated on
// NEVR_STOCK_DATA (a stock PC data folder); it is not run without one.
func TestTheTieOrderOfUnrelatedStepsChangesNoByte(t *testing.T) {
	data := os.Getenv("NEVR_STOCK_DATA")
	if data == "" {
		t.Skip("NEVR_STOCK_DATA is not set (a stock PC data folder: manifests/ and packages/)")
	}
	production := stockSteps(t)
	reversed, err := orderedWith(withoutUnsetGearIcons(steps), highestFirst)
	if err != nil {
		t.Fatal(err)
	}
	apply := func(list []Step) *Pack {
		p := newStockPack(t, data)
		for _, st := range list {
			if err := st.Apply(p); err != nil {
				t.Fatalf("%s: %v", st.Name, err)
			}
		}
		return p
	}
	a, b := apply(production), apply(reversed)
	if !reflect.DeepEqual(a.changes, b.changes) {
		t.Error("the changed resources differ between the production order and the reverse tie order")
	}
	if !reflect.DeepEqual(a.scripts, b.scripts) {
		t.Error("the scripts differ between the production order and the reverse tie order")
	}
	if !reflect.DeepEqual(a.rekeys, b.rekeys) {
		t.Error("the rekeys differ between the production order and the reverse tie order")
	}
}

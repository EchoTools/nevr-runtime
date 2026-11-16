package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"regexp"
	"slices"
	"strings"
	"testing"
)

// The runtime's tail order is the header's: a change there and not here (or the reverse) fails.
func TestTheTacModOrderIsTheRuntimes(t *testing.T) {
	header, err := os.ReadFile(filepath.Join("..", "..", "..", "src", "runtime", "patch", "ec_rules.h"))
	if err != nil {
		t.Fatal(err)
	}
	m := regexp.MustCompile(`kNewTacMods\[3\]\[8\] = \{"(\w+)", "(\w+)", "(\w+)"\};`).FindSubmatch(header)
	if m == nil {
		t.Fatal("ec_rules.h has no kNewTacMods line of three names")
	}
	if got := []string{string(m[1]), string(m[2]), string(m[3])}; !slices.Equal(got, tacModOrder) {
		t.Errorf("ec_rules.h kNewTacMods is %v, tacModOrder is %v", got, tacModOrder)
	}
}

// Each tac mod goes in front of the first entry of a later rank, else at the end, so the three steps give
// smoke, buff, point whatever order they run in.
func TestATacModIsInsertedAtItsRank(t *testing.T) {
	smoke, buff, point := gearEntry{"smoke", "S"}, gearEntry{"buff", "B"}, gearEntry{"point", "P"}
	for _, c := range []struct {
		name string
		in   string
		e    gearEntry
		want string
	}{
		{"point after buff", `{"gear_table":{"tacmods":[{"name":"heal"},{"name":"buff"}]}}`, point,
			`{"gear_table":{"tacmods":[{"name":"heal"},{"name":"buff"},{"name":"point","display_name":"P"}]}}`},
		{"smoke before buff", `{"gear_table":{"tacmods":[{"name":"heal"},{"name":"buff"},{"name":"point"}]}}`, smoke,
			`{"gear_table":{"tacmods":[{"name":"heal"},{"name":"smoke","display_name":"S"},{"name":"buff"},{"name":"point"}]}}`},
		{"smoke before point when buff is absent", `{"gear_table":{"tacmods":[{"name":"heal"},{"name":"point"}]}}`, smoke,
			`{"gear_table":{"tacmods":[{"name":"heal"},{"name":"smoke","display_name":"S"},{"name":"point"}]}}`},
		{"buff after smoke", `{"gear_table":{"tacmods":[{"name":"heal"},{"name":"smoke"}]}}`, buff,
			`{"gear_table":{"tacmods":[{"name":"heal"},{"name":"smoke"},{"name":"buff","display_name":"B"}]}}`},
		{"an empty list", `{"gear_table":{"tacmods":[]}}`, buff,
			`{"gear_table":{"tacmods":[{"name":"buff","display_name":"B"}]}}`},
	} {
		out, err := insertTacmod(jsonResource(c.in), c.e, nil)
		if err != nil {
			t.Fatalf("%s: %v", c.name, err)
		}
		if got := string(out[jsonHeader:]); got != c.want {
			t.Errorf("%s\n got %s\nwant %s", c.name, got, c.want)
		}
		if binary.LittleEndian.Uint64(out[jsonLengthOff:]) != uint64(len(c.want)) {
			t.Errorf("%s: the header's length is not the new body's", c.name)
		}
	}
}

func TestATacModTheRuntimeDoesNotOrderOrAListThatHasItIsRefused(t *testing.T) {
	in := jsonResource(`{"gear_table":{"tacmods":[{"name":"heal"},{"name":"point"}]}}`)
	if _, err := insertTacmod(in, gearEntry{"heal2", "X"}, nil); err == nil || !strings.Contains(err.Error(), "not one of the tac mods") {
		t.Errorf("an unordered name: %v", err)
	}
	if _, err := insertTacmod(in, gearEntry{"point", "X"}, nil); err == nil || !strings.Contains(err.Error(), "already lists point") {
		t.Errorf("a name already listed: %v", err)
	}
	refuse := func(map[string]bool) error { return os.ErrInvalid }
	if _, err := insertTacmod(in, gearEntry{"buff", "X"}, refuse); err != os.ErrInvalid {
		t.Errorf("the step's own check was not applied: %v", err)
	}
}

func TestTheTacModReaderReadsOnlyItsEntry(t *testing.T) {
	b := jsonResource(`{"gear_table":{"tacmods":[{"name":"heal","display_name":"H"},{"name":"buff","display_name":"B","x":1},{"name":"point"}]}}`)
	got, err := readTacmod("buff")(b)
	if want := `{"name":"buff","display_name":"B"}` + "\n"; err != nil || string(got) != want {
		t.Errorf("buff %q, %v; want %q", got, err, want)
	}
	if got, err := readTacmod("smoke")(b); err != nil || len(got) != 0 {
		t.Errorf("an absent entry read as %q, %v", got, err)
	}
	if _, err := readTacmod("buff")(jsonResource(`{"gear_table":{"weapons":[]}}`)); err == nil {
		t.Error("a resource with no tacmods list was read")
	}
}

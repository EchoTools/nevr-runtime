package contracts

import (
	"net/url"
	"testing"
)

func TestRegistrationSendsNoPolicyWithoutOverrides(t *testing.T) {
	q := RegistrationQuery(RegistrationFacts{Version: "5.6.0", Address: "203.0.113.7:6792", PingMs: map[string]int{"us-east": 31}, Features: []string{"combat"}}, Overrides{})
	v, err := url.ParseQuery(q)
	if err != nil {
		t.Fatal(err)
	}
	for _, k := range []string{"guilds", "regions"} {
		if _, has := v[k]; has {
			t.Errorf("%s sent with no override set", k)
		}
	}
	if v.Get("version") != "5.6.0" || v.Get("address") != "203.0.113.7:6792" {
		t.Errorf("identity facts missing: %v", v)
	}
}

func TestRegistrationOverridesAreOptional(t *testing.T) {
	q := RegistrationQuery(RegistrationFacts{Version: "5.6.0"}, Overrides{Guilds: []string{"g1", "g2"}, Regions: []string{"eu"}})
	v, _ := url.ParseQuery(q)
	if v.Get("guilds") != "g1,g2" || v.Get("regions") != "eu" {
		t.Fatalf("override not carried: %v", v)
	}
}

func TestAssignmentAnswer(t *testing.T) {
	a, err := ParseAssignment([]byte(`{"guilds":["7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a"],"regions":["us-east"]}`))
	if err != nil || len(a.Guilds) != 1 || a.Regions[0] != "us-east" {
		t.Fatalf("%+v %v", a, err)
	}
	if _, err := ParseAssignment([]byte(`{"guilds":["not-a-uuid"],"regions":[]}`)); err == nil {
		t.Error("a guild id that is not a uuid was accepted")
	}
	if _, err := ParseAssignment([]byte(`{"guilds":[],"regions":[],"extra":1}`)); err == nil {
		t.Error("unknown field accepted")
	}
	if a, err := ParseAssignment([]byte(`{"guilds":[],"regions":[]}`)); err != nil || len(a.Guilds) != 0 {
		t.Errorf("an empty assignment is valid (the server idles): %+v %v", a, err)
	}
}

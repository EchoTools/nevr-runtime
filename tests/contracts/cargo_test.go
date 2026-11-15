package contracts

import (
	"strings"
	"testing"
)

var (
	hexA = strings.Repeat("a", 64)
	hexB = strings.Repeat("b", 64)
	hexC = strings.Repeat("c", 64)
	hexD = strings.Repeat("d", 64)
)

func ch(h string) CargoChoice {
	return CargoChoice{URL: "https://cdn.example/m-" + h[:1] + ".json", SHA256: h}
}

func TestCargoPrecedence(t *testing.T) {
	login := CargoLogin{Session: ptr(ch(hexA)), PerGuild: map[string]CargoChoice{"g1": ch(hexB)}}
	def := ch(hexD)
	if got := SelectManifest(login, "g1", def); got.SHA256 != hexB {
		t.Errorf("guild row must win: %s", got.SHA256)
	}
	if got := SelectManifest(login, "g2", def); got.SHA256 != hexA {
		t.Errorf("no guild row: the session's choice: %s", got.SHA256)
	}
	if got := SelectManifest(CargoLogin{}, "g2", def); got.SHA256 != hexD {
		t.Errorf("nothing named: the default: %s", got.SHA256)
	}
}

func TestCargoFalsifier(t *testing.T) {
	login := CargoLogin{Session: ptr(ch(hexA)), PerGuild: map[string]CargoChoice{"g1": ch(hexB)}}
	def := ch(hexD)
	if !MountAllowed(hexA, login, def) || !MountAllowed(hexB, login, def) || !MountAllowed(hexD, login, def) {
		t.Error("nakama's and the default's manifests mount")
	}
	if MountAllowed(hexC, login, def) {
		t.Error("a manifest whose hash is neither nakama's nor the default's must not mount")
	}
}

func TestCargoLoginShape(t *testing.T) {
	good := `{"nevr_install":"7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a","nevr_cargo":{"schema":1,"session":{"url":"https://cdn.example/m.json","sha256":"` + hexA + `"},"guilds":{"7c1e2b0a-5d3f-4a8e-9b6c-1f2e3d4c5b6a":{"url":"https://cdn.example/g.json","sha256":"` + hexB + `"}}}}`
	l, err := ParseCargoLogin([]byte(good))
	if err != nil || l.Session == nil || len(l.PerGuild) != 1 {
		t.Fatalf("%+v %v", l, err)
	}
	for name, b := range map[string]string{
		"short hash":  `{"nevr_cargo":{"schema":1,"session":{"url":"https://cdn.example/m.json","sha256":"abc"}}}`,
		"http url":    `{"nevr_cargo":{"schema":1,"session":{"url":"http://cdn.example/m.json","sha256":"` + hexA + `"}}}`,
		"upper hash":  `{"nevr_cargo":{"schema":1,"session":{"url":"https://cdn.example/m.json","sha256":"` + strings.ToUpper(hexA) + `"}}}`,
		"bad install": `{"nevr_install":"x"}`,
	} {
		if _, err := ParseCargoLogin([]byte(b)); err == nil {
			t.Errorf("%s accepted", name)
		}
	}
	if l, err := ParseCargoLogin([]byte(`{}`)); err != nil || l.Session != nil {
		t.Errorf("a login with no nevr_cargo is the default case: %+v %v", l, err)
	}
}

func ptr(c CargoChoice) *CargoChoice { return &c }

package main

import (
	"strings"
	"testing"
)

func TestDiffSections(t *testing.T) {
	old := Corpus{
		0x00027229b6c95edd: "section01_first_speed_update",
		0x0000b27f29ed0f87: "mpl_cbt_gauss_s03_scaffolding_rail_b~instance_phys",
	}
	new := Corpus{
		0x00027229b6c95edd: "section01_first_speed_update_r2",
		0x0008c4da8d842133: "ambient.clothirradianceintensity",
	}
	diff := Diff(old, new)
	if len(diff.Added) != 1 || diff.Added[0].Name != "ambient.clothirradianceintensity" {
		t.Fatalf("added = %v", diff.Added)
	}
	if len(diff.Removed) != 1 || diff.Removed[0].Name != "mpl_cbt_gauss_s03_scaffolding_rail_b~instance_phys" {
		t.Fatalf("removed = %v", diff.Removed)
	}
	if len(diff.Changed) != 1 || diff.Changed[0].OldName != "section01_first_speed_update" || diff.Changed[0].NewName != "section01_first_speed_update_r2" {
		t.Fatalf("changed = %v", diff.Changed)
	}
	md := diff.Markdown()
	for _, want := range []string{"Added", "Removed", "Changed", "ambient.clothirradianceintensity"} {
		if !strings.Contains(md, want) {
			t.Fatalf("markdown lacks %q:\n%s", want, md)
		}
	}
	if len(Diff(new, new).Added)+len(Diff(new, new).Removed)+len(Diff(new, new).Changed) != 0 {
		t.Fatal("identical corpora produced a diff")
	}
}

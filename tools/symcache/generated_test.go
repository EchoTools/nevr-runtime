package main

import (
	"os"
	"testing"
)

// Resolves known hashes through the committed generated table, not the JSON.
func TestGeneratedTableResolvesKnownHashes(t *testing.T) {
	path := "../../src/runtime/log/symcache_data.cpp"
	f, err := os.Open(path)
	if err != nil {
		t.Fatalf("open %s: %v", path, err)
	}
	defer f.Close()
	table, err := ParseGenerated(f)
	if err != nil {
		t.Fatalf("ParseGenerated: %v", err)
	}
	known := map[uint64]string{
		0x0000b27f29ed0f87: "mpl_cbt_gauss_s03_scaffolding_rail_b~instance_phys",
		0x00027229b6c95edd: "section01_first_speed_update",
		0x0001aa01631e738f: "emote_selected_frame",
	}
	for hash, want := range known {
		got, found := table[hash]
		if !found {
			t.Errorf("resolve %#016x: hash is not in the generated table", hash)
			continue
		}
		if got != want {
			t.Errorf("resolve %#016x = %q, want %q", hash, got, want)
		}
	}
}

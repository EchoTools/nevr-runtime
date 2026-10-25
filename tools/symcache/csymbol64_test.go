package main

import "testing"

func TestCSymbol64Vectors(t *testing.T) {
	cases := []struct {
		name string
		want uint64
	}{
		{"mpl_cbt_gauss_s03_scaffolding_rail_b~instance_phys", 0x0000b27f29ed0f87},
		{"section01_first_speed_update", 0x00027229b6c95edd},
		{"ambient.clothirradianceintensity", 0x0008c4da8d842133},
		{"EXT_DEPLETED_DIG_0610_DRILL", 0x0002d0d68aeb0692},
		{"rwd_tint_0000", 0x74d228d09dc5dc86},
		{"serverdb", 0x25e886012ced8064},
		{"NEVRProtobufJSONMessageV1", 0xc6b3710cd9c4ef47},
		{"", 0xffffffffffffffff},
	}
	for _, c := range cases {
		if got := csymbol64(c.name); got != c.want {
			t.Errorf("csymbol64(%q) = %#016x, want %#016x", c.name, got, c.want)
		}
	}
}

func TestCSymbol64FoldsASCIIUpperToLower(t *testing.T) {
	if a, b := csymbol64("Ambient.ClothIrradianceIntensity"), csymbol64("ambient.clothirradianceintensity"); a != b {
		t.Fatalf("case folding differs: %#x != %#x", a, b)
	}
}

package csymbol

import "testing"

// The vectors are the ones symcache's own test pins (echo-symbols vectors.tsv), so the move changed no hash.
func TestHashVectors(t *testing.T) {
	cases := []struct {
		name string
		want uint64
	}{
		{"rwd_tint_0000", 0x74d228d09dc5dc86},
		{"serverdb", 0x25e886012ced8064},
		{"", 0xffffffffffffffff},
	}
	for _, c := range cases {
		if got := Hash(c.name); got != c.want {
			t.Errorf("Hash(%q) = %#016x, want %#016x", c.name, got, c.want)
		}
	}
}

// Append from a class's hash is the engine's on-disk type: CGTextureResource + "Win10" is the PC texture descriptor type
// (src/cdn/cdn_manifest.h kWindowsPairs), + "Android" the Quest one (kAndroidPairs).
func TestAppendIsHashOfTheJoinedName(t *testing.T) {
	const pcDescriptor, questDescriptor = 0x4a4c32c49300b8a0, 0xe2efe7289d5985b8
	base := Hash("CGTextureResource")
	if got := Append(base, "Win10"); got != pcDescriptor || got != Hash("CGTextureResourceWin10") {
		t.Errorf("Append(CGTextureResource, Win10) = %#016x, want %#016x", got, uint64(pcDescriptor))
	}
	if got := Append(base, "Android"); got != questDescriptor {
		t.Errorf("Append(CGTextureResource, Android) = %#016x, want %#016x", got, uint64(questDescriptor))
	}
}

package system

import (
	"bytes"
	"testing"
)

// These assertions use the real PE image when EVR_GAME_DIR is configured. The
// runtime checks the same conditional-jump opcode immediately before patching;
// a game update that moves or rewrites a gate therefore fails before a server
// can blind-write it.
func assertRVABytes(t *testing.T, image []byte, rva uint32, want []byte) {
	t.Helper()
	fileOffset, ok := rvaToFileOffset(image, rva)
	if !ok {
		t.Fatalf("RVA 0x%x cannot be mapped to a PE section", rva)
	}
	end := int(fileOffset) + len(want)
	if end > len(image) {
		t.Fatalf("RVA 0x%x maps outside image (file offset 0x%x)", rva, fileOffset)
	}
	if got := image[fileOffset:end]; !bytes.Equal(got, want) {
		t.Fatalf("RVA 0x%x: got % x, want % x", rva, got, want)
	}
}

func TestModePatchesHeadlessGatePrologues(t *testing.T) {
	exe := getGameBinary(t, "echovr.exe")
	gates := []struct {
		name string
		rva  uint32
		op   byte
	}{
		{"D3D12 device init", 0x154AF7F, 0x74},
		{"renderer init", 0x154B0E4, 0x74},
		{"GUI init", 0x154B38F, 0x74},
		{"render-submit init", 0x154D7E4, 0x74},
		{"render setup", 0x154B683, 0x75},
	}

	for _, gate := range gates {
		t.Run(gate.name, func(t *testing.T) {
			assertRVABytes(t, exe, gate.rva, []byte{gate.op})
		})
	}
}

func TestModePatchesSpectatorStreamCheckPrologue(t *testing.T) {
	exe := getGameBinary(t, "echovr.exe")
	assertRVABytes(t, exe, 0x116F3D, []byte{0x0F, 0x84, 0xDF, 0x00, 0x00, 0x00})
}

// Every ApplyPatch target in mode_patches.cpp is pinned to the unmodified game
// image.  These are deliberately original instructions, not NEVR replacement
// bytes: an update must force us to re-derive each patch before it can ship.
func TestModePatchesServerAndOfflinePrologues(t *testing.T) {
	exe := getGameBinary(t, "echovr.exe")
	targets := []struct {
		name string
		rva  uint32
		want []byte
	}{
		{"server flags", 0x1580C3, []byte{0x48, 0x8B, 0x08}},
		{"netserver logging", 0xFFA58, []byte{0x48, 0x0F, 0x45, 0xD8}},
		{"logging subject", 0xFFB0E, []byte{0x74, 0x0E}},
		{"allow incoming", 0xF7F904, []byte{0xE8, 0x87, 0xF0, 0x66, 0xFF}},
		{"offline multiplayer", 0xFDE0E, []byte{0xE8, 0x5D, 0xFC, 0xFF, 0xFF}},
		{"offline incidents", 0x17F0B1, []byte{0x74, 0x0A}},
		{"offline title", 0x17F77B, []byte{0x75, 0x12}},
		{"offline transaction capability", 0x17F817, []byte{0x74, 0x1E}},
		{"offline transaction service", 0x17F823, []byte{0x74, 0x12}},
		{"offline logon", 0x1AC83E, []byte{0x0F, 0x85, 0x91, 0x00, 0x00, 0x00}},
		{"offline tutorial", 0xA7C685, []byte{0xE8, 0x26, 0x18, 0x68, 0xFF}},
	}
	for _, target := range targets {
		t.Run(target.name, func(t *testing.T) { assertRVABytes(t, exe, target.rva, target.want) })
	}
}

func TestModePatchesOptionalSubsystemPrologues(t *testing.T) {
	exe := getGameBinary(t, "echovr.exe")
	targets := []struct {
		name string
		rva  uint32
		want []byte
	}{
		{"OVR branch", 0x1580E5, []byte{0x0F, 0x85, 0xC7, 0x00, 0x00, 0x00}},
		{"no OVR spectator", 0x11690D, []byte{0x75, 0x35}},
		{"deadlock monitor", 0x1D3881, []byte{0x7E, 0x0A}},
		{"loading tip pick", 0xBD9670, []byte{0x48, 0x89, 0x5C, 0x24, 0x10}},
		{"loading tip select", 0xBE6D10, []byte{0x48, 0x89, 0x54, 0x24, 0x10}},
		{"loading tip select two", 0xBE7C90, []byte{0x48, 0x89, 0x5C, 0x24, 0x10}},
		{"apply graphics", 0x109209, []byte{0xE8, 0x62, 0x86, 0xB2, 0x00}},
		{"direct input", 0x1055DBB, []byte{0xE8, 0xA6, 0xF8, 0x48, 0x00}},
		{"effects load", 0x62CA91, []byte{0x75, 0x41}},
		{"renderer", 0xFF581, []byte{0xA8, 0x01}},
		{"Wwise init", 0x209920, []byte{0x48, 0x89, 0x5C, 0x24, 0x08}},
		{"Wwise render", 0xFA5610, []byte{0x48, 0x83, 0xEC, 0x28}},
	}
	for _, target := range targets {
		t.Run(target.name, func(t *testing.T) { assertRVABytes(t, exe, target.rva, target.want) })
	}
}

func TestModePatchesTargetsDoNotOverlap(t *testing.T) {
	// The five one-byte gate rewrites and the six-byte spectator-stream rewrite
	// are independently installed in mode_patches.cpp.  Keep their ranges
	// disjoint so adding a new gate cannot accidentally mutate another patch.
	targets := []struct {
		name string
		rva  uint32
		size uint32
	}{
		{"D3D12 device init", 0x154AF7F, 1},
		{"renderer init", 0x154B0E4, 1},
		{"GUI init", 0x154B38F, 1},
		{"render-submit init", 0x154D7E4, 1},
		{"render setup", 0x154B683, 1},
		{"spectator stream", 0x116F3D, 6},
		{"server flags", 0x1580C3, 40},
		{"netserver logging", 0xFFA58, 4},
		{"logging subject", 0xFFB0E, 2},
		{"allow incoming", 0xF7F904, 5},
		{"offline multiplayer", 0xFDE0E, 5},
		{"offline incidents", 0x17F0B1, 2},
		{"offline title", 0x17F77B, 2},
		{"offline transaction capability", 0x17F817, 2},
		{"offline transaction service", 0x17F823, 2},
		{"offline logon", 0x1AC83E, 6},
		{"offline tutorial", 0xA7C685, 5},
		{"OVR branch", 0x1580E5, 6},
		{"no OVR spectator", 0x11690D, 2},
		{"deadlock monitor", 0x1D3881, 2},
		{"loading tip pick", 0xBD9670, 1},
		{"loading tip select", 0xBE6D10, 1},
		{"loading tip select two", 0xBE7C90, 1},
		{"apply graphics", 0x109209, 5},
		{"direct input", 0x1055DBB, 5},
		{"effects load", 0x62CA91, 2},
		{"renderer", 0xFF581, 2},
	}

	// SERVER_FLAGS_CHECK intentionally spans the OVR branch. Both write NOPs,
	// so applying either first is safe; keeping this exception named means every
	// other overlap remains a failure rather than becoming an accidental blind
	// spot in the ground-truth test.
	intentionalOverlap := func(first, second string) bool {
		return (first == "server flags" && second == "OVR branch") ||
			(first == "OVR branch" && second == "server flags")
	}

	for index, target := range targets {
		// echovr.exe .text begins at RVA 0x1000 and ends below 0x16c21c7 in
		// the supported image.  Several legitimate startup patches are below
		// 0x100000, so the earlier broad lower bound was not an image check.
		if target.rva < 0x1000 || target.rva >= 0x16c21c7 {
			t.Fatalf("%s target 0x%x is outside the game code range", target.name, target.rva)
		}
		for otherIndex := index + 1; otherIndex < len(targets); otherIndex++ {
			other := targets[otherIndex]
			if target.rva < other.rva+other.size && other.rva < target.rva+target.size &&
				!intentionalOverlap(target.name, other.name) {
				t.Fatalf("%s overlaps %s", target.name, other.name)
			}
		}
	}
}

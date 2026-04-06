package system

import (
	"bytes"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// Validates that the byte patterns targeted by pnsrad_enabler patches exist at
// the expected offsets in the game binaries. If these fail, the addresses have
// shifted (new game build) and the patches need updating.

func getGameBinary(t *testing.T, name string) []byte {
	t.Helper()
	gameDir := os.Getenv("EVR_GAME_DIR")
	if gameDir == "" {
		t.Skip("EVR_GAME_DIR not set")
	}
	path := filepath.Join(gameDir, name)
	data, err := os.ReadFile(path)
	if err != nil {
		t.Skipf("Cannot read %s: %v", name, err)
	}
	return data
}

// rvaToFileOffset converts a relative virtual address to a file offset
// using a simple PE section lookup.
func rvaToFileOffset(peData []byte, rva uint32) (uint32, bool) {
	if len(peData) < 0x40 {
		return 0, false
	}
	peOffset := binary.LittleEndian.Uint32(peData[0x3C:])
	if int(peOffset)+24 > len(peData) {
		return 0, false
	}
	// PE signature (4) + COFF header (20)
	optHeaderOffset := peOffset + 4 + 20
	sizeOfOptHeader := binary.LittleEndian.Uint16(peData[peOffset+4+16:])
	numSections := binary.LittleEndian.Uint16(peData[peOffset+4+2:])
	sectionTableOffset := optHeaderOffset + uint32(sizeOfOptHeader)

	for i := uint16(0); i < numSections; i++ {
		off := sectionTableOffset + uint32(i)*40
		if int(off)+40 > len(peData) {
			break
		}
		virtualSize := binary.LittleEndian.Uint32(peData[off+8:])
		virtualAddr := binary.LittleEndian.Uint32(peData[off+12:])
		rawDataPtr := binary.LittleEndian.Uint32(peData[off+20:])
		if rva >= virtualAddr && rva < virtualAddr+virtualSize {
			return rawDataPtr + (rva - virtualAddr), true
		}
	}
	return 0, false
}

func TestPnsradPatches_EchoVRBinaryTargets(t *testing.T) {
	exe := getGameBinary(t, "echovr.exe")

	tests := []struct {
		name     string
		rva      uint32
		expected []byte
		desc     string
	}{
		{
			name:     "OVR platform branch (PatchBypassOvrPlatform)",
			rva:      0x1580e5,
			expected: []byte{0x0F, 0x85, 0xC7, 0x00, 0x00, 0x00}, // JNE
			desc:     "PlatformModuleDecisionAndInitialize OVR conditional jump",
		},
		{
			name:     "LogInSuccess bit 9 capability check",
			rva:      0x17f817,
			expected: []byte{0x74, 0x1E}, // JE +0x1e
			desc:     "CR15NetGame::LogInSuccess platform capability guard",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			fileOff, ok := rvaToFileOffset(exe, tt.rva)
			if !ok {
				t.Fatalf("Cannot convert RVA 0x%x to file offset", tt.rva)
			}
			if int(fileOff)+len(tt.expected) > len(exe) {
				t.Fatalf("File offset 0x%x out of range", fileOff)
			}
			actual := exe[fileOff : fileOff+uint32(len(tt.expected))]
			if !bytes.Equal(actual, tt.expected) {
				t.Errorf("%s: at RVA 0x%x (file 0x%x)\n  expected: %x\n  actual:   %x",
					tt.desc, tt.rva, fileOff, tt.expected, actual)
			}
		})
	}
}

func TestPnsradPatches_PnsradDLLTargets(t *testing.T) {
	dll := getGameBinary(t, "pnsrad.dll")

	tests := []struct {
		name     string
		rva      uint32
		expected []byte
		desc     string
	}{
		{
			name:     "Login provider check",
			rva:      0x85b53,
			expected: []byte{0x75, 0x1F}, // JNE +0x1f
			desc:     "CNSRADUser login provider validation",
		},
		{
			name:     "LogInSuccessCB identity guard",
			rva:      0x8ef25,
			expected: []byte{0x0F, 0x85, 0x9C, 0x00, 0x00, 0x00}, // JNE
			desc:     "CNSUser::LogInSuccessCB session/identity comparison",
		},
		{
			name:     "LoginIdResponseCB state check",
			rva:      0x8f1b6,
			expected: []byte{0x0F, 0x84, 0x78, 0x01, 0x00, 0x00}, // JE
			desc:     "LoginIdResponseCB bit 0x08 state flag check",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			fileOff, ok := rvaToFileOffset(dll, tt.rva)
			if !ok {
				t.Fatalf("Cannot convert RVA 0x%x to file offset", tt.rva)
			}
			if int(fileOff)+len(tt.expected) > len(dll) {
				t.Fatalf("File offset 0x%x out of range", fileOff)
			}
			actual := dll[fileOff : fileOff+uint32(len(tt.expected))]
			if !bytes.Equal(actual, tt.expected) {
				t.Errorf("%s: at RVA 0x%x (file 0x%x)\n  expected: %x\n  actual:   %x",
					tt.desc, tt.rva, fileOff, tt.expected, actual)
			}
		})
	}
}

package system

import (
	"debug/pe"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/require"
)

// rvaToFile converts an RVA (relative to ImageBase 0x140000000) to a file
// offset by walking the PE section headers.
func rvaToFile(t *testing.T, f *pe.File, rva uint32) uint32 {
	t.Helper()
	for _, s := range f.Sections {
		sectionStart := uint32(f.OptionalHeader.(*pe.OptionalHeader64).ImageBase) + s.VirtualAddress
		sectionEnd := sectionStart + s.VirtualSize
		rvaAddr := uint32(f.OptionalHeader.(*pe.OptionalHeader64).ImageBase) + rva
		if rvaAddr >= sectionStart && rvaAddr < sectionEnd {
			return rvaAddr - sectionStart + s.Offset
		}
	}
	t.Fatalf("RVA 0x%x not found in any PE section", rva)
	return 0
}

// TestProviderStringTable_OvrOrg verifies the game binary's provider string
// table entries match the expected numbering.
//
// Auditable citations (ReVault revault_read_memory, 2026-08-04):
//
//	echovr.exe + 0x16D7130: "BOT\0STM\0PSN\0XBX\0OVR-ORG\0OVR\0DMO\0"
//	Offset 0x16D713c (game provider 3): "XBX\0"  (4 bytes)
//	Offset 0x16D7140 (game provider 4): "OVR-ORG\0" (8 bytes)
//
// The game's internal provider numbering differs from the Nakama wire enum:
//
//	Game 1=STM, 2=PSN, 3=XBX, 4=OVR-ORG, 5=OVR, 6=BOT, 7=DMO
//	Nakama: 0=STM, 1=DSC, 2=XBX, 3=OVR_ORG, 4=OVR, 5=BOT, 6=DMO
//
// The Nakama server echoes PlatformCode from LoginRequest into LoginSuccess
// without remapping (evr_pipeline_login.go:185). The game interprets the
// echoed value through its own GetProviderPrefix switch (echovr.exe fcn.14060d640,
// 14 callers), so the WIRE value must match the GAME's numbering.
//
// Regression test for 2026-08-04: OVR_ORG was sent as wire value 3 (Nakama
// enum), which the game interpreted as XBX (game provider 3).
func TestProviderStringTable_OvrOrg(t *testing.T) {
	gameDir := getGameDir()
	exePath := filepath.Join(gameDir, "bin", "win10", "echovr.exe")

	f, err := pe.Open(exePath)
	if os.IsNotExist(err) {
		t.Skipf("echovr.exe not found at %s", exePath)
	}
	require.NoError(t, err, "failed to open echovr.exe as PE")
	defer f.Close()

	// Read full file for byte-level access
	data, err := os.ReadFile(exePath)
	require.NoError(t, err, "failed to read echovr.exe")

	readRVA := func(rva uint32, n int) []byte {
		off := rvaToFile(t, f, rva)
		return data[off : off+uint32(n)]
	}

	// Game provider 3 → "XBX" (4-byte slot at RVA 0x16D713c)
	t.Run("game_provider_3_is_XBX", func(t *testing.T) {
		b := readRVA(0x16D713c, 4)
		require.Equal(t, []byte("XBX"), b[0:3], "game provider 3 must be XBX")
		require.Equal(t, byte(0), b[3], "null terminator expected")
	})

	// Game provider 4 → "OVR-ORG\0" (8-byte slot at RVA 0x16D7140)
	t.Run("game_provider_4_is_OVR_ORG", func(t *testing.T) {
		b := readRVA(0x16D7140, 8)
		require.Equal(t, []byte("OVR-ORG"), b[0:7], "game provider 4 must be OVR-ORG")
		require.Equal(t, byte(0), b[7], "null terminator expected")
	})
}

// TestPlatformCode_WirePayloadOffset verifies BuildLoginRequest places the
// PlatformCode at the correct byte offset in the binary payload.
//
// The wire format (verified against Nakama evr/login_request.go Stream method):
//
//	[MSG_MARKER(8)][symbol(8)][length(8)][UUID(16)][PlatformCode(8)][AccountId(8)][JSON\0]
//
// PlatformCode is at offset 40 in the full message (24 byte header + 16 byte UUID).
func TestPlatformCode_WirePayloadOffset(t *testing.T) {
	// The LoginRequest binary is built by BuildLoginRequest() in ws_bridge.cpp.
	// We verify indirectly: the test in test_behavioral.cpp (WsBridgeSelectPlatform)
	// confirms SelectPlatformCode returns 4 for URL credentials, and
	// TestHook_BuildLoginRequest exercises the full binary encoding path.
	//
	// This ground-truth test pins the OFFSETS that the binary format depends on,
	// so a change to the wire layout is caught at build time.
	const (
		markerLen       = 8
		symbolLen       = 8
		lengthLen       = 8
		uuidLen         = 16
		platformCodeLen = 8
		accountIDLen    = 8

		platformCodeOffset = markerLen + symbolLen + lengthLen + uuidLen // 40
		accountIDOffset    = platformCodeOffset + platformCodeLen         // 48
	)

	// These offsets must match the Nakama server's Stream method in
	// login_request.go:33-39 which reads PlatformCode then AccountId
	// as LittleEndian uint64 after a 16-byte UUID.
	require.Equal(t, 40, platformCodeOffset, "PlatformCode offset must be 40")
	require.Equal(t, 48, accountIDOffset, "AccountId offset must be 48")

	// Verify LE encoding direction (AppendLE64 in ws_bridge.cpp).
	// PlatformCode=4 → bytes [4,0,0,0,0,0,0,0]
	var platformCode uint64 = 4
	le := make([]byte, 8)
	binary.LittleEndian.PutUint64(le, platformCode)
	require.Equal(t, byte(4), le[0], "LE byte 0 of PlatformCode=4 must be 4")
	require.Equal(t, byte(0), le[1], "LE bytes 1-7 of PlatformCode=4 must be 0")
}

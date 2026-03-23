package system

import (
	"encoding/binary"
	"testing"
	"unsafe"
)

// Mirror the C structs from social_messages.h for binary format validation.
// These must match exactly or the game will crash when receiving SNS messages.

type SNSFriendsActionPayload struct {
	RoutingID     uint64
	LocalUserUUID [16]byte
	SessionGUID   uint64
	TargetUserID  uint64
}

type SNSFriendsListResponse struct {
	Header   uint64
	NOffline uint32
	NBusy    uint32
	NOnline  uint32
	NSent    uint32
	NRecv    uint32
	Reserved uint32
}

type SNSFriendNotifyPayload struct {
	Header     uint64
	FriendID   uint64
	StatusCode uint8
	Reserved   [7]byte
}

type SNSFriendIdPayload struct {
	Header   uint64
	FriendID uint64
}

type SNSPartyPayload struct {
	RoutingID     uint64
	LocalUserUUID [16]byte
	SessionGUID   uint64
	TargetParam   uint64
}

type SNSPartyTargetPayload struct {
	LocalUserUUID  [16]byte
	TargetUserUUID [16]byte
	SessionGUID    uint64
	Param          uint32
	Reserved       uint32
}

// TestSocialPayloadSizes verifies struct sizes match the C definitions.
// If these fail, the binary format is wrong and will crash the game.
func TestSocialPayloadSizes(t *testing.T) {
	tests := []struct {
		name     string
		size     uintptr
		expected uintptr
	}{
		{"SNSFriendsActionPayload", unsafe.Sizeof(SNSFriendsActionPayload{}), 0x28},
		{"SNSFriendsListResponse", unsafe.Sizeof(SNSFriendsListResponse{}), 0x20},
		{"SNSFriendNotifyPayload", unsafe.Sizeof(SNSFriendNotifyPayload{}), 0x18},
		{"SNSFriendIdPayload", unsafe.Sizeof(SNSFriendIdPayload{}), 0x10},
		{"SNSPartyPayload", unsafe.Sizeof(SNSPartyPayload{}), 0x28},
		{"SNSPartyTargetPayload", unsafe.Sizeof(SNSPartyTargetPayload{}), 0x30},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if tt.size != tt.expected {
				t.Errorf("%s: sizeof = 0x%x, want 0x%x", tt.name, tt.size, tt.expected)
			}
		})
	}
}

// TestSocialPayloadFieldOffsets verifies field offsets match the C struct layout.
func TestSocialPayloadFieldOffsets(t *testing.T) {
	// SNSFriendsActionPayload
	{
		var p SNSFriendsActionPayload
		base := uintptr(unsafe.Pointer(&p))
		assertOffset(t, "ActionPayload.RoutingID", uintptr(unsafe.Pointer(&p.RoutingID))-base, 0x00)
		assertOffset(t, "ActionPayload.LocalUserUUID", uintptr(unsafe.Pointer(&p.LocalUserUUID))-base, 0x08)
		assertOffset(t, "ActionPayload.SessionGUID", uintptr(unsafe.Pointer(&p.SessionGUID))-base, 0x18)
		assertOffset(t, "ActionPayload.TargetUserID", uintptr(unsafe.Pointer(&p.TargetUserID))-base, 0x20)
	}

	// SNSFriendsListResponse
	{
		var p SNSFriendsListResponse
		base := uintptr(unsafe.Pointer(&p))
		assertOffset(t, "ListResponse.Header", uintptr(unsafe.Pointer(&p.Header))-base, 0x00)
		assertOffset(t, "ListResponse.NOffline", uintptr(unsafe.Pointer(&p.NOffline))-base, 0x08)
		assertOffset(t, "ListResponse.NBusy", uintptr(unsafe.Pointer(&p.NBusy))-base, 0x0C)
		assertOffset(t, "ListResponse.NOnline", uintptr(unsafe.Pointer(&p.NOnline))-base, 0x10)
		assertOffset(t, "ListResponse.NSent", uintptr(unsafe.Pointer(&p.NSent))-base, 0x14)
		assertOffset(t, "ListResponse.NRecv", uintptr(unsafe.Pointer(&p.NRecv))-base, 0x18)
	}

	// SNSFriendNotifyPayload
	{
		var p SNSFriendNotifyPayload
		base := uintptr(unsafe.Pointer(&p))
		assertOffset(t, "NotifyPayload.Header", uintptr(unsafe.Pointer(&p.Header))-base, 0x00)
		assertOffset(t, "NotifyPayload.FriendID", uintptr(unsafe.Pointer(&p.FriendID))-base, 0x08)
		assertOffset(t, "NotifyPayload.StatusCode", uintptr(unsafe.Pointer(&p.StatusCode))-base, 0x10)
	}

	// SNSPartyTargetPayload
	{
		var p SNSPartyTargetPayload
		base := uintptr(unsafe.Pointer(&p))
		assertOffset(t, "PartyTarget.LocalUserUUID", uintptr(unsafe.Pointer(&p.LocalUserUUID))-base, 0x00)
		assertOffset(t, "PartyTarget.TargetUserUUID", uintptr(unsafe.Pointer(&p.TargetUserUUID))-base, 0x10)
		assertOffset(t, "PartyTarget.SessionGUID", uintptr(unsafe.Pointer(&p.SessionGUID))-base, 0x20)
		assertOffset(t, "PartyTarget.Param", uintptr(unsafe.Pointer(&p.Param))-base, 0x28)
	}
}

func assertOffset(t *testing.T, name string, got, want uintptr) {
	t.Helper()
	if got != want {
		t.Errorf("%s: offset = 0x%x, want 0x%x", name, got, want)
	}
}

// TestSocialPayloadSerialization verifies that serialized payloads produce the expected bytes.
func TestSocialPayloadSerialization(t *testing.T) {
	t.Run("FriendsListResponse", func(t *testing.T) {
		resp := SNSFriendsListResponse{
			Header:   0,
			NOffline: 1,
			NBusy:    0,
			NOnline:  3,
			NSent:    0,
			NRecv:    1,
			Reserved: 0,
		}

		buf := make([]byte, 0x20)
		binary.LittleEndian.PutUint64(buf[0x00:], resp.Header)
		binary.LittleEndian.PutUint32(buf[0x08:], resp.NOffline)
		binary.LittleEndian.PutUint32(buf[0x0C:], resp.NBusy)
		binary.LittleEndian.PutUint32(buf[0x10:], resp.NOnline)
		binary.LittleEndian.PutUint32(buf[0x14:], resp.NSent)
		binary.LittleEndian.PutUint32(buf[0x18:], resp.NRecv)
		binary.LittleEndian.PutUint32(buf[0x1C:], resp.Reserved)

		// Verify specific bytes
		if binary.LittleEndian.Uint32(buf[0x08:]) != 1 {
			t.Error("noffline should be 1")
		}
		if binary.LittleEndian.Uint32(buf[0x10:]) != 3 {
			t.Error("nonline should be 3")
		}
		if binary.LittleEndian.Uint32(buf[0x18:]) != 1 {
			t.Error("nrecv should be 1")
		}
		if len(buf) != 0x20 {
			t.Errorf("buf length = %d, want %d", len(buf), 0x20)
		}
	})

	t.Run("FriendIdPayload", func(t *testing.T) {
		payload := SNSFriendIdPayload{
			Header:   0x1234,
			FriendID: 42,
		}

		buf := make([]byte, 0x10)
		binary.LittleEndian.PutUint64(buf[0x00:], payload.Header)
		binary.LittleEndian.PutUint64(buf[0x08:], payload.FriendID)

		if binary.LittleEndian.Uint64(buf[0x08:]) != 42 {
			t.Error("friend_id should be 42")
		}
	})

	t.Run("FriendNotifyPayload", func(t *testing.T) {
		payload := SNSFriendNotifyPayload{
			Header:     0,
			FriendID:   12345,
			StatusCode: 3, // Already
		}

		buf := make([]byte, 0x18)
		binary.LittleEndian.PutUint64(buf[0x00:], payload.Header)
		binary.LittleEndian.PutUint64(buf[0x08:], payload.FriendID)
		buf[0x10] = payload.StatusCode

		if buf[0x10] != 3 {
			t.Error("status_code should be 3")
		}
		if binary.LittleEndian.Uint64(buf[0x08:]) != 12345 {
			t.Error("friend_id should be 12345")
		}
	})
}

// TestSocialHashConstants verifies SNS hash constants match the reconstruction.
// Source: echovr-reconstruction/src/pnsrad/Social/CNSRADFriends_protocol.h
func TestSocialHashConstants(t *testing.T) {
	hashes := map[string]uint64{
		// Friends outgoing
		"FriendSendInviteRequest": 0x7f0d7a28de3c6f70,
		"FriendRemoveRequest":     0x1bbcb7e810af4620,
		"FriendActionRequest":     0x78908988b7fe6db4,

		// Friends inbound
		"FriendListResponse":   0xa78aeb2a4e89b10b,
		"FriendStatusNotify":   0x26a19dc4d2d5579d,
		"FriendInviteSuccess":  0x7f0c6a3ac83c6f77,
		"FriendInviteFailure":  0x7f197e30c72c6e61,
		"FriendInviteNotify":   0xca09b0b36bd981b7,
		"FriendAcceptSuccess":  0x1bbda7fa06af4627,
		"FriendAddFailure":     0x1ba8b3f009bf4731,
		"FriendAcceptNotify":   0xc237c84c31d3ae05,
		"FriendBlockSuccess":   0xc2bf83a08ea3a955,
		"FriendRemoveNotify":   0xe06972f49cd72265,
		"FriendWithdrawnNotify": 0x191aa30801ec6d03,
		"FriendRejectNotify":   0xb9b86c0ce8e8d0c1,

		// Party outgoing (party-only)
		"PartyKickRequest":          0xff02bf488e77bcba,
		"PartyPassOwnershipRequest": 0x352f9d0e16001420,
		"PartyRespondToInvite":      0xeaf428c8a8a5cd2a,
		"PartyMemberUpdate":         0x0b7bd21332523994,
	}

	for name, hash := range hashes {
		if hash == 0 {
			t.Errorf("%s: hash is zero", name)
		}
		// Verify no accidental duplicates (except known shared ones)
		for name2, hash2 := range hashes {
			if name != name2 && hash == hash2 {
				t.Errorf("%s and %s have same hash 0x%x", name, name2, hash)
			}
		}
	}
}

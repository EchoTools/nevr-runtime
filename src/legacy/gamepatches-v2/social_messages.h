#pragma once

// Social SNS message payload structures for pnsrad ↔ Nakama bridge.
// Copied from echovr-reconstruction/src/pnsrad/Social/CNSRADFriends_protocol.h
// All structs match the exact binary layout pnsrad expects.

#include <cstdint>
#include <cstddef>

#pragma pack(push, 1)

// Outgoing payload (pnsrad → server): used by add_friend, remove_friend, accept, reject, block
struct SNSFriendsActionPayload {
    uint64_t routing_id;          // +0x00: g_provider_routing_id or 0xFFFFFFFFFFFFFFFF
    uint8_t  local_user_uuid[16]; // +0x08: local user UUID
    uint64_t session_guid;        // +0x18: session GUID
    uint64_t target_user_id;      // +0x20: target user's account ID (0 for by-name)
};
static_assert(sizeof(SNSFriendsActionPayload) == 0x28);

// Inbound: ListResponse — friend counts per category
struct SNSFriendsListResponse {
    uint64_t header;      // +0x00: SNS correlation/sequence
    uint32_t noffline;    // +0x08: offline friend count
    uint32_t nbusy;       // +0x0C: busy friend count
    uint32_t nonline;     // +0x10: online friend count
    uint32_t nsent;       // +0x14: sent request count
    uint32_t nrecv;       // +0x18: received request count
    uint32_t reserved;    // +0x1C: padding
};
static_assert(sizeof(SNSFriendsListResponse) == 0x20);

// Inbound: StatusNotify, AcceptSuccess, AddFailure, AcceptNotify, InviteFailure
struct SNSFriendNotifyPayload {
    uint64_t header;      // +0x00: SNS correlation/sequence
    uint64_t friend_id;   // +0x08: friend's user account ID
    uint8_t  status_code; // +0x10: error/status code
    uint8_t  reserved[7]; // +0x11: padding
};
static_assert(sizeof(SNSFriendNotifyPayload) == 0x18);

// Inbound: InviteSuccess, InviteNotify, BlockSuccess, RemoveNotify, WithdrawnNotify, RejectNotify
struct SNSFriendIdPayload {
    uint64_t header;      // +0x00: SNS correlation/sequence
    uint64_t friend_id;   // +0x08: friend's user account ID
};
static_assert(sizeof(SNSFriendIdPayload) == 0x10);

// Party outgoing payload (0x28 bytes)
struct SNSPartyPayload {
    uint64_t routing_id;          // +0x00
    uint8_t  local_user_uuid[16]; // +0x08
    uint64_t session_guid;        // +0x18
    uint64_t target_param;        // +0x20
};
static_assert(sizeof(SNSPartyPayload) == 0x28);

// Party outgoing payload with target user (0x30 bytes) — Kick, PassOwnership, RespondToInvite
struct SNSPartyTargetPayload {
    uint8_t  local_user_uuid[16]; // +0x00
    uint8_t  target_user_uuid[16]; // +0x10
    uint64_t session_guid;        // +0x20
    uint32_t param;               // +0x28: action param (kick reason, accept/reject)
    uint32_t reserved;            // +0x2C
};
static_assert(sizeof(SNSPartyTargetPayload) == 0x30);

#pragma pack(pop)

// InviteFailure error codes
enum class EFriendInviteError : uint8_t {
    BadRequest = 0,
    NotFound   = 1,
    Self       = 2,
    Already    = 3,
    Pending    = 4,
    Full       = 5,
};

// AddFailure error codes
enum class EFriendAddError : uint8_t {
    NotFound  = 0,
    Already   = 1,
    Withdrawn = 2,
    Full      = 3,
};

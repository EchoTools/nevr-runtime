#pragma once

/* @module: pnsrad.dll */
/* @purpose: CNSRADFriends SNS protocol — hashes, payload structs, callback map
 *
 * Extracted from:
 *   - register_callbacks @0x180082800 (12 inbound callback registrations)
 *   - 6 outgoing sender functions (add_friend_by_id, add_friend_by_name,
 *     remove_friend, accept_friend, reject_friend_request, block_user)
 *
 * Dispatch mechanism:
 *   sns_register_callback @0x18008a7f0 registers (hash, handler, deserializer,
 *   payload_size) into a hash table on the CTcpBroadcaster instance
 *   (this+0x168). When a server SNS message arrives with hash X, the
 *   broadcaster looks up the handler and calls deserializer(handler, payload).
 *   sns_make_closure @0x1800896d0 constructs the callback closure.
 *
 *   Outgoing messages use sns_send @0x18008b310, which dispatches through
 *   the broadcaster's vtable+0xa8 send method.
 *
 * Payload deserializer thunks (confirmed from decompilation):
 *   sns_deserialize_0x10 @0x18007ede0 — 0x10-byte payload (checks size > 0x10)
 *   sns_deserialize_0x18 @0x18007eda0 — 0x18-byte payload (checks size > 0x18)
 *   sns_deserialize_0x20 @0x18007ed60 — 0x20-byte payload (checks size > 0x20)
 *   sns_deserialize_0x28 @0x18007ed20 — 0x28-byte payload (checks size > 0x28)
 */

#include <cstdint>
#include <cstddef>

namespace NRadEngine {

// ============================================================================
// Outgoing SNS Hashes — sent by CNSRADFriends methods via sns_send
// ============================================================================

namespace SNSFriendsHashes {

// @0x180083cd0 (add_friend_by_id) and @0x180083b10 (add_friend_by_name)
// Same hash for both — add_friend_by_name appends the name string after the payload.
// Payload: SNSFriendsActionPayload (0x28 bytes) + optional name string
constexpr uint64_t SendInviteRequest   = 0x7f0d7a28de3c6f70ULL;

// @0x18007fe50 (remove_friend)
// Payload: SNSFriendsActionPayload (0x28 bytes), routing_id = 0xFFFFFFFFFFFFFFFF
// Note: same VA also used as CNSRADParty::JoinInternal — shared implementation,
// different vtable dispatch. Context (this pointer) determines semantics.
constexpr uint64_t RemoveFriendRequest = 0x1bbcb7e810af4620ULL;

// @0x180086800 (accept_friend), @0x180080610 (reject_friend_request), @0x180086380 (block_user)
// All three use the same hash — server differentiates by prior state.
// accept_friend: moves from pending to friends list, adjusts priority bucket at +0x170
// reject_friend_request: removes from pending requests array at +0x260/+0x290
// block_user: removes from friends array at +0x340/+0x370
// Payload: SNSFriendsActionPayload (0x28 bytes)
constexpr uint64_t FriendActionRequest = 0x78908988b7fe6db4ULL;

} // namespace SNSFriendsHashes

// ============================================================================
// Inbound SNS Hashes — registered in register_callbacks @0x180082800
// ============================================================================

namespace SNSFriendsInboundHashes {

// Server to client response/notification hashes.
// Format: hash registered with (handler_va, deserializer, payload_size, alignment)

// Friends list response — full list with counts per category
// Handler: ListResponseCB @0x180085400, deserializer: 0x20-byte
constexpr uint64_t ListResponse        = 0xa78aeb2a4e89b10bULL;

// Friend status change notification
// Handler: StatusNotifyCB @0x1800877c0, deserializer: 0x18-byte
constexpr uint64_t StatusNotify        = 0x26a19dc4d2d5579dULL;

// Friend invite sent successfully
// Handler: InviteSuccessCB @0x1800845b0, deserializer: 0x10-byte
constexpr uint64_t InviteSuccess       = 0x7f0c6a3ac83c6f77ULL;

// Friend invite failed (with error code)
// Handler: InviteFailureCB @0x180083e10, deserializer: 0x18-byte
constexpr uint64_t InviteFailure       = 0x7f197e30c72c6e61ULL;

// Incoming friend invite notification
// Handler: InviteNotifyCB @0x180084200, deserializer: 0x10-byte
constexpr uint64_t InviteNotify        = 0xca09b0b36bd981b7ULL;

// Friend accept succeeded
// Handler: AcceptSuccessCB @0x180080380, deserializer: 0x18-byte
constexpr uint64_t AcceptSuccess       = 0x1bbda7fa06af4627ULL;

// Friend add failed (with error code)
// Handler: AddFailureCB @0x18007ffb0, deserializer: 0x18-byte
constexpr uint64_t AddFailure          = 0x1ba8b3f009bf4731ULL;

// Friend accept notification (other user accepted your request)
// Handler: AcceptNotifyCB @0x180080220, deserializer: 0x18-byte
constexpr uint64_t AcceptNotify        = 0xc237c84c31d3ae05ULL;

// Block user succeeded
// Handler: BlockSuccessCB @0x180086bd0, deserializer: 0x10-byte
constexpr uint64_t BlockSuccess        = 0xc2bf83a08ea3a955ULL;

// Friend removed notification
// Handler: RemoveNotifyCB @0x180086a70, deserializer: 0x10-byte
constexpr uint64_t RemoveNotify        = 0xe06972f49cd72265ULL;

// Friend request withdrawn notification
// Handler: WithdrawnNotifyCB @0x180088910, deserializer: 0x10-byte
constexpr uint64_t WithdrawnNotify     = 0x191aa30801ec6d03ULL;

// Friend request rejected notification
// Handler: RejectNotifyCB @0x180086660, deserializer: 0x10-byte
constexpr uint64_t RejectNotify        = 0xb9b86c0ce8e8d0c1ULL;

} // namespace SNSFriendsInboundHashes

// ============================================================================
// Outgoing Payload Structs
// ============================================================================

// Common 0x28-byte outgoing payload — used by add_friend_by_id, remove_friend,
// accept_friend, reject_friend_request, block_user.
// Confirmed from 5 sender functions: all build this struct on-stack before
// calling sns_send with the corresponding hash.
//
// @0x180083cd0 (add_friend_by_id):
//   +0x00 = DAT_180376590 (g_provider_routing_id)
//   +0x08 = UUID from GetLocalUserUUID @0x18008efe0 (16 bytes)
//   +0x18 = session_guid from vtable+0x68 (GetSessionGUID)
//   +0x20 = target_user_id (param_2)
//
// @0x18007fe50 (remove_friend):
//   +0x00 = 0xFFFFFFFFFFFFFFFF (sentinel — no routing)
//   +0x08..+0x17 = UUID, +0x18 = session_guid, +0x20 = target_user_id
//
// @0x180083b10 (add_friend_by_name):
//   Same 0x28 payload, but target_user_id = 0. Name string appended after
//   payload as (ptr, strlen+1) to sns_send.
/* @size: 0x28 */ /* @confidence: H */
#pragma pack(push, 1)
struct SNSFriendsActionPayload {
    uint64_t routing_id;          // +0x00: DAT_180376590 or 0xFFFFFFFFFFFFFFFF
    uint8_t  local_user_uuid[16]; // +0x08: from GetLocalUserUUID @0x18008efe0
    uint64_t session_guid;        // +0x18: from vtable+0x68 (GetSessionGUID)
    uint64_t target_user_id;      // +0x20: target user's account ID (or 0 for by-name)
};
#pragma pack(pop)
static_assert(sizeof(SNSFriendsActionPayload) == 0x28);
static_assert(offsetof(SNSFriendsActionPayload, routing_id) == 0x00);
static_assert(offsetof(SNSFriendsActionPayload, local_user_uuid) == 0x08);
static_assert(offsetof(SNSFriendsActionPayload, session_guid) == 0x18);
static_assert(offsetof(SNSFriendsActionPayload, target_user_id) == 0x20);

// ============================================================================
// Inbound Payload Structs
// ============================================================================

// --- 0x20-byte inbound payload: ListResponse ---
// Accessed by ListResponseCB @0x180085400 via *(param_2+8) -> lVar1
//   lVar1+0x08: uint32_t noffline
//   lVar1+0x0c: uint32_t nbusy
//   lVar1+0x10: uint32_t nonline
//   lVar1+0x14: uint32_t nsent
//   lVar1+0x18: uint32_t nrecv
// Log string: "[FRIENDS] ListResponseCB: nonline=%u, nbusy=%u, noffline=%u, nsent=%u, nrecv=%u"
/* @size: 0x20 */ /* @confidence: H */
#pragma pack(push, 1)
struct SNSFriendsListResponse {
    uint64_t header;      // +0x00: SNS correlation/sequence
    uint32_t noffline;    // +0x08: offline friend count
    uint32_t nbusy;       // +0x0C: busy friend count
    uint32_t nonline;     // +0x10: online friend count
    uint32_t nsent;       // +0x14: sent request count
    uint32_t nrecv;       // +0x18: received request count
    uint32_t reserved;    // +0x1C: padding to 0x20
};
#pragma pack(pop)
static_assert(sizeof(SNSFriendsListResponse) == 0x20);
static_assert(offsetof(SNSFriendsListResponse, header) == 0x00);
static_assert(offsetof(SNSFriendsListResponse, noffline) == 0x08);
static_assert(offsetof(SNSFriendsListResponse, nbusy) == 0x0C);
static_assert(offsetof(SNSFriendsListResponse, nonline) == 0x10);
static_assert(offsetof(SNSFriendsListResponse, nsent) == 0x14);
static_assert(offsetof(SNSFriendsListResponse, nrecv) == 0x18);
static_assert(offsetof(SNSFriendsListResponse, reserved) == 0x1C);

// --- 0x18-byte inbound payload: StatusNotify, AcceptSuccess, AddFailure, AcceptNotify ---
// Common layout for friend-related notifications with a user ID.
// Accessed via *(param_2+8) -> payload_ptr
//   payload_ptr+0x08: uint64_t friend_id
//   payload_ptr+0x10: uint8_t error_code (for failure CBs) or status (for StatusNotify)
// For success/notify CBs, param_2+0x10 also carries friend_name (char*).
//
// StatusNotifyCB @0x1800877c0: logs "friendid=%llu" from +0x08
// AcceptSuccessCB @0x180080380: logs "friendid=%llu, friendname=%s" from +0x08, param_2+0x10
// AddFailureCB @0x18007ffb0: logs "friendid=%llu, friendname=%s, error=%s" from +0x08
// AcceptNotifyCB @0x180080220: logs "friendid=%llu" from +0x08
/* @size: 0x18 */ /* @confidence: H */
#pragma pack(push, 1)
struct SNSFriendNotifyPayload {
    uint64_t header;      // +0x00: SNS correlation/sequence
    uint64_t friend_id;   // +0x08: friend's user account ID
    uint8_t  status_code; // +0x10: error/status code (InviteFailureCB, AddFailureCB)
    uint8_t  reserved[7]; // +0x11: padding to 0x18
};
#pragma pack(pop)
static_assert(sizeof(SNSFriendNotifyPayload) == 0x18);
static_assert(offsetof(SNSFriendNotifyPayload, header) == 0x00);
static_assert(offsetof(SNSFriendNotifyPayload, friend_id) == 0x08);
static_assert(offsetof(SNSFriendNotifyPayload, status_code) == 0x10);
static_assert(offsetof(SNSFriendNotifyPayload, reserved) == 0x11);

// InviteFailureCB error codes (from switch at @0x180083e10):
//   0 = "bad request"
//   1 = "not found"
//   2 = "self"
//   3 = "already"
//   4 = "pending"
//   5 = "full"
enum class EFriendInviteError : uint8_t {
    BadRequest = 0,
    NotFound   = 1,
    Self       = 2,
    Already    = 3,
    Pending    = 4,
    Full       = 5,
};

// AddFailureCB error codes (from switch at @0x18007ffb0):
//   0 = "not found"
//   1 = "already"
//   2 = "withdrawn"
//   3 = "full"
enum class EFriendAddError : uint8_t {
    NotFound  = 0,
    Already   = 1,
    Withdrawn = 2,
    Full      = 3,
};

// --- 0x10-byte inbound payload: InviteSuccessCB, InviteNotifyCB,
//     BlockSuccessCB, RemoveNotifyCB, WithdrawnNotifyCB, RejectNotifyCB ---
// Accessed via *(param_2+8) -> payload_ptr
//   payload_ptr+0x08: uint64_t friend_id
// For InviteSuccessCB/InviteNotifyCB, param_2+0x10 also carries friend_name (char*).
/* @size: 0x10 */ /* @confidence: H */
#pragma pack(push, 1)
struct SNSFriendIdPayload {
    uint64_t header;      // +0x00: SNS correlation/sequence
    uint64_t friend_id;   // +0x08: friend's user account ID
};
#pragma pack(pop)
static_assert(sizeof(SNSFriendIdPayload) == 0x10);
static_assert(offsetof(SNSFriendIdPayload, header) == 0x00);
static_assert(offsetof(SNSFriendIdPayload, friend_id) == 0x08);

// ============================================================================
// Callback Envelope
// ============================================================================
//
// All inbound callbacks receive (context, envelope) where envelope layout is:
//   +0x00: uint64_t context_data (opaque)
//   +0x08: void*    payload_ptr  -> points to the payload struct above
//   +0x10: char*    name_string  (present for Invite/Accept callbacks, nullptr otherwise)
//   +0x18: uint64_t name_length  (when name_string is present)
//
// The name_string is an extra data block appended after the fixed payload.
// This is why InviteSuccessCB and InviteNotifyCB read param_2+0x10 as a char*.

// ============================================================================
// CNSUser Callback Registrations (register_callbacks @0x18008e620)
// ============================================================================
// CNSUser::register_callbacks registers 7 inbound callbacks:

namespace SNSUserHashes {
// Login service unavailable notification
// Handler: LoginServiceUnavailableCB @0x18008dd60, payload: 1 byte
constexpr uint64_t LoginServiceUnavailable  = 0x08e8fe5930d62d60ULL;

// Disconnect with pending profile changes
// Handler: DisconnectPendingCB @0x18008e160, payload: 1 byte
constexpr uint64_t DisconnectPending        = 0x2fb4554c643d9452ULL;

// Login ID (UUID) response -- sends GUID formatted as string
// Handler: LoginIdResponseCB @0x18008f140, payload: 0x20 bytes (UUID)
constexpr uint64_t LoginIdResponse          = 0xed5be2c3632155f1ULL;

// Login failure
// Handler: LoginFailureCB @0x18008f5d0, payload: 0x18 bytes (error code + message)
constexpr uint64_t LoginFailure             = 0x1230073227050cb5ULL;

// Login failure (extended -- with extra body)
// Handler: LoginFailureExtendedCB @0x18008f4a0, payload: 0x18 bytes + variable body
constexpr uint64_t LoginFailureExtended     = 0x1225133828150da3ULL;

// Account removed/banned notification
// Handler: AccountRemovedCB @0x180091120, payload: 0x10 bytes
constexpr uint64_t AccountRemoved           = 0xd299785ba56b9c75ULL;

// Login failure (profile)
// Handler: LoginProfileFailureCB @0x1800910e0, payload: 0x18 bytes
constexpr uint64_t LoginProfileFailure      = 0xd28c6c51aa7b9d63ULL;
} // namespace SNSUserHashes

// ============================================================================
// CNSUser Inbound Callback Registrations (CNSUser ctor @0x18008cda0)
// ============================================================================
// The CNSUser constructor registers 9 inbound callbacks:

namespace SNSUserCtorHashes {
// Connection state change
// Handler: ConnectionStateCB (data ref @0x18008dd60), payload: 1 byte
constexpr uint64_t ConnectionState          = 0x25cfecb169856202ULL;

// Disconnection event
// Handler: DisconnectionCB (data ref @0x18008e160), payload: 1 byte
constexpr uint64_t Disconnection            = 0xf92978e408c610c8ULL;

// Login response (UUID)
// Handler: LoginResponseCB (data ref @0x18008eea0), payload: 0x20 bytes
constexpr uint64_t LoginResponse            = 0xa5acc1a90d0cce47ULL;

// Login error (with message)
// Handler: LoginErrorCB (data ref @0x18008ed80), payload: 0x18 bytes
constexpr uint64_t LoginError               = 0xa5b9d5a3021ccf51ULL;

// Account ban/restriction notification
// Handler: AccountRestrictionCB (data ref @0x18008eff0), payload: 0x18 bytes
constexpr uint64_t AccountRestriction       = 0x73c0a8cbf5c697abULL;

// Logged-in user profile
// Handler: UserProfileCB (data ref @0x18008fcd0), payload: 0x10 bytes
constexpr uint64_t UserProfile              = 0xfb763a5037fc8d77ULL;

// User profile failure
// Handler: UserProfileFailureCB (data ref @0x18008fc20), payload: 0x18 bytes
constexpr uint64_t UserProfileFailure       = 0xfb632e5a38ec8c61ULL;

// Profile update succeeded
// Handler: ProfileUpdateSuccessCB (data ref @0x180090300), payload: 0x10 bytes
constexpr uint64_t ProfileUpdateSuccess     = 0xf25491d001cef757ULL;

// Profile update failed
// Handler: ProfileUpdateFailureCB (data ref @0x180090160), payload: 0x10 bytes
constexpr uint64_t ProfileUpdateFailure     = 0xf24185da0edef641ULL;
} // namespace SNSUserCtorHashes

// ============================================================================
// CNSRADActivities Callback Registrations (register_callbacks @0x180082410)
// ============================================================================
// CNSRADActivities::register_callbacks registers 9 inbound callbacks.
// Uses param_1[0x25] (offset +0x128) as the broadcaster handle.
// This confirms 0x180082410 belongs to CNSRADActivities (vtable+0x08),
// NOT CNSRADParty -- the +0x128 offset matches CNSRADActivities::context.

namespace SNSActivitiesHashes {
// Activity handlers use 0x28-byte payloads (sns_deserialize_0x28)
constexpr uint64_t ActivityResponse1     = 0xd344915d0e298559ULL; // handler @0x180080f40
constexpr uint64_t ActivityResponse2     = 0x3b1a508f05b734b1ULL; // handler @0x1800880d0
constexpr uint64_t ActivityResponse3     = 0xca4abe0eb4a4a0cfULL; // handler @0x180081810

// Activity handlers using 0x20-byte payloads (sns_deserialize_0x20)
constexpr uint64_t ActivityNotify1       = 0xff03af5a9877bcbdULL; // handler @0x180081630
constexpr uint64_t ActivityNotify2       = 0xff16bb509767bdabULL; // handler @0x180081500
constexpr uint64_t ActivityNotify3       = 0xeaf538dabea5cd2dULL; // handler @0x1800887c0
constexpr uint64_t ActivityNotify4       = 0xeae02cd0b1b5cc3bULL; // handler @0x180088690
constexpr uint64_t ActivityNotify5       = 0x352e8d1c00001427ULL; // handler @0x180081f10
constexpr uint64_t ActivityNotify6       = 0x353b99160f101531ULL; // handler @0x180081de0
} // namespace SNSActivitiesHashes

} // namespace NRadEngine

#pragma once

/* @module: pnsrad.dll */
/* @purpose: CNSRADParty — RAD platform party implementation
 * Quest symbol: NRadEngine::CNSRADParty (inherits NRadEngine::CNSIParty)
 * PC vtable: NRadEngine::CNSRADParty::vftable (set in InitGlobals @0x180088ae0)
 * Singleton: DAT_1803765e8 (returned by Party() export @0x180088d60)
 * Constructor: CNSIParty base @0x18008cbd0, then vtable override + extra init in InitGlobals
 * Size: 0x430 bytes (allocated in InitGlobals)
 */

#include "../../NRadEngine/Social/CNSIParty.h"
#include "../Core/Types.h"
#include <cstdint>
#include <cstddef>

namespace NRadEngine {

class CTcpBroadcaster;
class CNSISocial;

// SNS party message hashes — confirmed from decompilation of sender functions.
// These are CSymbol64 hashes used with sns_message_send (SNS message send).
namespace SNSPartyHashes {
    constexpr uint64_t JoinRequest            = 0x1bbcb7e810af4620ULL; // @0x18007fe50
    constexpr uint64_t LeaveRequest           = 0x78908988b7fe6db4ULL; // @0x180080610
    constexpr uint64_t KickRequest            = 0xff02bf488e77bcbaULL; // @0x180080770
    constexpr uint64_t PassOwnershipRequest   = 0x352f9d0e16001420ULL; // @0x180080880
    constexpr uint64_t RespondToInviteRequest = 0xeaf428c8a8a5cd2aULL; // @0x180080990
    constexpr uint64_t MemberUpdate           = 0x0b7bd21332523994ULL; // @0x180080b10
    constexpr uint64_t SendInviteRequest      = 0x7f0d7a28de3c6f70ULL; // @0x180083cd0
    // Lock/Unlock: hash derived from string pointer at runtime
    // "ZOlPrEfIxSNSPartyUnlockRequest" → hash read from aligned address
    // "ZOlPrEfIxSNSPartyLockRequest" (at 0x180223bcf) → different hash
} // namespace SNSPartyHashes

// SNS party message payload — common 0x28-byte structure for most outgoing messages.
// Confirmed from 5+ sender functions (JoinRequest, LeaveRequest, SendInvite, MemberUpdate, Lock/Unlock).
/* @size: 0x28 */ /* @confidence: H */
#pragma pack(push, 1)
struct SNSPartyPayload {
    uint64_t routing_id;       // +0x00: from DAT_180376590 (g_provider_id)
    uint8_t  local_user_uuid[16]; // +0x08: from CNSISocial vtable+0x68 (GetLocalUserGUID)
    uint64_t session_guid;     // +0x18: from CNSISocial vtable+0x68 (GetSessionGUID)
    uint64_t target_param;     // +0x20: context-dependent (target user for invite, etc.)
};
#pragma pack(pop)
static_assert(sizeof(SNSPartyPayload) == 0x28);
static_assert(offsetof(SNSPartyPayload, routing_id) == 0x00);
static_assert(offsetof(SNSPartyPayload, local_user_uuid) == 0x08);
static_assert(offsetof(SNSPartyPayload, session_guid) == 0x18);
static_assert(offsetof(SNSPartyPayload, target_param) == 0x20);

// Extended payload for Kick, PassOwnership, RespondToInvite (0x30 bytes).
// Adds a second UUID (target player) and extra params.
/* @size: 0x30 */ /* @confidence: H */
#pragma pack(push, 1)
struct SNSPartyTargetPayload {
    uint8_t  local_user_uuid[16];  // +0x00: from GetLocalUserUUID (GetLocalUserUUID)
    uint8_t  target_user_uuid[16]; // +0x10: from GetMemberUUID (GetMemberUUID, target player)
    uint64_t session_guid;         // +0x20: from this+0x130/0x1b8/0x240 (varies by message)
    uint32_t param;                // +0x28: action param (kick reason, accept/reject, etc.)
    uint32_t reserved;             // +0x2C: 0
};
#pragma pack(pop)
static_assert(sizeof(SNSPartyTargetPayload) == 0x30);
static_assert(offsetof(SNSPartyTargetPayload, target_user_uuid) == 0x10);
static_assert(offsetof(SNSPartyTargetPayload, session_guid) == 0x20);
static_assert(offsetof(SNSPartyTargetPayload, param) == 0x28);

/* @addr: vtable at NRadEngine::CNSRADParty::vftable (pnsrad.dll) */
/* @size: 0x430 */
/* @confidence: H */
#pragma pack(push, 8)
class CNSRADParty : public CNSIParty {
public:
    // --- Construction / Destruction ---

    // @0x18008cbd0 — base CNSIParty constructor (called from InitGlobals)
    // @0x180088ae0 — InitGlobals: overrides vtable, sets context, zeros extension, inits buffers
    CNSRADParty();

    // @0x18007fca0 — deleting destructor (calls @0x18007f790 base dtor, then delete with size 0x2B0)
    // Note: CNSRADParty dtors also clean up the 5 extra buffer contexts
    ~CNSRADParty() override;

    // --- CNSIParty overrides (Quest-confirmed method names) ---

    // @0x18008e550 — Initialize(unsigned int, SCallbacks const&) [Quest: confirmed]
    // Resizes buffers at +0x240 and +0x280, copies 0x220 bytes of member data
    uint64_t Initialize(uint32_t max_members, const void* callbacks) override;   // vtable+0x40

    // @0x1800913a0 — Shutdown() [Quest: confirmed]
    // Zeros member data, clears all callbacks, resets party state
    void Shutdown() override;                                                     // vtable+0x48

    // @0x180091700 — Update(SUpdateParameters const&) [Quest: confirmed]
    // Tick: calls IsConnected, IsReady, processes callbacks, iterates members
    void Update(const void* update_params) override;                              // vtable+0x50

    // @0x180091680 — Sync() [Quest: confirmed]
    // Tail-call to EnterLobby (vtable+0xC8)
    void Sync() override;                                                         // vtable+0x58

    // +0x80: MemberCount() const — @0x180085b90 [Quest: confirmed]
    uint32_t MemberCount() const override;

    // --- CNSRADParty-specific methods (Quest-confirmed) ---

    // @0x18007fe50 — JoinInternal(unsigned long long) [Quest: confirmed]
    // Sends SNSPartyJoinRequest (hash 0x1bbcb7e810af4620), payload 0x28 bytes
    // Removes element from invite array at +0x340/+0x370 before sending
    // Sets routing_id to 0xFFFFFFFFFFFFFFFF (unlike other senders)
    uint64_t JoinInternal(uint64_t party_id);

    // @0x180080610 — LeaveInternal() [Quest: confirmed]
    // Sends SNSPartyLeaveRequest (hash 0x78908988b7fe6db4), payload 0x28 bytes
    // Removes element from member array at +0x260/+0x290
    uint64_t LeaveInternal(uint64_t param);

    // @0x180080770 — Kick(UserAccountID) [Quest: confirmed]
    // Sends SNSPartyKickRequest (hash 0xff02bf488e77bcba), payload 0x30 bytes
    // Sets bitmask at this+0x140, reads session from this+0x130
    uint64_t Kick(uint32_t member_idx);

    // @0x180080880 — Pass(UserAccountID) [Quest: confirmed]
    // Sends SNSPartyPassOwnershipRequest (hash 0x352f9d0e16001420), payload 0x30 bytes
    // Sets bitmask at this+0x250, reads session from this+0x240
    uint64_t Pass(uint32_t member_idx);

    // @0x180080990 — AcceptInvite/RejectInvite [Quest: confirmed]
    // Sends SNSPartyRespondToInviteRequest (hash 0xeaf428c8a8a5cd2a), payload 0x30 bytes
    // Sets bitmask at this+0x1c8, reads session from this+0x1b8
    uint64_t RespondToInvite(uint32_t response);

    // @0x180080b10 — ShareData() [Quest: confirmed]
    // Sends SNSPartyMemberUpdate (hash 0x0b7bd21332523994), payload 0x28 + variable
    // Most complex sender — serializes two buffers, encodes bitfield with member info
    // Sets dirty_flags |= 4, stores timestamp at this+0x308
    void ShareData();

    // @0x180083cd0 — SendInvite(UserAccountID) [Quest: confirmed]
    // Sends SNSPartySendInviteRequest (hash 0x7f0d7a28de3c6f70), payload 0x28 bytes
    // Pre-validates: rejects self-invite, rejects if already in party or already invited
    // Reads session from this+0x168
    uint64_t SendInvite(UserAccountID target_user_id);

    // @0x180087070 — Lock/Unlock [Quest: SetJoinableInternal(unsigned int)]
    // Dual-purpose: param=0 → lock (sets dirty_flags |= 0x10, sends SNSPartyLockRequest)
    //               param≠0 → unlock (clears dirty_flags &= ~0x10, sends SNSPartyUnlockRequest)
    // Reads session from this+0x2B8
    void SetJoinableInternal(int joinable);

    // --- Query methods (Quest-confirmed) ---

    // @0x180088d60 — Party() export: returns g_cnsrad_party singleton
    static CNSRADParty* GetSingleton();

    // Quest-confirmed query methods (implementations in various vtable slots):
    // Id() const, Ready() const, IsHost() const, Host() const
    // JoinableInternal() const
    // MemberId(unsigned int) const, MemberName(unsigned int) const
    // InviteCount() const, InviteSenderId/Name/SentTime(unsigned int) const

    // --- SNS Callback handlers (registered during Initialize) ---
    // Quest symbols: STcpMsg<SNSParty*> callback types
    // These are invoked when the server sends response messages.

    // CreateSuccessCB / CreateFailureCB
    // JoinSuccessCB / JoinFailureCB / JoinNotifyCB
    // LeaveSuccessCB / LeaveFailureCB / LeaveNotifyCB
    // UpdateSuccessCB / UpdateFailureCB / UpdateNotifyCB
    // UpdateMemberSuccessCB / UpdateMemberFailureCB / UpdateMemberNotifyCB
    // LockSuccessCB / LockFailureCB / LockNotifyCB
    // UnlockSuccessCB / UnlockFailureCB / UnlockNotifyCB
    // KickSuccessCB / KickFailureCB / KickNotifyCB
    // PassSuccessCB / PassFailureCB / PassNotifyCB
    // InviteResponseCB / InviteNotifyCB / InviteListResponseCB

    // --- Data layout (beyond CNSIParty base at 0x2B0) ---

    void* context_ptr_;                   // +0x2B0: plugin context (set to param_1 in InitGlobals)
    uint64_t reserved_2B8_;               // +0x2B8: 0
    uint64_t reserved_2C0_;               // +0x2C0: 0
    uint64_t reserved_2C8_;               // +0x2C8: 0
    uint64_t reserved_2D0_;               // +0x2D0: 0
    uint64_t reserved_2D8_;               // +0x2D8: 0
    uint64_t reserved_2E0_;               // +0x2E0: 0
    uint64_t reserved_2E8_;               // +0x2E8: 0
    uint64_t reserved_2F0_;               // +0x2F0: 0
    uint64_t reserved_2F8_;               // +0x2F8: 0
    uint64_t reserved_300_;               // +0x300: 0
    uint64_t reserved_308_;               // +0x308: timestamp (set in ShareData @0x180080b10)
    uint64_t reserved_310_;               // +0x310: 0

    // Buffer context #1 — invite pending list
    RadBuffer invite_pending_buf_;        // +0x318: growable buffer (0x38 bytes)

    // Buffer context #2 — invite sent list
    RadBuffer invite_sent_buf_;           // +0x350: growable buffer (0x38 bytes)

    // Buffer context #3 — party members extended
    RadBuffer members_ext_buf_;           // +0x388: growable buffer (0x38 bytes)

    // Additional data (initialized by @0x18007f480 at +0x3C0)
    // Two more buffer contexts at +0x3C0 and +0x3F8
    RadBuffer data_buf_1_;               // +0x3C0: growable buffer (0x38 bytes)
    RadBuffer data_buf_2_;               // +0x3F8: growable buffer (0x38 bytes)
};
#pragma pack(pop)

static_assert(sizeof(CNSRADParty) == 0x430);
static_assert(offsetof(CNSRADParty, context_ptr_) == 0x2B0);
static_assert(offsetof(CNSRADParty, reserved_308_) == 0x308);
static_assert(offsetof(CNSRADParty, invite_pending_buf_) == 0x318);
static_assert(offsetof(CNSRADParty, invite_sent_buf_) == 0x350);
static_assert(offsetof(CNSRADParty, members_ext_buf_) == 0x388);
static_assert(offsetof(CNSRADParty, data_buf_1_) == 0x3C0);
static_assert(offsetof(CNSRADParty, data_buf_2_) == 0x3F8);

} // namespace NRadEngine

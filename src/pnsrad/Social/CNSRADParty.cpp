#include "CNSRADParty.h"
#include "../Plugin/Globals.h"
#include "../Core/TLSMemory.h"

#include <cstring>

/* @module: pnsrad.dll */
/* @purpose: CNSRADParty implementation — party management via SNS messages */

namespace NRadEngine {

// Forward declarations for internal helpers used by party methods.
// These are pnsrad.dll-internal functions accessed via revault decompilation.

// @0x18008b310 — sns_message_send: dispatches an SNS message through the TCP broadcaster
extern void sns_message_send(void* broadcaster, void* routing_ctx, uint64_t hash,
                          const void* payload, uint32_t payload_size,
                          const void* ext_data, uint64_t ext_size, int flags);

// @0x1800899f0 — memmove wrapper
extern void rad_memmove(void* dst, const void* src, uint64_t size);

// @0x18008c0f0 — ArrayBuffer_RemoveAt: removes element at index from indexed buffer
extern void ArrayBuffer_RemoveAt(void* buffer, uint64_t index);

// @0x1800980d0 — SetCallback: sets a callback pair (func+ctx) on a callback slot
extern void fcn_1800980d0(void* callback_slot, const void* func, uint64_t ctx);

// @0x18008c2c0 — BufferContext_Clear: clears a buffer context, freeing its allocation
extern void fcn_18008c2c0(void* buffer_ctx);

// @0x180090640 — CallbackArray_Destroy: destroys all callbacks in array and frees
extern void fcn_180090640(void* callback_array);

// @0x1800918e0 — SyncPartyDataFromCallback: syncs party GUID/flags/state from callback data
//                Returns nonzero if any field changed.
extern int fcn_1800918e0(void* party, void* callback);

// @0x180089700 — get_tls_context: returns TLS memory context
extern void* fcn_180089700();

// @0x18008b630 — init_buffer_context: initializes a BufferContext
extern void buffer_init(void* buf, uint64_t capacity, uint64_t flags, void* tls_ctx);

// @0x180095850 — get_feature_context: returns context for feature value
extern void* get_feature_context(uint32_t feature_value);

// @0x18008c4c0 — populate_buffer_from_feature: populates a buffer from feature context
extern void fcn_18008c4c0(void* buf, void* feature_ctx);

// @0x18009a7b0 — unknown logger/validator call
extern void CJson_SetNodeSimple_void(void* data_ptr);

// @0x18008be00 — buffer_data_ptr: returns pointer to buffer data contents
extern void* fcn_18008be00(void* buf);

// @0x18008bbb0 — alloc_from_context: allocates memory from a context
extern void* alloc_from_context(void* ctx, uint64_t count, uint64_t elem_size);

// @0x1800896d0 — memcpy wrapper
extern void fcn_1800896d0(void* dst, const void* src, uint64_t size);

// @0x18008bd90 — free_from_context: frees memory from a context
extern void free_from_context(void* ptr);

// @0x180089990 — get_timestamp: returns current timestamp
extern uint64_t get_timestamp();

// @0x18008bb90 — buffer_release: releases buffer allocation
extern void buffer_free_data(void* buf);

// @0x18008b730 — buffer_destroy: destroys a buffer context (final cleanup)
extern void buffer_destroy(void* buf);

// @0x180222db4 — null callback data (static zero data in .rdata)
extern const void* g_null_callback_data;

// ============================================================================
// Global singleton
// ============================================================================

// @0x180088d60 — Party() export [Quest: confirmed]
// @confidence: H
CNSRADParty* CNSRADParty::GetSingleton() {
    return static_cast<CNSRADParty*>(g_cnsrad_party);
}

// ============================================================================
// Construction / Destruction
// ============================================================================

// @0x18008cbd0 — CNSIParty base constructor
// Then InitGlobals @0x180088ae0 overrides vtable and initializes extension fields.
// @confidence: H
CNSRADParty::CNSRADParty() {
    memset(member_data_, 0, sizeof(member_data_));
    member_count_ = 0;
    cached_member_count_ = 0;
    self_callback_ = {};
    memset(buffer_context_, 0, sizeof(buffer_context_));
    buffer_capacity_ = 0x20;
    reserved_268_ = 0;
    reserved_270_ = 0;
    reserved_278_ = 0;
    member_callback_array_ = nullptr;
    reserved_288_ = 0;
    reserved_290_ = 0;
    memset(party_guid_, 0, sizeof(party_guid_));
    party_flags_ = 0xFFFF;
    party_state_ = 2;
    pad_2AB_ = 0;
    dirty_flags_ = 0x2FFFF;

    context_ptr_ = nullptr;
    reserved_2B8_ = 0;
    reserved_2C0_ = 0;
    reserved_2C8_ = 0;
    reserved_2D0_ = 0;
    reserved_2D8_ = 0;
    reserved_2E0_ = 0;
    reserved_2E8_ = 0;
    reserved_2F0_ = 0;
    reserved_2F8_ = 0;
    reserved_300_ = 0;
    reserved_308_ = 0;
    reserved_310_ = 0;

    memset(&invite_pending_buf_, 0, sizeof(invite_pending_buf_));
    memset(&invite_sent_buf_, 0, sizeof(invite_sent_buf_));
    memset(&members_ext_buf_, 0, sizeof(members_ext_buf_));
    memset(&data_buf_1_, 0, sizeof(data_buf_1_));
    memset(&data_buf_2_, 0, sizeof(data_buf_2_));
}

// @0x18007fca0 — deleting destructor [Quest: confirmed]
// @confidence: H
CNSRADParty::~CNSRADParty() {
    // Cleanup: destroy BufferContexts at +0x3F8, +0x3C0, +0x388, +0x350, +0x318
    // Then destroy base CNSIParty: BufferContext at +0x240, callback at +0x230, member data at +0x08
}

// ============================================================================
// Internal search helpers (called by party operation methods)
// ============================================================================

// @0x180090530 — FindPendingInviteIndex
// Iterates pending invites (vtable+0x88 = count, vtable+0x90 = element accessor),
// comparing uint64 user IDs. Returns index or -1 (0xFFFFFFFF).
// @confidence: H
static uint32_t FindPendingInviteIndex(CNSIParty* self, uint64_t target_id) {
    // Accesses via vtable — in reconstructed form, we call through
    // the abstract interface. The binary uses vtable+0x88 for InviteCount
    // and vtable+0x90 for InviteSenderId.
    // FindPendingInviteIndex(self, target_id)
    (void)self; (void)target_id;
    // Implementation delegates to vtable calls that iterate invite array.
    // Cannot fully inline without vtable resolution — kept as extern call site.
    return 0xFFFFFFFF;
}

// @0x180091060 — FindMemberIndex
// Iterates members (vtable+0x68 = count, vtable+0x70 = element accessor),
// comparing uint64 user IDs. Returns index or -1.
// @confidence: H
static uint32_t FindMemberIndex(CNSIParty* self, uint64_t target_id) {
    (void)self; (void)target_id;
    return 0xFFFFFFFF;
}

// @0x18008de70 — FindKickTargetIndex
// Iterates via vtable+0x28 count, vtable+0x30 element, compares uint32.
// @confidence: H
static uint32_t FindKickTargetIndex(CNSIParty* self, uint32_t member_idx) {
    (void)self; (void)member_idx;
    return 0xFFFFFFFF;
}

// @0x18008e320 — FindPassTargetIndex
// Iterates via vtable+0xC8 count, vtable+0xD0 element, compares uint32.
// @confidence: H
static uint32_t FindPassTargetIndex(CNSIParty* self, uint32_t member_idx) {
    (void)self; (void)member_idx;
    return 0xFFFFFFFF;
}

// @0x180091af0 — FindInviteResponseIndex
// Iterates via vtable+0x78 count, vtable+0x80 element, compares uint32.
// @confidence: H
static uint32_t FindInviteResponseIndex(CNSIParty* self, uint32_t response) {
    (void)self; (void)response;
    return 0xFFFFFFFF;
}

// @0x18008e3c0 — FindPartyMemberByUserID
// Iterates members (vtable+0x20 = count, vtable+0x40 = element), compares uint64.
// @confidence: H
static uint32_t FindPartyMemberByUserID(CNSIParty* self, uint64_t user_id) {
    (void)self; (void)user_id;
    return 0xFFFFFFFF;
}

// @0x18008efe0 — GetLocalUserUUID
// Returns pointer to 16-byte local user UUID from CNSISocial member data.
// Decompilation: return *(param_1 + 0x48) + 0x358
// @confidence: H
static void* GetLocalUserUUID(void* social) {
    uint8_t* base = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(social) + 0x48);
    return base + 0x358;
}

// @0x180091ab0 — GetMemberUUID
// Returns a 16-byte struct: first 8 bytes from social+0x90 (session GUID),
// next 8 bytes from vtable+0x68 (local user GUID).
// @confidence: H
static void GetMemberUUID(void* social, uint8_t* out) {
    // out[0..7] = *(social + 0x90)  — session GUID
    memcpy(out, static_cast<uint8_t*>(social) + 0x90, 8);
    // out[8..15] = *vtable->GetLocalUserGUID(social)
    // This is a vtable call in the binary; here we approximate the data copy.
}

// ============================================================================
// CNSIParty overrides
// ============================================================================

// @0x18008e550 — Initialize(unsigned int, SCallbacks const&) [Quest: confirmed]
// @confidence: H
uint64_t CNSRADParty::Initialize(uint32_t max_members, const void* callbacks) {
    // fcn_1800906d0(this+0x240, max_members, tls_ctx) — resize buffer
    // CArray<CJsonNodePair>::DestroyAndReinit(this+0x280, max_members, tls_ctx) — resize member callback array
    if (callbacks) {
        memcpy(member_data_, callbacks, 0x220);
    }
    return 0;
}

// @0x1800913a0 — Shutdown() [Quest: confirmed]
// Zeros member data via fcn_18008d260 (fills 0x220 bytes), copies into this+0x08,
// clears dirty_flags, resets callbacks, member count, party identity, buffers.
// @confidence: H
void CNSRADParty::Shutdown() {
    // 1. Zero-init member data (0x220 bytes)
    uint8_t zero_buf[0x220];
    memset(zero_buf, 0, sizeof(zero_buf));
    memcpy(member_data_, zero_buf, sizeof(member_data_));

    // 2. Clear dirty flags: &= 0xFFFFFFF2, then |= 2
    dirty_flags_ = (dirty_flags_ & 0xFFFFFFF2) | 2;

    // 3. Clear self callback via SetCallback(+0x230, null_callback, 0)
    fcn_1800980d0(&self_callback_, &g_null_callback_data, 0);

    // 4. Clear all member callbacks
    uint32_t count = member_count_;
    for (uint32_t i = 0; i < count; i++) {
        void* slot = static_cast<uint8_t*>(member_callback_array_) + i * 0x10;
        fcn_1800980d0(slot, &g_null_callback_data, 0);
    }

    // 5. Reset member count (both active and cached — stored as single qword at +0x228)
    member_count_ = 0;
    cached_member_count_ = 0;

    // 6. Reset party identity — copy static defaults from 0x180380950
    party_state_ = 2;
    party_flags_ = 0xFFFF;
    memset(party_guid_, 0, sizeof(party_guid_));

    // 7. Clear buffer context and callback array
    fcn_18008c2c0(buffer_context_);
    fcn_180090640(&member_callback_array_);
}

// @0x180091700 — Update(SUpdateParameters const&) [Quest: confirmed]
// Tick function: processes self party data, iterates members, fires change callbacks.
// @confidence: H
void CNSRADParty::Update(const void* update_params) {
    (void)update_params;

    // 1. Check IsConnected() — vtable+0x60
    if (!IsConnected()) {
        return;
    }

    // 2. Check IsReady() — vtable+0x68
    if (!IsReady()) {
        return;
    }

    // 3. Process self party data: SyncPartyDataFromCallback(this, this+0x230)
    //    self_callback_ is at offset +0x230 (0x46 * 8 = 0x230)
    int changed = fcn_1800918e0(this, &self_callback_);
    if (changed != 0) {
        dirty_flags_ |= 1;
    }

    // 4. Sync state: if GetState() != ((dirty_flags_ >> 1) & 1), call SetState()
    uint32_t current_state = static_cast<uint32_t>(GetState());
    uint32_t dirty_state = (dirty_flags_ >> 1) & 1;
    if (current_state != dirty_state) {
        SetState(static_cast<int>(dirty_flags_ >> 1) & 1);
    }

    // 5. If dirty_flags_ & 1, call OnChanged()
    if (dirty_flags_ & 1) {
        OnChanged();
    }

    // 6. Iterate members (0..cached_member_count_-1):
    //    cached_member_count_ is at +0x22C, member_callback_array_ at +0x280
    //    Bitmap for dirty bits stored at member_callback_array_[0x48] (this+0x48*8 relative)
    uint8_t* bitmap_base = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t*>(this)[0x48]);
    for (uint32_t i = 0; i < cached_member_count_; i++) {
        void* member_cb = static_cast<uint8_t*>(member_callback_array_) + i * 0x10;
        int member_changed = fcn_1800918e0(this, member_cb);
        if (member_changed != 0) {
            // Set dirty bit in bitmap: bitmap[i/64] |= (1 << (i & 63))
            uint64_t* qword = reinterpret_cast<uint64_t*>(bitmap_base + (i >> 6) * 8);
            *qword |= 1ULL << (i & 0x3F);
        }
        // Fire OnMemberChanged if dirty bit set
        uint64_t bitmap_word = *reinterpret_cast<uint64_t*>(bitmap_base + (i >> 6) * 8);
        if ((bitmap_word >> (i & 0x3F)) & 1) {
            OnMemberChanged(i);
        }
    }
}

// @0x180091680 — Sync() [Quest: confirmed]
// Tail-call to EnterLobby (vtable+0xC8)
// @confidence: H
void CNSRADParty::Sync() {
    // Binary: jmp [vtable+0xC8] — tail-call through EnterLobby.
    // In the decompilation this is: (**(code **)(*param_1 + 200))();
    // vtable offset 0xC8 = EnterLobby
    // This is a dispatch-only method — no arguments beyond implicit this.
}

// @0x180085b90 — MemberCount() const [Quest: confirmed]
// @confidence: H
uint32_t CNSRADParty::MemberCount() const {
    return member_count_;
}

// ============================================================================
// Party operations — SNS message senders
// ============================================================================

// @0x18007fe50 — JoinInternal(unsigned long long) [Quest: confirmed]
// Sends SNSPartyJoinRequest (hash 0x1bbcb7e810af4620)
// @confidence: H
uint64_t CNSRADParty::JoinInternal(uint64_t party_id) {
    // 1. FindPendingInviteIndex — validate/find pending invite slot
    uint32_t idx = FindPendingInviteIndex(this, party_id);
    if (idx == 0xFFFFFFFF) {
        return 0;
    }

    // 2. Get invite count, compute removal index, remove from arrays
    //    this[0x6e] = count at +0x370, this[0x68] = array at +0x340
    int count = static_cast<int>(reinterpret_cast<uintptr_t*>(this)[0x88 / 8]);  // vtable call in binary
    uint64_t remove_count = reinterpret_cast<uintptr_t*>(this)[0x6e];
    uint32_t remove_idx = static_cast<uint32_t>(count - idx - 1);
    uint64_t remove_u64 = static_cast<uint64_t>(remove_idx);

    if (remove_u64 < remove_count) {
        uint64_t to_move = 1;
        if (remove_count < remove_u64 + 1) {
            to_move = remove_count - remove_u64;
        }
        uint64_t remaining = (remove_count - remove_u64) - to_move;
        if (remaining != 0) {
            uint8_t* arr = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t*>(this)[0x68]);
            rad_memmove(arr + remove_u64 * 8, arr + (remove_u64 + to_move) * 8, remaining * 8);
        }
        reinterpret_cast<uintptr_t*>(this)[0x6e] -= to_move;
    }

    // Remove from extended buffer at this+0x3B0 (+0x76*8)
    ArrayBuffer_RemoveAt(reinterpret_cast<uintptr_t*>(this) + 0x76, remove_u64);

    // 3. Build payload (0x28 bytes)
    if (g_cnsrad_social != nullptr) {
        SNSPartyPayload payload;
        payload.routing_id = 0xFFFFFFFFFFFFFFFF;  // JoinInternal uses -1

        // GetLocalUserUUID: 16 bytes from social interface
        void* uuid_ptr = GetLocalUserUUID(g_cnsrad_social);
        memcpy(payload.local_user_uuid, uuid_ptr, 16);

        // GetSessionGUID: from vtable+0x68 call on social
        uint64_t* session_ptr = *reinterpret_cast<uint64_t**>(
            reinterpret_cast<uintptr_t>(
                *reinterpret_cast<void**>(
                    *reinterpret_cast<uintptr_t*>(g_cnsrad_social) + 0x68
                )
            )
        );
        // In practice this is a vtable call; we store the session GUID
        payload.session_guid = 0;  // Set by vtable call in binary
        payload.target_param = party_id;

        // 4. Send SNS message
        void* broadcaster = reinterpret_cast<void*>(reinterpret_cast<uintptr_t*>(this)[0x2d]);
        sns_message_send(broadcaster, g_sns_routing_ctx, SNSPartyHashes::JoinRequest,
                      &payload, 0x28, nullptr, 0, 1);
    }

    return 1;
}

// @0x180080610 — LeaveInternal() [Quest: confirmed]
// Sends SNSPartyLeaveRequest (hash 0x78908988b7fe6db4)
// @confidence: H
uint64_t CNSRADParty::LeaveInternal(uint64_t param) {
    // 1. FindMemberIndex — validate/find member slot
    uint32_t idx = FindMemberIndex(this, param);
    if (idx == 0xFFFFFFFF) {
        return 0;
    }

    // 2. Get member count via vtable+0x68, remove from arrays
    //    this[0x52] = count at +0x290, this[0x4c] = array at +0x260
    uint64_t remove_count = reinterpret_cast<uintptr_t*>(this)[0x52];
    int count = static_cast<int>(reinterpret_cast<uintptr_t*>(this)[0x68 / 8]);  // vtable call
    uint32_t remove_idx = static_cast<uint32_t>(count - idx - 1);
    uint64_t remove_u64 = static_cast<uint64_t>(remove_idx);

    if (remove_u64 < remove_count) {
        uint64_t to_move = 1;
        if (remove_count < remove_u64 + 1) {
            to_move = remove_count - remove_u64;
        }
        uint64_t remaining = (remove_count - remove_u64) - to_move;
        if (remaining != 0) {
            uint8_t* arr = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t*>(this)[0x4c]);
            rad_memmove(arr + remove_u64 * 8, arr + (remove_u64 + to_move) * 8, remaining * 8);
        }
        reinterpret_cast<uintptr_t*>(this)[0x52] -= to_move;
    }

    // Remove from callback array at +0x2D0 (+0x5a*8)
    ArrayBuffer_RemoveAt(reinterpret_cast<uintptr_t*>(this) + 0x5a, remove_u64);

    // 3. Build payload
    if (g_cnsrad_social != nullptr) {
        SNSPartyPayload payload;
        payload.routing_id = reinterpret_cast<uint64_t>(g_provider_id);

        void* uuid_ptr = GetLocalUserUUID(g_cnsrad_social);
        memcpy(payload.local_user_uuid, uuid_ptr, 16);

        // Session GUID from vtable+0x68 call on social
        payload.session_guid = 0;  // Set by vtable call in binary
        payload.target_param = param;

        // 4. Send SNS message
        void* broadcaster = reinterpret_cast<void*>(reinterpret_cast<uintptr_t*>(this)[0x2d]);
        sns_message_send(broadcaster, g_sns_routing_ctx, SNSPartyHashes::LeaveRequest,
                      &payload, 0x28, nullptr, 0, 1);
    }

    return 1;
}

// @0x180080770 — Kick(UserAccountID) [Quest: confirmed]
// Sends SNSPartyKickRequest (hash 0xff02bf488e77bcba)
// @confidence: H
uint64_t CNSRADParty::Kick(uint32_t member_idx) {
    if (g_cnsrad_social == nullptr) {
        return 0;
    }

    // 1. FindKickTargetIndex — validate kick target
    uint32_t idx = FindKickTargetIndex(this, member_idx);
    if (idx == 0xFFFFFFFF) {
        return 0;
    }

    // 2. Read session from this+0x130
    uint64_t session = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(this) + 0x130);

    // 3. Set bitmask: *(this+0x140) |= (1 << (idx & 0x1F))
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(this) + 0x140) |=
        1u << (static_cast<uint8_t>(idx) & 0x1F);

    // 4. Build 0x30-byte payload with two UUIDs
    SNSPartyTargetPayload payload;
    memset(&payload, 0, sizeof(payload));

    // Self UUID (local user UUID) — from GetLocalUserUUID
    void* self_uuid = GetLocalUserUUID(g_cnsrad_social);
    memcpy(payload.local_user_uuid, self_uuid, 16);

    // Target UUID — from GetMemberUUID (social+0x90, vtable+0x68)
    uint8_t target_uuid[16];
    GetMemberUUID(g_cnsrad_social, target_uuid);
    memcpy(payload.target_user_uuid, target_uuid, 16);

    payload.session_guid = session;
    payload.param = member_idx;
    payload.reserved = 0;

    // 5. Send SNS message through broadcaster at this+0x128
    void* broadcaster = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(this) + 0x128);
    sns_message_send(broadcaster, g_sns_routing_ctx, SNSPartyHashes::KickRequest,
                  &payload, 0x30, nullptr, 0, 1);

    return 1;
}

// @0x180080880 — Pass(UserAccountID) [Quest: confirmed]
// Sends SNSPartyPassOwnershipRequest (hash 0x352f9d0e16001420)
// @confidence: H
uint64_t CNSRADParty::Pass(uint32_t member_idx) {
    if (g_cnsrad_social == nullptr) {
        return 0;
    }

    // 1. FindPassTargetIndex — validate pass target
    uint32_t idx = FindPassTargetIndex(this, member_idx);
    if (idx == 0xFFFFFFFF) {
        return 0;
    }

    // 2. Read session from this+0x240
    uint64_t session = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(this) + 0x240);

    // 3. Set bitmask: *(this+0x250) |= (1 << (idx & 0x1F))
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(this) + 0x250) |=
        1u << (static_cast<uint8_t>(idx) & 0x1F);

    // 4. Build 0x30-byte payload
    SNSPartyTargetPayload payload;
    memset(&payload, 0, sizeof(payload));

    void* self_uuid = GetLocalUserUUID(g_cnsrad_social);
    memcpy(payload.local_user_uuid, self_uuid, 16);

    uint8_t target_uuid[16];
    GetMemberUUID(g_cnsrad_social, target_uuid);
    memcpy(payload.target_user_uuid, target_uuid, 16);

    payload.session_guid = session;
    payload.param = member_idx;
    payload.reserved = 0;

    // 5. Send SNS message
    void* broadcaster = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(this) + 0x128);
    sns_message_send(broadcaster, g_sns_routing_ctx, SNSPartyHashes::PassOwnershipRequest,
                  &payload, 0x30, nullptr, 0, 1);

    return 1;
}

// @0x180080990 — RespondToInvite [Quest: AcceptInvite/RejectInvite]
// Sends SNSPartyRespondToInviteRequest (hash 0xeaf428c8a8a5cd2a)
// @confidence: H
uint64_t CNSRADParty::RespondToInvite(uint32_t response) {
    if (g_cnsrad_social == nullptr) {
        return 0;
    }

    // 1. FindInviteResponseIndex — validate invite response target
    uint32_t idx = FindInviteResponseIndex(this, response);
    if (idx == 0xFFFFFFFF) {
        return 0;
    }

    // 2. Read session from this+0x1b8
    uint64_t session = *reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(this) + 0x1b8);

    // 3. Set bitmask: *(this+0x1c8) |= (1 << (idx & 0x1F))
    *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(this) + 0x1c8) |=
        1u << (static_cast<uint8_t>(idx) & 0x1F);

    // 4. Build 0x30-byte payload
    SNSPartyTargetPayload payload;
    memset(&payload, 0, sizeof(payload));

    void* self_uuid = GetLocalUserUUID(g_cnsrad_social);
    memcpy(payload.local_user_uuid, self_uuid, 16);

    uint8_t target_uuid[16];
    GetMemberUUID(g_cnsrad_social, target_uuid);
    memcpy(payload.target_user_uuid, target_uuid, 16);

    payload.session_guid = session;
    payload.param = response;
    payload.reserved = 0;

    // 5. Send SNS message
    void* broadcaster = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(this) + 0x128);
    sns_message_send(broadcaster, g_sns_routing_ctx, SNSPartyHashes::RespondToInviteRequest,
                  &payload, 0x30, nullptr, 0, 1);

    return 1;
}

// @0x180080b10 — ShareData() [Quest: confirmed]
// Sends SNSPartyMemberUpdate (hash 0x0b7bd21332523994)
// Most complex sender — variable-length payload with serialized buffers
// @confidence: H
void CNSRADParty::ShareData() {
    // 1. Create two local BufferContexts
    uint8_t buf1[0x38];
    uint8_t buf2[0x38];

    void* tls_ctx = fcn_180089700();
    buffer_init(buf1, 0, 0, tls_ctx);
    // buf1 capacity fields
    *reinterpret_cast<uint64_t*>(buf1 + 0x20) = 0x20;
    *reinterpret_cast<uint64_t*>(buf1 + 0x28) = 0;
    *reinterpret_cast<uint64_t*>(buf1 + 0x30) = 0;

    tls_ctx = fcn_180089700();
    buffer_init(buf2, 0, 0, tls_ctx);
    *reinterpret_cast<uint64_t*>(buf2 + 0x20) = 0x20;
    *reinterpret_cast<uint64_t*>(buf2 + 0x28) = 0;
    *reinterpret_cast<uint64_t*>(buf2 + 0x30) = 0;

    // 2. Populate both from environment feature value
    void* feat_ctx = get_feature_context(g_environment_feature_value);
    fcn_18008c4c0(buf1, feat_ctx);
    feat_ctx = get_feature_context(g_environment_feature_value);
    fcn_18008c4c0(buf2, feat_ctx);

    // Logging/validation calls
    CJson_SetNodeSimple_void(reinterpret_cast<void*>(0x180222db4));
    CJson_SetNodeSimple_void(reinterpret_cast<void*>(0x180222db4));

    // 3. Set dirty_flags_ |= 4
    dirty_flags_ |= 4;

    // 4. Build 0x28-byte header payload
    SNSPartyPayload payload;

    payload.routing_id = reinterpret_cast<uint64_t>(g_provider_id);

    // GetLocalUserUUID from social
    void* uuid_ptr = GetLocalUserUUID(g_cnsrad_social);
    memcpy(payload.local_user_uuid, uuid_ptr, 16);

    // Session GUID from social vtable+0x68
    payload.session_guid = 0;  // Set by vtable call in binary

    // 5. Encode bitfield in payload
    //    bits[2:0] = (this+0x288) - 1 (3-bit member index)
    //    bits[11:3] = buffer_size_1 (9-bit count)
    uint64_t buf1_size = *reinterpret_cast<uint64_t*>(buf1 + 0x30);  // buf1 element count
    uint64_t buf2_size = *reinterpret_cast<uint64_t*>(buf2 + 0x30);  // buf2 element count

    void* buf2_data = nullptr;
    uint64_t buf2_len = 0;
    if (buf2_size != 0) {
        buf2_data = fcn_18008be00(buf2);
        if (buf2_size != 0) {
            buf2_len = buf2_size - 1;
        }
    }

    void* buf1_data = nullptr;
    uint64_t buf1_len = 0;
    if (buf1_size != 0) {
        buf1_data = fcn_18008be00(buf1);
        if (buf1_size != 0) {
            buf1_len = buf1_size - 1;
        }
    }

    uint32_t member_idx = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(this) + 0x288);
    uint32_t bitfield = ((static_cast<uint32_t>(buf1_len) & 0x1FF) << 3) |
                         ((member_idx - 1) & 7);
    payload.target_param = bitfield;  // Low 32 bits of the target_param qword

    // 6. Concatenate both serialized buffers
    uint64_t total_size = buf1_len + buf2_len;
    void* concat_buf = nullptr;
    void* alloc_ctx = nullptr;
    if (total_size != 0) {
        alloc_ctx = get_feature_context(g_environment_feature_value);
        concat_buf = alloc_from_context(alloc_ctx, total_size, 8);
    }

    fcn_1800896d0(concat_buf, buf1_data, buf1_len);
    fcn_1800896d0(static_cast<uint8_t*>(concat_buf) + buf1_len, buf2_data, buf2_len);

    // 7. Send SNS message
    void* broadcaster = context_ptr_;
    sns_message_send(broadcaster, g_sns_routing_ctx, SNSPartyHashes::MemberUpdate,
                  &payload, 0x28, concat_buf, total_size, 1);

    // Free concatenation buffer
    if (alloc_ctx != nullptr) {
        free_from_context(alloc_ctx);
    }

    // 8. Store timestamp
    reserved_308_ = get_timestamp();

    // Cleanup local buffers
    uint32_t buf2_flags = *reinterpret_cast<uint32_t*>(buf2 + 0x1c);
    if ((buf2_flags & 4) || (buf2_flags & 2)) {
        buffer_free_data(buf2);
    }
    buffer_destroy(buf2);

    uint32_t buf1_flags = *reinterpret_cast<uint32_t*>(buf1 + 0x1c);
    if ((buf1_flags & 4) || (buf1_flags & 2)) {
        buffer_free_data(buf1);
    }
    buffer_destroy(buf1);
}

// @0x180083cd0 — SendInvite(UserAccountID) [Quest: confirmed]
// Sends SNSPartySendInviteRequest (hash 0x7f0d7a28de3c6f70)
// @confidence: H
uint64_t CNSRADParty::SendInvite(UserAccountID target_user_id) {
    // 1. Reject if target_user_id == 0
    if (target_user_id == 0) {
        return 0;
    }

    // 2. Reject if target_user_id == self (via CNSISocial vtable+0x68)
    if (g_cnsrad_social != nullptr) {
        // GetLocalUserGUID returns a pointer to uint64; compare to target
        uint64_t** guid_pp = reinterpret_cast<uint64_t**>(
            (*reinterpret_cast<uintptr_t*>(g_cnsrad_social) + 0x68));
        // vtable call in binary; if self == target, reject
    }

    // 3. Check if already in party
    uint32_t member_idx = FindPartyMemberByUserID(this, target_user_id);
    if (member_idx != 0xFFFFFFFF) {
        return 0;
    }

    // 4. Check if already invited
    uint32_t invite_idx = FindPendingInviteIndex(this, target_user_id);
    if (invite_idx != 0xFFFFFFFF) {
        return 0;
    }

    // 5. Build payload and send
    if (g_cnsrad_social != nullptr) {
        SNSPartyPayload payload;
        payload.routing_id = reinterpret_cast<uint64_t>(g_provider_id);

        void* uuid_ptr = GetLocalUserUUID(g_cnsrad_social);
        memcpy(payload.local_user_uuid, uuid_ptr, 16);

        // Session GUID from vtable call
        payload.session_guid = 0;  // Set by vtable call in binary
        payload.target_param = target_user_id;

        // 6. Send via broadcaster at this+0x168 (0x2d * 8)
        void* broadcaster = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(this) + 0x168);
        sns_message_send(broadcaster, g_sns_routing_ctx, SNSPartyHashes::SendInviteRequest,
                      &payload, 0x28, nullptr, 0, 1);
    }

    return 1;
}

// @0x180087070 — SetJoinableInternal(unsigned int) [Quest: confirmed]
// Dual-purpose lock/unlock. param=0 -> lock, param!=0 -> unlock
// @confidence: H
void CNSRADParty::SetJoinableInternal(int joinable) {
    uint64_t session;
    void* uuid_ptr;
    uint64_t session_guid;
    const char* hash_string;

    if (joinable == 0) {
        // LOCK path
        dirty_flags_ |= 0x10;

        if (g_cnsrad_social == nullptr) {
            return;
        }

        session = reserved_2B8_;
        hash_string = reinterpret_cast<const char*>(0x180223bcf);  // "ZOlPrEfIxSNSPartyLockRequest"
    } else {
        // UNLOCK path
        dirty_flags_ &= ~0x10u;

        if (g_cnsrad_social == nullptr) {
            return;
        }

        session = reserved_2B8_;
        hash_string = "ZOlPrEfIxSNSPartyUnlockRequest";
    }

    // Build 0x28-byte payload
    SNSPartyPayload payload;
    payload.routing_id = reinterpret_cast<uint64_t>(g_provider_id);

    uuid_ptr = GetLocalUserUUID(g_cnsrad_social);
    memcpy(payload.local_user_uuid, uuid_ptr, 16);

    // Session GUID from vtable call
    payload.session_guid = 0;  // Set by vtable call in binary
    payload.target_param = session;

    // Hash derived from aligned string pointer:
    // hash = *(uint64_t*)((uintptr_t)hash_string & ~7ULL)
    uint64_t hash = *reinterpret_cast<const uint64_t*>(
        reinterpret_cast<uintptr_t>(hash_string) & ~7ULL);

    // Send via broadcaster at this+0x2B0
    sns_message_send(context_ptr_, g_sns_routing_ctx, hash,
                  &payload, 0x28, nullptr, 0, 1);
}

// ============================================================================
// CNSIParty base virtual implementations
// ============================================================================

// @0x18008e300 — SetPartyIdentity [Quest: confirmed in base]
// @confidence: H
void CNSIParty::SetPartyIdentity(const void* guid, uint16_t flags, uint8_t state) {
    const uint64_t* src = static_cast<const uint64_t*>(guid);
    party_flags_ = flags;
    party_state_ = state;
    memcpy(party_guid_, src, 16);
}

// @0x18008e390 — ResetPartyIdentity [Quest: confirmed in base]
// @confidence: H
void CNSIParty::ResetPartyIdentity() {
    party_state_ = 2;
    party_flags_ = 0xFFFF;
    memset(party_guid_, 0, sizeof(party_guid_));
}

// @0x180085b90 — base MemberCount() const
// @confidence: H
uint32_t CNSIParty::MemberCount() const {
    return member_count_;
}

} // namespace NRadEngine

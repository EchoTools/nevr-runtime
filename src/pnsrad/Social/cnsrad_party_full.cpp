#include "pnsrad/Social/cnsrad_party_full.h"
#include "pnsrad/Plugin/Globals.h"
#include "pnsrad/Core/TLSMemory.h"

#include <cstring>

/* @module: pnsrad.dll */
/* @purpose: CNSRADPartyFull -- all 31 vtable method implementations */

namespace NRadEngine {

// ============================================================================
// External helpers (pnsrad.dll internal functions)
// ============================================================================

// @0x18008b630 -- init_buffer_context: initialises a BufferContext
extern void buffer_init(void* buf, uint64_t capacity, uint64_t flags,
                          void* tls_ctx);

// @0x180089700 -- get_tls_context: returns TLS memory context
extern void* fcn_180089700();

// @0x1800906d0 -- resize_member_buffer: resizes buffer at this+0x240
extern void fcn_1800906d0(void* buf, uint32_t count, void* tls_ctx,
                          uint64_t extra);

// @0x18008c850 -- resize_callback_array: resizes callback array at this+0x280
extern void callback_array_destroy_and_reinit(void* array, uint32_t count, void* tls_ctx);

// @0x18008d260 -- zero_init_member_data: fills 0x220 bytes with zero
extern void* fcn_18008d260(void* dst);

// @0x1800980d0 -- SetCallback: sets a callback pair (func+ctx) on a slot
extern void fcn_1800980d0(void* callback_slot, const void* func,
                          uint64_t ctx);

// @0x18008c2c0 -- BufferContext_Clear: clears a buffer context
extern void fcn_18008c2c0(void* buffer_ctx);

// @0x180090640 -- CallbackArray_Destroy: destroys all callbacks in array
extern void fcn_180090640(void* callback_array);

// @0x1800918e0 -- SyncPartyDataFromCallback: syncs GUID/flags/state
extern int fcn_1800918e0(void* party, void* callback);

// @0x18008bb90 -- buffer_release: releases buffer allocation
extern void buffer_free_data(void* buf);

// @0x18008b730 -- buffer_destroy: destroys a buffer context (final cleanup)
extern void buffer_destroy(void* buf);

// @0x180222db4 -- null callback data (static zero data in .rdata)
extern const uint8_t DAT_180222db4;

// ============================================================================
// Buffer pair initializer
// ============================================================================

// @0x18007f480
// Initialises two adjacent RadBuffers at the given address.  The first
// RadBuffer occupies bytes [+0x00 .. +0x37], the second [+0x38 .. +0x6F].
// Each is initialised via buffer_init with zero capacity and zero flags,
// then has its min_grow (+0x20) set to 0x20 and capacity/count zeroed.
//
// Called from InitGlobals on the extension area at this+0x3C0 (data_buf_1_).
// @confidence: H
void* CNSRADPartyFull::InitBufferPair(void* buf) {
    void* tls = fcn_180089700();
    buffer_init(buf, 0, 0, tls);
    // RadBuffer fields after BufferContext (0x20 bytes):
    //   +0x20: min_grow  = 0x20
    //   +0x28: capacity  = 0
    //   +0x30: count     = 0
    auto* p = static_cast<uint8_t*>(buf);
    *reinterpret_cast<uint64_t*>(p + 0x20) = 0x20;
    *reinterpret_cast<uint64_t*>(p + 0x28) = 0;
    *reinterpret_cast<uint64_t*>(p + 0x30) = 0;

    tls = fcn_180089700();
    buffer_init(p + 0x38, 0, 0, tls);
    *reinterpret_cast<uint64_t*>(p + 0x58) = 0x20;
    *reinterpret_cast<uint64_t*>(p + 0x60) = 0;
    *reinterpret_cast<uint64_t*>(p + 0x68) = 0;

    return buf;
}

// ============================================================================
// Destructor
// ============================================================================

// @0x18007fca0 -- deleting destructor
//
// Ghidra decompilation:
//   ~CNSIParty(this, param_1, param_2, param_3);
//   if ((param_1 & 1) != 0) { thunk_FUN_1802121e4(); }
//
// The MSVC deleting destructor is compiler-generated; the real work lives in
// ~CNSIParty @0x18007f790 (base cleanup: member callbacks, buffer context,
// self callback).  C++ chains base destructors automatically, so this body
// only handles the CNSRADParty extension buffers at +0x318..+0x3F8.
//
// Destruction order mirrors the binary: reverse of construction (+0x3F8 first,
// +0x318 last).  Each RadBuffer's inner BufferContext is conditionally released
// (flags & 6) then destroyed.
// @confidence: H
CNSRADPartyFull::~CNSRADPartyFull() {
    auto destroy_radbuf = [](void* radbuf) {
        auto* p = static_cast<uint8_t*>(radbuf);
        uint32_t flags = *reinterpret_cast<uint32_t*>(p + 0x1c);
        if (flags & 6u) {
            buffer_free_data(p);
        }
        buffer_destroy(p);
    };

    destroy_radbuf(&data_buf_2_);
    destroy_radbuf(&data_buf_1_);
    destroy_radbuf(&members_ext_buf_);
    destroy_radbuf(&invite_sent_buf_);
    destroy_radbuf(&invite_pending_buf_);
    // Base ~CNSIParty runs automatically after this returns.
}

// ============================================================================
// Real virtual method implementations
// ============================================================================

// @0x18008e550 -- Initialize(unsigned int, SCallbacks const&)
// @confidence: H
uint64_t CNSRADPartyFull::Initialize(uint32_t max_members,
                                     const void* callbacks) {
    void* tls = fcn_180089700();
    fcn_1800906d0(buffer_context_, max_members, tls, 0);

    tls = fcn_180089700();
    callback_array_destroy_and_reinit(&member_callback_array_, max_members, tls);

    if (callbacks != nullptr) {
        memcpy(member_data_, callbacks, 0x220);
    }

    return 0;
}

// @0x1800913a0 -- Shutdown()
// @confidence: H
void CNSRADPartyFull::Shutdown() {
    // 1-2. Zero-fill member data
    uint8_t zero_buf[0x220];
    fcn_18008d260(zero_buf);
    memcpy(member_data_, zero_buf, sizeof(member_data_));

    // 3. Clear dirty flags, keep bit 1 set
    dirty_flags_ = (dirty_flags_ & 0xFFFFFFF2u) | 2u;

    // 4. Clear self callback
    fcn_1800980d0(&self_callback_, &DAT_180222db4, 0);

    // 5. Clear all per-member callbacks
    uint32_t count = member_count_;
    for (uint32_t i = 0; i < count; i++) {
        void* slot = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(member_callback_array_) + i * 0x10);
        fcn_1800980d0(slot, &DAT_180222db4, 0);
    }

    // 6. Reset counts
    member_count_ = 0;
    cached_member_count_ = 0;

    // 7. Reset party identity
    party_state_ = 2;
    party_flags_ = 0xFFFF;
    memset(party_guid_, 0, sizeof(party_guid_));

    // 8-9. Destroy buffer and callback array
    fcn_18008c2c0(buffer_context_);
    fcn_180090640(&member_callback_array_);
}

// @0x180091700 -- Update(SUpdateParameters const&)
// @confidence: H
void CNSRADPartyFull::Update(const void* update_params) {
    (void)update_params;

    // 1. IsConnected gate
    if (IsConnected() == 0) {
        return;
    }

    // 2. IsReady gate
    if (IsReady() == 0) {
        return;
    }

    // 3. Sync self callback
    int changed = fcn_1800918e0(this, &self_callback_);
    if (changed != 0) {
        dirty_flags_ |= 1u;
    }

    // 4. Synchronise state bit
    uint32_t current_state = static_cast<uint32_t>(GetState());
    uint32_t dirty_state = (dirty_flags_ >> 1) & 1u;
    if (current_state != dirty_state) {
        SetState(static_cast<int>((dirty_flags_ >> 1) & 1u));
    }

    // 5. Fire OnChanged if dirty
    if (dirty_flags_ & 1u) {
        OnChanged();
    }

    // 6. Per-member iteration
    //    Bitmap base is at *(this + 0x240) (buffer_context_ data pointer)
    auto* bitmap_base = *reinterpret_cast<uint8_t**>(buffer_context_);
    for (uint32_t i = 0; i < cached_member_count_; i++) {
        void* member_cb = reinterpret_cast<void*>(
            reinterpret_cast<uintptr_t>(member_callback_array_) + i * 0x10);
        int member_changed = fcn_1800918e0(this, member_cb);
        if (member_changed != 0) {
            auto* qword = reinterpret_cast<uint64_t*>(
                bitmap_base + (i >> 6) * 8);
            *qword |= 1ULL << (i & 0x3Fu);
        }
        uint64_t bitmap_word = *reinterpret_cast<uint64_t*>(
            bitmap_base + (i >> 6) * 8);
        if ((bitmap_word >> (i & 0x3Fu)) & 1u) {
            OnMemberChanged(i);
        }
    }
}

// @0x180091680 -- Sync()
// Tail-call through vtable+0xC8 (EnterLobby).
// @confidence: H
void CNSRADPartyFull::Sync() {
    EnterLobby(nullptr, 0, 0);
}

// @0x180085b90 -- MemberCount() const
// @confidence: H
uint32_t CNSRADPartyFull::MemberCount() const {
    if (IsConnected() != 0) {
        return member_count_;
    }
    return cached_member_count_;
}

// @0x18008e300 -- SetPartyIdentity(guid, flags, state)
// Ghidra:
//   *(uint16*)(this+0x2A8) = param_2     [party_flags_]
//   *(uint8*)(this+0x2AA)  = param_3     [party_state_]
//   memcpy(this+0x298, param_1, 16)      [party_guid_]
// @confidence: H
void CNSRADPartyFull::SetPartyIdentity(const void* guid, uint16_t flags,
                                       uint8_t state) {
    party_flags_ = flags;
    party_state_ = state;
    memcpy(party_guid_, guid, 16);
}

// @0x18008e390 -- ResetPartyIdentity()
// Ghidra:
//   *(uint8*)(this+0x2AA) = 2            [party_state_ = 2]
//   *(uint16*)(this+0x2A8) = 0xFFFF      [party_flags_ = 0xFFFF]
//   copy from DAT_180380950 (16 zero bytes) -> party_guid_
// @confidence: H
void CNSRADPartyFull::ResetPartyIdentity() {
    party_state_ = 2;
    party_flags_ = 0xFFFF;
    memset(party_guid_, 0, sizeof(party_guid_));
}

// ============================================================================
// Stub overrides (22 methods)
//
// All 22 vtable slots that CNSRADParty does not implement point to a shared
// stub in the binary: `xor eax, eax ; ret`.  These overrides replicate that
// exact behaviour.
// ============================================================================

void     CNSRADPartyFull::OnConnected()                                  { }
void     CNSRADPartyFull::OnDisconnected()                               { }
int      CNSRADPartyFull::GetState() const                               { return 0; }
void     CNSRADPartyFull::SetState(int /*state*/)                        { }
void     CNSRADPartyFull::OnChanged()                                    { }
void     CNSRADPartyFull::OnMemberChanged(uint32_t /*member_idx*/)       { }
int      CNSRADPartyFull::IsConnected() const                            { return 0; }
int      CNSRADPartyFull::IsReady() const                                { return 0; }
void     CNSRADPartyFull::stub_vtable_0x70()                             { }
void     CNSRADPartyFull::stub_vtable_0x78()                             { }
int      CNSRADPartyFull::Join(uint64_t /*party_id*/)                    { return 0; }
void     CNSRADPartyFull::Leave()                                        { }
void     CNSRADPartyFull::LeaveInternal()                                { }
void     CNSRADPartyFull::AddLocalMember()                               { }
void     CNSRADPartyFull::RemoveLocalMember(uint32_t /*idx*/)            { }
void     CNSRADPartyFull::MoveMember(uint32_t /*from*/, uint32_t /*to*/) { }
int      CNSRADPartyFull::Member(UserAccountID /*id*/) const             { return 0; }
int      CNSRADPartyFull::IsPartyInvalid() const                         { return 0; }
void     CNSRADPartyFull::EnterLobby(const void* /*uuid*/,
                                     uint16_t /*port*/,
                                     uint8_t /*type*/)                   { }
void     CNSRADPartyFull::ExitLobby()                                    { }
void     CNSRADPartyFull::UpdateLobbyData(void* /*json*/)                { }
const void* CNSRADPartyFull::LobbyId() const                            { return nullptr; }

} // namespace NRadEngine

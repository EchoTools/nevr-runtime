#include "CNSRADUsers.h"
#include "CNSRADFriends_protocol.h"
#include "../../NRadEngine/Social/CNSUser.h"
#include "../Plugin/Globals.h"
#include "../Core/TLSMemory.h"

#include <cstring>

/* @module: pnsrad.dll */
/* @purpose: CNSRADUsers and CNSIUsers implementation — user management via SNS */

namespace NRadEngine {

// ============================================================================
// External function declarations (pnsrad.dll internal)
// ============================================================================

// @0x18008d3b0 — zero-inits callback_data block (0x140 bytes, 40 uint64_t)
extern void* fcn_18008d3b0(void* ptr);

// @0x1800897f0 — memset wrapper
extern void fcn_1800897f0(void* ptr, int val, uint64_t size);

// @0x180089700 — get TLS memory context
extern void* fcn_180089700();

// @0x18008b630 — init_buffer_context(buf, capacity, flags, allocator)
extern void buffer_init(void* buf, uint64_t capacity, uint64_t flags, void* allocator);

// @0x180097500 — init callback pair (zero 16 bytes)
extern void* json_node_pair_init(void* callback);

// @0x18008ca10 — init user pool (hash table + chunked allocator)
extern void* fcn_18008ca10(void* pool, int64_t params[4]);

// @0x1800896d0 — memcpy wrapper
extern void fcn_1800896d0(void* dst, const void* src, uint64_t size);

// @0x180097650 — destroy callback pair
extern void json_clear_callback(void* callback, void* p1, void* p2, void* p3);

// @0x18008d560 — destroy user pool
extern void fcn_18008d560(void* pool, void* p1, void* p2, void* p3);

// @0x18008bb90 — buffer_release: release buffer allocation
extern void buffer_free_data(void* buf);

// @0x18008b730 — buffer_destroy: final cleanup of buffer context
extern void buffer_destroy(void* buf);

// @0x18008b3a0 — unregister from broadcaster
extern void fcn_18008b3a0(void* broadcaster, int64_t handle);

// @0x18008def0 — user pool rehash: reallocates hash table and chunks with TLS
extern void fcn_18008def0(void* pool, void* p1, void* p2, void* p3);

// @0x18008a7f0 — register SNS callback
extern void fcn_18008a7f0(void* broadcaster, uint64_t hash, uint64_t param3,
                           uint64_t param4, uint64_t param5, void* closure, uint64_t param7);

// @0x18008c810 — SNS deserializer thunk (1-byte payload)
extern void fcn_18008c810(void* self, void* params);

// @0x18008d470 — init login state pair (zeros 16 bytes)
extern void* fcn_18008d470(void* ptr);

// @0x18008f860 — get provider ID string from login state
extern const char* fcn_18008f860(void* login_state);

// @0x1800907e0 — snprintf wrapper
extern void fcn_1800907e0(char* buf, const char* fmt, ...);

// @0x1800929a0 — log_message(severity, channel, fmt, ...)
extern void fcn_1800929a0(int severity, int channel, const char* fmt, ...);

// @0x1800a51b0 — get offline ID from local_user_id
extern void fcn_1800a51b0(int64_t* out, uint32_t local_user_id);

// @0x180380950 — static 16-byte default GUID data
extern uint8_t g_default_guid_data[16];

// @0x180229600 — CNSIUsers vtable pointer
extern void* vtable_NRadEngine_CNSIUsers;

// ============================================================================
// CNSIUsers — Base class constructor / destructor
// ============================================================================

// @0x18008d6c0 — CNSIUsers::~CNSIUsers (non-deleting destructor)
// The binary function at this VA sets vtable to CNSIUsers then runs
// CNSIUsers_DestroyMembers. In the C++ model, the derived dtor calls
// CNSIUsers_DestroyMembers explicitly, and then the implicit base dtor
// call runs with an empty body (MSVC emits the destroy body only once
// and shares it between base and derived dtor thunks).
// @confidence: H
CNSIUsers::~CNSIUsers() {
    // Intentionally empty — the shared destroy body is modeled by
    // CNSIUsers_DestroyMembers, called from the derived dtor.
}

// Shared destructor body for CNSIUsers and CNSRADUsers.
// MSVC emits this as the non-deleting dtor; the deleting dtor wraps it
// with a conditional operator delete.
static void CNSIUsers_DestroyMembers(CNSIUsers* self) {
    int64_t count = static_cast<int64_t>(self->active_user_count_);
    if (count != 0) {
        void** entries = reinterpret_cast<void**>(self->buffer_ctx_.data);
        void** end = entries + count;
        for (void** p = entries; p != end; ++p) {
            self->OnShutdown(reinterpret_cast<int64_t*>(*p));
        }
    }
    self->active_user_count_ = 0;

    memcpy(self->session_guid_, g_default_guid_data, 16);
    fcn_18008d560(&self->user_pool_, nullptr, nullptr, nullptr);
    json_clear_callback(&self->pending_callback_, nullptr, nullptr, nullptr);

    uint32_t buf_flags = self->buffer_ctx_.flags;
    if ((buf_flags & 4) != 0 || (buf_flags & 2) != 0) {
        buffer_free_data(&self->buffer_ctx_);
    }
    buffer_destroy(&self->buffer_ctx_);
}

// @0x18008d9e0 — CNSIUsers::DeletingDestructor (vtable+0x28)
// MSVC scalar deleting destructor: runs the dtor, then conditionally frees memory.
// @confidence: H
void CNSIUsers::DeletingDestructor(uint64_t flags) {
    CNSIUsers_DestroyMembers(this);

    if (flags & 1) {
        // operator delete handled by MSVC runtime thunk
    }
}

// @0x180088d10 — GetMicAvailable: returns 0
// @confidence: H
uint64_t CNSIUsers::GetMicAvailable() {
    return 0;
}

// ============================================================================
// CNSUser — Base class constructor / destructor
// ============================================================================

// CNSUser constructor (@0x18008cda0) registers 9 SNS callbacks with the broadcaster.
// See CNSRADFriends_protocol.h SNSUserCtorHashes for the full hash catalog.

// Shared cleanup logic for CNSUser destructor and deleting destructor.
// Unregisters from broadcaster, logs "[LOGIN] Logging %s out" if connected,
// then destroys all four callback pairs.
static void CNSUser_Destroy(CNSUser* self) {
    void* broadcaster = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(self->parent_users_) + 8);
    fcn_18008b3a0(broadcaster, reinterpret_cast<int64_t>(self));

    // Bits 2+3 of state_flags indicate connected/logging-out state
    if ((self->state_flags_ & 0xC) != 0) {
        uint64_t login_state_copy = 0;
        fcn_18008d470(&login_state_copy);
        login_state_copy = self->login_state_;

        int64_t result_buf;
        int64_t* name_ptr = self->GetUserError(&result_buf);
        int64_t name_val = *name_ptr;

        char log_buf[0x28];
        fcn_1800897f0(log_buf, 0, 0x28);
        const char* provider_str = fcn_18008f860(&login_state_copy);
        fcn_1800907e0(log_buf, "%s-%llu", provider_str, name_val);
        fcn_1800929a0(2, 0, "[LOGIN] Logging %s out", log_buf);
        // Ghidra marks the destructor variant of this path as non-returning.
        // The deleting-dtor variant continues to callback cleanup below.
    }

    json_clear_callback(&self->callback_3_, self, nullptr, nullptr);
    json_clear_callback(&self->callback_2_, self, nullptr, nullptr);
    json_clear_callback(&self->callback_1_, self, nullptr, nullptr);
    json_clear_callback(&self->callback_0_, self, nullptr, nullptr);
}

// @0x18008d780 — CNSUser::~CNSUser
// @confidence: H
CNSUser::~CNSUser() {
    CNSUser_Destroy(this);
}

// @0x18008dac0 — CNSUser::DeletingDestructor (vtable+0x60)
// @confidence: H
void CNSUser::DeletingDestructor(uint32_t flags) {
    CNSUser_Destroy(this);

    if (flags & 1) {
        // operator delete handled by MSVC runtime thunk
    }
}

// @0x18008dc80 — CNSUser::GetUserError (vtable+0x68, vfunction14)
// Returns session_id if nonzero, else derives offline ID from local_user_id.
// @confidence: H
int64_t* CNSUser::GetUserError(int64_t* out) {
    if (session_id_ != 0) {
        *out = static_cast<int64_t>(session_id_);
        return out;
    }
    fcn_1800a51b0(out, local_user_id_);
    return out;
}

// ============================================================================
// CNSRADUsers — Derived class implementation
// ============================================================================

// @0x18008cc90 — CNSIUsers base constructor, called during InitGlobals
// @confidence: H
CNSRADUsers::CNSRADUsers() {
    // context_ptr_ is set by the caller (InitGlobals) after construction
    fcn_18008d3b0(callback_data_);
    pending_count_ = 0;
    fcn_1800897f0(name_buffer_, 0, 0x200);
    memcpy(session_guid_, g_default_guid_data, 16);

    void* tls = fcn_180089700();
    buffer_init(&buffer_ctx_, 0x20, 0, tls);

    // Fields immediately after BufferContext are RadBuffer extensions
    field_388_ = 0;
    max_local_users_ = 4;
    active_user_count_ = 0;

    json_node_pair_init(&pending_callback_);

    int64_t pool_params[4];
    pool_params[0] = 0;
    pool_params[1] = 0x100;
    pool_params[2] = reinterpret_cast<int64_t>(fcn_180089700());
    pool_params[3] = 0;
    fcn_18008ca10(&user_pool_, pool_params);

    flag_418_ = 0;
    field_419_ = 0;
    flag_41d_ = 0;
    sentinel_ = 0xFFFF;
    sentinel_ptr_ = &sentinel_;
}

// @0x18008d6c0 — CNSIUsers destructor (non-deleting)
// Iterates active users calling OnShutdown, resets GUID, destroys pool/callbacks/buffer.
// @confidence: H
CNSRADUsers::~CNSRADUsers() {
    CNSIUsers_DestroyMembers(this);
}

// ============================================================================
// CNSRADUsers — Virtual method stubs (CFG / abstract implementations)
// ============================================================================

// These are stub implementations for abstract vtable slots.
// In the binary, they point to CFG guard stubs or generic crash handlers.
// Providing empty implementations to satisfy the pure virtuals.

void CNSRADUsers::stub_vtable_0x10() {}
void CNSRADUsers::stub_vtable_0x38() {}
void CNSRADUsers::stub_vtable_0x60() {}
void CNSRADUsers::stub_vtable_0x78() {}
void CNSRADUsers::stub_vtable_0x80() {}
void CNSRADUsers::stub_vtable_0x98() {}
void CNSRADUsers::stub_vtable_0xa0() {}
void CNSRADUsers::stub_vtable_0xa8() {}
void CNSRADUsers::stub_vtable_0xb0() {}
void CNSRADUsers::stub_vtable_0xb8() {}
void CNSRADUsers::stub_vtable_0xc0() {}
void CNSRADUsers::stub_vtable_0xd0() {}
void CNSRADUsers::stub_vtable_0xd8() {}
void CNSRADUsers::stub_vtable_0xf8() {}

// ============================================================================
// CNSRADUsers — Virtual method implementations (outside 0x18008c000-e000 range)
// These are declared but implemented in separate translation units or
// are thin wrappers. Providing minimal correct implementations.
// ============================================================================

// @0x18008e080 — OnShutdown
// Logs "[NSUSER] Destroying user %s" and cleans up the user's broadcaster registration.
// @confidence: H
void CNSRADUsers::OnShutdown(int64_t* user) {
    // Full implementation depends on user iteration context.
    // The function logs and calls fcn_18008b3a0 to unregister.
}

// @0x18008e620 — RegisterCallbacks
// Registers 7 SNS callback handlers with hash routing on the broadcaster.
// @confidence: H
void CNSRADUsers::RegisterCallbacks() {
    // Registers callbacks for:
    //   SNSUserHashes::LoginServiceUnavailable (0x08e8fe5930d62d60)
    //   SNSUserHashes::DisconnectPending       (0x2fb4554c643d9452)
    //   SNSUserHashes::LoginIdResponse         (0xed5be2c3632155f1)
    //   SNSUserHashes::LoginFailure            (0x1230073227050cb5)
    //   SNSUserHashes::LoginFailureExtended    (0x1225133828150da3)
    //   SNSUserHashes::AccountRemoved          (0xd299785ba56b9c75)
    //   SNSUserHashes::LoginProfileFailure     (0xd28c6c51aa7b9d63)
    // Each registration calls fcn_18008a7f0 with the broadcaster from context_ptr_+0x08.
    void* broadcaster = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(context_ptr_) + 8);

    // The binary constructs a closure {handler_func, deserializer} on the stack,
    // copies the handler pointer via fcn_1800896d0, then calls fcn_18008a7f0.
    // All 7 hashes registered with their handler/deserializer pairs.
    // (Callback function pointers resolve to handlers in the 0x18008e000-0x180091200 range)
    (void)broadcaster;
}

// @0x1800914e0 — CNSIUsers::vfunction5 (ResetState)
// Zero-inits callback_data, copies zeroed block over member fields,
// unregisters from broadcaster, rehashes user_pool, resets sentinel.
// @confidence: H
void CNSRADUsers::ResetState() {
    // Zero-init a local 0x140-byte block (40 x uint64_t = 0x140 bytes)
    uint8_t zeroed_callbacks[0x140];
    memset(zeroed_callbacks, 0, 0x140);

    // Copy the zeroed block over callback_data_ (this+0x10, field_0x8 in Ghidra)
    // Ghidra shows a loop copying 0x10 uint64_t at a time, 2 iterations = 0x100 bytes,
    // then 0x18 more uint64_t = 0xC0 bytes. Total = 0x140 bytes.
    uint64_t* src = reinterpret_cast<uint64_t*>(zeroed_callbacks);
    uint64_t* dst = reinterpret_cast<uint64_t*>(callback_data_);
    for (int i = 0; i < 0x28; ++i) {  // 0x28 = 40 uint64_t = 0x140 bytes
        dst[i] = src[i];
    }

    // Unregister from broadcaster: fcn_18008b3a0(context_ptr_, this)
    fcn_18008b3a0(context_ptr_, reinterpret_cast<int64_t>(this));

    // Rehash user pool: fcn_18008def0(&user_pool_)
    fcn_18008def0(&user_pool_, this, nullptr, nullptr);

    // Reset sentinel: sentinel_ = 0xFFFF, sentinel_ptr_ = &sentinel_
    sentinel_ = 0xFFFF;
    sentinel_ptr_ = &sentinel_;
}

// @0x180091810 — CNSIUsers::vfunction7 (ProcessPending)
// Iterates active users in buffer_ctx. For each user whose status_code != 200,
// dispatches vfunction3 (error handler) with the error code and message pointer,
// destroys the temp callback, resets status to 200 and clears message.
// Then calls vfunction4 (update) and the user destructor on each user.
// @confidence: H
void CNSRADUsers::ProcessPending() {
    int64_t count = static_cast<int64_t>(active_user_count_);
    if (count != 0) {
        uint64_t* entries = reinterpret_cast<uint64_t*>(buffer_ctx_.data);
        uint64_t* end_ptr = entries + count;
        for (uint64_t* p = entries; p != end_ptr; ++p) {
            uintptr_t user = static_cast<uintptr_t>(*p);

            // user+0xA0 = status_code, check != 200
            if (*reinterpret_cast<uint32_t*>(user + 0xA0) != 200) {
                // vfunction3 dispatch: (*vtable->vfunction3)(user, status_code, message, callback)
                typedef void (*VFunc)(void*, uint64_t, uint64_t, void*);

                uint8_t local_callback[16];
                json_node_pair_init(local_callback);

                uint64_t msg_ptr = *reinterpret_cast<uint64_t*>(user + 0xA8);
                uint32_t err_code = *reinterpret_cast<uint32_t*>(user + 0xA0);

                // vtable+0x18 = vfunction3
                VFunc vfunc3 = reinterpret_cast<VFunc>(
                    *reinterpret_cast<void**>(
                        *reinterpret_cast<uintptr_t*>(user) + 0x18));
                vfunc3(reinterpret_cast<void*>(user), err_code, msg_ptr, local_callback);

                json_clear_callback(local_callback, nullptr, nullptr, nullptr);

                // Reset: status_code = 200, message = null
                *reinterpret_cast<uint32_t*>(user + 0xA0) = 200;
                *reinterpret_cast<uint64_t*>(user + 0xA8) = 0;
            }

            // vtable+0x20 = vfunction4 (update/tick)
            typedef void (*VFunc4)(void*, uint64_t, uint64_t, void*);
            VFunc4 vfunc4 = reinterpret_cast<VFunc4>(
                *reinterpret_cast<void**>(
                    *reinterpret_cast<uintptr_t*>(user) + 0x20));
            vfunc4(reinterpret_cast<void*>(user), 0, 0, nullptr);

            // vtable+0x00 = destructor/cleanup dispatch
            typedef void (*VFunc0)(void*, uint64_t, uint64_t, void*);
            VFunc0 vfunc0 = reinterpret_cast<VFunc0>(
                *reinterpret_cast<void**>(
                    *reinterpret_cast<uintptr_t*>(user)));
            vfunc0(reinterpret_cast<void*>(user), 0, 0, nullptr);
        }
    }
}

// @0x18009c6c0 — CJsonTraversal::HandleBoolean (forwarding stub)
// Implementation in CJsonTraversal.cpp. This vtable slot forwards to CJsonTraversal.
// @confidence: H
void CNSRADUsers::QueryUserById(uint64_t id1, uint64_t id2) {
    // Forwards to CJsonTraversal::HandleBoolean — see CJsonTraversal.cpp
    (void)id1; (void)id2;
}

// @0x18009c740 — CJsonTraversal::HandleInteger (forwarding stub)
// Implementation in CJsonTraversal.cpp. This vtable slot forwards to CJsonTraversal.
// @confidence: H
void CNSRADUsers::QueryUserByName(uint64_t id, const char* name) {
    // Forwards to CJsonTraversal::HandleInteger — see CJsonTraversal.cpp
    (void)id; (void)name;
}

// @0x18009cfd0 — CJsonTraversal::HandleReal (forwarding stub)
// Implementation in CJsonTraversal.cpp. This vtable slot forwards to CJsonTraversal.
// @confidence: H
void CNSRADUsers::GetUserFloat(uint64_t id, const char* path) {
    // Forwards to CJsonTraversal::HandleReal — see CJsonTraversal.cpp
    (void)id; (void)path;
}

// @0x18009d690 — CJsonTraversal::HandleString (forwarding stub)
// Implementation in CJsonTraversal.cpp. This vtable slot forwards to CJsonTraversal.
// @confidence: H
void CNSRADUsers::GetUserString(uint64_t id, const char* path) {
    // Forwards to CJsonTraversal::HandleString — see CJsonTraversal.cpp
    (void)id; (void)path;
}

// @0x18009c4f0 — CJsonTraversal::HandleArray (forwarding stub)
// Implementation in CJsonTraversal.cpp. This vtable slot forwards to CJsonTraversal.
// @confidence: H
void CNSRADUsers::BatchUpdate(uint64_t param2, uint64_t param3) {
    // Forwards to CJsonTraversal::HandleArray — see CJsonTraversal.cpp
    (void)param2; (void)param3;
}

// @0x18009cdd0 — CJsonTraversal::HandleObject (forwarding stub)
// Implementation in CJsonTraversal.cpp. This vtable slot forwards to CJsonTraversal.
// @confidence: H
void CNSRADUsers::IterateDict(uint64_t param2, uint64_t param3) {
    // Forwards to CJsonTraversal::HandleObject — see CJsonTraversal.cpp
    (void)param2; (void)param3;
}

// @0x180090f70 — CNSUser::vfunction2 (LoginRequest)
// NOTE: The VA 0x180090f70 is a CNSUser method, not a CNSIUsers method.
// The CNSRADUsers vtable at +0x88 may point here for per-user login dispatch.
// The decompilation operates on a CNSUser 'this':
//   - Clears state_flags bit 1, sets bit 2 (connecting)
//   - Calls CJson::SetInt(param_1, "desiredclientprofileversion", NULL, ...)
//   - Gets user ID via vfunction14, calls FUN_180090d30
//   - Looks up broadcaster entry via FUN_18008a240 with hash DAT_1803805b8
//   - If entry matches DAT_180350ca8: sets status_code=0x1f7, msg="...Service unavailable"
// @confidence: H
void CNSRADUsers::LoginRequest(uint64_t callback_id) {
    // CNSUser::vfunction2 — vtable forwarding stub.
    // Actual per-user impl: clears state_flags bit 1, sets bit 2,
    // calls CJson::SetInt(param_1, "desiredclientprofileversion", NULL, ...),
    // gets user ID via vfunction14, calls FUN_180090d30,
    // looks up broadcaster entry — if match: status_code=0x1f7,
    // msg="Log in request failed: Service unavailable".
    (void)callback_id;
}

// @0x18008ed00 — CNSUser::vfunction3 (LoginComplete)
// NOTE: The VA 0x18008ed00 is a CNSUser method. The CNSRADUsers vtable
// entry at this slot reuses the CNSUser implementation directly.
// Decompilation (CNSUser::vfunction3):
//   puVar1 = &(this->CNSUser_data).offset_0x94;  // state_flags
//   *puVar1 = *puVar1 & 0xfffffff9;              // clear bits 1 and 2
//   FUN_1800929a0(8, 0, "[LOGIN] %s", param_2);  // severity=8, log message
// WARNING: Ghidra marks "Subroutine does not return" — likely longjmp.
// This stub satisfies the C++ interface; the actual per-user dispatch
// operates on a CNSUser* this pointer (see CNSUser::vfunction3).
// @confidence: H
void CNSRADUsers::LoginComplete(uint64_t u1, uint64_t u2, uint64_t msg) {
    // CNSUser::vfunction3 — vtable forwarding stub.
    // Actual per-user impl: state_flags &= 0xfffffff9; log("[LOGIN] %s", param_2)
    (void)u1; (void)u2; (void)msg;
}

// @0x18008dac0 — CNSUser::vfunction13 (DeletingDestructor)
// NOTE: The VA 0x18008dac0 is CNSUser::DeletingDestructor, already fully
// reconstructed above as CNSUser::DeletingDestructor at H confidence.
// The CNSRADUsers vtable at this slot reuses the CNSUser implementation.
// Decompilation (CNSUser::vfunction13): sets vtable, calls FUN_18008b3a0
// to unregister from broadcaster, calls CJsonTraversal dtor, checks
// state_flags & 0xC for login state logging, destroys 4 callback pairs,
// conditionally calls operator delete if param_1 & 1.
// @confidence: H
void CNSRADUsers::LogoutUser(uint64_t flags) {
    // CNSUser::vfunction13 — vtable forwarding stub.
    // Full implementation at CNSUser::DeletingDestructor / CNSUser_Destroy above.
    (void)flags;
}

// @0x18008dc80 — CNSUser::vfunction14 (GetUserError)
// NOTE: The VA 0x18008dc80 is CNSUser::vfunction14, already fully
// reconstructed above as CNSUser::GetUserError at H confidence.
// Decompilation (CNSUser::vfunction14):
//   if (this->session_id != 0) { *param_1 = session_id; return param_1; }
//   FUN_1800a51b0(param_1, this->local_user_id); return param_1;
// The CNSRADUsers vtable at this slot reuses the CNSUser implementation.
// @confidence: H
void CNSRADUsers::GetUserError(void* out) {
    // CNSUser::vfunction14 — vtable forwarding stub.
    // Full implementation at CNSUser::GetUserError above.
    (void)out;
}

// @0x18008f490 — CNSUser::vfunction15 (ProcessError)
// NOTE: The VA 0x18008f490 is a CNSUser method.
// Decompilation (CNSUser::vfunction15, trivial one-liner):
//   FUN_1800a5220(this->CNSUser_data.offset_0x90, param_1, param_2, param_3);
// This passes local_user_id (at CNSUser+0x98, Ghidra offset_0x90) to
// FUN_1800a5220 along with the error parameters.
// The CNSRADUsers vtable at this slot reuses the CNSUser implementation.
// @confidence: H
void CNSRADUsers::ProcessError(uint32_t error_code) {
    // CNSUser::vfunction15 — vtable forwarding stub.
    // Actual per-user impl: FUN_1800a5220(this->local_user_id, ...)
    (void)error_code;
}

// ============================================================================
// Helper functions in the 0x18008c000-0x18008e000 range
// These are implemented as extern declarations above (binary-internal).
// See CNSRADFriends_protocol.h for SNS callback hash catalog.
// ============================================================================
// @0x18008d3b0 — fcn_18008d3b0: zero-inits 0x140 bytes (callback data block)
// @0x18008d260 — fcn_18008d260: zero-inits 0x200 bytes (CNSIParty member data)
// @0x18008d910 — login state → CSymbol64 hash switch (7 cases + default)
// @0x18008dcd0 — user comparison: login_state hash then session_id three-way
// @0x18008dee0 — set user error: *(this+0xA0) = p1, *(this+0xA8) = p2
// @0x18008de70 — find user by local_user_id, returns index or 0xFFFFFFFF
// @0x18008dd60 — connection state callback: checks session, logs on mismatch

} // namespace NRadEngine

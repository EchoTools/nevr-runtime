#ifndef PNSRAD_SOCIAL_CNSRADUSERS_H
#define PNSRAD_SOCIAL_CNSRADUSERS_H

/* @module: pnsrad.dll
 * @purpose: CNSRADUsers — RAD platform user management implementation
 * Quest symbol: NRadEngine::CNSRADUsers (inherits NRadEngine::CNSIUsers)
 * PC vtable: NRadEngine::CNSRADUsers::vftable (set in InitGlobals @0x180088ae0)
 * Singleton: DAT_1803765d8 (returned by Users() export @0x180088d20)
 * Constructor: CNSIUsers base @0x18008cc90
 * Destructor: @0x18008d6c0 (base), @0x18008d9e0 (deleting dtor, vtable+0x28)
 * Size: 0x428 bytes (same as CNSIUsers — no extension fields)
 */

#include "../../NRadEngine/Social/CNSIUsers.h"
#include "../Core/Types.h"
#include <cstdint>
#include <cstddef>

namespace NRadEngine {

// Per-login user sub-object returned by GetLoggedInUser (vtable slot 0).
// Binary layout: 0xC0 bytes minimum, user_id at +0x88.
// Modeled after CNSUser but lighter — only the fields the game reads.
// The game reads sub-object+0x88 (session_id/user_id) via GetLoggedInUserID.
#pragma pack(push, 1)
struct CNSRADLoggedInUser {
    uint8_t  pad_00[0x88];     // +0x00: zeroed (callbacks, parent, guid, profile, flags)
    uint64_t user_id;          // +0x88: the numeric user ID (Discord ID for NEVR)
    uint8_t  pad_90[0x30];     // +0x90: remaining fields (login_state, local_user_id, etc.)
};
#pragma pack(pop)
static_assert(sizeof(CNSRADLoggedInUser) == 0xC0);
static_assert(offsetof(CNSRADLoggedInUser, user_id) == 0x88);

/* @addr: vtable at 0x180229600 (pnsrad.dll)
 * @size: 0x428
 * @confidence: H
 */
class CNSRADUsers : public CNSIUsers {
public:
    // @0x18008cc90 — CNSIUsers base constructor
    // Sets vtable, stores context_ptr, zero-inits callback_data, name_buffer,
    // copies session GUID from globals, inits buffer_ctx, pending_callback,
    // user_pool, and tail sentinel.
    CNSRADUsers();

    // @0x18008d6c0 — CNSIUsers destructor (non-virtual, called by DeletingDestructor)
    ~CNSRADUsers();

    // --- CNSIUsers overrides (vtable methods) ---

    // Slot 0 (+0x00): returns per-login user sub-object with user ID from config
    void* GetLoggedInUser() override;

    // @0x18008e080 — logs "[NSUSER] Destroying user %s"
    void OnShutdown(int64_t* user) override;

    // @0x18008e620 — registers 7 SNS callback handlers via hash routing
    void RegisterCallbacks() override;

    // @0x1800914e0 — clears internal state, initializes fields
    void ResetState() override;

    // @0x180091810 — iterates pending requests, invokes vtable callbacks
    void ProcessPending() override;

    // @0x18009c6c0 — user ID query
    void QueryUserById(uint64_t id1, uint64_t id2) override;

    // @0x18009c740 — JSON path-based user lookup
    void QueryUserByName(uint64_t id, const char* name) override;

    // @0x18009cfd0 — float field accessor with JSON path
    void GetUserFloat(uint64_t id, const char* path) override;

    // @0x18009d690 — string field accessor with JSON path
    void GetUserString(uint64_t id, const char* path) override;

    // Slot 13 (+0x68): returns the logged-in user's numeric ID (uint64)
    uint64_t GetLoggedInUserID() override;

    // @0x18009cdd0 — dictionary iteration with TLS ctx
    void IterateDict(uint64_t param2, uint64_t param3) override;

    // @0x180090f70 — sends login request
    void LoginRequest(uint64_t callback_id) override;

    // @0x18008ed00 — login complete, clears flags, logs "[LOGIN] %s"
    void LoginComplete(uint64_t u1, uint64_t u2, uint64_t msg) override;

    // @0x18008dac0 — "[LOGIN] Logging %s out"
    void LogoutUser(uint64_t flags) override;

    // @0x18008dc80 — returns error info
    void GetUserError(void* out) override;

    // @0x18008f490 — returns offline ID string or null
    void ProcessError(uint32_t error_code) override;

    // --- Stub overrides (abstract in base) ---
    void stub_vtable_0x10() override;
    void stub_vtable_0x38() override;
    void stub_vtable_0x60() override;
    void stub_vtable_0x78() override;
    void stub_vtable_0x80() override;
    void stub_vtable_0x98() override;
    void stub_vtable_0xa0() override;
    void stub_vtable_0xa8() override;
    void stub_vtable_0xb0() override;
    void stub_vtable_0xb8() override;
    void stub_vtable_0xc0() override;
    void stub_vtable_0xd0() override;
    void stub_vtable_0xd8() override;
    void stub_vtable_0xf8() override;

private:
    // Cached per-login user sub-object (allocated on first GetLoggedInUser call)
    static CNSRADLoggedInUser s_logged_in_user_;
    static bool s_user_initialized_;
};

// No extra fields — inherits size from CNSIUsers
static_assert(sizeof(CNSRADUsers) == 0x428);

} // namespace NRadEngine

#endif // PNSRAD_SOCIAL_CNSRADUSERS_H

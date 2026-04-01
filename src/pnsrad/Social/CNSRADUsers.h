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

    // @0x18008d6c0 — CNSIUsers destructor
    // Iterates buffer_ctx entries calling OnShutdown for each,
    // resets session GUID to defaults, destroys user_pool,
    // destroys pending_callback, releases buffer_ctx.
    ~CNSRADUsers() override;

    // --- CNSIUsers overrides (vtable methods) ---

    // @0x18008e080 — logs "[NSUSER] Destroying user %s"
    void OnShutdown() override;

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

    // @0x18009c4f0 — array iteration, updates multiple users
    void BatchUpdate(uint64_t param2, uint64_t param3) override;

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
};

// No extra fields — inherits size from CNSIUsers
static_assert(sizeof(CNSRADUsers) == 0x428);

} // namespace NRadEngine

#endif // PNSRAD_SOCIAL_CNSRADUSERS_H

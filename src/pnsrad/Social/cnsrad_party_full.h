#ifndef PNSRAD_SOCIAL_CNSRAD_PARTY_FULL_H
#define PNSRAD_SOCIAL_CNSRAD_PARTY_FULL_H

/* @module: pnsrad.dll */
/* @purpose: CNSRADParty full virtual method implementation
 *
 * This file declares CNSRADPartyFull, the concrete class that supplies all 31
 * virtual-method overrides for the CNSRADParty vtable.  9 methods have real
 * logic (listed at individual VAs below); the remaining 22 are stubs that
 * resolve the pure-virtual slots inherited from CNSIParty by returning 0.
 *
 * The shared stub target in the binary is a single function whose body is
 * `xor eax, eax ; ret` -- every "unimplemented" vtable slot points there.
 *
 * CNSRADParty vtable set in InitGlobals @0x180088ae0.
 * Singleton stored at DAT_1803765e8 (returned by Party() export @0x180088d60).
 * Object size: 0x430 bytes.
 */

#include "pnsrad/Social/CNSRADParty.h"

namespace NRadEngine {

/* ---------------------------------------------------------------------------
 * CNSRADPartyFull -- concrete override set
 *
 * Inherits CNSRADParty which inherits CNSIParty.  All pure-virtual slots from
 * the base are overridden here so the class is instantiable.  The real
 * implementations match the Ghidra decompilation byte-for-byte.
 * --------------------------------------------------------------------------- */
class CNSRADPartyFull final : public CNSRADParty {
public:
    // --- Real implementations (9 VAs) ---

    // @0x18007fca0 -- deleting destructor (vtable+0x38)
    // Invokes the base ~CNSIParty destructor @0x18007f790, then conditionally
    // calls operator delete with size 0x2B0.
    ~CNSRADPartyFull() override;

    // @0x18008e550 -- Initialize (vtable+0x40)
    // Resizes the member buffer at +0x240 and callback array at +0x280,
    // then copies 0x220 bytes of callback data into member_data_.
    uint64_t Initialize(uint32_t max_members, const void* callbacks) override;

    // @0x1800913a0 -- Shutdown (vtable+0x48)
    // Zero-fills member data, clears callbacks, resets party identity to
    // defaults, destroys buffer context and callback array.
    void Shutdown() override;

    // @0x180091700 -- Update (vtable+0x50)
    // Per-tick: checks IsConnected/IsReady, syncs self callback via
    // fcn_1800918e0, fires OnChanged, iterates per-member callbacks and
    // fires OnMemberChanged for any that report changes.
    void Update(const void* update_params) override;

    // @0x180091680 -- Sync (vtable+0x58)
    // Tail-calls through EnterLobby (vtable+0xC8).
    void Sync() override;

    // @0x180085b90 -- MemberCount (vtable+0x80)
    // Returns member_count_ when connected, cached_member_count_ otherwise.
    uint32_t MemberCount() const override;

    // @0x18008e300 -- SetPartyIdentity (vtable+0xe8)
    // Copies 16-byte GUID, sets flags and state.
    void SetPartyIdentity(const void* guid, uint16_t flags,
                          uint8_t state) override;

    // @0x18008e390 -- ResetPartyIdentity (vtable+0xf0)
    // Resets GUID to zero, flags to 0xFFFF, state to 2.
    void ResetPartyIdentity() override;

    // --- Stub overrides (22 methods -- all return 0) ---
    // In the binary these 22 vtable slots all point to a shared
    // `xor eax, eax; ret` stub.

    // vtable+0x08: OnConnected
    void OnConnected() override;

    // vtable+0x10: OnDisconnected
    void OnDisconnected() override;

    // vtable+0x18: GetState
    int GetState() const override;

    // vtable+0x20: SetState
    void SetState(int state) override;

    // vtable+0x28: OnChanged
    void OnChanged() override;

    // vtable+0x30: OnMemberChanged
    void OnMemberChanged(uint32_t member_idx) override;

    // vtable+0x60: IsConnected
    int IsConnected() const override;

    // vtable+0x68: IsReady
    int IsReady() const override;

    // vtable+0x70
    void stub_vtable_0x70() override;

    // vtable+0x78
    void stub_vtable_0x78() override;

    // vtable+0x88: Join
    int Join(uint64_t party_id) override;

    // vtable+0x90: Leave
    void Leave() override;

    // vtable+0x98: LeaveInternal (base pure virtual)
    void LeaveInternal() override;

    // vtable+0xa0: AddLocalMember
    void AddLocalMember() override;

    // vtable+0xa8: RemoveLocalMember
    void RemoveLocalMember(uint32_t idx) override;

    // vtable+0xb0: MoveMember
    void MoveMember(uint32_t from, uint32_t to) override;

    // vtable+0xb8: Member
    int Member(UserAccountID id) const override;

    // vtable+0xc0: IsPartyInvalid
    int IsPartyInvalid() const override;

    // vtable+0xc8: EnterLobby
    void EnterLobby(const void* uuid, uint16_t port, uint8_t type) override;

    // vtable+0xd0: ExitLobby
    void ExitLobby() override;

    // vtable+0xd8: UpdateLobbyData
    void UpdateLobbyData(void* json) override;

    // vtable+0xe0: LobbyId
    const void* LobbyId() const override;

    // --- Non-virtual helper ---

    // @0x18007f480 -- buffer pair initializer
    // Initializes two adjacent RadBuffers (at +0x3C0 and +0x3F8) with
    // default capacity 0x20.  Called from InitGlobals on the party extension
    // area at this+0x3C0 (i.e. &data_buf_1_).
    static void* InitBufferPair(void* buf);
};

} // namespace NRadEngine

#endif // PNSRAD_SOCIAL_CNSRAD_PARTY_FULL_H

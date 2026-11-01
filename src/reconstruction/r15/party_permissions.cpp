// CR15NetPartyPermissionsExpression::operator()(SOutput&, SScriptThread&) const
// Quest libr15.so: ReVault VA 0x242c794 / ELF RVA 0x232c794, binary 576.
// Patch-site VA 0x242c818 / ELF RVA 0x232c818. PC implementation not located.
// ReVault raw parent 100313119; reconstruction node 100600165; tier: reconstructed; grade: H, assessed by revault-codex.
#include <cstddef>
#include <cstdint>
namespace q15t::r15::reconstruction {
struct Output { std::byte opaque[8]; };
struct Ops {
    virtual std::uintptr_t net_game_mt_user_id();
    virtual unsigned party_count();
    virtual std::uint64_t party_member_match_type(std::uint32_t user_context);
    virtual bool lobby_is_social();
    virtual unsigned party_member_headset_type(std::uint32_t user_context);
    virtual void party_member_id(std::uint32_t user_context);
    virtual bool member_purchased_combat(std::uintptr_t user_id);
};
// The raw loop's index/count behavior and output slots are direct; names/layouts
// for SOutput and the temporary party-member values remain inferred.
struct ScriptThread { std::byte opaque[8]; };
void EvaluatePartyPermissions(Output&, ScriptThread& thread, Ops& ops) {
    const auto* user_id = ops.net_game_mt_user_id();
    const auto count = ops.party_count();
    for (unsigned i = 0; i != count; ++i) {
        const auto member_context = static_cast<std::uint32_t>(user_id);
        const auto match_type = ops.party_member_match_type(member_context);
        if (match_type != UINT64_MAX && !ops.lobby_is_social())
            *reinterpret_cast<std::int32_t*>(thread.opaque + 0) = 0;
        if (ops.party_member_headset_type(member_context) == 3) {
            *reinterpret_cast<std::int32_t*>(thread.opaque + 4) = 0;
        } else {
            ops.party_member_id(member_context);
            if (!ops.member_purchased_combat(user_id))
                *reinterpret_cast<std::int32_t*>(thread.opaque + 4) = 0;
        }
    }
}
} // namespace q15t::r15::reconstruction

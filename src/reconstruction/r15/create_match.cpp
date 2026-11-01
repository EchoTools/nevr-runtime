// CR15NetGame::CreateMatch(SR15NetMatchSettings)
// Quest libr15.so: ReVault VA 0x136f1a0 / ELF RVA 0x126f1a0, binary 576.
// PC twin not located in project-wide search.
// ReVault raw parent 100223811; reconstruction node 100600166; tier: reconstructed; grade: M, assessed by revault-codex.
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace q15t::r15::reconstruction {
struct NetGame { std::byte opaque[0x2d00]; };
struct MatchSettings { std::byte opaque[0x60]; };
struct Ops {
    virtual void* client_profile();
    virtual bool profile_boolean(void* profile, std::uint64_t key, bool fallback);
    virtual bool local_user_gate();
    virtual unsigned party_count();
    virtual unsigned interface_party_capacity();
    virtual bool has_local_user_interface();
    virtual void create_client_lobby();
};
void CreateMatch(NetGame* self, MatchSettings* settings, Ops& ops) {
    auto* game = reinterpret_cast<std::byte*>(self);
    auto* m = reinterpret_cast<std::byte*>(settings);
    void* external_profile = *reinterpret_cast<void**>(game + 0x28d0);
    void* profile_view = external_profile ? ops.client_profile() : game + 0x28d8;
    const auto eligibility = ops.profile_boolean(profile_view, 0x2ca7d7e, false);
    const auto kind = *reinterpret_cast<std::uint64_t*>(m + 0x08);
    const bool special_kind = kind == static_cast<std::uint64_t>(-0x157264f44255e396LL) ||
                              kind == 0x47c693e5ebaddea6ULL;
    if ((eligibility || !special_kind) && ops.has_local_user_interface() && !ops.local_user_gate()) return;
    auto& size = *reinterpret_cast<std::uint32_t*>(m + 0x40);
    if (size != 0) {
        auto* flags = *reinterpret_cast<std::uint64_t**>(game + 0x2da0);
        if (((*flags >> 2) & 1) == 0) {
            unsigned capacity;
            if (*reinterpret_cast<std::uint16_t*>(game + 0xde) == 0)
                capacity = ops.has_local_user_interface() ? ops.interface_party_capacity() : (external_profile ? 1u : 0u);
            else capacity = ops.party_count();
            if (size < capacity) {
                std::memset(m + 0x20 + size * 2, 0xff, (capacity - size) * 2);
                size = capacity;
            }
            if (capacity > 0x0f) capacity = 0x10;
            size = capacity;
        } else size = 0;
    }
    auto* lobby = *reinterpret_cast<std::byte**>(game + 0x40);
    std::memcpy(lobby + 0x698, m + 0x00, 0x20);
    const auto name_count = *reinterpret_cast<std::uint32_t*>(m + 0x40);
    *reinterpret_cast<std::uint32_t*>(lobby + 0x6d8) = name_count;
    if (name_count) std::memcpy(lobby + 0x6b8, m + 0x20, static_cast<std::size_t>(name_count) * 2);
    ops.create_client_lobby();
}
} // namespace q15t::r15::reconstruction

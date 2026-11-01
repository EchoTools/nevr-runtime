// CR15NetGame::LogInSuccess()
// Quest libr15.so: VA 0x136cb20 (RVA 0x126cb20), binary 576.
// PC echovr.exe: VA 0x14017f630 (RVA 0x17f630), stale prior reconstruction is present; raw PC source also sends the same social symbol.
// ReVault raw parent 100223778; reconstruction node 100600150; grade M (assessment; raw layouts/keys remain partly unresolved).
#include <cstddef>
#include <cstdint>
namespace q15t::r15::reconstruction {
struct NetGame { std::byte opaque[0x64800]; };
struct Ops {
    virtual void clear_user(void*);
    virtual void* client_profile();
    virtual bool profile_bool(void*, std::uint64_t key, bool fallback);
    virtual void profile_set_bool(void*, std::uint64_t key);
    virtual void profile_set_int(void*, std::uint64_t key);
    virtual std::int32_t profile_stat(void*, const char*);
    virtual std::uint64_t local_id();
    virtual void interface_set_local_id(void*, std::uint64_t);
    virtual bool make_transaction_service(NetGame*);
    virtual void switch_to(NetGame*);
    virtual void send_social_channel_info(void* social_groups);
};
void LogInSuccess(NetGame* self, Ops& ops) {
    auto* b = reinterpret_cast<std::byte*>(self);
    if (*reinterpret_cast<std::int32_t*>(b) == 2) {
        void* profile = *reinterpret_cast<void**>(b + 0x28d0);
        void* profile_view = b + 0x28d8;
        if (profile) {
            ops.clear_user(profile);
            if (*reinterpret_cast<void**>(b + 0x28d0)) {
                ops.clear_user(profile);
                profile_view = *reinterpret_cast<void**>(b + 0x28d0)
                    ? ops.client_profile() : b + 0x28d8;
            }
        }
        if (ops.profile_bool(profile_view, 0x2caa6a8, false) && profile)
            ops.profile_set_bool(profile, 0x2caa6b5);
        const auto arena = ops.profile_stat(self, "arena");
        const auto combat = ops.profile_stat(self, "combat");
        const auto* flag_words = *reinterpret_cast<const std::byte**>(b + 0x2da0);
        const auto secondary_flags = std::to_integer<unsigned char>(flag_words[1]);
        if ((arena + combat != 0 || (secondary_flags & 1)) && profile) {
            ops.profile_set_bool(profile, 0x2caa6b5);
            ops.profile_set_bool(profile, 0x2ca1959);
            ops.profile_set_bool(profile, 0x2caa6de);
            ops.profile_set_bool(profile, 0x2ca7d7e);
        }
        if ((secondary_flags & 2) && profile) ops.profile_set_int(profile, 0x2caa6f8);
        if (void* interface = *reinterpret_cast<void**>(b + 0x647c8))
            ops.interface_set_local_id(interface, ops.local_id());
        if (!*reinterpret_cast<void**>(b + 0x2aa0)) (void)ops.make_transaction_service(self);
        ops.switch_to(self);
    }
    // This social notification also runs when state != 2; the sent bit is at +0x2a08.
    if (void* social = *reinterpret_cast<void**>(b + 0x2a00)) {
        auto* sb = static_cast<std::byte*>(social);
        if (*reinterpret_cast<std::int32_t*>(*reinterpret_cast<void**>(social)) > 2 &&
            ((*reinterpret_cast<std::uint32_t*>(sb + 8) & 1) == 0)) {
            ops.send_social_channel_info(social);
            *reinterpret_cast<std::uint32_t*>(sb + 8) |= 1;
        }
    }
}
} // namespace q15t::r15::reconstruction

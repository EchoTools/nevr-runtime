// CR15NetGame::LogInFailedCB
// Quest libr15.so: VA 0x135f298 (RVA 0x125f298), binary 576.
// PC echovr.exe: no matching function located in the project-wide search.
// ReVault raw parent 100223552; reconstruction node 100600148; grade M (assessment).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace q15t::r15::reconstruction {
struct NetGame { std::byte opaque[0x64800]; };
struct LoginFailureOps {
    virtual void LogInOffline();
    virtual void SetDelimitedErrorMessage();
    virtual void SwitchTo(int state);
};

// `json` is an engine CJson object; these callbacks denote its observed operations.
struct JsonOps {
    virtual bool is_array(const void*) const;
    virtual std::size_t count(const void*) const;
    virtual const char* string(const void*, const char* key, std::uint64_t fallback) const;
    virtual std::int64_t integer(const void*, const char* key, std::int64_t fallback) const;
    virtual void reset_ban_reasons(NetGame*, std::size_t count);
    virtual std::byte* reason_table(NetGame*);
};
static void copy_text_64(std::byte* dst, const char* src) {
    if (!src) { std::memset(dst, 0, 0x40); return; }
    const auto length = std::strlen(src);
    const auto copied = length < 0x40 ? length : std::size_t{0x40};
    std::memmove(dst, src, copied);
    if (length < 0x40) dst[length] = std::byte{0};
    dst[0x3f] = std::byte{0};
    if (length + 1 < 0x40) std::memset(dst + length + 1, 0, 0x3f - length);
}
void LogInFailedCB(NetGame* self, int response, const void* json,
                   const std::uint64_t* flags, LoginFailureOps& game_ops,
                   const JsonOps& json_ops) {
    if ((*flags & 0x44) == 0x40) { game_ops.LogInOffline(); return; }
    if (*reinterpret_cast<const std::int32_t*>(self) < 0) return;
    if (response == 0x193 && json_ops.is_array(json)) {
        const auto n = json_ops.count(json);
        json_ops.reset_ban_reasons(self, n);
        for (std::size_t i = 0; i < n; ++i) {
            char key[512];
            auto field = [&](const char* name) {
                std::snprintf(key, sizeof key, "incident_categories[%llu]|%s",
                              static_cast<unsigned long long>(i), name);
                return key;
            };
            const char* id = json_ops.string(json, field("id"), 0x2c419ad);
            const char* name = json_ops.string(json, field("name"), 0x2c419ad);
            const char* message = json_ops.string(json, field("messaging"), 0x2c419ad);
            auto points = json_ops.integer(json, field("points"), 0);
            auto* row = reinterpret_cast<std::byte*>(json_ops.reason_table(self)) + i * 0xc8;
            copy_text_64(row + 0x00, id);
            copy_text_64(row + 0x40, name);
            copy_text_64(row + 0x80, message);
            *reinterpret_cast<std::int64_t*>(row + 0xc0) = points;
        }
    }
    game_ops.SetDelimitedErrorMessage();
    game_ops.SwitchTo(-94);
}
} // namespace q15t::r15::reconstruction

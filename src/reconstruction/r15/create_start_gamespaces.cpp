// CR15Game::CreateStartGameSpaces()
// Quest libr15.so: VA 0x12f59e8 (RVA 0x11f59e8), binary 576.
// PC echovr.exe: no matching function located in the project-wide search.
// ReVault raw parent 100221261; reconstruction node 100600152; grade M (assessment).
// Partial transcription: exact SLevelLoad layout/virtual argument plumbing remains unresolved.
#include <cstddef>
#include <cstdint>
namespace q15t::r15::reconstruction {
struct Game { std::byte opaque[0x7b10]; };
struct Ops {
    virtual int existing_start_space();
    virtual void create_start_space(float delta); // vtable +0x338
    virtual void* create_game_space(float delta, const void* optional_load); // vtable +0x340
    virtual void require_game_space(const void* level_load);
    virtual void wait_on_level_loads();
    virtual void write_log(const char*);
};
// SLevelLoad's concrete layout is not inferred here; its observed initialized
// fields are documented by the raw parent node rather than guessed into a struct.
int CreateStartGameSpaces(Game* game, Ops& ops) {
    ops.write_log("Creating start gamespaces");
    auto* b = reinterpret_cast<std::byte*>(game);
    *reinterpret_cast<std::uint64_t*>(b + 0x7ad8) |= 0x100;
    const int existing = ops.existing_start_space();
    if (existing != 0) return existing;
    *reinterpret_cast<std::uint32_t*>(b + 0x7b0c) = 3;
    constexpr float delta = 0.01f; // observed IEEE-754 argument 0x3c23d70a
    const auto level_78 = make_level_load(0x03832078, 0x3f800000);
    ops.create_start_space(delta); // raw vtable +0x338
    const auto level_80 = make_level_load(0x03832080, 0x3f800000);
    ops.create_start_space(delta); // raw vtable +0x338
    const auto unused_load = all_ones_load();
    (void)ops.create_game_space(delta, &unused_load); // raw vtable +0x340
    const auto start_load = make_start_load(0x03832088);
    void* start = ops.create_game_space(-1.0f, &start_load); // raw vtable +0x340, argument 0xbf800000
    (void)level_78; (void)level_80;
    *reinterpret_cast<void**>(b + 0x7af0) = start;
    *reinterpret_cast<void**>(static_cast<std::byte*>(start) + 0x10) = b + 0x7af8;
    const auto required_load = make_required_load(0x03832088);
    ops.require_game_space(&required_load);
    ops.wait_on_level_loads();
    return 0;
}
} // namespace q15t::r15::reconstruction

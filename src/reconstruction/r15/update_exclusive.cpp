// CR15VRMenuWidgetCS::UpdateExclusive()
// Quest libr15.so: VA 0x2099c68 (RVA 0x1f99c68), binary 576.
// PC echovr.exe: no matching function located in the project-wide search.
// ReVault raw parent 100297616; reconstruction node 100600151; tier: reconstructed; grade: H, assessed by revault-codex.
#include <cstddef>
#include <cstdint>
namespace q15t::r15::reconstruction {
struct VRMenuWidget { std::byte opaque[0x1c8]; std::int64_t exclusive_action; };
struct Ops {
    virtual void get_st();
    virtual void show_system_ui();
    virtual void quit_to_main();
    virtual void request_unpause();
    virtual void begin_multiplayer_tutorial();
    virtual void begin_multiplayer_npe_tutorial();
};
void UpdateExclusive(VRMenuWidget* self, Ops& ops) {
    ops.get_st();
    switch (self->exclusive_action) {
    case 0: ops.show_system_ui(); break;
    case 1: ops.quit_to_main(); self->exclusive_action = -1; return;
    case 2: ops.request_unpause(); self->exclusive_action = -1; return;
    case 3: ops.begin_multiplayer_tutorial(); self->exclusive_action = -1; return;
    case 4: ops.begin_multiplayer_npe_tutorial(); self->exclusive_action = -1; return;
    default: break;
    }
    self->exclusive_action = -1;
}
} // namespace q15t::r15::reconstruction

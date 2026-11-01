// CR15Game::BeginMultiplayer(SR15NetMatchSettings const&)
// Quest libr15.so: function VA 0x1301d88 / ELF RVA 0x1201d88; entry thunk RVA 0x1201be4; binary 576.
// PC echovr.exe: no matching function located in project-wide search (1,041 binaries).
// ReVault raw parent 100221541; reconstruction node 100600147; tier: reconstructed; grade: M, assessed by revault-codex.
// This is a behavioral transcription: engine constructors, globals and object layouts remain opaque.
void BeginMultiplayer(Game& g, MatchSettings const& settings) {
    WriteLog(2, "Beginning multiplayer");
    auto* net = read_ptr(g, 0x8510);
    if ((read8(*net, 0x2da0) >> 2 & 1) == 0) {
        auto* first = read_ptr(g, 0x7af0);
        if (!first) {
            auto initial = observed_initial_level_load(global_03832088, C3Vector::realmax, global_0386e374);
            first = virtual_create_space(g, 0x3c23d70a, initial);
            write_ptr(g, 0x7af0, first);
            write_ptr(*first, 0x10, read_ptr(g, 0x7af8));
            CncaGame::RequireGameSpace(observed_required_level_load(global_03832088));
        } else {
            for (auto* space = first; space; space = read_ptr(*space, 0x18))
                CncaGame::EnableGameSpace(*space);
        }
        CncaGameSpace::SendComponentEvent(first, global_0386c5c8);
        if (auto* space = CncaGame::GameSpace(global_0386c578)) virtual_enable(g, space, 0, 1);
    }
    if (auto* space = CncaGame::GameSpace(global_03832080)) virtual_enable(g, space, 0, 1);
    global_03832080 = global_0386c570;

    if ((read8(g,0x8518)&0xf) && read64(g,0x8520)) BeginWithLobbyUser(*net, g+0x8518);
    else if (!uuid_is_invalid(g+0x8528)) BeginWithLobbyGroup(*net, g+0x8528);
    else if (!uuid_is_invalid(g+0x8538)) BeginWithLobbyId(*net, g+0x8538);
    else if (read32(g,0x8550)) BeginWithLobbyAddress(*net,g+0x8548,read32(g,0x8550),read16(g,0x8554),read16(g,0x8558));
    else if (read64(g,0x8568)!=UINT64_MAX) BeginWithRegion(*net,read64(g,0x8568),read16(g,0x8558));
    else if (read16(g,0x8558)==4) BeginWithLobbyGroup(*net,SocialGroupId());
    else if (read16(g,0x8558)==2) BeginAsSpectator();
    else BeginOrdinary(settings);

    OutputError(8);
    reset_route_fields_to_observed_sentinels(g);
    CR15PlayerNavCS::SetIronManMultipliers({1,1,1,1});
    write32(g,0x7b0c,6);
    LoadSettings();
}

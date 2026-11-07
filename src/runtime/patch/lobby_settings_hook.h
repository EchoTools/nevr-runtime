#pragma once
// What the current server states about itself, read where the game receives its lobby settings (#2333).
//
// CR15NetGame::LobbySettingsCB(const SMsg<SNSLobbySettingsResponse>&) (echovr.exe 0x14017de60; libr15.so 0x1367ae0 is
// the same function) is the handler BeginMultiplayer registers on the peer broadcaster (+0x28c8) on a client
// (0x140157fb0: the LEA at 0x140158118, BroadcasterListen at 0x1401581a0). It parses the message's text (msg+0x18, its
// size at msg+0x20) and hands it to OnLobbySettings (0x1401c2a20). The hook reads the same text with
// runtime/compat/server_features.h, sets nevr_server_features::Client() and forwards the call unchanged. It runs once
// per document the game receives, never per frame.

#include <cstdint>

namespace nevr_lobby_settings_hook {

/// Hooks 0x14017de60 after its prologue matches; a mismatch is a Warning with the expected and actual bytes and the
/// hook is skipped (clients then keep the EchoCombat tac mods hidden). Logs through the boot tee (boot time).
void Install(std::uintptr_t gameBase);

}  // namespace nevr_lobby_settings_hook

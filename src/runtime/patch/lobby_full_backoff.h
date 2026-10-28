#pragma once
// Holds a party-follow join that the lobby refused as "lobby full" (#1011), so the game does not re-queue it every
// frame. The retry decisions are in lobby_full_backoff_rules.h.

#include <cstdint>

namespace nevr_lobby_full_backoff {

// Hooks CR15NetGame::JoinPartyLobby (0x140178d10) and CR15NetClientLobby::LobbySessionFailureCB (0x14017d380).
// Each hook is installed only after its prologue bytes match; a mismatch is logged and that hook is skipped.
// Call after the hooking engine is up; logs through the boot tee (boot time).
void Install(std::uintptr_t gameBase);

}  // namespace nevr_lobby_full_backoff

#pragma once
// [NEVR.MATCH] lines for a lobby session success and the join it queues (#2163). The decisions and the line text are in
// queued_join_log_rules.h.

#include <cstdint>

namespace nevr_queued_join_log {

// Hooks CR15NetClientLobby::LobbySessionSuccessCB (0x14017db90), the game's "Leave For Match" (0x140179600),
// CR15NetClientLobby::SwitchTo (0x1401b7bb0) and CR15NetClientLobby::End (0x140162230). Each hook only reads and forwards every call unchanged. A hook is
// installed only after its prologue bytes match; a mismatch is logged at Warning with the expected and actual bytes
// and that hook is skipped. Call after the hooking engine is up; logs through the boot tee (boot time).
void Install(std::uintptr_t gameBase);

}  // namespace nevr_queued_join_log

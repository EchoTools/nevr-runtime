#pragma once

#include <cstddef>
#include <cstdint>

// The lobby-full policy on the Quest client (#1001, the twin of #889). Three hooks on the patched libr15.so, each a row of
// the build-time patch table (quest/patch/libr15_patch_table.h; docs/adr/0021): `lobby_full.join`
// (CR15NetClientLobby::Join), `lobby_full.failure` (its refusal callback LobbySessionFailureCB) and `lobby_full.success`
// (its success callback LobbySessionSuccessCB). A join the server refused as lobby full is not sent again until the wait
// of quest_social::lobby_full::FullLobbyBackoff has passed, and the refusal's message is shown on the error page under
// the title "lobby full" (#1443's page, the Quest side).
//
// The addresses and the stock words are the table's, checked against the store file by the patcher and by
// libr15_patcher_pinned_test; this file holds no address. A hook is activated with one atomic store into its row's slot
// (sentinel::InstallRowThunk); a row that is not verified stays pass-through and its status is logged.
namespace quest_social::lobby_full {

// The game's lobby-session failure code for "lobby full". The name table is `FailureReason` in src/runtime/compat/lobby_frame.h
// (PC). The game's Quest refusal callback (LobbySessionFailureCB) has no case that shows text for code 8: it only
// changes the lobby state. The failure hook writes the service's message onto the error page itself (lobby_full_layout.h).
inline constexpr std::int8_t kLobbyFullCode = 8;

// Activates the three rows. Returns how many were activated (0 to 3). Each hook is independent: one that refuses leaves
// the others as they are. Call once, after the social facade is in place and Attach has verified libr15. Logs one line per
// hook.
std::size_t InstallLobbyFullHooks() noexcept;

}  // namespace quest_social::lobby_full

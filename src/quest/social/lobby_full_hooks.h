#pragma once

#include <cstddef>
#include <cstdint>

// The lobby-full policy on the Quest client (#1001, the twin of #889). Three inline hooks on the pinned libr15.so
// (sentinel/inline_hook.h, arm64 only): CR15NetClientLobby::Join, its refusal callback LobbySessionFailureCB and its
// success callback LobbySessionSuccessCB. A join the server refused as lobby full is not sent again until the wait
// of quest_social::lobby_full::FullLobbyBackoff has passed.
//
// Link-time addresses of the pinned build (libr15 build id in sentinel/pinned_targets.h). The ReVault address is the
// pinned one plus 0x100000 (memory: reference-revault-libr15-offset). Each hook's first four words are the prologue
// the install checks before it writes (sentinel::CheckInlineTarget); a mismatch refuses that hook and logs it.
namespace quest_social::lobby_full {

inline constexpr std::uint64_t kJoinVaddr = 0x1240ba8ULL;     // CR15NetClientLobby::Join(this, msg, arg)
inline constexpr std::uint64_t kFailureVaddr = 0x1240cc8ULL;  // CR15NetClientLobby::LobbySessionFailureCB(this, msg)
inline constexpr std::uint64_t kSuccessVaddr = 0x124024cULL;  // CR15NetClientLobby::LobbySessionSuccessCB(this, msg)

// The game's lobby-session failure code for "lobby full". The name table is src/runtime/compat/lobby_frame.h:201-206
// (PC); the Quest refusal callback reads the same byte and its switch has no text for it (libr15 0x1240cc8 pinned).
inline constexpr std::int8_t kLobbyFullCode = 8;

// Installs the three hooks on the loaded libr15.so. Returns how many were installed (0 to 3). Each hook is
// independent: one that refuses leaves the others as they are. Call once, after the social facade is in place
// (libr15 is loaded by then). Logs one line per hook.
std::size_t InstallLobbyFullHooks() noexcept;

}  // namespace quest_social::lobby_full

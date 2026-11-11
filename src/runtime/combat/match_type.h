// match_type.h: whether the server's current session is a private match, for the combat module's match control (#2289).
//
// VeilNet's MAP SWITCH buttons and END MATCH (F8) act only in a private match. he_is_the_cat's plugin read the match type
// from the game's own session line, `[NETLOBBY] Starting session (gametype <type>, ...`, which echovr.exe logs; the
// module does not watch echovr.exe's logger (the runtime owns it), so on PC it never saw the line. The runtime already
// stores the session's game type from the session messages the server receives (lifecycle/arena_round_time.h,
// SessionGameType), and this reads that.
//
// The rule is his: a game type with "private" in its name is a private match (echo_arena_private, echo_combat_private,
// social_2.0_private in server/game_type_table.h's kGameTypes), any other type is public, and no session yet is unknown.
// Header-only and pure, so a test drives it.
#pragma once

#include <optional>
#include <string>

namespace nevr_combat {

enum class MatchPrivacy {
  kUnknown,  // no session yet in this process
  kPublic,
  kPrivate,
};

inline MatchPrivacy PrivacyOf(const std::optional<std::string>& game_type) {
  if (!game_type) return MatchPrivacy::kUnknown;
  return game_type->find("private") != std::string::npos ? MatchPrivacy::kPrivate : MatchPrivacy::kPublic;
}

// The word a refusal or a change line gives for a privacy: "private match", "not a private match", "match type unknown".
inline const char* PrivacyText(MatchPrivacy privacy) {
  switch (privacy) {
    case MatchPrivacy::kPrivate: return "private match";
    case MatchPrivacy::kPublic: return "not a private match";
    case MatchPrivacy::kUnknown: return "match type unknown";
  }
  return "match type unknown";
}

}  // namespace nevr_combat

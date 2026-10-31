// Between the FRIENDS panel's two Quest units: the platform (friends_platform_quest.cpp, with exceptions) and the hooks
// (friends_hooks_quest.cpp, without: callback_thunk.h).
#pragma once

namespace nevr_quest_friends {

/// Pins the game functions, writes the pictures set, registers the frame work and the picture server. Returns the
/// pictures set's data root (valid for the rest of the process), or nullptr with the reason logged.
const char* PrepareFriendsPanel() noexcept;

}  // namespace nevr_quest_friends

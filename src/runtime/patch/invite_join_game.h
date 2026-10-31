#pragma once
// #1401 (D3 of #906), docs/adr/0017 ("Join"): the platform half of the join. The decisions are patch/invite_join.h; this file
// reads and writes the game object, calls the game's begin, and reads the thread and the clock.
#include <string_view>
#include <vector>

#include "runtime/patch/invite_worker.h"

namespace nevr_invite_join_game {

/// Offers the match each resolved answer names to the join, once per answer. Game-loop thread only: its one caller is
/// nevr_invite_apply::OnFrame, which the per-frame dispatcher runs on that thread (frame/tick.cpp). Every answer that is a
/// resolve with a result of Ok reaches the join; one with no usable match id is a not-started line, never a silent drop.
void OfferResolved(const std::vector<nevr_invite::Answer>& answers);

/// Offers one match to the join, as OfferResolved does for a resolved answer: the share page's JOIN offers the match the player
/// created (#1731). Game-loop thread only; the join logs its own start or refusal.
void OfferMatch(std::string_view match_id);

}  // namespace nevr_invite_join_game

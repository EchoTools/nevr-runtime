// menu_friends_profile.h: the main-menu FRIENDS panel's friend profile pop-up (#1535, prototype).
//
// The pop-up is the main menu's SUPPORT & SOCIAL pop-up (actor 0x3dc5205e08a99ca4, canvas menu_popup_panel_image_social,
// page script ui_mnu_main_page_popup_panel_image_social), which the hidden SUPPORT & SOCIAL card no longer opens. A row
// click is rewritten, before the game handles it, into that card's click (evt_discord_btn_clicked), so the game opens
// the pop-up with its own page change and its own CONTINUE close. The runtime writes the pop-up's texts: the friend's
// name, status, party and player stats (the service's player/profile RPC), INVITE on the left button and UNFRIEND on the
// right one. A left or right click is rewritten into an event no script handles, so the stock Discord and Help links
// never open, and the runtime acts on it instead. The package's own canvas (ui_mnu_friend_profile) puts the avatar top
// left with status and party under it, and an ARENA / COMBAT switch over eight stat lines on the right; the switch's two
// buttons send events no stock script handles. UNFRIEND (red) turns the stats column into a confirmation with CANCEL and
// YES, UNFRIEND; only YES removes the friend.
#pragma once

#include <cstdint>

namespace nevr_menu_friends_profile {

// The pop-up actor: events it sends go to OnPopupEvent.
inline constexpr std::uint64_t kPopupActor = 0x3DC5205E08A99CA4ULL;

// A plain click on row `row` (no select mode): fills the pop-up for that friend, starts the stats fetch and rewrites the
// click `event` (an SComponentEvent the game has not handled yet) into the SUPPORT & SOCIAL card's click. Returns false
// and changes nothing when the row holds no friend. Game-loop thread.
bool OpenFromRowClick(std::uint32_t row, std::uint64_t event);

// True for the events only the pop-up sends and no stock script knows (the ARENA / COMBAT switch): they go to
// OnPopupEvent whoever the event names as its sender (the switch's button instances name another sender).
bool IsProfileEvent(std::uint64_t symbol);

// An event the pop-up actor sent, before the game handles it. Game-loop thread. Never throws.
void OnPopupEvent(std::uint64_t event, std::uint64_t symbol) noexcept;

// Writes the pop-up texts that changed (stats arrive from a worker thread). Game-loop thread.
void OnFrame();

}  // namespace nevr_menu_friends_profile

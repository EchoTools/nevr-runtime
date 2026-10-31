// The FRIENDS panel on the Quest (#1535, #1599): the platform the shared panel logic runs on
// (src/runtime/patch/friends_panel_platform.h, ui_text_writer.h) and its install.
//
// The panel's canvases and menu tables are rebuilt as the game reads them (src/quest/sentinel/friends_panel_swap.h, in
// the read-finished hook menu_cards_hook.cpp). This adds the rest of what the PC has:
//   - the pictures set (the avatar textures, ASTC 8x8) written under <files dir>/nevr/friends_panel and added through
//     libr15's own CResourceManager::AddPackage slot, right after the first stock set the game adds;
//   - every component event the game sends, seen before the game through libr15's SendComponentEvent slot (rows,
//     INVITE FRIENDS, the profile pop-up's buttons);
//   - the texts, posted to the Quest's UI text writer (quest/invite/ui_text_quest.h), which writes them on the game
//     thread through the game's own canvas functions;
//   - the pictures, set through the game's dynamic-texture behaviour (CCanvasUICS::ElementID, FindBehavior,
//     CR15DynLoadTextureBehavior::LoadAndApplyTexture), each friend's picture encoded to ASTC and copied over the
//     slot's payload when the game reads it.
// Each game function is called at its pinned address after its first four instructions matched (friends_quest_pins.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace nevr_quest_friends {

struct Service {
  std::string httpUri;                // auth.http_uri: the community service's HTTP address
  std::function<std::string()> token;  // the Quest token, read per request; "" when there is none yet
};

/// The service the panel's requests go to (production_steps.cpp, with the token auth). Any thread.
void SetService(Service service);

/// Pins the game functions, writes the pictures set, registers the panel's frame work and installs the three libr15 slots
/// (SendComponentEvent, AddPackage, CFileTypes::GetOutputDirectory). Returns the slots installed (0 to 3); the panel acts
/// only when all are. Logs one
/// `friends_panel` line for each outcome. Outside any game frame (the sentinel's constructor).
std::size_t InstallFriendsPanel() noexcept;

}  // namespace nevr_quest_friends

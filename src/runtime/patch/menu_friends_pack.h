// menu_friends_pack.h: the main-menu FRIENDS panel (#1535): its install, its rows and its clicks.
//
// The panel is the main menu's SEASON INFO card, its friend profile the SUPPORT & SOCIAL pop-up; both are rebuilt in
// memory from the canvases and level tables the game delivers (friends_panel_resources.h). The only resources of the
// panel's own are its avatar textures, which the runtime writes as a package set under
// <nEVR root>\content\friends_panel and adds with the engine's own CResourceManager::AddPackage
// (docs/engine/asset-system.md, "Startup"), after the two stock sets and before any resource type is installed or any
// archive loads.
//
// The logic is shared with the Quest (#1599): what differs between echovr.exe and libr15 is friends_panel_platform.h.
// Install is the PC's (friends_panel_pc.cpp); the Quest installs through src/quest/friends.
#pragma once

#include <cstdint>

namespace nevr_menu_friends_pack {

// Writes the panel's pictures set and registers the transforms, then adds the set now when the resource manager exists
// (InitGlobals has run), else detours AddPackage and adds it right after the boot's second stock call returns. Logs one
// [NEVR.MENU] line for each decision. Client only; after InstallResourceOverride.
void Install(std::uintptr_t gameBase);

// The platform's install says the panel's pictures set is added (PC: friends_panel_pc.cpp; Quest: the AddPackage slot,
// src/quest/friends/friends_hooks_quest.cpp). Nothing of the panel acts before it: without the set the panel's
// pictures cannot load. Any thread.
void MarkPackageAdded();
bool PackageAdded();

// Fills the panel's rows (friend<i>_name, friend<i>_status, friend<i>_mark on the panel actor) and the INVITE FRIENDS
// label from the friends roster the bridge keeps (nevr_social_roster::Global()), through the UI text writer. Checks once
// a second, and at once after a click changed something; posts only the texts that changed. Game-loop thread only
// (frame/tick.cpp).
void OnFrame();

// Every component event the game sends passes here first (the SendComponentEvent hook, invite_choice_hook.cpp). Events
// from the panel actor are logged once per distinct (symbol, component). A row's click opens that friend's profile (the
// event is rewritten into the SUPPORT & SOCIAL card's click, menu_friends_profile.h); INVITE FRIENDS turns select mode
// on, and in it a row click ticks or unticks that friend and the button sends one party invite to each ticked friend.
// Events from the profile pop-up go to menu_friends_profile.h. `space` is the game space, `event` the SComponentEvent.
// Never throws.
void OnComponentEvent(std::uint64_t space, std::uint64_t event) noexcept;

}  // namespace nevr_menu_friends_pack

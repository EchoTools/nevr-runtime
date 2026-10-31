// menu_friends_avatars.h: the main-menu FRIENDS panel's avatars, each friend's Discord picture (#1535, prototype).
//
// The package set carries two 128x128 BGRA avatar textures per row (nevr_friend_avatar_<row>_<a|b>) and puts the
// engine's dynamic-texture behaviour (CR15DynLoadTextureBehavior) on each row's avatar element. The runtime:
//   - serves each avatar texture's payload (buffer 1, a DDS) from a resource transform: the picture the slot holds now;
//   - captures each avatar element's behaviour from the behaviour's own SetTexture (echovr.exe 0x140c42a20), which the
//     game calls with the default texture when it initializes the element;
//   - fetches the player's friends from the API (GET <nevr_http_uri>/v2/friend, the player's token), matches them to the
//     roster rows by name, downloads each avatar_url (Discord) as a 128 px PNG, decodes it and writes a DDS;
//   - on the game thread, gives a row's next slot the picture and calls SetTexture with that slot's name, so the engine
//     loads it through the transform and applies it.
#pragma once

#include <cstdint>

namespace nevr_menu_friends_avatars {

// Registers the slot transforms and hooks SetTexture. Logs one [NEVR.MENU] line for each decision. Client only.
void Install(std::uintptr_t gameBase);

// The friend profile pop-up shows `rosterId`'s picture (its profile_avatar element, the same picture as the row's) once
// it has arrived. Game-loop thread.
void ShowProfile(std::uint64_t rosterId);

// Starts a fetch when the roster changed (at most once a minute) and applies pictures that arrived. Game-loop thread.
void OnFrame();

}  // namespace nevr_menu_friends_avatars

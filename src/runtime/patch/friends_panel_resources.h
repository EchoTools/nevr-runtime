#pragma once

// #1535: what the FRIENDS panel changes in the game's resources on PC, all made in memory or by the runtime itself.
//
//   - The pictures set (friends_package.h): the avatar textures, written under <nEVR root>\content\friends_panel when
//     its bytes differ from what is there; menu_friends_pack.cpp adds it with the engine's AddPackage.
//   - Resource transforms (resource_override.h) that answer every load of: ui_mnu_season_info (the panel) and
//     menu_popup_panel_image_social (the friend profile), built by friends_canvas.h from the delivered canvas; and
//     mnu_master's pointer meshes, transforms, actor data, page elements and scripts, edited by friends_level.h.
//     Each builds from the bytes the game delivered (cached by their digest) and declines, leaving the stock bytes,
//     when they are not what the edit expects. One [NEVR.MENU] line per resource says which.

#include <string>

namespace nevr_friends_panel_resources {

// Writes the pictures set under `data_root` (<data_root>\rad15\win10\manifests\<set>, packages\<set>_0). True when the
// files hold the set afterwards. Logs one line.
bool WritePicturesSet(const std::string& data_root);

// Registers the seven transforms. Returns how many are in the registry. Logs one line.
int RegisterTransforms();

}  // namespace nevr_friends_panel_resources

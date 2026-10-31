/* #1535 / #1599: the Quest twin of the PC FRIENDS panel's resource changes (src/runtime/patch/friends_panel_resources.h),
 * the gate and the builder.
 *
 * The callback block (menu_cards_swap.h) reports every finished primary buffer the streaming reader delivers. Seven of
 * them are the panel's: the canvases ui_mnu_season_info (the panel) and menu_popup_panel_image_social (the friend
 * profile), class CUICanvasResource; and the main menu's (mnu_master_lowspec) pointer meshes, transforms, actor data,
 * page elements and scripts. The Quest's canvases are byte for byte the PC's and its tables hold the same actors and
 * values (the headset's data, read 2026-10), so the PC builders (friends_canvas.h) and edits (friends_level.h) serve
 * both: only the level's name differs. Each refuses bytes that are not what it expects, and the stock buffer then stays.
 *
 * The data is the headset's own, rewritten in memory; nothing derived from Ready At Dawn's data is stored or shipped.
 * This header is pure: no game call, no allocation through the game, no exceptions.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "menu_cards_swap.h"
#include "nevr/symbol_hash.h"
#include "runtime/patch/friends_canvas.h"
#include "runtime/patch/friends_level.h"
#include "runtime/patch/ui_canvas.h"

namespace nevr_quest::friends_panel {

enum class Kind { kNone, kPanel, kProfile, kPointerMeshes, kTransforms, kActorData, kPageElements, kScripts };

inline constexpr std::uint64_t kCanvasType = EchoVR::CSymbol64Hash("CUICanvasResource");

inline const char* KindName(Kind k) {
  switch (k) {
    case Kind::kPanel: return "panel_canvas";
    case Kind::kProfile: return "profile_canvas";
    case Kind::kPointerMeshes: return "pointer_meshes";
    case Kind::kTransforms: return "transforms";
    case Kind::kActorData: return "actor_data";
    case Kind::kPageElements: return "page_elements";
    case Kind::kScripts: return "scripts";
    case Kind::kNone: break;
  }
  return "none";
}

// Which of the panel's resources `d` is, by class and name; kNone for every other delivery.
inline Kind Classify(const menu_cards::Delivery& d) {
  namespace fc = nevr_friends_canvas;
  namespace fl = nevr_friends_level;
  if (d.classSymbol == kCanvasType) {
    if (d.nameSymbol == fc::kSeasonCanvas) return Kind::kPanel;
    if (d.nameSymbol == fc::kPopupCanvas) return Kind::kProfile;
    return Kind::kNone;
  }
  if (d.nameSymbol != fl::kQuestLevel) return Kind::kNone;
  if (d.classSymbol == fl::kPointerMeshType) return Kind::kPointerMeshes;
  if (d.classSymbol == fl::kTransformType) return Kind::kTransforms;
  if (d.classSymbol == fl::kActorDataType) return Kind::kActorData;
  if (d.classSymbol == fl::kPageElementType) return Kind::kPageElements;
  if (d.classSymbol == fl::kScriptType) return Kind::kScripts;
  return Kind::kNone;
}

// The bytes the panel serves for `kind`, built from the delivered `data`. False, with `why`, when the delivered bytes are
// not what the build expects (the stock buffer then stays).
inline bool Build(Kind kind, const unsigned char* data, std::size_t size, std::vector<std::uint8_t>* out, std::string* why) {
  namespace fc = nevr_friends_canvas;
  namespace fl = nevr_friends_level;
  namespace uc = nevr_ui_canvas;
  const fl::Bytes in(data, data + size);
  switch (kind) {
    case Kind::kPanel:
    case Kind::kProfile: {
      uc::Canvas stock, built;
      if (!uc::Decode(data, size, &stock, why)) return false;
      const bool ok = kind == Kind::kPanel ? fc::BuildFriendsPanel(stock, fc::StandardPictures(), &built, why)
                                           : fc::BuildFriendProfile(stock, fc::StandardPictures(), &built, why);
      if (!ok) return false;
      *out = uc::Encode(built);
      return true;
    }
    case Kind::kPointerMeshes: return fl::EditPointerMeshes(in, out, why);
    case Kind::kTransforms: return fl::EditTransforms(in, out, why);
    case Kind::kActorData: return fl::EditActorData(in, out, why);
    case Kind::kPageElements: return fl::EditPageElements(in, out, why);
    case Kind::kScripts: return fl::EditScripts(in, out, why);
    case Kind::kNone: break;
  }
  if (why != nullptr) *why = "not a resource of the panel";
  return false;
}

}  // namespace nevr_quest::friends_panel

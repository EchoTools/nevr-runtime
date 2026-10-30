/* #1478: the Quest twin of the PC net-gun bullet rewrite (#1420, src/runtime/patch/netgun_bullets.h), the gate half.
 *
 * The callback block (menu_cards_swap.h) reports every finished primary buffer the streaming reader delivers. A delivery is
 * a net-gun resource when its class is CParticleEffectCR or CR15NetBulletCR and its name is one of the five combat levels
 * that carry the net guns (kLevels). That decides only whether the rewrite is offered: the bytes must still be the pinned
 * stock digests, which nevr_netgun_bullets::Decide checks before anything is written (menu_cards_hook.cpp, OfferNetgun).
 *
 * The data is the headset's own, rewritten in memory; nothing derived from Ready At Dawn's data is stored or shipped.
 * This header is pure: no game call, no allocation.
 */
#pragma once

#include "menu_cards_swap.h"
#include "runtime/patch/netgun_bullets.h"

namespace nevr_quest::netgun_bullets {

/// True when `d` is one of the two net-gun resources of one of the five combat levels; `*res` names which. The class
/// symbols are the same logical types the PC rewrite keys on (netgun_bullets.h kParticleType, kBulletType), read at the
/// same resource offsets menu_cards_swap.h names.
inline bool IsNetgunDelivery(const menu_cards::Delivery& d, nevr_netgun_bullets::Resource* res) {
  if (d.classSymbol == nevr_netgun_bullets::kParticleType) {
    *res = nevr_netgun_bullets::Resource::Particles;
  } else if (d.classSymbol == nevr_netgun_bullets::kBulletType) {
    *res = nevr_netgun_bullets::Resource::Bullets;
  } else {
    return false;
  }
  for (const nevr_netgun_bullets::Level& level : nevr_netgun_bullets::kLevels) {
    if (level.hash == d.nameSymbol) return true;
  }
  return false;
}

}  // namespace nevr_quest::netgun_bullets

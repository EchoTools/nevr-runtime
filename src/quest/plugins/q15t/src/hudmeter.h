// hudmeter (row R57): the wrist HUD's weapon-slot heat fill stops being 0 x 0. Port of the source build's hudmeter
// (RE-34 FX-21, "Guard (rev 2)" with rev 3's "any canvas component"; the rev-4 old-bar part is not built, see
// docs/quest/q15t/modules/hudmeter.md). A GOT wrapper on CSetCanvasElementAttributesNode::Enter's JUMP_SLOT (ELF 0x36c6ce8): a size write of
// exactly 0 x 0 to the item weapon_meter becomes a write of the authored full size 86 x 88 for that one call.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

// `base`: the game library's load base. Logs one structured line; a refusal is an ERROR and leaves the slot alone.
InstallResult InstallHudmeter(std::uintptr_t base);

struct HudmeterStats {
  std::uint32_t calls_guarded;  // size writes to weapon_meter seen (any value)
  std::uint32_t full_writes;    // 0 x 0 writes made 86 x 88
  std::uint32_t unreadable;     // data arguments that could not be read (passed on untouched)
};
HudmeterStats GetHudmeterStats();
void ResetHudmeterForTest();

}  // namespace q15t

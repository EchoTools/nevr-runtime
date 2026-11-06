#include "quest/combat/voipdrop.h"

namespace nevr_quest::combat::voipdrop {

ModuleResult Install(bool on) noexcept { return {false, on ? "stub" : "off_config", !on}; }
void NoteEnd() noexcept {}

}  // namespace nevr_quest::combat::voipdrop

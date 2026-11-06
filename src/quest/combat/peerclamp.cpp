#include "quest/combat/peerclamp.h"

namespace nevr_quest::combat::peerclamp {

ModuleResult Install(bool on) noexcept { return {false, on ? "stub" : "off_config", !on}; }
void NoteEnd() noexcept {}

}  // namespace nevr_quest::combat::peerclamp

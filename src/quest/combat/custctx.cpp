#include "quest/combat/custctx.h"

namespace nevr_quest::combat::custctx {

ModuleResult Install(bool on) noexcept { return {false, on ? "stub" : "off_config", !on}; }
void NoteEnd() noexcept {}

}  // namespace nevr_quest::combat::custctx

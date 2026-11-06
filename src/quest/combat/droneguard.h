/* STUB (#2178): replaced by the real droneguard module in this unit. */
#pragma once

#include "quest/combat/combat_install.h"

namespace nevr_quest::combat::droneguard {

ModuleResult Install(bool on, bool stage1) noexcept;
void NoteEnd() noexcept;

}  // namespace nevr_quest::combat::droneguard

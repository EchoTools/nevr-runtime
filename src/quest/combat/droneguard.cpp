#include "quest/combat/droneguard.h"

namespace nevr_quest::combat::droneguard {

ModuleResult Install(bool on, bool stage1) noexcept {
  (void)stage1;
  return {false, on ? "stub" : "off_config", !on};
}
void NoteEnd() noexcept {}

}  // namespace nevr_quest::combat::droneguard

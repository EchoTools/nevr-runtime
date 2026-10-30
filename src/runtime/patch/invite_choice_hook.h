#pragma once

#include <cstdint>

#include "runtime/patch/invite_choices.h"

// #906, docs/adr/0017 "Runtime hook": the observer on the game's component events. Every card and button the CREATE
// GAME / JOIN VIA CODE wizard's pages send passes through CncaGameSpace::SendComponentEvent; the observer reads the
// wizard's events out of it (patch/invite_choices.h) and keeps what the cards chose, for the click-metric observer
// (invite_wizard_hook.cpp) to plan its request from. It changes nothing and forwards every event.
namespace nevr_invite_choice_hook {

/// Installs the observer on SendComponentEvent. Client only; the caller decides. Returns true when it is installed. A
/// prologue mismatch leaves the game untouched, logs a Warning with the name, the address and the expected and actual
/// bytes, leaves the choices empty (a create click then plans nothing) and returns false; boot then keeps the invite
/// service off.
bool Install(std::uintptr_t gameBase);

/// What the cards have chosen so far.
nevr_invite_choices::State& Choices();

}  // namespace nevr_invite_choice_hook

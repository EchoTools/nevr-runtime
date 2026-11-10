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

/// An observer of every SComponentEvent the game sends through SendComponentEvent (echovr.exe 0x14050fb60): the game space
/// and the event's words (word 0 the event symbol, 2 and 3 its link and parameter), before the game handles it; it changes
/// nothing. The combat module is the one user (runtime/combat): one owner per function, so it rides this detour instead of
/// detouring the function a second time. Installs the detour alone when Install has not (a server, where Install is not
/// called), without the wizard observer. Returns false when the detour could not be installed (the observer is then not set).
/// A null observer removes it and installs nothing.
using EventObserver = void (*)(std::uint64_t space, const std::uint64_t* event);
bool InstallEventObserver(std::uintptr_t gameBase, EventObserver observer);

/// What the cards have chosen so far.
nevr_invite_choices::State& Choices();

}  // namespace nevr_invite_choice_hook

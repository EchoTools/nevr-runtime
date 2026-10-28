#pragma once

#include <cstdint>

// #906, docs/adr/0017 "Order of work" step 4 (first part): the observer on the game's remote-log submit. It reads the
// CREATE GAME / JOIN VIA CODE wizard's click metric, plans the request the click would make and logs both. It sends
// nothing: the worker that posts the request waits for the service RPCs (nakama, ADR 0017 step 2).
namespace nevr_invite_hook {

/// The name of a level symbol in the generated symbol cache (the game's PC and Quest names), nullptr when it has none. The
/// plan names a map card's level through it (nevr_invite::LevelNamer).
const char* LevelName(std::uint64_t symbol);

/// Installs the observer on the remote-log submit. Client only; the caller decides. A prologue mismatch leaves the
/// game untouched and logs why.
void Install(std::uintptr_t gameBase);

}  // namespace nevr_invite_hook

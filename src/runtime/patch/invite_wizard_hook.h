#pragma once

#include <cstdint>

// #906, docs/adr/0017 "Order of work" step 4 (first part): the observer on the game's remote-log submit. It reads the
// CREATE GAME / JOIN VIA CODE wizard's click metric, plans the request the click would make and logs both. It sends
// nothing: the worker that posts the request waits for the service RPCs (nakama, ADR 0017 step 2).
namespace nevr_invite_hook {

/// Installs the observer on the remote-log submit. Client only; the caller decides. A prologue mismatch leaves the
/// game untouched and logs why.
void Install(std::uintptr_t gameBase);

}  // namespace nevr_invite_hook

#pragma once

// What the self-update swap (update_swap.cpp) asks of its test seams, in every build. Production answers are the
// constants: the lock wait of ADR 0024 section 4.1, no step ends a start, no move fails. The seams themselves
// (SetUpdate*ForTest, update_swap.h) exist only under NEVR_HOST_TESTING and live in update_seams.cpp, so
// update_swap.cpp compiles to the same object with and without the define (tools/verify_host_seams.py).

#include <windows.h>

namespace nevr_host {

/// How long a start waits for update\lock, and how often it tries.
DWORD SwapLockTimeoutMs() noexcept;
DWORD SwapLockStepMs() noexcept;

/// True when the start must end after its `step`-th change to the disk, as a kill would end it.
bool SwapStepEnds(int step) noexcept;

/// True, with `*error` set, when the `move`-th file move of a start must fail as a move to another volume does.
bool SwapMoveFails(int move, DWORD* error) noexcept;

}  // namespace nevr_host

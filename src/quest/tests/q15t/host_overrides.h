// Force-included into the plugin sources by src/quest/tests/q15t/run_host_tests.sh: the host test's fake game function stands in for
// CncaGame::ExclusiveUpdateGame, so its "pinned vaddr" is its address minus the fake base.
#pragma once
#include <cstdint>
extern "C" void FakeExclusiveUpdateGame(void*);
#define Q15T_SLOT_EXCLUSIVEUPDATEGAME 0x20
#define Q15T_REAL_EXCLUSIVEUPDATEGAME (reinterpret_cast<std::uintptr_t>(&FakeExclusiveUpdateGame) - ctx->base_addr)
#define Q15T_GAME_VPTR 0x100

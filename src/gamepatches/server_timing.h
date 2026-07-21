/* SYNTHESIS -- custom tool code, not from binary */
#pragma once
#include <cstdint>
namespace ServerTiming {
    // Init removed (N26) — dead code, zero call sites.
    // Only Shutdown is wired (dllmain.cpp:109).
    void OnFrame();
    void OnGameStateChange(uint32_t old_state, uint32_t new_state);
    void Shutdown();
}

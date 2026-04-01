/* SYNTHESIS -- custom tool code, not from binary */
#pragma once
#include <cstdint>
namespace BuiltinServerTiming {
    void Init(uintptr_t base_addr, bool is_server);
    void OnFrame();
    void OnGameStateChange(uint32_t old_state, uint32_t new_state);
    void Shutdown();
}

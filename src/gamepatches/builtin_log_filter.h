/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

namespace BuiltinLogFilter {
    void Init(uintptr_t base_addr, bool is_server);
    void Shutdown();
}

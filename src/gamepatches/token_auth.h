#pragma once

#include <cstdint>

namespace TokenAuth {
void Init(uintptr_t base_addr, bool is_server);
void Shutdown();
}

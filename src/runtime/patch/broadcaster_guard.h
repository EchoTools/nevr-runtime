/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

namespace BroadcasterGuard {

void Install(uintptr_t base_addr);
void Shutdown();

}

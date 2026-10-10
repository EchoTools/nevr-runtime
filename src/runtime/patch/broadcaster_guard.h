/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>

namespace nevr_broadcaster_guard {

void Install(uintptr_t base_addr);
void Shutdown();

}

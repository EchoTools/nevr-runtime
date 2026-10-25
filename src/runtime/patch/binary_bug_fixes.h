/* SYNTHESIS -- custom tool code, not from binary */
#pragma once
#include <cstdint>
namespace nevr_binary_bug_fixes {
    template <typename Write>
    bool WriteBusyWaitByte(void* address, const std::uint8_t* value, bool& patched, bool installed, Write&& write) {
        if (!write(address, value, 1)) return false;
        patched = installed;
        return true;
    }

    void Init(uintptr_t base_addr);
    void Shutdown();
}

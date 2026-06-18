/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
#define NEVR_EXPORT __declspec(dllexport)
#define NEVR_IMPORT __declspec(dllimport)
#else
#define NEVR_EXPORT __attribute__((visibility("default")))
#define NEVR_IMPORT
#endif

/* SYNTHESIS marker: marks code that is custom tooling, not from the original binary */
#define NEVR_SYNTHESIS /* custom tool code, not from binary */

namespace nevr {

static constexpr uint64_t ECHOVR_DEFAULT_IMAGE_BASE = 0x140000000ULL;

/*
 * ResolveVA - Convert a virtual address from the PC binary to an in-process pointer.
 *
 * The PC binary has an image base of 0x140000000. Given the actual base address
 * where the module is loaded, this computes the real pointer for a known VA.
 *
 * Returns nullptr if the VA is below the image base (underflow) or the resolved
 * address falls outside the loaded image (detects MinHook trampolines, unmapped
 * sections, and arithmetic bugs).
 */
inline void* ResolveVA(uintptr_t base, uint64_t va) {
    if (va < ECHOVR_DEFAULT_IMAGE_BASE) return nullptr;
    uint64_t rva = va - ECHOVR_DEFAULT_IMAGE_BASE;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi;
    void* addr = reinterpret_cast<void*>(base + rva);
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) return nullptr;
    if (mbi.State != MEM_COMMIT) return nullptr;
    return addr;
#else
    return reinterpret_cast<void*>(base + rva);
#endif
}

/* Unchecked variant for hot paths where the address is known-good (e.g. hook
   targets verified at init time). Prefer ResolveVA for new code. */
inline void* ResolveVA_Unchecked(uintptr_t base, uint64_t va) {
    return reinterpret_cast<void*>(base + (va - ECHOVR_DEFAULT_IMAGE_BASE));
}

/*
 * ValidatePrologue - Check that the bytes at a resolved address match an expected
 * function prologue. Returns true if the first `len` bytes match.
 */
inline bool ValidatePrologue(void* addr, const uint8_t* expected, size_t len) {
    if (!addr || !expected || len == 0) return false;
    const auto* bytes = static_cast<const uint8_t*>(addr);
    for (size_t i = 0; i < len; ++i) {
        if (bytes[i] != expected[i]) return false;
    }
    return true;
}

/*
 * LoadConfigFile - Read an entire file into a string. Intended for loading
 * JSON/JSONC config files. Searches the given path, then parent directories
 * (../ and ../../) to support nested directory layouts. Returns empty on failure.
 */
inline std::string LoadConfigFile(const std::string& path) {
    const std::string prefixes[] = {"", "..\\", "..\\..\\", "../", "../../"};
    for (const auto& prefix : prefixes) {
        std::ifstream file(prefix + path, std::ios::in | std::ios::binary);
        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }
    }
    return {};
}

} // namespace nevr

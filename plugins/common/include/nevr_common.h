/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <fstream>
#include <sstream>

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

/*
 * ResolveVA - Convert a virtual address from the PC binary to an in-process pointer.
 *
 * The PC binary has an image base of 0x140000000. Given the actual base address
 * where the module is loaded, this computes the real pointer for a known VA.
 */
inline void* ResolveVA(uintptr_t base, uint64_t va) {
    return reinterpret_cast<void*>(base + (va - 0x140000000));
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
 * JSON/JSONC config files. Returns an empty string on failure.
 */
inline std::string LoadConfigFile(const std::string& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file.is_open()) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

} // namespace nevr

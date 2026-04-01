/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

/*
 * Filesystem Loader — Intercept Resource_InitFromBuffers to serve modified
 * resource files from disk instead of from compressed archive packages.
 *
 * Hook target: Resource_InitFromBuffers @ 0x140fa2510
 *   void __fastcall (CResource* resource, void* buf1, uint64_t size1,
 *                    void* buf2, uint64_t size2)
 *
 * CResource layout (from echovr-reconstruction CResource.h):
 *   +0x28: CResourceID type_symbol (8 bytes, wraps CSymbol64)
 *   +0x38: CResourceID name_symbol (8 bytes, wraps CSymbol64)
 *   +0x40: void* data_buffer_1
 *   +0x48: void* data_buffer_2
 *   +0x50: uint64_t data_size_1
 *   +0x58: uint64_t data_size_2
 *   +0x60: uint32_t load_state (4 = loaded)
 *
 * Override files can be named either:
 *   - By hex hash: "0x1234abcd5678ef90" (matches name_symbol directly)
 *   - By human-readable name: "mpl_arena_a" (hashed via CSymbol64_Hash)
 *
 * Source: echovr-reconstruction src/NRadEngine/Core/CResource.h
 * Confidence: M (address verified, reconstruction quality M)
 */

namespace nevr::filesystem_loader {

struct OverrideEntry {
    std::string file_path;    /* absolute path to the override file on disk */
    std::string display_name; /* original filename for logging */
    uint64_t type_hash = 0;   /* type_symbol filter (0 = match any type) */
};

struct LoaderConfig {
    std::string override_dir = "_overrides/";
    bool valid = false;
};

LoaderConfig ParseConfig(const std::string& json_text);

/*
 * Scan the override directory and build the hash-to-file map.
 * Returns the number of override files found.
 */
int ScanOverrideDirectory(const std::string& dir_path);

/*
 * Install the MinHook on Resource_InitFromBuffers.
 * Returns true on success.
 */
bool InstallHook(uintptr_t base_addr);

/*
 * Remove the hook and free override data.
 */
void RemoveHook();

} // namespace nevr::filesystem_loader

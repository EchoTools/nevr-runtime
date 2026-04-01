#pragma once
#include <cstdint>

// Convert a PE executable to a DLL by flipping characteristics.
// Saves the original AddressOfEntryPoint to *out_entry_rva.
// Patches entry point to a stub DllMain (returns TRUE).
// Returns true on success.
bool ConvertExeToDll(const char* exe_path, const char* dll_path, uint32_t* out_entry_rva);

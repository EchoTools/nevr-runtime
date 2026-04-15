#pragma once

#include <cstdint>

/// Install the resource override hook on AsyncResourceIOCallback.
/// Registers built-in overrides (splash texture) and scans _overrides/
/// directory for additional file-based overrides.
void InstallResourceOverride();

/// Register an embedded data override (data pointer must outlive the hook).
void RegisterResourceOverride(uint64_t type_hash, uint64_t name_hash,
                              const void* data, uint64_t size,
                              const char* label);

/// Register a file-based override (loaded from disk, memory owned by the registry).
void RegisterResourceOverrideFromFile(uint64_t type_hash, uint64_t name_hash,
                                      const char* file_path,
                                      const char* label);

/// Reset all applied flags so overrides can re-trigger on next level load.
void ResetResourceOverrides();

/// Deallocate the resource override registry and uninstall hooks.
void ShutdownResourceOverride();

/// Remove all overrides whose data pointer falls within [data_start, data_end).
/// Call from plugin shutdown before the plugin DLL is freed to prevent
/// dangling pointers in the override registry.
void DeregisterResourceOverrides(const void* data_start, const void* data_end);

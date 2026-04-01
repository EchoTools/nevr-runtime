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

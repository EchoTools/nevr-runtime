#pragma once

#include <cstdint>

#ifdef NEVR_TEST_HOOKS
void TestHook_InitResourceOverrides(void (*original)(void*));
void TestHook_SetOriginal(void (*original)(void*));
void TestHook_SetDeregisterWaitMs(unsigned ms);
unsigned long long TestHook_DeregisterTimeouts();
void TestHook_RunResourceCallback(void* callback_data);
void TestHook_CloseResourceOverrides();
#endif

/// Where the fail-closed path (a callback deregistering its own replacement) leaves its one line before it
/// terminates the process. The sink must not allocate or take locks. Optional: stderr is the fallback.
void SetResourceOverrideFatalLineSink(void (*sink)(const char* line));

/// What ShutdownResourceOverride found, returned so the figures are testable as well as logged.
struct ResourceOverrideShutdownReport {
    uint64_t registered;
    uint64_t never_applied;
    bool hook_installed;
};

/// Reports the registry at dynamic unload: one warning per override the game never requested, and a summary
/// line. When the hook is live the module is pinned (it cannot be unloaded), so the registry is kept; when it
/// never installed, owned buffers are freed and the registry released.
ResourceOverrideShutdownReport ShutdownResourceOverride();

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

/// Remove all overrides whose data pointer falls within [data_start, data_end).
/// Call from plugin shutdown before the plugin DLL is freed to prevent
/// dangling pointers in the override registry. Do not call this from the
/// original resource callback while it is using the same replacement.
void DeregisterResourceOverrides(const void* data_start, const void* data_end);

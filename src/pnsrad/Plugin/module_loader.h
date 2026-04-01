#ifndef PNSRAD_PLUGIN_MODULE_LOADER_H
#define PNSRAD_PLUGIN_MODULE_LOADER_H

/* @module: pnsrad.dll */
/* @purpose: Module/DLL symbol resolution for the plugin loading subsystem.
 *   Wraps Win32 GetProcAddress / GetModuleFileNameW and manages the
 *   config-driven search loop that resolves the host module path at
 *   plugin init time.
 */

#include <cstdint>

#ifndef _WIN32
#include <cstddef>
typedef void* HMODULE;
typedef int (*FARPROC)();
#else
#include <windows.h>
#endif

namespace NRadEngine {

// @0x1800953d0 — GetProcAddress wrapper
// Returns null if either argument is null or the symbol is not found.
// On failure, zeros an 0x80-byte scratch buffer and calls get_last_error().
/* @addr: 0x1800953d0 (pnsrad.dll) */ /* @confidence: H */
FARPROC ResolveSymbol(HMODULE hModule, const char* symbol_name);

// @0x180095420 — Resolve the directory containing the given module.
// Calls GetModuleFileNameW + _wsplitpath_s, converts to multibyte,
// and truncates the result at the last backslash so that `out_dir`
// contains only the directory portion (null-terminated, max 0x200 bytes).
/* @addr: 0x180095420 (pnsrad.dll) */ /* @confidence: H */
void GetModuleDirectory(HMODULE hModule, char* out_dir);

// @0x180095510 — Module symbol setup / config search loop.
// Copies the module directory into a working buffer, then enters
// a loop that walks the filesystem upward, checking for the
// presence of known config sentinel files (e.g. ".radconfig",
// ".dev", ".release", ".staging", ".production", ".test").
// On finding a match it copies the resolved path into the global
// g_module_root_path buffer (DAT_180380d10, 0x200 bytes).
// If no sentinel is found, falls back to an environment-defined default.
/* @addr: 0x180095510 (pnsrad.dll) */ /* @confidence: H */
void SetupModuleSymbols(const char* module_dir);

// @0x180095c20 — Acquire scoped module operation lock.
// Calls the internal environment accessor (fcn.18008a740).  If the
// environment handle is valid, clears a 32-bit result flag to zero
// (success).  Returns `this` for RAII chaining.
/* @addr: 0x180095c20 (pnsrad.dll) */ /* @confidence: H */
void* ScopedModuleLockAcquire(void* result_out);

// @0x180095cb0 — Release scoped module operation lock.
// Calls the same internal environment accessor to release.
/* @addr: 0x180095cb0 (pnsrad.dll) */ /* @confidence: H */
void ScopedModuleLockRelease();

// @0x180091df0 — Init memory statics (resolves RadPluginSetAllocator)
// Called by RadPluginInitMemoryStatics. Resolves the host's
// RadPluginSetAllocator symbol from the loaded module handle and
// invokes it with the global allocator pointer.
/* @addr: 0x180091df0 (pnsrad.dll) */ /* @confidence: H */
void InitMemoryStatics(HMODULE hModule);

// @0x180091ea0 — Init non-memory statics
// Called by RadPluginInitNonMemoryStatics.  Resolves all remaining
// RadPluginSet* functions (PresenceFactory, FileTypes, Environment,
// EnvironmentMethods, SymbolDebugMethodsMethod, SystemInfo) from
// the host module and invokes each with the corresponding global.
/* @addr: 0x180091ea0 (pnsrad.dll) */ /* @confidence: H */
void InitNonMemoryStatics(HMODULE hModule);

// @0x180092310 — Module initialization (plugin loader entry).
// Called by RadPluginInit. Sets up TLS, validates allocator,
// resolves the module directory path, and runs SetupModuleSymbols.
/* @addr: 0x180092310 (pnsrad.dll) */ /* @confidence: H */
void ModuleInit(HMODULE hModule, uint64_t param_1, uint64_t param_2, uint64_t param_3);

} // namespace NRadEngine

#endif // PNSRAD_PLUGIN_MODULE_LOADER_H

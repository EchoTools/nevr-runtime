/*
 * nevr_module_interface.h — Interface for nevr-runtime loadable modules.
 *
 * Modules are DLLs loaded by the boot sequence before plugins. They run
 * inside the loader's address space and have access to the game's base
 * address and function pointers. Each module DLL exports NvrModuleInit()
 * and optionally NvrModuleShutdown().
 *
 * Unlike plugins (which load after game init), modules load during
 * PreprocessCommandLineHook — before the game's own initialization.
 * This allows them to install hooks that must be active before the
 * game makes its first network call or loads config.
 *
 * Each module DLL statically links MinHook. MinHook statics are per-DLL,
 * so each module MUST call Hooking::Initialize() before any hook calls.
 */

#pragma once

#include <cstdint>

#ifdef _WIN32
  #ifdef NEVR_MODULE_EXPORTS
    #define NEVR_MODULE_API extern "C" __declspec(dllexport)
  #else
    #define NEVR_MODULE_API extern "C" __declspec(dllimport)
  #endif
#else
  #define NEVR_MODULE_API extern "C" __attribute__((visibility("default")))
#endif

/* Host flags (same values as NvrHostFlags for plugins) */
#define NEVR_HOST_IS_SERVER   0x02
#define NEVR_HOST_IS_CLIENT   0x04
#define NEVR_HOST_IS_HEADLESS 0x10

/* Context passed to every module at init time */
struct NvrModuleContext {
    uintptr_t base_addr;      /* echovr.exe ImageBase */
    void*     early_config;   /* g_earlyConfigPtr (game JSON), may be NULL */
    uint32_t  flags;          /* NEVR_HOST_IS_* flags */

    /* Log callback — modules use this instead of importing Log directly.
     * Signature matches: void log(int level, const char* fmt, ...) */
    void (*log)(int level, const char* fmt, ...);

    /* Cross-module proc resolution — returns NULL if not registered */
    void* (*get_proc)(const char* name);
};

/* Required export: called once after LoadLibrary. Return 0 on success, non-zero on failure.
 * On failure, the loader calls FatalError and the game does not start. */
typedef int (*NvrModuleInit_fn)(const NvrModuleContext* ctx);

/* Optional export: called during shutdown for cleanup */
typedef void (*NvrModuleShutdown_fn)(void);

/* Optional export: called each game tick */
typedef void (*NvrModuleOnFrame_fn)(const NvrModuleContext* ctx);

/* Optional export: called on game state transitions */
typedef void (*NvrModuleOnGameStateChange_fn)(const NvrModuleContext* ctx, uint32_t old_state, uint32_t new_state);

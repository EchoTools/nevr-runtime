/*
 * nevr_plugin_interface.h - Plugin interface for nEVR game patches
 *
 * This header defines the interface that gamepatches and legacy
 * use to load and manage plugins. Plugins export a set of C functions
 * that the host calls at defined lifecycle points.
 *
 * Drop this header into:
 *   nevr-runtime/src/gamepatches/nevr_plugin_interface.h
 *   nevr-runtime/src/legacy/nevr_plugin_interface.h
 */

#pragma once

#include <cstdint>

#ifdef _WIN32
  #ifdef NEVR_PLUGIN_EXPORTS
    #define NEVR_PLUGIN_API extern "C" __declspec(dllexport)
  #else
    #define NEVR_PLUGIN_API extern "C" __declspec(dllimport)
  #endif
#else
  #define NEVR_PLUGIN_API extern "C" __attribute__((visibility("default")))
#endif

/* Plugin info returned by NvrPluginGetInfo */
struct NvrPluginInfo {
    const char* name;           /* short identifier, e.g. "disabled_weapons" */
    const char* description;    /* human-readable description */
    uint32_t    version_major;
    uint32_t    version_minor;
    uint32_t    version_patch;
};

/* Game context passed to the plugin by the host */
struct NvrGameContext {
    uintptr_t   base_addr;      /* echovr.exe base address (ImageBase) */
    void*       net_game;       /* CR15NetGame* if available, else nullptr */
    uint32_t    game_state;     /* ENetGameState enum value */
    uint32_t    flags;          /* host capability flags */
};

/* Host capability flags */
enum NvrHostFlags : uint32_t {
    NEVR_HOST_HAS_NETGAME   = 0x01, /* net_game pointer is valid */
    NEVR_HOST_IS_SERVER     = 0x02, /* running as dedicated server */
    NEVR_HOST_IS_CLIENT     = 0x04, /* running as client */
    NEVR_HOST_COMBAT_MODE   = 0x08, /* game is in combat mode */
    NEVR_HOST_IS_HEADLESS   = 0x10, /* running in headless mode (no graphics/audio) */
};

/*
 * Plugin lifecycle:
 *
 *   1. Host calls LoadLibrary on plugin DLL
 *   2. Host calls NvrPluginGetInfo() to read metadata
 *   3. Host calls NvrPluginInit(context) once after game init
 *   4. Host calls NvrPluginOnFrame(context) each server/client tick (optional)
 *   5. Host calls NvrPluginOnGameStateChange(context, old, new) on state transitions
 *   6. Host calls NvrPluginShutdown() before unload
 *
 * All functions are optional except NvrPluginGetInfo.
 * The host checks GetProcAddress for each and skips if not exported.
 */

/* Required: return plugin metadata */
typedef NvrPluginInfo (*NvrPluginGetInfo_fn)(void);

/* Optional: one-time initialization after game modules are loaded */
typedef int (*NvrPluginInit_fn)(const NvrGameContext* ctx);

/* Optional: per-frame tick */
typedef void (*NvrPluginOnFrame_fn)(const NvrGameContext* ctx);

/* Optional: game state transition */
typedef void (*NvrPluginOnGameStateChange_fn)(const NvrGameContext* ctx,
                                              uint32_t old_state,
                                              uint32_t new_state);

/* Optional: cleanup before DLL unload */
typedef void (*NvrPluginShutdown_fn)(void);

/*
 * Plugin API versioning.
 *
 * Bump NEVR_PLUGIN_API_VERSION when NvrPluginInfo, NvrGameContext, or any
 * cross-DLL export signature changes in a backward-incompatible way.
 * Adding new optional exports or new NvrHostFlags values does NOT require a bump.
 *
 * The host resolves NvrPluginGetApiVersion via GetProcAddress. If absent,
 * the plugin is v1 (pre-versioning). Fully backward-compatible — existing
 * plugins don't need recompilation.
 */
#define NEVR_PLUGIN_API_VERSION 2

/* Optional: return the API version the plugin was compiled against */
typedef uint32_t (*NvrPluginGetApiVersion_fn)(void);

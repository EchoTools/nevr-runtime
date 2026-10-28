/*
 * nevr_plugin_interface.h - Plugin interface for NEVR game patches
 *
 * This header defines the interface that gamepatches and legacy
 * use to load and manage plugins. Plugins export a set of C functions
 * that the host calls at defined lifecycle points.
 *
 * Drop this header into:
 *   nevr-runtime/src/runtime/nevr_plugin_interface.h
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

/* Plugin info returned by the host's plugin-query API (get_plugin_info, v5+).
 * A plugin calling get_plugin_info(n) receives a pointer to this struct. The
 * pointer is process-lifetime stable (g_plugins never shrinks after load). */
struct NvrLoadedPluginInfo {
    const char* name;
    const char* description;
    uint32_t    version_major;
    uint32_t    version_minor;
    uint32_t    version_patch;
    uint32_t    api_version;     /* NEVR_PLUGIN_API_VERSION the plugin was compiled against */
    uint32_t    capabilities;    /* NvrPluginCapabilities bitmask (0 = UNDECLARED) */
};

/* Log levels a plugin passes to ctx->log (API v6). */
enum NevrLogLevel : uint32_t {
    NEVR_LOG_DEBUG   = 0, /* narrative detail; off in production */
    NEVR_LOG_INFO    = 1, /* one summary line per event */
    NEVR_LOG_WARNING = 2, /* degraded but running */
    NEVR_LOG_ERROR   = 3, /* broken; the operator must act */
};

/* One key/value pair on a plugin log line (API v6). Both strings are copied
 * before ctx->log returns; a null key drops the pair, a null value reads as "". */
struct NevrLogField {
    const char* key;
    const char* value;
};

/* The host's structured log, as a plugin sees it (API v6). `plugin_id` is the
 * ctx->plugin_id the host handed this plugin. The host writes the line through
 * its own Log(), tagged [NEVR.PLUGIN] and plugin=<plugin_id>, at the level given
 * (an out-of-range level is logged as a warning that names it). `message` is
 * plain text, never a printf format. Thread-safe. */
typedef void (*NevrLogFn)(const char* plugin_id, uint32_t level, const char* message,
                          const NevrLogField* fields, uint32_t field_count);

/* Game context passed to the plugin by the host.
 *
 * The host fills every field; a pre-v5 plugin compiled without the trailing
 * fields still reads the first four at their original offsets. A v5+ plugin can
 * check ctx_size to decide whether ctx->get_plugin_count and friends are valid;
 * a v6+ plugin checks it covers ctx->log before calling it. */
struct NvrGameContext {
    uintptr_t   base_addr;      /* echovr.exe base address (ImageBase) */
    void*       net_game;       /* CR15NetGame* if available, else nullptr */
    uint32_t    game_state;     /* ENetGameState enum value */
    uint32_t    flags;          /* host capability flags */

    /* --- v5 additions (N134 S8) --- */
    uint32_t    ctx_size;       /* sizeof(NvrGameContext) as the host sees it —
                                 *   a plugin compares this against its own
                                 *   compile-time sizeof to decide which trailing
                                 *   fields are present. */
    int         (*get_plugin_count)(void);
    const NvrLoadedPluginInfo* (*get_plugin_info)(int index);

    /* --- v6 additions (#598) --- */
    const char* plugin_id;      /* the host's name for this plugin: the `name` of its config.yaml
                                 *   entry. Process-lifetime stable, so a plugin may keep it and
                                 *   log with it from any thread, including from Shutdown. */
    NevrLogFn   log;            /* the host's structured log; see NevrLogFn */

    /* --- v8 additions (#1094) --- */
    uint64_t    level_symbol;   /* CSymbol64 of the level the game holds as its main level, 0 while it holds none
                                 *   (or the host cannot tell). Read on every context the host builds, so it is
                                 *   current at init, on every frame and at every state change. */
    const char* level_name;     /* level_symbol's name when the host's symbol table resolves it, else nullptr (a
                                 *   level a plugin registered is not in the table: compare level_symbol with the
                                 *   CSymbol64 of the name your config gives you). Process-lifetime stable. */

    /* --- Quest additions (#438; additive, no version bump: ADR 0019, section 4) --- */
    const char* game_build_id;  /* Quest only: the running libr15's build ID in lowercase hex, process-lifetime
                                 *   stable. nullptr on PC, and on a Quest host that could not read it. A plugin
                                 *   checks ctx_size covers it first, and NEVR_HOST_PLATFORM_QUEST is set. */
};

/* Host capability flags */
enum NvrHostFlags : uint32_t {
    NEVR_HOST_HAS_NETGAME   = 0x01, /* net_game pointer is valid */
    NEVR_HOST_IS_SERVER     = 0x02, /* running as dedicated server */
    NEVR_HOST_IS_CLIENT     = 0x04, /* running as client */
    NEVR_HOST_COMBAT_MODE   = 0x08, /* game is in combat mode */
    NEVR_HOST_IS_HEADLESS   = 0x10, /* running in headless mode (no graphics/audio) */
    NEVR_HOST_PLATFORM_QUEST = 0x20, /* running on Quest (#438): base_addr is the libr15 load base, net_game is
                                      *   null and game_state is 0 until the Quest host measures a source for them */
};

/*
 * Plugin lifecycle:
 *
 *   1. Host loads plugins from config.yaml in list order (v5: ordered by
 *      capability priority within each explicit-ordering band).
 *   2. Host calls LoadLibrary on plugin DLL
 *   3. Host calls NvrPluginGetInfo() to read metadata
 *   4. Host calls NvrPluginInitEx(ctx, args_json) if exported (v4); otherwise
 *      NvrPluginInit(ctx) if exported (v3); else skips init (init is optional).
 *   5. Host calls NvrPluginOnFrame(ctx) each server/client tick (optional)
 *   6. Host calls NvrPluginOnGameStateChange(ctx, old, new) on state transitions
 *   7. During explicit normal-thread teardown, the host may call
 *      NvrPluginShutdown() in REVERSE load order before releasing its module
 *      references (last loaded shuts down first, so hooks installed on top of
 *      earlier plugins' hooks are torn down before what they depend on).
 *      If init returns non-zero and this export exists, the host calls it
 *      immediately before releasing the failed plugin. Without the export,
 *      the host retains that module until process exit because init may have
 *      installed hooks or acquired resources that cannot safely be undone.
 *      Shutdown is not guaranteed on process exit. Runtime dynamic unloading
 *      is not a supported lifecycle path.
 *
 * All functions are optional except NvrPluginGetInfo.
 * The host checks GetProcAddress for each and skips if not exported.
 *
 * v5 addition: ctx->get_plugin_count() and ctx->get_plugin_info(n) let a plugin
 * discover its neighbours at runtime. A plugin compiled against v5 checks
 * ctx->ctx_size >= sizeof(NvrGameContext) before calling these — a pre-v5 host
 * sends a smaller ctx struct, so the pointers land at offsets the host didn't
 * fill and their values are undefined.
 */

/* Required: return plugin metadata */
typedef NvrPluginInfo (*NvrPluginGetInfo_fn)(void);

/* Optional: one-time initialization after game modules are loaded */
typedef int (*NvrPluginInit_fn)(const NvrGameContext* ctx);

/*
 * Optional (API v4): one-time initialization WITH per-instance args.
 *
 * The host passes the plugin's `args` map from its config.yaml entry, serialized
 * to a JSON object string. When the entry declared no args, `args_json` is the
 * empty object "{}" (never null). Return 0 on success, non-zero to fail the load
 * (a `required: true` plugin that returns non-zero is fatal on a server).
 *
 * Backward compatibility (REQUIRED): the host prefers NvrPluginInitEx when the
 * plugin exports it, and otherwise calls NvrPluginInit(ctx) — so a v3 plugin
 * that exports only NvrPluginInit still loads unchanged, just without args. A
 * plugin exporting BOTH gets NvrPluginInitEx (NvrPluginInit is not also called).
 *
 * args_json shape (v4 contract): a FLAT JSON object whose keys are the entry's
 * args flattened to dotted paths (a nested `a: {b: 1}` becomes "a.b") and whose
 * values are strings (post-interpolation; the runtime interpolates ${VAR} refs
 * before serialization, and an unset bare ${VAR} stays as written). Example: {"greeting":"hi","limits.max":"5"}.
 */
typedef int (*NvrPluginInitEx_fn)(const NvrGameContext* ctx, const char* args_json);

/* Optional: per-frame tick */
typedef void (*NvrPluginOnFrame_fn)(const NvrGameContext* ctx);

/* Optional: game state transition */
typedef void (*NvrPluginOnGameStateChange_fn)(const NvrGameContext* ctx,
                                              uint32_t old_state,
                                              uint32_t new_state);

/* Optional: cleanup during explicit normal-thread host teardown. Process exit
 * does not guarantee this callback. */
typedef void (*NvrPluginShutdown_fn)(void);

/*
 * Plugin API versioning.
 *
 * Bump NEVR_PLUGIN_API_VERSION when NvrPluginInfo, NvrGameContext, or any
 * cross-DLL export signature changes in a backward-incompatible way.
 * Adding new optional exports or new NvrHostFlags values does NOT require a bump.
 *
 * v8 (#1094): adds level_symbol + level_name to the TAIL of NvrGameContext, so a plugin can switch itself on
 * for the levels its config names. A plugin checks ctx->ctx_size covers ctx->level_name before reading them,
 * exactly as for v5 and v6; a pre-v8 plugin never reads them. The host fills them in every context it passes
 * (init, per-frame, state change). The game sets the level as it enters the "loading level" state, so the
 * state-change context of that transition already carries it, and it holds none (0) at the menus. The measured
 * runs changed it only with a state transition; the per-frame context carries it as well, so a plugin that
 * polls in OnFrame misses nothing if a build changes it without one.
 *
 * v7 (#432): plugin-carried assets. The host exports NEVR_RegisterAsset / NEVR_DeregisterAssets (see "Plugin
 * assets" below) so a plugin can add levels and resources that live inside its own image. Additive: no
 * existing struct or signature changes, so the bump is a CAPABILITY signal. A plugin that registers assets
 * should declare api version 7 and treat a missing export as "this host cannot carry assets".
 *
 * v6 (N135, #598): adds plugin_id + log to the TAIL of NvrGameContext. A plugin
 * checks ctx->ctx_size covers ctx->log before calling it, exactly as for the v5
 * query API; a pre-v6 plugin never reads the new fields. The host fills them in
 * every context it passes (init, per-frame, state change), per plugin, and the
 * first context a plugin ever sees is the one passed to its init: there is no
 * host log before that, and none from DllMain (the loader lock is held there;
 * log from init onward, and keep ctx->plugin_id and ctx->log for Shutdown).
 * Lines land in the host's own log, so they reach the same file, filter and
 * level control as the host's lines.
 *
 * v5 (N134 S8): adds ctx_size + get_plugin_count / get_plugin_info to the TAIL
 * of NvrGameContext. This is backward-compatible — a pre-v5 plugin still reads
 * base_addr / net_game / game_state / flags at their original offsets — so the
 * bump is a CAPABILITY signal, not a break: a plugin can check ctx_size at
 * runtime to discover whether the host provides the query API, and the host can
 * check a plugin's declared API version to know whether it understands ctx_size.
 *
 * v4 (N134 S6): the additive NvrPluginInitEx(ctx, args_json) export + config-driven
 * ordered loading. This is a compatible addition — a v3 plugin (NvrPluginInit
 * only) still loads — so the bump is a CAPABILITY signal, not a break: a plugin
 * can query the host version to decide whether to rely on receiving args. The
 * args_json string shape is part of this version's contract (see NvrPluginInitEx).
 *
 * v3 (N114): added NvrPluginGetCapabilities export (a plugin can declare what it
 * does). Additive; existing plugins keep loading.
 *
 * The host resolves NvrPluginGetApiVersion via GetProcAddress. If absent,
 * the plugin is v1 (pre-versioning). Fully backward-compatible — existing
 * plugins don't need recompilation.
 */
#define NEVR_PLUGIN_API_VERSION 8

/* Optional: return the API version the plugin was compiled against */
typedef uint32_t (*NvrPluginGetApiVersion_fn)(void);

/*
 * Plugin capabilities — what the plugin DOES. (API v3.)
 *
 * NvrHostFlags runs the other way: host -> plugin, describing what the host is
 * offering. There was no channel for the reverse, so a plugin that rewrites
 * match rules was indistinguishable from one that recolours a menu.
 *
 * This matters for third-party game-mode plugins: a server has to know which
 * loaded plugins affect gameplay before it can decide whether a session is
 * valid, and an operator has to be able to see it without reading the source.
 *
 * THIS IS A DECLARATION, NOT AN ENFORCEMENT. A plugin that lies will be
 * believed — the host cannot verify these bits, and nothing here stops a
 * plugin from doing whatever the process can do. What it buys is that honest
 * plugins are legible, that a server can REQUIRE a declaration and refuse
 * plugins that make none, and that operators get a manifest. Do not describe
 * it as a security control, because it is not one.
 *
 * Declared via an optional export, exactly like NvrPluginGetApiVersion: absent
 * means UNDECLARED, and existing v1/v2 plugins keep loading unchanged. Adding
 * a field to NvrPluginInfo instead would have changed its size, and the host
 * reads that struct by value from the plugin — an older plugin would return a
 * smaller struct and the host would read past its end.
 */
enum NvrPluginCapabilities : uint32_t {
    /* No declaration was made. NOT a claim of harmlessness — it is the absence
     * of a claim, and a strict host should treat it as unknown, not as safe. */
    NEVR_PLUGIN_CAP_UNDECLARED      = 0x00,

    NEVR_PLUGIN_CAP_OBSERVES_ONLY   = 0x01, /* reads game state, never writes it */
    NEVR_PLUGIN_CAP_COSMETIC        = 0x02, /* visuals/audio only, no gameplay effect */
    NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY = 0x04, /* changes what players experience: physics, weapons, movement */
    NEVR_PLUGIN_CAP_ALTERS_RULES    = 0x08, /* changes match rules, scoring, or the game mode itself */
    NEVR_PLUGIN_CAP_NETWORK         = 0x10, /* opens sockets or talks to an external service */
    NEVR_PLUGIN_CAP_HOOKS_ENGINE    = 0x20, /* installs its own detours — see N84: a second MinHook
                                             * instance does not share the host's hook table */
};

/* Optional: return a bitwise OR of NvrPluginCapabilities. Absent => UNDECLARED. */
typedef uint32_t (*NvrPluginGetCapabilities_fn)(void);

/*
 * Plugin assets (API v7).
 *
 * A plugin carries its levels and resources inside its own image and registers them with the host; the host
 * hands the engine a pointer into the plugin's mapped image, so nothing is copied. The host exports two C
 * functions from the runtime module (GetProcAddress on the host module, like the resource override exports):
 *
 *   NEVR_RegisterAsset(type_hash, name_hash, data, size, label) -> NevrAssetStatus
 *   NEVR_DeregisterAssets(data_start, data_end)
 *
 * type_hash / name_hash are the engine's 64-bit symbol hashes of the resource's type and name (the same pair
 * NEVR_RegisterResourceOverride takes). `data` must stay valid until the asset is deregistered: put it in the
 * plugin image (a const array or a resource section), never on the heap. `size` is in bytes, 1..0xFFFFFFFF.
 * `label` is a short name for logs and for the `nevr_assets` list the game client reports to the game service
 * at login; it is copied.
 *
 * Register before the engine first loads the resource (NvrPluginInit/InitEx is early enough). A name the
 * engine has already loaded is refused with NEVR_ASSET_ALREADY_LOADED, never silently ignored.
 *
 * Lifetime: call NEVR_DeregisterAssets(image_start, image_end) from NvrPluginShutdown. The host also drops
 * every asset inside a plugin's image before it frees that image.
 *
 * Only single-buffer resources are served in v7; the host logs a warning naming any asset it cannot serve.
 */
enum NevrAssetStatus : int32_t {
    NEVR_ASSET_OK             = 0,
    NEVR_ASSET_BAD_ARGUMENT   = 1, /* null/empty data, zero type or name, or size above 0xFFFFFFFF */
    NEVR_ASSET_DUPLICATE      = 2, /* this (type, name) is already registered */
    NEVR_ASSET_ALREADY_LOADED = 3, /* the engine has already loaded this (type, name) */
};

typedef int  (*NevrRegisterAsset_fn)(uint64_t type_hash, uint64_t name_hash, const void* data, uint64_t size,
                                    const char* label);
typedef void (*NevrDeregisterAssets_fn)(const void* data_start, const void* data_end);

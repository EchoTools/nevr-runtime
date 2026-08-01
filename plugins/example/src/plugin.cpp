/*
 * plugin.cpp — Minimal reference plugin for the nEVR plugin API v3.
 *
 * Demonstrates every required and optional plugin lifecycle export, config
 * loading, and a safe MinHook hook with prologue validation and graceful
 * degradation when the game binary is absent.
 *
 * API surface demonstrated:
 *   NvrPluginGetInfo        — required: plugin metadata
 *   NvrPluginGetApiVersion  — optional: API version the plugin was built against
 *   NvrPluginGetCapabilities— optional: what this plugin DOES (declare it)
 *   NvrPluginInitEx         — optional (v4): one-time init WITH per-instance args
 *   NvrPluginInit           — optional (v3): one-time init (no args; kept as the
 *                             v3 reference — the host prefers InitEx when present)
 *   NvrPluginShutdown       — optional: cleanup before unload
 *
 * Helper libraries used:
 *   plugin_logger.h   — NEVR_DEFINE_PLUGIN_LOG macro for printf-style logging
 *   hook_manager.h    — nevr::HookManager for scoped MinHook lifecycle
 *   nevr_common.h     — ResolveVA_Checked, ValidatePrologue, LoadConfigFile
 *   yaml_config.h     — ParseYamlConfig (YAML subset parser)
 *   nlohmann/json     — JSON config parsing (pulled in via vcpkg)
 *
 * Build: included in the root CMakeLists.txt via plugins/CMakeLists.txt.
 *   just build  — compiles alongside all other plugins
 */

#include "extension/plugin_interface.h"
#include "plugin_logger.h"
#include "hook_manager.h"
#include "nevr_common.h"
#include "yaml_config.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

/* ── Per-plugin logging ─────────────────────────────────────────────── */

NEVR_DEFINE_PLUGIN_LOG("[nevr_example]")

/* ── Hook demonstration ─────────────────────────────────────────────── */

/*
 * Hook target: CMatSym::Hash (CSymbol64_Hash) at 0x140107F80.
 *
 * This is a pure query function (no side effects) with a clean, stable
 * prologue: PUSH RBX; SUB RSP, 0x20.  6 bytes, MinHook-safe, no RIP-relative
 * offsets in the prologue — the validation bytes are build-independent.
 *
 * The detour logs the first N calls then passes through silently, so even
 * a frequently-called target won't flood the log.
 */
static constexpr uint64_t  kHookTargetVA    = 0x140107F80;
static constexpr uint8_t   kPrologue[]      = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20};
static constexpr int       kMaxHookLogLines = 5;

/* Function signature extracted from ReVault decompilation. */
using HashFn = uint64_t (__fastcall *)(const char* str, uint64_t seed);

static HashFn        g_original_hash = nullptr;
static nevr::HookManager g_hooks;

static uint64_t __fastcall Detour_Hash(const char* str, uint64_t seed)
{
    static int callCount = 0;

    if (callCount < kMaxHookLogLines) {
        ++callCount;
        PluginLog("hook: CMatSym::Hash called (call #%d) str=%.40s seed=0x%llx",
                  callCount,
                  (str != nullptr) ? str : "(null)",
                  static_cast<unsigned long long>(seed));
    }

    return g_original_hash(str, seed);
}

/* ── Plugin lifecycle exports ───────────────────────────────────────── */

NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void)
{
    NvrPluginInfo info = {};
    info.name         = "nevr_example";
    info.description  = "Minimal reference plugin for the nEVR plugin API v4";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

/*
 * Declare what this plugin does. Optional, but DECLARE IT — a server needs to
 * know which loaded plugins affect gameplay before it can judge a session, and
 * a plugin that declares nothing is treated as unknown rather than harmless.
 *
 * Be honest and be specific. If you alter the rules of a match, say
 * ALTERS_RULES; if you only draw things, say COSMETIC. The host cannot check
 * you, which is exactly why an inaccurate declaration is worse than none.
 *
 * This example only reads state and logs, so it observes and nothing more.
 */
NEVR_PLUGIN_API uint32_t NvrPluginGetCapabilities(void)
{
    return NEVR_PLUGIN_CAP_OBSERVES_ONLY;
}

NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void)
{
    return NEVR_PLUGIN_API_VERSION;
}

NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx)
{
    if (ctx == nullptr) {
        PluginLog("init: context is null");
        return -1;
    }

    PluginLog("init: base=0x%llx flags=0x%x",
              static_cast<unsigned long long>(ctx->base_addr),
              static_cast<unsigned int>(ctx->flags));

    /* ── 1. Config loading ──────────────────────────────────────────── */

    /*
     * LoadConfigFile searches the DLL directory and parent directories,
     * so it works whether the plugin runs from build/bin/ or the
     * deployed bin/win10/ directory.
     */
    std::string configRaw = nevr::LoadConfigFile("_local/config.json");
    if (!configRaw.empty()) {
        try {
            auto j = nlohmann::json::parse(configRaw);
            if (j.contains("nevr_http_uri")) {
                PluginLog("config: nevr_http_uri=%s",
                          j["nevr_http_uri"].get<std::string>().c_str());
            } else {
                PluginLog("config: loaded _local/config.json (no nevr_http_uri key)");
            }
        } catch (const nlohmann::json::parse_error& e) {
            PluginLog("config: parse error in _local/config.json: %s", e.what());
        }
    } else {
        PluginLog("config: _local/config.json not found (normal during build)");
    }

    /* ── 2. Hook setup ──────────────────────────────────────────────── */

    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        PluginLog("hook: MH_Initialize failed: %s", MH_StatusToString(mhStatus));
        return -1;
    }

    void* target = nevr::ResolveVA_Checked(ctx->base_addr, kHookTargetVA);
    if (target == nullptr) {
        PluginLog("hook: VA 0x%llx not valid in this process — no game binary loaded",
                  static_cast<unsigned long long>(kHookTargetVA));
        return 0; /* not an error: plugin works without the game */
    }

    if (!nevr::ValidatePrologue(target, kPrologue, sizeof(kPrologue))) {
        PluginLog("hook: prologue mismatch at 0x%llx — wrong game version? skipping",
                  static_cast<unsigned long long>(kHookTargetVA));
        return 0;
    }

    MH_STATUS s = g_hooks.CreateAndEnable(
        target,
        reinterpret_cast<void*>(&Detour_Hash),
        reinterpret_cast<void**>(&g_original_hash));

    if (s != MH_OK) {
        PluginLog("hook: CreateAndEnable failed: %s", MH_StatusToString(s));
        return -1;
    }

    PluginLog("hook: installed on CMatSym::Hash @ 0x%llx (%zu hooks tracked)",
              static_cast<unsigned long long>(kHookTargetVA), g_hooks.count());

    return 0;
}

/*
 * v4 init: same lifecycle as NvrPluginInit, plus the per-instance `args` from
 * this plugin's config.yaml entry, serialized to a flat JSON object string. The
 * host prefers this export over NvrPluginInit when both are present.
 *
 * A reference plugin must model good behaviour: it logs that args ARRIVED and one
 * known non-secret key, but never dumps the whole args_json — a real plugin's
 * args can hold secrets. It then delegates to the v3 init body so this file stays
 * a single source of truth for the hook setup.
 */
NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext* ctx, const char* args_json)
{
    // N134 S8: demonstrate the ctx_size pattern — how a v5+ plugin discovers
    // at runtime whether the host provides the query API. The host always fills
    // ctx_size = sizeof(NvrGameContext); we compare against our own compile-time
    // sizeof to know which trailing fields are present.
    if (ctx->ctx_size >= sizeof(NvrGameContext)) {
        const int count = ctx->get_plugin_count();
        PluginLog("initex: host v5+ (ctx_size=%u). %d plugin(s) loaded already at init time.",
                  ctx->ctx_size, count);
        // Log our neighbours' names — a plugin can use this to discover
        // whether a dependency is present without probing via GetProcAddress.
        for (int i = 0; i < count; i++) {
            const NvrLoadedPluginInfo* other = ctx->get_plugin_info(i);
            if (other) {
                PluginLog("initex: neighbour[%d]: %s v%u.%u.%u caps=0x%02X",
                          i, other->name,
                          other->version_major, other->version_minor, other->version_patch,
                          other->capabilities);
            }
        }
    } else {
        PluginLog("initex: host pre-v5 (ctx_size=%u < %u) — query API unavailable",
                  ctx->ctx_size, (unsigned)sizeof(NvrGameContext));
    }

    if (args_json != nullptr) {
        try {
            auto a = nlohmann::json::parse(args_json);
            PluginLog("initex: received %zu arg(s) from config.yaml", a.size());
            if (a.contains("greeting")) {
                PluginLog("initex: greeting arg = %s",
                          a["greeting"].get<std::string>().c_str());
            }
        } catch (const nlohmann::json::parse_error& e) {
            PluginLog("initex: args_json parse error: %s", e.what());
        }
    } else {
        PluginLog("initex: no args_json (host passed null)");
    }

    return NvrPluginInit(ctx);
}

NEVR_PLUGIN_API void NvrPluginShutdown(void)
{
    PluginLog("shutdown: removing %zu hooks", g_hooks.count());
    g_hooks.RemoveAll();
    PluginLog("shutdown: complete");
}

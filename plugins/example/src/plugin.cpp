/*
 * plugin.cpp — Behavioral Acceptance Criterion (BAC) for the nEVR plugin API v5.
 *
 * THIS FILE IS THE REFERENCE. Community plugin authors copy from it. Every plugin
 * feature the host supports is demonstrated here with comments that explain WHY,
 * not just WHAT. A new author reading only this file should understand:
 *
 *   - What the host expects from each export
 *   - When each export is called (load order, lifecycle)
 *   - What they can do in each callback
 *   - What they must NOT do (don't block, don't leak, don't log secrets)
 *   - The API version compatibility contract
 *
 * === PLUGIN LIFECYCLE (the host calls these in this order) ===
 *
 *   ┌─────────────────────────────────────────────────────────────┐
 *   │ DLL load                                                      │
 *   │   LoadLibrary("nevr_example.dll")                             │
 *   │                                                               │
 *   │ 1. NvrPluginGetInfo()              REQUIRED — first call     │
 *   │      Returns name, description, semver. The host logs this   │
 *   │      before any init so operators see what loaded.           │
 *   │                                                               │
 *   │ 2. NvrPluginGetApiVersion()         OPTIONAL — v1+ capability│
 *   │      Returns NEVR_PLUGIN_API_VERSION. Absent = pre-versioning│
 *   │      (treated as v1). The host uses this to decide whether   │
 *   │      the plugin understands newer context fields.            │
 *   │                                                               │
 *   │ 3. NvrPluginGetCapabilities()       OPTIONAL — v3+ capability│
 *   │      Returns a bitmask. Absent = UNDECLARED (0x00), which a  │
 *   │      strict server treats as unsafe. Declare honestly.       │
 *   │                                                               │
 *   │ 4. NvrPluginInitEx(ctx, args_json)  OPTIONAL — v4+ init      │
 *   │      OR NvrPluginInit(ctx)          OPTIONAL — v3 fallback   │
 *   │      The host prefers InitEx when both are exported.         │
 *   │      Called ONCE, after game modules load, before the first  │
 *   │      tick. Return 0 = success; non-zero = abort this plugin. │
 *   │      A required:true plugin returning non-zero is FATAL on   │
 *   │      a server (the process aborts).                          │
 *   │                                                               │
 *   │── Game running ──────────────────────────────────────────────│
 *   │                                                               │
 *   │ 5. NvrPluginOnFrame(ctx)             OPTIONAL — per-tick     │
 *   │      Called every server/client tick (≈60-90 Hz depending on │
 *   │      game mode). MUST NOT block, allocate heavily, or take   │
 *   │      locks that contend with the game's own code.            │
 *   │                                                               │
 *   │ 6. NvrPluginOnGameStateChange(ctx, old, new)  OPTIONAL       │
 *   │      Called when ENetGameState transitions. The host calls   │
 *   │      this AFTER the transition; old and new are the enum      │
 *   │      values. Plugins can use this to defer init of systems   │
 *   │      that only make sense once the game is in a specific     │
 *   │      state (e.g. waiting for IN_GAME before touching the     │
 *   │      netgame pointer).                                       │
 *   │                                                               │
 *   │── Shutdown ──────────────────────────────────────────────────│
 *   │                                                               │
 *   │ 7. NvrPluginShutdown()              OPTIONAL — cleanup       │
 *   │      Called in REVERSE load order before FreeLibrary.        │
 *   │      Last loaded shuts down first, so hooks installed on     │
 *   │      top of earlier plugins are torn down before what they   │
 *   │      depend on. Remove your hooks, close your sockets,       │
 *   │      join your threads. After this returns, your DLL is      │
 *   │      unloaded — any thread still running is a crash.         │
 *   └─────────────────────────────────────────────────────────────┘
 *
 * === WHAT YOU MUST NOT DO (the negative space is load-bearing) ===
 *
 *   - Don't block in NvrPluginOnFrame. Per-tick callbacks run on
 *     the game's main thread; blocking stalls rendering and netcode.
 *   - Don't leak handles, memory, or thread handles. NvrPluginShutdown
 *     must clean up everything allocated since init.
 *   - Don't log secrets. The args_json from config.yaml can contain
 *     keys, tokens, and passwords. Log individual known keys if needed,
 *     but never dump the whole string.
 *   - Don't call MH_DisableHook(MH_ALL_HOOKS). That disables every
 *     hook in the process (crash handler, log system, other plugins).
 *     Use a HookManager instance and only remove your own hooks.
 *   - Don't call MH_Uninitialize(). The host owns MinHook's lifecycle.
 *   - Don't assume plugin load order. Use the query API (v5+) or
 *     capabilities to discover neighbours at runtime.
 *   - Don't return a different version of NvrPluginInfo on each call.
 *     The host may call NvrPluginGetInfo more than once (e.g. for
 *     logging and for the query-cache).
 *
 * === API VERSION COMPATIBILITY CONTRACT ===
 *
 *   The host resolves exports via GetProcAddress. Absent = that
 *   feature didn't exist when the plugin was built; the host treats
 *   the absence as "not supported" rather than "error."
 *
 *   NEVR_PLUGIN_API_VERSION is currently 5 (N134 S8).
 *
 *   | Export present        | What it signals                                 |
 *   |-----------------------|-------------------------------------------------|
 *   | NvrPluginGetApiVersion| Plugin understands NEVR_PLUGIN_API_VERSION       |
 *   | NvrPluginGetCapabilities| Plugin declares what it DOES (v3+)            |
 *   | NvrPluginInitEx       | Plugin accepts per-instance args (v4+)          |
 *   | ctx_size >= sizeof(NvrGameContext) at runtime | Host provides query API (v5+) |
 *
 *   A plugin compiled against v5 on a pre-v5 host: ctx->ctx_size will
 *   be smaller than sizeof(NvrGameContext). The plugin checks this
 *   BEFORE calling ctx->get_plugin_count() — calling through a pointer
 *   the host didn't fill is undefined behaviour.
 *
 * === INCLUDES AND HELPERS ===
 *
 *   Builds against these plugin_common headers (all plugins link
 *   nevr_plugin_common, an INTERFACE library):
 *
 *     plugin_interface.h  — types, enums, NEVR_PLUGIN_API macro
 *     plugin_logger.h     — NEVR_DEFINE_PLUGIN_LOG(prefix)
 *     hook_manager.h      — nevr::HookManager (scoped MinHook lifecycle)
 *     nevr_common.h       — ResolveVA_Checked, ValidatePrologue, LoadConfigFile
 *     yaml_config.h       — ParseYamlConfig (YAML subset parser)
 *     address_registry.h  — verified VA constants for game functions
 *
 *   Additional vcpkg deps added per-plugin in CMakeLists.txt:
 *     nlohmann_json, minhook::minhook
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

/*
 * ── NEVR_PLUGIN_EXPORTS ────────────────────────────────────────────
 *
 * Define NEVR_PLUGIN_EXPORTS before including plugin_interface.h so the
 * NEVR_PLUGIN_API macro expands to `extern "C" __declspec(dllexport)`.
 *
 * Without this define, the macro expands to `__declspec(dllimport)`,
 * which is correct for code that *calls* plugin functions (the host)
 * but wrong for the plugin DLL itself — it would generate thunks
 * expecting the function to live in another module.
 *
 * This is the ONLY preprocessor define a plugin must set. The build
 * system may set it via target_compile_definitions; setting it here as
 * well is harmless and self-documents the intent.
 */

/*
 * ── PER-PLUGIN LOGGING ─────────────────────────────────────────────
 *
 * NEVR_DEFINE_PLUGIN_LOG generates an inline PluginLog() function that
 * writes printf-style formatted output to stderr. Call it ONCE per
 * plugin at file scope. The prefix identifies this plugin's log lines.
 *
 * For multi-translation-unit plugins, wrap in a namespace:
 *
 *   namespace my_plugin { NEVR_DEFINE_PLUGIN_LOG("[my_plugin]") }
 *   // Call as my_plugin::PluginLog("msg %d", val);
 */
NEVR_DEFINE_PLUGIN_LOG("[nevr_example]")

/*
 * ── HOOK DEMONSTRATION ─────────────────────────────────────────────
 *
 * Every hook installed by a plugin must follow this pattern:
 *
 *   1. RESOLVE the VA: nevr::ResolveVA_Checked(base, VA) — returns
 *      nullptr if the page is unmapped. This guards against running
 *      without the game binary (e.g. in a test harness).
 *
 *   2. VALIDATE the prologue: nevr::ValidatePrologue(addr, bytes, len)
 *      — returns false if the first N bytes at the target don't match.
 *      This catches wrong game versions BEFORE the hook writes anything.
 *      A blind write at the wrong VA can corrupt the game silently.
 *
 *   3. CREATE + ENABLE via HookManager: g_hooks.CreateAndEnable(...)
 *      — this tracks the target so NvrPluginShutdown can remove it.
 *      If enable fails, the created hook is removed to avoid leaks.
 *
 *   4. NEVER call MH_DisableHook(MH_ALL_HOOKS) — that disables every
 *      hook in the process. Only remove the hooks YOU installed.
 *
 *   5. NEVER call MH_Uninitialize() — the host owns the MinHook
 *      lifecycle. Multiple plugins share one MH instance.
 *
 *   6. MH_Initialize() returns MH_ERROR_ALREADY_INITIALIZED harmlessly
 *      if the host (or an earlier plugin) already called it. Check for
 *      both MH_OK and MH_ERROR_ALREADY_INITIALIZED.
 */

/*
 * Hook target: CMatSym::Hash (CSymbol64_Hash) at 0x140107F80.
 *
 * This is a pure query function (no side effects) with a clean, stable
 * prologue: PUSH RBX; SUB RSP, 0x20.  Six bytes, MinHook-safe — no
 * RIP-relative offsets in the prologue that would change across builds.
 *
 * The detour logs the first N calls then passes through silently to the
 * original, so even a frequently-called target (CSymbol64_Hash is used
 * by the game's string interning) won't flood the log.
 *
 * The VA and prologue bytes are sourced from ReVault and validated at
 * init time — they are NOT guesses. If the game binary is not present
 * or these bytes don't match, the hook is skipped gracefully.
 */
static constexpr uint64_t  kHookTargetVA    = 0x140107F80;
static constexpr uint8_t   kPrologue[]      = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20};
static constexpr int       kMaxHookLogLines = 5;

/*
 * Function signature extracted from ReVault decompilation.
 *
 * IMPORTANT: Game functions use __fastcall. The first two integer/pointer
 * arguments go in RCX and RDX (not on the stack). Getting this wrong
 * produces corrupt arguments and crashes that look like heap corruption
 * because MinHook writes a valid trampoline — the calling convention
 * mismatch manifests at runtime as wrong register contents.
 *
 * Use `this` as the first parameter name when the original is a
 * member function — the decompiler may label it as `this` or `__this`.
 */
using HashFn = uint64_t (__fastcall *)(const char* str, uint64_t seed);

static HashFn             g_original_hash = nullptr;
static nevr::HookManager  g_hooks;

/*
 * Detour function — called INSTEAD of CMatSym::Hash.
 *
 * Rules for detour bodies:
 *   - Call the original through g_original_hash (not the VA) so the
 *     trampoline handles the displaced instructions.
 *   - Keep it fast. This function is called on the game's hot path.
 *     No allocations, no locks, no I/O (logging is I/O — rate-limit it).
 *   - The calling convention MUST match the original. MinHook preserves
 *     registers across the detour call, but the detour signature must
 *     match or the compiler will read arguments from the wrong place.
 */
static uint64_t __fastcall Detour_Hash(const char* str, uint64_t seed)
{
    static int callCount = 0;

    /*
     * Rate-limit logging: this function is called thousands of times
     * per frame. Logging every call would saturate stderr and slow the
     * game to a crawl. Log the first N calls to prove the hook works,
     * then go silent.
     */
    if (callCount < kMaxHookLogLines) {
        ++callCount;
        PluginLog("hook: CMatSym::Hash called (call #%d) str=%.40s seed=0x%llx",
                  callCount,
                  (str != nullptr) ? str : "(null)",
                  static_cast<unsigned long long>(seed));
    }

    /*
     * ALWAYS forward to the original. A hook that consumes the call
     * (returns without calling the original) removes the function from
     * the game — do that only when you intend to replace the behaviour.
     * This example observes, so it passes through.
     */
    return g_original_hash(str, seed);
}

/*
 * ── PLUGIN LIFECYCLE EXPORTS ───────────────────────────────────────
 *
 * Every export uses the NEVR_PLUGIN_API macro, which expands to:
 *
 *   extern "C" __declspec(dllexport)    (Windows, building the plugin)
 *   extern "C" __declspec(dllimport)    (Windows, consuming the plugin)
 *   extern "C" __attribute__((visibility("default")))  (non-Windows)
 *
 * The extern "C" is load-bearing: it prevents C++ name mangling so the
 * host can resolve these by name via GetProcAddress. Without it, the
 * host would have to guess the mangled symbol (which changes per
 * compiler version).
 */

/*
 * ── NvrPluginGetInfo ────────────────────────────────────────────────
 *
 * REQUIRED. The ONLY export the host demands. Every other export is
 * optional and resolved by GetProcAddress.
 *
 * This is the FIRST function the host calls after LoadLibrary. It must
 * succeed — if it returns garbage, the host has no way to identify the
 * plugin and logs a generic error.
 *
 * Fields:
 *   name           — Short identifier used in logs, config.yaml
 *                    (e.g. "nevr_example"). Must be unique among loaded
 *                    plugins. The name in config.yaml must match this
 *                    field exactly (case-sensitive).
 *   description    — Human-readable summary. Shown in operator tooling.
 *                    Keep it under ~100 chars; this is not a README.
 *   version_major  — Semver major. Bump when you break backward compat.
 *   version_minor  — Semver minor. Bump when you add features.
 *   version_patch  — Semver patch. Bump for bugfixes.
 *
 * The host may call this more than once (e.g. at load time for logging
 * and again when building the query cache for other plugins). Always
 * return the same values — returning a different version on a
 * subsequent call is undefined behaviour.
 *
 * The struct is returned BY VALUE (not via a pointer), which means the
 * host gets a copy. The string pointers (name, description) must point
 * to string literals or static storage — they must outlive the call.
 * Pointing at a stack buffer or a temporary std::string is a
 * use-after-free bug.
 */
NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void)
{
    NvrPluginInfo info = {};
    info.name          = "nevr_example";
    info.description   = "Reference plugin demonstrating every nEVR plugin API feature";
    info.version_major = 1;
    info.version_minor = 0;
    info.version_patch = 0;
    return info;
}

/*
 * ── NvrPluginGetApiVersion ──────────────────────────────────────────
 *
 * OPTIONAL (v1+). Returns the NEVR_PLUGIN_API_VERSION the plugin was
 * compiled against.
 *
 * WHY THIS EXISTS: Before this export, the host had no way to know what
 * API a plugin expects. A plugin built against v5 on a v3 host would
 * try to access ctx->ctx_size (which the v3 host didn't fill) and read
 * garbage. Now the host can check the declared version and decide:
 *
 *   - Version <= host's own version: load normally.
 *   - Version > host's own version: log a warning, load anyway
 *     (additions are tail-extensions; the plugin should check
 *     ctx_size at runtime to degrade gracefully).
 *
 * Absent = v1 (pre-versioning). The host treats an unknown plugin as
 * v1 and only fills the original four NvrGameContext fields.
 *
 * The host reads this BEFORE calling init, so a plugin can't "upgrade"
 * itself after load. The version is baked in at compile time.
 */
NEVR_PLUGIN_API uint32_t NvrPluginGetApiVersion(void)
{
    return NEVR_PLUGIN_API_VERSION;
}

/*
 * ── NvrPluginGetCapabilities ────────────────────────────────────────
 *
 * OPTIONAL (v3+). Returns a bitmask of NvrPluginCapabilities declaring
 * what this plugin DOES.
 *
 * WHY THIS EXISTS: NvrHostFlags runs host → plugin (describing what the
 * host offers). There was no channel for the reverse, so a plugin that
 * rewrites match rules was indistinguishable from one that recolours a
 * menu. A server operator needs to know, at a glance, which loaded
 * plugins affect gameplay.
 *
 * THIS IS A DECLARATION, NOT AN ENFORCEMENT. The host cannot verify
 * these bits. A plugin that lies is believed. What this buys:
 *
 *   1. Legibility: operators get a manifest of loaded plugins and what
 *      they claim to do.
 *   2. Gating: a server can REQUIRE a declaration (non-zero) and refuse
 *      plugins that make none.
 *   3. Ordering: v5+ loads plugins ordered by capability priority
 *      (e.g. OBSERVES_ONLY first, ALTERS_RULES last).
 *
 * Capability bits (bitwise OR to combine):
 *
 *   NEVR_PLUGIN_CAP_UNDECLARED      0x00  Absence of a claim. NOT safe.
 *   NEVR_PLUGIN_CAP_OBSERVES_ONLY   0x01  Reads game state, never writes.
 *   NEVR_PLUGIN_CAP_COSMETIC        0x02  Visuals/audio only.
 *   NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY 0x04  Changes physics, weapons, movement.
 *   NEVR_PLUGIN_CAP_ALTERS_RULES    0x08  Changes match rules, scoring.
 *   NEVR_PLUGIN_CAP_NETWORK         0x10  Opens sockets, talks to external services.
 *   NEVR_PLUGIN_CAP_HOOKS_ENGINE    0x20  Installs its own detours (see N84).
 *
 * Be honest and be specific. If you alter match rules, say ALTERS_RULES.
 * If you only draw things, say COSMETIC. An inaccurate declaration is
 * worse than none — it spends trust the ecosystem needs.
 *
 * This example reads state and logs, and its hook (CMatSym::Hash) is a
 * pure query function. It touches nothing, so OBSERVES_ONLY is accurate.
 * We do NOT also set HOOKS_ENGINE even though we install a detour — that
 * bit is reserved for plugins that run their own hooking infrastructure
 * (a second MinHook instance, VEH handlers, etc.), not for plain detours
 * through the host's shared MinHook.
 */
NEVR_PLUGIN_API uint32_t NvrPluginGetCapabilities(void)
{
    return NEVR_PLUGIN_CAP_OBSERVES_ONLY;
}

/*
 * ── NvrPluginInitEx ─────────────────────────────────────────────────
 *
 * OPTIONAL (v4+). One-time initialization WITH per-instance args from
 * config.yaml. The host prefers this over NvrPluginInit when both are
 * exported; it does NOT call both.
 *
 * ctx:       The NvrGameContext. See below for field documentation.
 *            NEVER null — the host guarantees a valid pointer.
 *
 * args_json: The plugin's `args` map from its config.yaml entry,
 *            serialized to a flat JSON object string. When the entry
 *            declared no args, this is "{}" (the empty object string),
 *            never null. Keys are flattened dotted paths (a nested
 *            YAML `a: {b: 1}` becomes `"a.b"`), and values are
 *            strings (post-interpolation; the host resolves ${VAR}
 *            references before serialization).
 *
 *            Example: {"greeting":"hi","limits.max":"5"}
 *
 *            DO NOT log the entire args_json string. Args can contain
 *            secrets (API keys, tokens). Log individual known-safe keys
 *            if needed for debugging.
 *
 * Return 0 for success, non-zero to abort this plugin's load. A
 * `required: true` plugin in config.yaml that returns non-zero is
 * FATAL on a server — the host calls FatalError and aborts the process.
 * A `required: false` plugin that returns non-zero is unloaded quietly.
 *
 * ctx_size pattern (v5+): A plugin compiled against v5 can check at
 * runtime whether the host provides the query API by comparing
 * ctx->ctx_size against its own sizeof(NvrGameContext):
 *
 *   if (ctx->ctx_size >= sizeof(NvrGameContext)) {
 *       // Safe: the host's struct is at least as large as ours.
 *       // The query API pointers (get_plugin_count, get_plugin_info)
 *       // were filled by the host and are valid to call.
 *   } else {
 *       // Pre-v5 host sent a smaller struct. The trailing fields
 *       // are at offsets the host didn't write — their values are
 *       // undefined. Do not call through them.
 *   }
 *
 * This is a RUNTIME check, not a compile-time #ifdef. The same binary
 * works on v4 and v5 hosts, degrading gracefully when the query API is
 * absent. This is the pattern that makes tail-extensions backward-
 * compatible without recompilation.
 */
NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext* ctx, const char* args_json)
{
    /*
     * ── ctx_size check (v5+ query API discovery) ──────────────────
     *
     * The host always fills ctx_size = sizeof(NvrGameContext) from the
     * host's perspective. We compare against our own compile-time
     * sizeof to know which trailing fields are present.
     *
     * If ctx_size is large enough, the query API is valid. Use it to
     * discover neighbouring plugins — a plugin can check whether a
     * dependency is present at runtime without probing via
     * GetProcAddress or assuming load order.
     */

    if (ctx->ctx_size >= sizeof(NvrGameContext)) {
        /*
         * ctx->get_plugin_count() returns the number of plugins loaded
         * SO FAR at the time of the call (including plugins that
         * already shut down — the count never decreases). During
         * InitEx, a plugin loaded later in the order has not been
         * initialized yet and is not in this count.
         *
         * ctx->get_plugin_info(n) returns a pointer to
         * NvrLoadedPluginInfo for plugin index n (0-based). The
         * pointer is process-lifetime stable (the host's plugin array
         * never shrinks). Returns nullptr for out-of-range n.
         *
         * NvrLoadedPluginInfo fields:
         *   name, description  — from NvrPluginGetInfo()
         *   version_major/minor/patch — semver
         *   api_version       — from NvrPluginGetApiVersion() (0 if absent)
         *   capabilities      — from NvrPluginGetCapabilities() (0 if absent)
         */
        const int count = ctx->get_plugin_count();
        PluginLog("initex: host v5+ (ctx_size=%u)."
                  " %d plugin(s) loaded already at init time.",
                  ctx->ctx_size, count);

        for (int i = 0; i < count; i++) {
            const NvrLoadedPluginInfo* other = ctx->get_plugin_info(i);
            if (other) {
                PluginLog("initex: neighbour[%d]: %s v%u.%u.%u caps=0x%02X",
                          i, other->name,
                          other->version_major, other->version_minor,
                          other->version_patch,
                          other->capabilities);
            }
        }
    } else {
        PluginLog("initex: host pre-v5 (ctx_size=%u < %u) —"
                  " query API unavailable",
                  ctx->ctx_size, (unsigned)sizeof(NvrGameContext));
    }

    /*
     * ── Config args from config.yaml ───────────────────────────────
     *
     * Parse the args_json string with nlohmann::json. The string is a
     * flat JSON object with string values. Each key is a dotted path
     * from the plugin's YAML args block.
     *
     * SECURITY: Never log the full args_json. It can contain secrets.
     * Log individual known-safe keys only.
     */

    if (args_json != nullptr) {
        try {
            auto a = nlohmann::json::parse(args_json);
            PluginLog("initex: received %zu arg(s) from config.yaml", a.size());
            /* Log a known-safe key, not the whole blob. */
            if (a.contains("greeting")) {
                PluginLog("initex: greeting arg = %s",
                          a["greeting"].get<std::string>().c_str());
            }
        } catch (const nlohmann::json::parse_error& e) {
            PluginLog("initex: args_json parse error: %s", e.what());
            /* Do not fail the load for a parse error — the plugin may
             * work fine with defaults. A `required: true` plugin that
             * truly can't function without args should return non-zero
             * here. */
        }
    } else {
        /*
         * The v4 contract says args_json is "{}" (never null) when
         * the entry has no args. A null here means either a buggy
         * host or a pre-v4 host that exported InitEx via GetProcAddress
         * but passed null. Handle it gracefully.
         */
        PluginLog("initex: no args_json (host passed null —"
                  " pre-v4 host or bug)");
    }

    /*
     * Delegate to the v3 init body. This file keeps the hook setup in
     * one place (NvrPluginInit) so both init paths share the same
     * logic. A plugin exporting only InitEx can inline everything here.
     */
    return NvrPluginInit(ctx);
}

/*
 * ── NvrPluginInit ───────────────────────────────────────────────────
 *
 * OPTIONAL (v3+). Legacy one-time init. The host calls this when
 * NvrPluginInitEx is NOT exported. A plugin that exports BOTH gets
 * NvrPluginInitEx (this function is NOT also called).
 *
 * This is kept as the v3 reference implementation — a new plugin should
 * export NvrPluginInitEx instead. The host still supports this path so
 * older plugins load unchanged.
 *
 * ctx: The NvrGameContext. Fields available on ALL API versions:
 *
 *   base_addr   — echovr.exe ImageBase. CRITICAL: every hook VA must
 *                 be resolved through this (base + VA - 0x140000000).
 *                 It is NOT always 0x140000000 — ASLR can rebase the
 *                 image, and Wine may load at a different base than
 *                 bare-metal Windows.
 *
 *   net_game    — CR15NetGame* if available, else nullptr. Valid when
 *                 NEVR_HOST_HAS_NETGAME is set in flags. The netgame
 *                 is the game's top-level networking object. It may be
 *                 nullptr during early init and in non-networked modes.
 *
 *   game_state  — ENetGameState enum value at the time init is called.
 *                 Typically NOT_IN_GAME during init; the game hasn't
 *                 started yet. Use NvrPluginOnGameStateChange to react
 *                 to transitions.
 *
 *   flags       — NvrHostFlags bitmask describing the current host:
 *
 *       NEVR_HOST_HAS_NETGAME 0x01  net_game pointer is valid
 *       NEVR_HOST_IS_SERVER   0x02  running as dedicated server
 *       NEVR_HOST_IS_CLIENT   0x04  running as a client
 *       NEVR_HOST_COMBAT_MODE 0x08  game is in combat mode
 *       NEVR_HOST_IS_HEADLESS 0x10  headless (no graphics/audio)
 *
 *     Check these BEFORE assuming net_game is valid or the game mode.
 *     A plugin that unconditionally dereferences net_game will crash
 *     when loaded on a headless server where it's nullptr.
 *
 *   ctx_size    — (v5+) sizeof(NvrGameContext) as the host sees it.
 *                 A plugin compares this against its own compile-time
 *                 sizeof to discover whether the trailing query API
 *                 fields are present. See NvrPluginInitEx for the full
 *                 pattern. On a pre-v5 host this field is at an offset
 *                 the host didn't write — its value is undefined.
 */
NEVR_PLUGIN_API int NvrPluginInit(const NvrGameContext* ctx)
{
    /*
     * ctx is guaranteed non-null by the host, but defensive null-checks
     * cost nothing and prevent crashes if the plugin is loaded by a
     * non-standard host (test harness, manual LoadLibrary).
     */
    if (ctx == nullptr) {
        PluginLog("init: context is null");
        return -1;
    }

    PluginLog("init: base=0x%llx flags=0x%x game_state=%u",
              static_cast<unsigned long long>(ctx->base_addr),
              static_cast<unsigned int>(ctx->flags),
              static_cast<unsigned int>(ctx->game_state));

    /*
     * ── 1. CONFIG LOADING ─────────────────────────────────────────
     *
     * nevr::LoadConfigFile searches the DLL directory and parent
     * directories (../ and ../../), so it works whether the plugin
     * runs from build/<preset>/bin/ or the deployed bin/win10/
     * directory.
     *
     * Returns an empty string on failure — no separate error code.
     * This is intentional: config files are optional, and the plugin
     * should work with defaults when they're absent.
     */
    std::string configRaw = nevr::LoadConfigFile("_local/config.json");
    if (!configRaw.empty()) {
        try {
            auto j = nlohmann::json::parse(configRaw);
            if (j.contains("nevr_http_uri")) {
                PluginLog("config: nevr_http_uri=%s",
                          j["nevr_http_uri"].get<std::string>().c_str());
            } else {
                PluginLog("config: loaded _local/config.json"
                          " (no nevr_http_uri key)");
            }
        } catch (const nlohmann::json::parse_error& e) {
            PluginLog("config: parse error in _local/config.json: %s",
                      e.what());
        }
    } else {
        PluginLog("config: _local/config.json not found"
                  " (normal during build, or when no local overrides"
                  " are configured)");
    }

    /*
     * ── 2. HOOK SETUP ─────────────────────────────────────────────
     *
     * Full hook installation pattern, step by step.
     */

    /*
     * Step 1: Initialize MinHook. Returns MH_ERROR_ALREADY_INITIALIZED
     * if the host (or an earlier plugin) already called MH_Initialize,
     * which is harmless. Do NOT call MH_Uninitialize() — the host owns
     * the MinHook lifecycle and will uninitialize it after all plugins
     * shut down.
     */
    MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        PluginLog("hook: MH_Initialize failed: %s",
                  MH_StatusToString(mhStatus));
        return -1;
    }

    /*
     * Step 2: Resolve the VA. ResolveVA_Checked returns nullptr if
     * the page is unmapped (no game binary loaded, wrong base address,
     * or the VA is out of range). This lets the plugin work in test
     * harnesses and non-game processes.
     */
    void* target = nevr::ResolveVA_Checked(ctx->base_addr, kHookTargetVA);
    if (target == nullptr) {
        PluginLog("hook: VA 0x%llx not valid in this process —"
                  " no game binary loaded, or VA out of range;"
                  " plugin continues without this hook",
                  static_cast<unsigned long long>(kHookTargetVA));
        return 0; /* NOT an error: plugin works without the game */
    }

    /*
     * Step 3: Validate the prologue. The bytes at the target address
     * must match exactly. If they don't, the game binary is a different
     * version or build — installing a hook at the wrong location would
     * corrupt the process. Skip the hook instead.
     */
    if (!nevr::ValidatePrologue(target, kPrologue, sizeof(kPrologue))) {
        PluginLog("hook: prologue mismatch at 0x%llx —"
                  " wrong game version? skipping this hook",
                  static_cast<unsigned long long>(kHookTargetVA));
        return 0;
    }

    /*
     * Step 4: Create and enable the hook via HookManager.
     * HookManager::CreateAndEnable does three things atomically:
     *   a) MH_CreateHook(target, detour, original)
     *   b) MH_EnableHook(target) — on failure, removes the created
     *      hook to avoid leaking a disabled hook
     *   c) Tracks the target for cleanup
     *
     * g_original_hash receives the trampoline — call this, not the
     * original VA, to forward to the real function.
     */
    MH_STATUS s = g_hooks.CreateAndEnable(
        target,
        reinterpret_cast<void*>(&Detour_Hash),
        reinterpret_cast<void**>(&g_original_hash));

    if (s != MH_OK) {
        PluginLog("hook: CreateAndEnable failed: %s",
                  MH_StatusToString(s));
        return -1;
    }

    PluginLog("hook: installed on CMatSym::Hash @ 0x%llx"
              " (%zu hooks tracked)",
              static_cast<unsigned long long>(kHookTargetVA),
              g_hooks.count());

    return 0;
}

/*
 * ── NvrPluginOnFrame ────────────────────────────────────────────────
 *
 * OPTIONAL. Called every server or client tick (≈60-90 Hz depending on
 * game mode and frame rate). This runs on the game's main thread.
 *
 * WHEN TO USE:
 *   - Polling state that changes faster than ENetGameState transitions
 *   - Accumulating per-frame metrics
 *   - Triggering actions on a fixed schedule (every Nth frame)
 *
 * WHEN NOT TO USE:
 *   - Long-running work → offload to a worker thread
 *   - Blocking I/O → the main thread stalls rendering and netcode
 *   - Heavy allocations → per-frame malloc/free fragments the heap
 *   - printf-style logging every frame → saturates stderr
 *
 * RULES:
 *   - Return quickly. If you need more than ~100us, reconsider.
 *   - Don't block. No Sleep(), no blocking socket reads, no mutex wait.
 *   - Don't allocate heavily. Pre-allocate during init and reuse buffers.
 *   - Don't call MH_Initialize or MH_Uninitialize.
 *
 * This stub is intentionally minimal — the host calls it unconditionally
 * if exported, so an empty body is correct for a plugin that only needs
 * init and shutdown.
 *
 * To USE this callback, add it to your CMakeLists.txt's .def file (if
 * using a module-definition file) or ensure it's not stripped — the
 * host resolves it by name via GetProcAddress.
 */
NEVR_PLUGIN_API void NvrPluginOnFrame(const NvrGameContext* ctx)
{
    /*
     * ctx is the same NvrGameContext passed to init, with updated
     * game_state and flags reflecting the current tick.
     *
     * Example (uncomment to track frame count):
     *
     *   static uint64_t s_frameCount = 0;
     *   ++s_frameCount;
     *   if (s_frameCount % 600 == 0) {  // every ~10s at 60Hz
     *       PluginLog("onframe: tick #%llu game_state=%u",
     *                 s_frameCount, ctx->game_state);
     *   }
     */

    (void)ctx; /* unused in this minimal example */
}

/*
 * ── NvrPluginOnGameStateChange ──────────────────────────────────────
 *
 * OPTIONAL. Called when the game's ENetGameState transitions. The host
 * calls this AFTER the transition completes.
 *
 * old_state: the previous state
 * new_state: the current state
 *
 * ENetGameState values (from the game binary — these are the known
 * transitions; the full enum is in the reconstruction):
 *
 *   NOT_IN_GAME (0) → LOADING (1)     — game is loading a level
 *   LOADING (1)     → IN_GAME (2)     — level loaded, gameplay active
 *   IN_GAME (2)     → LOADING (1)     — returning to lobby/loading
 *   IN_GAME (2)     → NOT_IN_GAME (0) — exiting to main menu
 *
 * WHEN TO USE:
 *   - Initialize systems that need the game to be fully loaded
 *     (defer touching net_game until new_state == IN_GAME)
 *   - Tear down per-match state when the match ends
 *   - Track session boundaries for metrics
 *
 * WHEN NOT TO USE:
 *   - Work that should happen exactly once at plugin load → use init
 *   - Per-frame polling → use NvrPluginOnFrame
 *
 * RULES:
 *   - Don't block. State transitions happen on the main thread.
 *   - Don't assume old_state and new_state are adjacent enum values.
 *     The game can skip states (e.g. NOT_IN_GAME → LOADING without
 *     visiting every intermediate).
 *   - Don't dereference net_game unless new_state == IN_GAME AND
 *     NEVR_HOST_HAS_NETGAME is set in flags. Net_game can be nullptr
 *     during transitions.
 */
NEVR_PLUGIN_API void NvrPluginOnGameStateChange(const NvrGameContext* ctx,
                                                 uint32_t old_state,
                                                 uint32_t new_state)
{
    /*
     * Example (uncomment to track state transitions):
     *
     *   PluginLog("state: %u → %u flags=0x%x",
     *             old_state, new_state, ctx->flags);
     *
     *   if (new_state == 2 /* IN_GAME *​/ && (ctx->flags & NEVR_HOST_HAS_NETGAME)) {
     *       PluginLog("state: entered IN_GAME, net_game=%p", ctx->net_game);
     *   }
     */

    (void)ctx;
    (void)old_state;
    (void)new_state;
}

/*
 * ── NvrPluginShutdown ───────────────────────────────────────────────
 *
 * OPTIONAL. Called before FreeLibrary, in REVERSE load order (last
 * loaded shuts down first).
 *
 * WHY REVERSE ORDER: A plugin loaded later may depend on hooks or state
 * from an earlier plugin. Tearing down the later plugin first ensures
 * its hooks — which may have been installed on top of the earlier
 * plugin's hooks (chained detours) — are removed before what they
 * depend on.
 *
 * RULES:
 *   - Remove every hook you installed (HookManager::RemoveAll or
 *     individual MH_DisableHook + MH_RemoveHook per target).
 *   - Close every socket and handle you opened.
 *   - Join every thread you spawned.
 *   - Free every allocation you made.
 *   - Do NOT call MH_DisableHook(MH_ALL_HOOKS) — that disables hooks
 *     belonging to OTHER plugins and the host itself.
 *   - Do NOT call MH_Uninitialize() — the host owns the MinHook
 *     lifecycle and will uninitialize after the last plugin shuts down.
 *   - After this function returns, the host calls FreeLibrary. Your
 *     DLL's code is no longer mapped. Any thread still executing your
 *     code at that point will crash the process.
 *
 * This function is called even if NvrPluginInit/NvrPluginInitEx
 * returned an error — the host calls shutdown unconditionally if it
 * was exported, so cleanup code must handle partially-initialized
 * state.
 */
NEVR_PLUGIN_API void NvrPluginShutdown(void)
{
    PluginLog("shutdown: removing %zu hooks", g_hooks.count());
    g_hooks.RemoveAll();
    PluginLog("shutdown: complete");
}

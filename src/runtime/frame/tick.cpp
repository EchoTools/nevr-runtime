/*
 * tick.cpp — the runtime's per-frame work dispatcher.
 *
 * It lives apart from patch/binary_bug_fixes.cpp, whose hook drives it: it shares
 * no state with any of the engine bug-fix hooks there.
 *
 * WHICH SITE DRIVES THIS, AND WHY IT MATTERS (N86)
 *
 * The obvious frame site — CPrecisionSleep::Wait, the engine's own frame pacer —
 * is entered ZERO times on a dedicated server. Measured across a full run, with
 * the hook confirmed installed. Wiring per-frame work there and watching
 * `just verify` go green proved the call site existed in source; it proved
 * nothing about whether it runs. Plugins got no OnFrame, modules got no tick,
 * and nothing said so.
 *
 * GetTimeMicroseconds IS live in server mode, and far hotter than once a frame,
 * so dispatch is rate-limited by elapsed time using that hook's own return
 * value — no extra clock, and it tracks real time rather than a call count that
 * varies with engine load.
 *
 * Both sites call this one function. Separate copies could disagree about
 * something as basic as whether the host was a server (N110). One dispatcher,
 * always.
 */

#include "runtime/frame/tick.h"
#include "runtime/frame/frame_stats.h"
#include "runtime/frame/window_watch.h"
#ifdef NEVR_SCENARIO_CONTROL
#include "runtime/scenario/scenario_control.h"
#endif

#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "core/logging.h"
#include "runtime/compat/self_check.h"  // Flush
#include "runtime/ext/module_loader.h"    // TickModules
#include "runtime/ext/plugin_loader.h"    // TickPlugins
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/hook_liveness.h"
#include "runtime/lifecycle/cli.h"             // g_isServer
#include "runtime/lifecycle/config.h"          // LogServerStepReadBack
#include "runtime/lifecycle/crash_recovery.h"  // EnsureStackReserve
#include "runtime/lifecycle/server_timestep.h"
#include "runtime/log/builtin_filter.h"
#include "runtime/patch/invite_answer_apply.h"
#include "runtime/patch/menu_friends_avatars.h"
#include "runtime/patch/menu_friends_pack.h"
#include "runtime/patch/pnsrad_enabler.h"
#include "runtime/patch/render_config_hook.h"
#include "runtime/patch/ui_text_writer.h"

namespace nevr_frame {

namespace {

volatile LONG g_tickReentry = 0;

uint64_t g_lastTickUs = 0;

constexpr uint64_t kTickIntervalUs = 8000;  // ~125 Hz, the frame-pacer cadence
constexpr uint64_t kSelfCheckIntervalUs = 250000;  // self-check probes and sends: four times a second

/// Ticks between periodic liveness reports: ~30s at 8ms.
constexpr LONG kLivenessReportEvery = 3750;

}  // namespace

void DispatchPerFrameWork(uint64_t nowUs) {
    // 0 means the caller had no usable clock. Bail rather than let the unsigned
    // subtraction below wrap to a huge value and fire the tick.
    if (nowUs == 0) return;
    if (nowUs - g_lastTickUs < kTickIntervalUs) return;
    if (InterlockedExchange(&g_tickReentry, 1) != 0) return;  // already inside
    g_lastTickUs = nowUs;

    EnsureStackReserve();  // N69: covers whatever thread drives the loop
    nevr_pnsrad_enabler::ApplyLoadsMissedAtLibraryReturn();
    nevr_pnsrad_enabler::InstallPendingDiagnostics();
    nevr_builtin_log_filter::InstallPnsradHook();  // N90: idempotent; installs once pnsrad.dll loads
    nevr_builtin_log_filter::PollHealth();         // N89: health must not depend on the hook it watches
    nevr_window_watch::Poll(nowUs / 1000);         // #341: a client whose X window vanished quits (Wine only; a no-op on a server)
    nevr_frame_stats::Poll(nowUs);                 // #954: drains the frame-time rings; one line per 10 s (a no-op while diagnostics.frame_stats is off)
    // #1139, ADR 0022: the render_config line. A no-op until Install ran on a client; then one evaluation a second.
    // No new dispatcher, thread or Tick* site (N86): it runs inside this function like the calls around it.
    nevr_render_config_hook::Poll(nowUs);

    // Self-checks: run the probes and send what is queued, a few times a second (a no-op when off).
    {
        static uint64_t s_lastSelfCheckUs = 0;
        if (!g_isServer && nowUs - s_lastSelfCheckUs >= kSelfCheckIntervalUs) {
            s_lastSelfCheckUs = nowUs;
            nevr_self_check::Flush();
        }
    }

    // Liveness + N83/N84 evidence.
    {
        static volatile LONG s_ticks = 0;
        const LONG t = InterlockedIncrement(&s_ticks);
        // #515: one read-back of the server's clock, about a second in. N86/N110: no new dispatcher or
        // Tick* site; this runs inside DispatchPerFrameWork like the rest.
        if (nevr_server_timestep::ShouldReadBack(g_isServer != FALSE, t)) LogServerStepReadBack(t);
        if (t == 1) {
            Log(EchoVR::LogLevel::Info,
                "[NEVR.PATCH] per-frame tick ALIVE — plugin/module OnFrame "
                "now dispatched (host=%s)", g_isServer ? "server" : "client");
        }
        if ((t % kLivenessReportEvery) == 0) {
            // N86-class standing check: name every hook that installed and has
            // never been entered. This is the measurement whose absence let a
            // dead per-frame tick ship for a day.
            nevr_hook_liveness::Report("periodic");

            // N84's only production call site was plugin_loader.cpp, gated on a
            // plugin finishing its init — a server that loads zero plugins (a
            // plain broadcaster host, no plugins/ deployed) never re-verified a
            // single guarded address for its entire run. Any hook clobbered by
            // something other than a plugin double-detour was structurally
            // undetectable. VerifyAll's own ERROR log fires on mismatch; a clean
            // pass gets one quiet heartbeat line instead of nothing.
            const int hookCollisions = nevr_hook_guard::VerifyAll("periodic");
            if (hookCollisions == 0) {
                Log(EchoVR::LogLevel::Debug,
                    "[NEVR.PATCH] hook guard: %d guarded address(es) verified clean (periodic)",
                    nevr_hook_guard::RecordedCount());
            } else {
                // Deliberate severity asymmetry, not an inconsistency: this periodic
                // check reports and keeps the process running (VerifyAll's own ERROR
                // above names the site), while plugin_loader.cpp calls the same
                // VerifyAll() right after a plugin's init and escalates to
                // ServerFatal on the identical nonzero-collisions condition. At
                // plugin-load time the process is still setting up and the collision
                // is almost certainly that plugin's own doing, so failing loud is
                // cheap and precise. Here the server is already live; killing it over
                // a hook that may not even be load-bearing right now would cost more
                // than degrading and reporting does. Logged once per process, not
                // every periodic tick, to match the edge-triggered ERROR above it.
                static bool s_explainedNonFatal = false;
                if (!s_explainedNonFatal) {
                    s_explainedNonFatal = true;
                    Log(EchoVR::LogLevel::Debug,
                        "[NEVR.PATCH] hook guard: periodic collision(s) are reported and "
                        "the server keeps running here — not fatal, unlike "
                        "plugin_loader.cpp's post-plugin-init check on the same "
                        "condition (setup-time collisions fail loud; a live session is "
                        "kept up and degraded instead)");
                }
            }
        }
    }

    NvrGameContext gctx = {};
    gctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
    gctx.flags = g_isServer ? NEVR_HOST_IS_SERVER : NEVR_HOST_IS_CLIENT;
    if (g_isHeadless) gctx.flags |= NEVR_HOST_IS_HEADLESS;
    gctx.ctx_size = sizeof(NvrGameContext);
    gctx.get_plugin_count = GetLoadedPluginCount;
    gctx.get_plugin_info = GetLoadedPluginInfo;
    TickPlugins(&gctx);

    NvrModuleContext mctx = {};
    mctx.base_addr = reinterpret_cast<uintptr_t>(EchoVR::g_GameBaseAddress);
    mctx.flags = gctx.flags;
    TickModules(&mctx);

    // #906, #896: the UI text writer touches the game's UI state, so it runs on the thread that runs the game's main
    // wrapper (crash_recovery.cpp records it) only. This dispatcher is also driven from GetTimeMicroseconds, which any
    // thread may call. The client's CPrecisionSleep::Wait hook is not a usable site: measured entries=0 on a client.
    {
        const unsigned long loop = GameLoopThreadId();
        // #1278: the invite answers become texts first, so the writer below can write them in the same frame.
        if (loop != 0 && loop == GetCurrentThreadId()) nevr_invite_apply::OnFrame();
        // #1535: the FRIENDS panel posts its texts and applies its pictures before the writer below runs.
        if (loop != 0 && loop == GetCurrentThreadId()) nevr_menu_friends_pack::OnFrame();
        if (loop != 0 && loop == GetCurrentThreadId()) nevr_menu_friends_avatars::OnFrame();
        if (loop != 0 && loop == GetCurrentThreadId()) nevr_ui_text::OnFrame();
    }

#ifdef NEVR_SCENARIO_CONTROL
    nevr_scenario_control::OnFrame();  // test builds only: queued scenario actions run on the loop's thread
#endif

    InterlockedExchange(&g_tickReentry, 0);
}

}  // namespace nevr_frame

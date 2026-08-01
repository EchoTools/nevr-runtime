/*
 * tick.cpp — the runtime's per-frame work dispatcher.
 *
 * Extracted from patch/binary_bug_fixes.cpp (was wave0_instrumentation.cpp) on
 * 2026-07-29. It had been sitting in a file of engine bug-fix hooks because the
 * hook that drives it lives there, not because it belongs there: it shares no
 * state with any of them.
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
 * Both sites call this one function. They used to have a copy each, and the
 * copies disagreed about something as basic as whether the host was a server
 * (N110). One dispatcher, always.
 */

#include "runtime/frame/tick.h"

#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "core/logging.h"
#include "runtime/ext/module_loader.h"    // TickModules
#include "runtime/ext/plugin_loader.h"    // TickPlugins
#include "runtime/hook/hook_liveness.h"
#include "runtime/lifecycle/cli.h"             // g_isServer
#include "runtime/lifecycle/crash_recovery.h"  // EnsureStackReserve
#include "runtime/log/builtin_filter.h"
#include "runtime/patch/mode_patches.h"   // LogBroadcasterHookStats

namespace Frame {

namespace {

volatile LONG g_tickReentry = 0;
uint64_t g_lastTickUs = 0;

constexpr uint64_t kTickIntervalUs = 8000;  // ~125 Hz, the frame-pacer cadence

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
    BuiltinLogFilter::InstallPnsradHook();  // N90: idempotent; installs once pnsrad.dll loads
    BuiltinLogFilter::PollHealth();         // N89: health must not depend on the hook it watches

    // Liveness + N83/N84 evidence.
    {
        static volatile LONG s_ticks = 0;
        const LONG t = InterlockedIncrement(&s_ticks);
        if (t == 1) {
            Log(EchoVR::LogLevel::Info,
                "[NEVR.PATCH] per-frame tick ALIVE — plugin/module OnFrame "
                "now dispatched (host=%s)", g_isServer ? "server" : "client");
        }
        if ((t % kLivenessReportEvery) == 0) {
            LogBroadcasterHookStats();
            // N86-class standing check: name every hook that installed and has
            // never been entered. This is the measurement whose absence let a
            // dead per-frame tick ship for a day.
            HookLiveness::Report("periodic");
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

    InterlockedExchange(&g_tickReentry, 0);
}

}  // namespace Frame

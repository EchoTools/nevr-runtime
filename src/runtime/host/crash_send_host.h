#pragma once

// The PC half of the crash report send (core/crash_send.h, #1689): the BugSplat64.dll host sends the reports a crash left
// in %LOCALAPPDATA%\nevr\echovr\crashes to nevr-api, on every start, before it loads nevr-runtime.dll. It needs no runtime,
// no config, no login and no game state, so it works when the runtime crashed or failed to load. The POST is WinHTTP loaded
// by name (crash_send_winhttp.h); everything above it is the shared header.

#include "runtime/host/bootstrap_diagnostics.h"

namespace nevr_host {

/// Starts the send on a thread of its own, once per process, and returns at once: a dead network, a slow answer or a
/// service that is down never delays the game. Call it from a thread that is not under the loader lock (the detour on the
/// game's main wrapper, before the runtime is loaded). Does nothing, and writes nothing, while the build has the send
/// switched off (nevr_crash_send::kSendEnabled). Each event is one `crash_send` record in the host log; a report's text
/// and the request body are never logged.
void StartCrashSend(BootstrapDiagnostics& diagnostics) noexcept;

}  // namespace nevr_host

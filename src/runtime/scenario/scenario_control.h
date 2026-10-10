#pragma once

// Scenario control endpoint. Compiled ONLY when the CMake option NEVR_SCENARIO_CONTROL is ON (the
// mingw-scenario preset); release builds have no trace of it, and `just verify` checks that the
// release DLL carries none of its strings. It can inject messages into a live session, so it must
// never ship. Protocol: src/runtime/scenario/scenario_protocol.h; design:
// docs/design/2026-10-01-social-scenario-harness.md.

namespace nevr_scenario_control {

/// Opens the 127.0.0.1 control socket (ephemeral port, logged) and starts its thread. Boot, after
/// the websocket bridge is up.
void Start();

/// Runs queued game-side actions (fire). Called from the per-frame tick, on the thread that drives
/// the game loop.
void OnFrame();

/// Closes the control socket and joins its thread. Graceful-shutdown path.
void Stop();

}  // namespace nevr_scenario_control

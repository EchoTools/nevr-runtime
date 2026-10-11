#pragma once
// Server-side diagnostic for co-op AI bots that never move (issue #63). It changes nothing in the game:
// it hooks the bot blackboard's update and its two setup lookups, and logs each bot's setup result,
// lookup actor and phase when they change, plus a heartbeat with the game clock. Gates and evidence:
// coop_ai_trace_rules.h.

#include <cstdint>

namespace nevr_coop_ai_trace {

// Hooks CR15AIBBNetbot vslot 4 (0x1412c1a30), the goals lookup (0x1412c10f0) and the waypoints lookup
// (0x1412c13d0). Each hook is installed only after its prologue is validated; a mismatch is logged and
// that hook is skipped. Server only; call after the CLI is parsed.
void Install(std::uintptr_t gameBase);

}  // namespace nevr_coop_ai_trace

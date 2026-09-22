# NEVR Runtime Source Bugs

The N-prefix ledger was retired on 2026-08-02. New defects are tracked in
GitHub issues; this file remains so repository workflows have a stable ledger
location and can distinguish runtime defects from binary-audit findings.

## Resolved in the current worktree

- `PlatformPrefix` omitted the documented Nakama `BOT` platform code (`5`) in
  `src/runtime/compat/ws_bridge.cpp`. Production-linked Wine coverage now pins
  all defined platform prefixes, and the mapping returns `BOT`.
- The N71 attribution table was documented by the sprint as 31 sites, while
  the measured reconstruction held 30 named entries and an accidental empty
  array element. It now has an exact 30-entry type, with a Wine test and the
  persistent `just verify` sensor pinned to that source of truth.
- Device-code poll error replies were previously folded into the retry state.
  Parsing now distinguishes `verified`, `authorization_pending`, `expired`,
  malformed, and explicit error responses; the handler returns an error rather
  than consuming every server-side rejection as another poll interval.

## pnsradmatchmaking.dll reload drops our patch (matchmaking terminal blank / no queue)

**Found 2026-09-13 07:39 by Andrew, live in the lobby.** `pnsradmatchmaking.dll`
is UNLOADED and RELOADED during a session (not resident for the whole run). Our
patch is applied once at boot, so after the module reloads the reloaded copy is
UNPATCHED: the matchmaking terminal fails to populate ("PUBLIC MATCH" screen
blank) and queuing does not work.

**Corroborating symptom this session:** a single native
`[WARNING] ping request received from non-matchmaker peer, disconnecting`
(pnsradmatchmaking.dll VA 0x1801c8868) at lobby-scene load — peer-identity
gating in the reloaded, unpatched module.

**Fix direction (Andrew):** the patch must be applied EVERY TIME the module
loads, not once. Hook the module-load path (LdrLoadDll / LoadLibrary detour, or
detect the module base changing) and re-install the pnsradmatchmaking patches
idempotently on each (re)appearance, keyed on module base.

**NOT the lockout.** The debug_lockout force (bit-46) blocking queue is separate
and working as intended; this is the matchmaking module patch not surviving a
reload. — filed by glow ꩜🌊

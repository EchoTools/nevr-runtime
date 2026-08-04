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

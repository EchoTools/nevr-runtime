// Package updatewiring holds the source-wiring checks for the runtime's half of PC self-update (docs/adr/0024-pc-self-update.md, #1200):
// what the host tests (test_update_worker, test_update_core_link) cannot see, which is where the worker is started from, what feeds its
// mark, and the one claim ADR 0024 makes about files (the check and the confirmation write nothing outside the nevr root). Its tests read
// the tree. They are source-shape checks (rank 3 of docs/standards/verification.md): they fail when a call is removed or moved and say
// nothing about whether a run does it; only a run of the game shows that (`update check` in the log).
package updatewiring

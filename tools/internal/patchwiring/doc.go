// Package patchwiring holds source-wiring checks for the runtime's game patches (src/runtime/patch): what a host test cannot
// see, because the files that hook the game (new_unlocks_clear.cpp and its siblings) link no test binary. Its tests read the
// tree. They are source-shape checks (rank 3 of docs/standards/verification.md): they fail when a call is removed or moved and
// say nothing about whether a run does it.
package patchwiring

// Package serverwiring holds source-wiring checks for the PC dedicated server (src/runtime/server): what a host test cannot
// see, because the files that the game calls into (join_hold.cpp and its siblings) link no test binary. Its tests read the
// tree.
package serverwiring

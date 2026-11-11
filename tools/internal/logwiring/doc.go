// Package logwiring holds source-wiring checks for the runtime's log path: what a host test cannot see, because the
// files that hook the game (log/builtin_filter.cpp, lifecycle/crash_recovery.cpp) link no test binary. Its tests read
// the tree.
package logwiring

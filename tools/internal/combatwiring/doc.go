// Package combatwiring holds source-wiring checks for the built-in EchoCombat module (src/runtime/combat): what a host
// test cannot see, because the files that hook the game (veilnet.cpp and its siblings) link no test binary. Its tests
// read the tree.
package combatwiring

// Package questlifts checks docs/quest/combat/script_lifts.md, the reference of what each aarch64 lift of
// he_is_the_cat's EchoCombat scripts in the reference Quest combat build carries (#2242).
//
// That page's table under "## The lifts" is the one list of the scripts. Parse reads its rows: each starts with the
// script's 16-hex hash in backticks, then the PC name, then two backticked 64-hex sha256 values (Cat's image, then the
// lift). Parse refuses a row whose hash or digests are malformed, a hash named twice, and a table whose row count is not
// ScriptCount, each error naming the row. CheckLibs hashes lib<hash>.so in a directory for every row and names each file
// that is missing or whose sha256 differs from the row's lift digest.
//
// The test reads the page from the checkout. The libraries are a per-machine input: NEVR_QUEST_REFERENCE_LIBS names the
// directory that holds the reference APK's lib<hash>.so files (its lib/arm64-v8a); without it that part is skipped.
// Standard library only (ADR 0044).
package questlifts

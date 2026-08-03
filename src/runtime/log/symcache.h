#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace evr {

/// One entry in the embedded symbol cache (auto-generated).
struct SymCacheEntry {
    uint64_t hash;
    const char* name;
};

/// Load the embedded symbol cache into memory. Safe to call multiple times (no-ops
/// after the first call). Must be called before ResolveSymbolHashes or AnnotateGameLine.
void InitSymbolCache();

/// Replace all 0x{16} hashes in `line` with their resolved names.
/// Unknown hashes pass through unchanged. Returns the modified line.
/// Requires InitSymbolCache() to have been called first.
std::string ResolveSymbolHashes(const std::string& line);

/// Wrap a game-native log line with the [EVR] prefix and resolved symbols.
/// Input:  raw game log line (already has timestamp/level from the game's logger)
/// Output: same line with [EVR] prefix and resolved 0x{16} symbol hashes
/// Requires InitSymbolCache() to have been called first.
std::string AnnotateGameLine(const std::string& line);

}  // namespace evr
